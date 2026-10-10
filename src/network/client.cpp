#include "client.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace voxel::net {
namespace {
constexpr float kPlayerRadius = 0.35f;
constexpr float kPlayerHeight = 2.0f;

glm::vec2 resolveHorizontalMovement(const VoxelWorld& world,
                                    glm::vec3 position,
                                    glm::vec2 delta) {
    const auto collides = [&](glm::vec3 candidate) {
        const int minX = static_cast<int>(std::floor(candidate.x - kPlayerRadius));
        const int maxX = static_cast<int>(std::floor(candidate.x + kPlayerRadius));
        const int minY = static_cast<int>(std::floor(candidate.y - kPlayerRadius));
        const int maxY = static_cast<int>(std::floor(candidate.y + kPlayerRadius));
        const int minZ = static_cast<int>(std::floor(candidate.z));
        const int maxZ = static_cast<int>(
            std::ceil(candidate.z + kPlayerHeight) - 1.0f);
        for (int z = minZ; z <= maxZ; ++z)
            for (int y = minY; y <= maxY; ++y)
                for (int x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || z < 0 ||
                        x >= static_cast<int>(kWorldEdge) ||
                        y >= static_cast<int>(kWorldEdge) ||
                        z >= static_cast<int>(kWorldEdge)) {
                        return true;
                    }
                    const auto index = static_cast<std::size_t>(x) +
                                       kWorldEdge * (static_cast<std::size_t>(y) +
                                       kWorldEdge * static_cast<std::size_t>(z));
                    if (world[index].material != 0) return true;
                }
        return false;
    };

    glm::vec3 resolved = position;
    resolved.x = std::clamp(position.x + delta.x, kPlayerRadius,
                            static_cast<float>(kWorldEdge) - kPlayerRadius);
    if (collides(resolved)) resolved.x = position.x;
    resolved.y = std::clamp(position.y + delta.y, kPlayerRadius,
                            static_cast<float>(kWorldEdge) - kPlayerRadius);
    if (collides(resolved)) resolved.y = position.y;
    return {resolved.x - position.x, resolved.y - position.y};
}
}

Client::Client(Peer server) : m_server(std::move(server)) {
    m_render.world = m_authoritative.world;
}
Client::~Client() { stop(); }
void Client::start() {
    if (m_running.exchange(true)) return;
    m_thread = std::thread(&Client::receiveLoop, this);
}
void Client::stop() {
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();
}
void Client::submit(InputIntent intent, float frameSeconds) {
    std::scoped_lock lock(m_mutex);
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
    const glm::vec2 resolvedDelta = resolveHorizontalMovement(
        m_authoritative.world, m_render.localPlayer.position, delta);
    m_accumulatedHorizontalMovement += resolvedDelta;
    m_accumulatedLook += intent.look;
    m_latestClientTick = intent.clientTick;
    m_jumpRequested = m_jumpRequested || intent.jumpPressed;
    if (!m_initialInputSent) {
        InputIntent initialIntent = intent;
        initialIntent.horizontalDelta = m_accumulatedHorizontalMovement;
        initialIntent.look = m_accumulatedLook;
        initialIntent.jumpPressed = m_jumpRequested;
        if (m_socket.send(encode(initialIntent), m_server)) {
            m_initialInputSent = true;
            if (glm::length(initialIntent.horizontalDelta) > 0.0f) {
                m_pendingMovement.push_back({
                    initialIntent.clientTick,
                    glm::vec3(initialIntent.horizontalDelta, 0.0f)});
            }
            m_accumulatedHorizontalMovement = glm::vec2(0.0f);
            m_accumulatedLook = glm::vec2(0.0f);
            m_jumpRequested = false;
        }
    }
    m_localHorizontalMovement += glm::vec3(resolvedDelta, 0.0f);
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
        AuthoritativeState snapshot{};
        if (decode(bytes, snapshot)) { acceptSnapshot(std::move(snapshot)); continue; }
        VoxelChange change{};
        if (decode(bytes, change)) {
            std::scoped_lock lock(m_mutex);
            if (change.revision > m_authoritative.worldRevision) m_changes.push_back(change);
        }
        WorldChunk chunk{};
        if (decode(bytes, chunk)) {
            std::scoped_lock lock(m_mutex);
            m_worldChunks.push_back(std::move(chunk));
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
        incoming.world = m_authoritative.world;
        incoming.worldRevision = m_authoritative.worldRevision;
        if (!m_hasSnapshots) {
            m_previous = incoming;
            m_next = std::move(incoming);
            m_hasSnapshots = true;
        } else {
            m_previous = m_next;
            m_next = std::move(incoming);
        }
        m_authoritative = m_next;
        glm::vec3 acknowledgedMovement{0.0f};
        while (!m_pendingMovement.empty() &&
               m_pendingMovement.front().clientTick <=
                   m_authoritative.acknowledgedInputTick) {
            acknowledgedMovement += m_pendingMovement.front().displacement;
            m_localHorizontalMovement -= m_pendingMovement.front().displacement;
            m_pendingMovement.pop_front();
        }
        m_interpolatingHorizontalMovement = acknowledgedMovement;
        m_clock = 0.0f;

        InputIntent intent{};
        intent.clientTick = m_latestClientTick;
        intent.horizontalDelta = m_accumulatedHorizontalMovement;
        intent.look = m_accumulatedLook;
        intent.jumpPressed = m_jumpRequested;
        if (intent.clientTick != 0) {
            m_socket.send(encode(intent), m_server);
            if (glm::length(intent.horizontalDelta) > 0.0f) {
                m_pendingMovement.push_back({
                    intent.clientTick,
                    glm::vec3(intent.horizontalDelta, 0.0f)});
            }
            m_accumulatedHorizontalMovement = glm::vec2(0.0f);
            m_accumulatedLook = glm::vec2(0.0f);
            m_jumpRequested = false;
        }
    }
    while (!m_changes.empty()) {
        const auto change = m_changes.front(); m_changes.pop_front();
        if (change.revision > m_authoritative.worldRevision) {
            m_authoritative.world[change.index] = change.value;
            m_authoritative.worldRevision = change.revision;
        }
    }
    while (!m_worldChunks.empty()) {
        WorldChunk chunk = std::move(m_worldChunks.front());
        m_worldChunks.pop_front();
        for (std::size_t i = 0; i < chunk.values.size(); ++i)
            m_authoritative.world[chunk.offset + i] = chunk.values[i];
        m_authoritative.worldRevision = std::max(m_authoritative.worldRevision, chunk.revision);
    }
    m_clock = std::min(m_clock + frameSeconds, kTickSeconds);
    m_render.tick = m_authoritative.tick;
    m_render.worldRevision = m_authoritative.worldRevision;
    m_render.localPlayer = m_authoritative.localPlayer;
    m_render.world = m_authoritative.world;
    m_render.localPlayer.yaw = m_localYaw;
    m_render.localPlayer.pitch = m_localPitch;
    if (!m_hasSnapshots) return;
    const float alpha = std::clamp(m_clock / kTickSeconds, 0.0f, 1.0f);
    m_render.localPlayer.position = glm::mix(m_previous.localPlayer.position,
                                             m_next.localPlayer.position, alpha);
    m_render.localPlayer.position +=
        m_localHorizontalMovement +
        m_interpolatingHorizontalMovement * (1.0f - alpha);
    m_render.localPlayer.velocity = glm::mix(m_previous.localPlayer.velocity,
                                             m_next.localPlayer.velocity, alpha);
}
}
