#include "client.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

namespace voxel::net {

Client::Client(Peer server, std::string host)
    : m_server(std::move(server)), m_host(std::move(host)) {
    std::random_device random;
    for (auto& byte : m_uuid)
        byte = static_cast<std::uint8_t>(random());
    m_localYaw = 0.0f;
    m_localPitch = -18.0f;
}

// A destructor must never let an exception escape: that terminates the process.
Client::~Client() noexcept {
    try {
        stop();
    } catch (const std::exception& error) {
        std::cerr << "Client: shutdown failed: " << error.what() << '\n';
    }
}

void Client::start() {
    if (m_running.exchange(true)) return;
    m_socket.send(encode(Registration{m_uuid}), m_server);
    m_thread = std::thread(&Client::receiveLoop, this);
    m_worldThread = std::thread(&Client::worldReceiveLoop, this);
}

void Client::stop() {
    // Always join, even if stop() was already called: a std::thread whose
    // destructor runs while it is still joinable terminates the process.
    m_running.store(false);
    m_worldSocket.shutdown();
    if (m_worldThread.joinable()) m_worldThread.join();
    if (m_thread.joinable()) m_thread.join();
}

void Client::receiveLoop() {
    m_socket.nonBlocking();
    std::array<std::byte, 2048> buffer{};
    while (m_running.load()) {
        Peer peer;
        const auto size = m_socket.receive(buffer, peer);
        if (size <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const auto bytes = std::span<const std::byte>(
            buffer.data(), static_cast<std::size_t>(size));

        RegistrationAck acknowledgement{};
        if (decode(bytes, acknowledgement)) {
            std::scoped_lock lock(m_mutex);
            if (acknowledgement.uuid != m_uuid) return;
            m_registered = true;
            // The server assigns the id; it is needed for the world stream
            // handshake, which can happen before the first snapshot arrives.
            m_playerId = acknowledgement.playerId;
            continue;
        }
        AuthoritativeState snapshot{};
        if (decode(bytes, snapshot)) {
            acceptSnapshot(std::move(snapshot));
            continue;
        }
    }
}

void Client::worldReceiveLoop() {
    // Wait for UDP registration: the server refuses a stream it cannot tie to
    // a registered client.
    float waited = 0.0f;
    while (m_running.load()) {
        {
            std::scoped_lock lock(m_mutex);
            if (m_registered) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        waited += 0.02f;
        if (waited > 5.0f) {
            std::cerr << "Client: world stream waiting for registration\n";
            waited = 0.0f;
        }
    }
    if (!m_running.load()) return;

    for (;;) {
        try {
            m_worldSocket.connect(
                m_host.c_str(),
                static_cast<std::uint16_t>(m_server.port + 1));
            break;
        } catch (const std::runtime_error&) {
            if (!m_running.load()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

    WorldInterest hello{};
    hello.uuid = m_uuid;
    {
        std::scoped_lock lock(m_mutex);
        hello.playerId = m_playerId;
        hello.radius = m_interestRadius;
    }
    if (!m_worldSocket.sendFrame(encode(hello))) return;

    std::vector<std::byte> frame;
    // 1. the world descriptor: geometry and the server-owned material table
    if (!m_worldSocket.receiveFrame(frame)) return;
    WorldDescriptor descriptor{};
    if (!decode(frame, descriptor)) {
        std::cerr << "Client: invalid world descriptor\n";
        return;
    }
    {
        std::scoped_lock lock(m_mutex);
        m_spec = voxel::WorldSpec{descriptor.sizeX, descriptor.sizeY,
                                  descriptor.sizeZ, descriptor.brick};
        m_streamSpec = m_spec;
        m_materials = descriptor.materials;
        m_hasSpec = true;
        m_render.sizeX = descriptor.sizeX;
        m_render.sizeY = descriptor.sizeY;
        m_render.sizeZ = descriptor.sizeZ;
        m_render.brick = descriptor.brick;
        m_render.rootDimX = descriptor.rootDimX;
        m_render.rootDimY = descriptor.rootDimY;
        m_render.rootDimZ = descriptor.rootDimZ;
        m_render.rootCount = descriptor.rootCount;
        m_interestRadius = static_cast<std::uint8_t>(
            std::min<std::uint8_t>(m_interestRadius,
                                  static_cast<std::uint8_t>(6)));
        // Handed to the renderer once; the renderer clears it after upload.
        m_render.materials = std::move(descriptor.materials);
    }

    // 2. stream bricks, acknowledging each one so the server stops resending.
    constexpr int kWaitMs = 50;
    while (m_running.load() && m_worldSocket.valid()) {
        WorldInterest interest{};
        std::vector<WorldAck> acks;
        bool sendInterest = false;
        {
            std::scoped_lock lock(m_mutex);
            if (m_hasSnapshots) {
                const std::int32_t brick =
                    static_cast<std::int32_t>(m_streamSpec.brick);
                const std::int32_t dimX =
                    static_cast<std::int32_t>(m_streamSpec.rootDimX());
                const std::int32_t dimY =
                    static_cast<std::int32_t>(m_streamSpec.rootDimY());
                const std::int32_t dimZ =
                    static_cast<std::int32_t>(m_streamSpec.rootDimZ());
                const std::int32_t cx = static_cast<std::int32_t>(
                    std::floor(m_render.localPlayer.position.x));
                const std::int32_t cy = static_cast<std::int32_t>(
                    std::floor(m_render.localPlayer.position.y));
                const std::int32_t cz = static_cast<std::int32_t>(
                    std::floor(m_render.localPlayer.position.z));
                const std::int32_t bx =
                    std::clamp(cx / brick, 0, dimX - 1) * brick;
                const std::int32_t by =
                    std::clamp(cy / brick, 0, dimY - 1) * brick;
                const std::int32_t bz =
                    std::clamp(cz / brick, 0, dimZ - 1) * brick;
                if (!m_hasInterest || bx != m_interestX || by != m_interestY ||
                    bz != m_interestZ) {
                    m_interestX = bx;
                    m_interestY = by;
                    m_interestZ = bz;
                    m_hasInterest = true;
                    sendInterest = true;
                    interest.uuid = m_uuid;
                    interest.playerId = m_playerId;
                    interest.centerX = bx;
                    interest.centerY = by;
                    interest.centerZ = bz;
                    interest.radius = m_interestRadius;
                }
            }
            acks.swap(m_pendingAcks);
        }
        if (sendInterest && !m_worldSocket.sendFrame(encode(interest))) return;
        for (const WorldAck& ack : acks)
            if (!m_worldSocket.sendFrame(encode(ack))) return;

        if (!m_worldSocket.pollReadable(kWaitMs)) continue;
        if (!m_worldSocket.receiveFrame(frame)) return;
        BrickSnapshot snapshot{};
        if (!decode(frame, snapshot)) continue;
        enqueueBrick(std::move(snapshot));
    }
}

void Client::enqueueBrick(BrickSnapshot snapshot) {
    const voxel::WorldSpec spec = m_streamSpec;
    if (spec.rootCount() == 0 || snapshot.rootIndex >= spec.rootCount())
        return;
    std::scoped_lock lock(m_mutex);
    const auto found = m_resident.find(snapshot.rootIndex);
    if (found != m_resident.end() && found->second.revision == snapshot.revision)
        return;   // already held at this revision

    voxel::BrickUpdate update{};
    update.rootIndex = snapshot.rootIndex;
    update.revision = snapshot.revision;
    update.nodes = std::move(snapshot.nodes);
    m_render.bricks.push_back(std::move(update));
    m_resident[snapshot.rootIndex].revision = snapshot.revision;
    m_pendingAcks.push_back(WorldAck{m_uuid, snapshot.playerId,
                                     snapshot.rootIndex, snapshot.revision});
    // Bounded queue: a slow render thread must not grow the client heap.
    while (m_render.bricks.size() > 256) m_render.bricks.erase(m_render.bricks.begin());
}

void Client::submit(const InputIntent& intent, float frameSeconds) {
    std::scoped_lock lock(m_mutex);
    m_registrationRetry -= frameSeconds;
    m_inputAccumulator += frameSeconds;
    if (!m_registered && m_registrationRetry <= 0.0f) {
        m_socket.send(encode(Registration{m_uuid}), m_server);
        m_registrationRetry = 0.5f;
    }

    // Raw intent only. The server computes displacement from its own yaw, so
    // no client-side movement maths ever reaches the wire.
    const glm::vec2 move = glm::clamp(intent.move, glm::vec2(-1.0f),
                                      glm::vec2(1.0f));
    m_accumulatedLook += intent.look;
    m_accumulatedLook = glm::clamp(m_accumulatedLook, glm::vec2(-0.5f),
                                   glm::vec2(0.5f));
    m_latestClientTick = intent.clientTick;
    m_jumpRequested = m_jumpRequested || intent.jumpPressed;

    if (m_registered && m_inputAccumulator >= voxel::kTickSeconds) {
        InputIntent out{};
        out.clientTick = m_latestClientTick;
        out.move = move;
        out.look = m_accumulatedLook;
        out.jumpPressed = m_jumpRequested;
        if (m_socket.send(encode(out), m_server)) {
            m_inputAccumulator -= voxel::kTickSeconds;
            m_accumulatedLook = glm::vec2(0.0f);
            m_jumpRequested = false;
        }
    }

    // Look stays local for responsiveness; the snapshot still carries the
    // authoritative orientation for everyone else.
    m_localYaw += intent.look.x;
    while (m_localYaw > 180.0f) m_localYaw -= 360.0f;
    while (m_localYaw < -180.0f) m_localYaw += 360.0f;
    m_localPitch = std::clamp(m_localPitch + intent.look.y, -89.0f, 89.0f);
    m_render.localPlayer.yaw = m_localYaw;
    m_render.localPlayer.pitch = m_localPitch;
}

void Client::acceptSnapshot(AuthoritativeState snapshot) {
    std::scoped_lock lock(m_mutex);
    const std::uint32_t newest =
        m_hasSnapshots ? m_next.tick : m_authoritative.tick;
    if (snapshot.tick <= newest) return;
    m_snapshots.push_back(std::move(snapshot));
    while (m_snapshots.size() > 8) m_snapshots.pop_front();
}

void Client::updateRender(float frameSeconds) {
    std::scoped_lock lock(m_mutex);
    while (!m_snapshots.empty()) {
        AuthoritativeState incoming = std::move(m_snapshots.front());
        m_snapshots.pop_front();
        if (!m_hasSnapshots) {
            m_previous = incoming;
            m_next = std::move(incoming);
            m_hasSnapshots = true;
        } else {
            m_previous = m_next;
            m_next = std::move(incoming);
        }
        m_authoritative = m_next;
        m_clock = 0.0f;
    }

    // Evict bricks that fell out of the interest window. The renderer frees
    // their GPU slice and clears the root table entry.
    if (m_hasSpec && m_hasInterest) {
        const std::int32_t brick = static_cast<std::int32_t>(m_spec.brick);
        const std::int32_t dimX = static_cast<std::int32_t>(m_spec.rootDimX());
        const std::int32_t dimY = static_cast<std::int32_t>(m_spec.rootDimY());
        const std::int32_t cx = m_interestX / brick;
        const std::int32_t cy = m_interestY / brick;
        const std::int32_t cz = m_interestZ / brick;
        const std::int32_t radius = static_cast<std::int32_t>(m_interestRadius);
        for (auto it = m_resident.begin(); it != m_resident.end();) {
            const std::int32_t bx = static_cast<std::int32_t>(it->first % dimX);
            const std::int32_t by = static_cast<std::int32_t>(
                (it->first / dimX) % dimY);
            const std::int32_t bz = static_cast<std::int32_t>(
                it->first / (dimX * dimY));
            const std::int32_t distance = std::max(
                {std::abs(bx - cx), std::abs(by - cy), std::abs(bz - cz)});
            if (distance > radius) {
                voxel::BrickUpdate update{};
                update.rootIndex = it->first;
                update.remove = true;
                m_render.bricks.push_back(std::move(update));
                it = m_resident.erase(it);
            } else {
                ++it;
            }
        }
    }

    m_clock = std::min(m_clock + frameSeconds, voxel::kTickSeconds);
    m_render.localPlayer = m_authoritative.localPlayer;
    m_render.localPlayer.yaw = m_localYaw;
    m_render.localPlayer.pitch = m_localPitch;
    if (!m_hasSnapshots) return;
    const float alpha = std::clamp(m_clock / voxel::kTickSeconds, 0.0f, 1.0f);
    m_render.localPlayer.position = glm::mix(m_previous.localPlayer.position,
                                             m_next.localPlayer.position, alpha);
    m_render.localPlayer.velocity = glm::mix(m_previous.localPlayer.velocity,
                                             m_next.localPlayer.velocity, alpha);
}

} // namespace voxel::net
