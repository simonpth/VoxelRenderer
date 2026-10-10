#include "client.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <random>
#include <iostream>
#include <stdexcept>

namespace voxel::net {
Client::Client(Peer server) : m_server(std::move(server)) {
    std::random_device random;
    for (auto& byte : m_uuid) byte = static_cast<std::uint8_t>(random());
    m_localPitch = -18.0f;
}
Client::~Client() { stop(); }
void Client::start() {
    if (m_running.exchange(true)) return;
    m_socket.send(encode(Registration{m_uuid}), m_server);
    m_thread = std::thread(&Client::receiveLoop, this);
    m_worldThread = std::thread(&Client::worldReceiveLoop, this);
}
void Client::stop() {
    if (!m_running.exchange(false)) return;
    m_worldSocket.shutdown();
    if (m_thread.joinable()) m_thread.join();
    if (m_worldThread.joinable()) m_worldThread.join();
}
void Client::worldReceiveLoop() {
    try {
        while (m_running) {
            RegionCoord center{};
            {
                std::scoped_lock lock(m_mutex);
                const auto& position = m_render.localPlayer.position;
                center.x = static_cast<std::uint16_t>(std::clamp(
                    static_cast<std::int32_t>(std::floor(position.x)) /
                        static_cast<std::int32_t>(kRegionEdge),
                    0, static_cast<std::int32_t>(kRegionCountX - 1)));
                center.y = static_cast<std::uint16_t>(std::clamp(
                    static_cast<std::int32_t>(std::floor(position.y)) /
                        static_cast<std::int32_t>(kRegionEdge),
                    0, static_cast<std::int32_t>(kRegionCountY - 1)));
                center.z = static_cast<std::uint16_t>(std::clamp(
                    static_cast<std::int32_t>(std::floor(position.z)) /
                        static_cast<std::int32_t>(kRegionEdge),
                    0, static_cast<std::int32_t>(kRegionCountZ - 1)));
            }
            for (std::int32_t dz = -1; dz <= 1 && m_running; ++dz) {
                for (std::int32_t dy = -1; dy <= 1 && m_running; ++dy) {
                    for (std::int32_t dx = -1; dx <= 1 && m_running; ++dx) {
                        RegionCoord requested = center;
                        const auto requestedX =
                            static_cast<std::int32_t>(center.x) + dx;
                        const auto requestedY =
                            static_cast<std::int32_t>(center.y) + dy;
                        if (requestedX < 0 ||
                            requestedX >= static_cast<std::int32_t>(kRegionCountX) ||
                            requestedY < 0 ||
                            requestedY >= static_cast<std::int32_t>(kRegionCountY))
                            continue;
                        requested.x = static_cast<std::uint16_t>(requestedX);
                        requested.y = static_cast<std::uint16_t>(requestedY);
                        requested.z = static_cast<std::uint16_t>(
                            std::clamp(static_cast<std::int32_t>(center.z) + dz,
                                       0, static_cast<std::int32_t>(kRegionCountZ - 1)));
                        {
                            std::scoped_lock lock(m_mutex);
                            if (m_regionRevisions.contains(requested)) continue;
                        }
                        TcpSocket connection;
                        try {
                            connection.connect(m_server.host.c_str(),
                                               static_cast<std::uint16_t>(m_server.port + 1));
                        } catch (const std::runtime_error&) {
                            continue;
                        }
                        if (!connection.sendFrame(encode(Registration{m_uuid})) ||
                            !connection.sendFrame(encode(RegionInterestRequest{
                                m_uuid, center, requested, 1})))
                            continue;
                std::vector<std::byte> frame;
                if (!connection.receiveFrame(frame)) continue;
                RegionSnapshot snapshot{};
                if (!decode(frame, snapshot)) continue;
                std::scoped_lock lock(m_mutex);
                m_regionSnapshots.push_back(std::move(snapshot));
                while (m_regionSnapshots.size() > 64) m_regionSnapshots.pop_front();
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    } catch (const std::runtime_error& error) {
        std::cerr << "Client: world stream unavailable: " << error.what() << '\n';
    }
}
void Client::submit(InputIntent intent, float frameSeconds) {
    std::scoped_lock lock(m_mutex);
    m_registrationRetry -= frameSeconds;
    m_inputSendAccumulator += frameSeconds;
    if (!m_registered && m_registrationRetry <= 0.0f) {
        m_socket.send(encode(Registration{m_uuid}), m_server);
        m_registrationRetry = 0.5f;
    }
    const float length = glm::length(intent.move);
    const glm::vec2 direction = length > 1.0f ? intent.move / length : intent.move;
    const float yawRadians = glm::radians(m_localYaw);
    const glm::vec2 forward{std::cos(yawRadians), std::sin(yawRadians)};
    const glm::vec2 right{-forward.y, forward.x};
    // Project local WASD intent onto the current local camera yaw. The server
    // receives the resulting displacement, while look remains independently
    // replicated for other players.
    const glm::vec2 delta = (right * direction.x + forward * direction.y) *
                            5.0f * frameSeconds;
    if (!m_registered) {
        m_accumulatedHorizontalMovement = glm::vec2(0.0f);
        m_accumulatedLook = glm::vec2(0.0f);
    }
    m_accumulatedHorizontalMovement += delta;
    // The server validates one tick's displacement. Do not let time spent
    // waiting for registration turn into a permanently rejected packet.
    const float movementLength = glm::length(m_accumulatedHorizontalMovement);
    if (movementLength > 0.20f)
        m_accumulatedHorizontalMovement *= 0.20f / movementLength;
    m_accumulatedLook += intent.look;
    m_accumulatedLook = glm::clamp(m_accumulatedLook,
                                   glm::vec2(-45.0f), glm::vec2(45.0f));
    m_latestClientTick = intent.clientTick;
    m_jumpRequested = m_jumpRequested || intent.jumpPressed;
    if (m_registered && m_inputSendAccumulator >= kTickSeconds) {
        intent.horizontalDelta = m_accumulatedHorizontalMovement;
        intent.look = m_accumulatedLook;
        intent.jumpPressed = m_jumpRequested;
        if (m_socket.send(encode(intent), m_server)) {
            m_inputSendAccumulator -= kTickSeconds;
            m_accumulatedHorizontalMovement = glm::vec2(0.0f);
            m_accumulatedLook = glm::vec2(0.0f);
            m_jumpRequested = false;
        }
    }
    m_localYaw += intent.look.x;
    m_localPitch = std::clamp(m_localPitch + intent.look.y, -89.0f, 89.0f);
    m_render.localPlayer.yaw = m_localYaw;
    m_render.localPlayer.pitch = m_localPitch;
}
void Client::receiveLoop() {
    m_socket.nonBlocking();
    std::array<std::byte, 2048> buffer{};
    while (m_running) {
        Peer peer; const auto size = m_socket.receive(buffer, peer);
        if (size <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const auto bytes = std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(size));
        RegistrationAck acknowledgement{};
        if (decode(bytes, acknowledgement)) {
            std::scoped_lock lock(m_mutex);
            if (acknowledgement.uuid == m_uuid) m_registered = true;
            continue;
        }
        AuthoritativeState snapshot{};
        if (decode(bytes, snapshot)) { acceptSnapshot(std::move(snapshot)); continue; }
        VoxelChange change{};
        if (decode(bytes, change)) {
            std::scoped_lock lock(m_mutex);
            if (change.revision > m_authoritative.worldRevision) m_changes.push_back(change);
        }
    }
}
void Client::acceptSnapshot(AuthoritativeState snapshot) {
    std::scoped_lock lock(m_mutex);
    const std::uint32_t newest = m_hasSnapshots ? m_next.tick : m_authoritative.tick;
    if (snapshot.tick <= newest) return;
    m_snapshots.push_back(std::move(snapshot));
    while (m_snapshots.size() > 8) m_snapshots.pop_front();
}
void Client::updateRender(float frameSeconds) {
    std::scoped_lock lock(m_mutex);
    while (!m_snapshots.empty()) {
        AuthoritativeState incoming = std::move(m_snapshots.front());
        m_snapshots.pop_front();
        // Movement snapshots intentionally do not contain the voxel array.
        // Keep the separately synchronized authoritative world intact.
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
    while (!m_changes.empty()) {
        const auto change = m_changes.front(); m_changes.pop_front();
        if (change.revision > m_authoritative.worldRevision) {
            m_authoritative.world[change.index] = change.value;
            m_authoritative.worldRevision = change.revision;
        }
    }
    while (!m_regionSnapshots.empty()) {
        RegionSnapshot snapshot = std::move(m_regionSnapshots.front());
        m_regionSnapshots.pop_front();
        const auto found = m_regionRevisions.find(snapshot.coordinate);
        if (found == m_regionRevisions.end() ||
            snapshot.revision > found->second) {
            m_regionRevisions[snapshot.coordinate] = snapshot.revision;
            voxel::RenderState::Region update{};
            update.x = snapshot.coordinate.x;
            update.y = snapshot.coordinate.y;
            update.z = snapshot.coordinate.z;
            update.depth = snapshot.depth;
            update.revision = snapshot.revision;
            update.packedNodes.reserve(snapshot.nodes.size() * 3);
            for (const auto& node : snapshot.nodes) {
                update.packedNodes.push_back(node.childMask);
                update.packedNodes.push_back(node.firstChild);
                update.packedNodes.push_back(static_cast<std::uint32_t>(node.material) |
                                             (static_cast<std::uint32_t>(node.flags) << 16u));
            }
            m_render.regions.push_back(std::move(update));
        }
        const RegionCoord currentRegion{
            static_cast<std::uint16_t>(
                static_cast<std::int64_t>(std::floor(m_render.localPlayer.position.x)) /
                static_cast<std::int64_t>(kRegionEdge) % kRegionCountX),
            static_cast<std::uint16_t>(
                static_cast<std::int64_t>(std::floor(m_render.localPlayer.position.y)) /
                static_cast<std::int64_t>(kRegionEdge) % kRegionCountY),
            static_cast<std::uint16_t>(std::clamp(
                static_cast<std::int32_t>(std::floor(m_render.localPlayer.position.z)) /
                    static_cast<std::int32_t>(kRegionEdge),
                0, static_cast<std::int32_t>(kRegionCountZ - 1)))};
        for (auto it = m_regionRevisions.begin(); it != m_regionRevisions.end();) {
            const auto distance = std::abs(static_cast<int>(it->first.x) -
                                           static_cast<int>(currentRegion.x)) +
                                  std::abs(static_cast<int>(it->first.y) -
                                           static_cast<int>(currentRegion.y)) +
                                  std::abs(static_cast<int>(it->first.z) -
                                           static_cast<int>(currentRegion.z));
            if (distance > 6) {
                voxel::RenderState::Region update{};
                update.x = it->first.x;
                update.y = it->first.y;
                update.z = it->first.z;
                update.remove = true;
                m_render.regions.push_back(std::move(update));
                it = m_regionRevisions.erase(it);
            }
            else ++it;
        }
    }
    m_clock = std::min(m_clock + frameSeconds, kTickSeconds);
    m_render.tick = m_authoritative.tick;
    m_render.worldRevision = m_authoritative.worldRevision;
    m_render.localPlayer = m_authoritative.localPlayer;
    m_render.localPlayer.yaw = m_localYaw;
    m_render.localPlayer.pitch = m_localPitch;
    if (!m_hasSnapshots) return;
    const float alpha = std::clamp(m_clock / kTickSeconds, 0.0f, 1.0f);
    m_render.localPlayer.position = glm::mix(m_previous.localPlayer.position,
                                             m_next.localPlayer.position, alpha);
    m_render.localPlayer.velocity = glm::mix(m_previous.localPlayer.velocity,
                                             m_next.localPlayer.velocity, alpha);
}
}
