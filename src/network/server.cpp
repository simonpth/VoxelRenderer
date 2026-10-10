#include "server.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#include <cmath>
#include <algorithm>

namespace voxel::net {
Server::Server(std::uint16_t port)
    : m_port(port), m_world(std::make_shared<SvoWorld>()) {}
Server::~Server() { stop(); }
void Server::start() {
    if (m_running.exchange(true)) return;
    m_thread = std::thread(&Server::run, this);
    m_worldThread = std::thread(&Server::worldRun, this);
}
void Server::stop() {
    if (!m_running.exchange(false)) return;
    m_worldListener.shutdown();
    if (m_thread.joinable()) m_thread.join();
    if (m_worldThread.joinable()) m_worldThread.join();
}
void Server::worldRun() {
    try {
        m_worldListener.listen(static_cast<std::uint16_t>(m_port + 1));
        m_worldListener.nonBlocking();
        while (m_running) {
            TcpSocket connection = m_worldListener.accept();
            if (!connection.valid()) {
                if (m_running) std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            std::vector<std::byte> registrationBytes;
            if (!connection.receiveFrame(registrationBytes)) continue;
            Registration registration{};
            if (!decode(registrationBytes, registration)) continue;
            std::vector<std::byte> interestBytes;
            if (!connection.receiveFrame(interestBytes)) continue;
            RegionInterestRequest request{};
            if (!decode(interestBytes, request) || request.uuid != registration.uuid)
                continue;
            // One request produces exactly one complete region frame. The
            // client schedules the neighborhood and reconnects for the next
            // region, preventing one slow generation from blocking a batch.
            const RegionCoord coordinate{
                request.region.x,
                request.region.y,
                request.region.z};
            if (coordinate.x >= kRegionCountX ||
                coordinate.y >= kRegionCountY ||
                coordinate.z >= kRegionCountZ) continue;
            glm::vec3 playerPosition{
                (static_cast<float>(request.center.x) + 0.5f) * kRegionEdge,
                (static_cast<float>(request.center.y) + 0.5f) * kRegionEdge,
                (static_cast<float>(request.center.z) + 0.5f) * kRegionEdge};
            {
                std::scoped_lock lock(m_mutex);
                const auto client = std::find_if(
                    m_clients.begin(), m_clients.end(),
                    [&](const ClientConnection& candidate) {
                        return candidate.uuid == request.uuid;
                    });
                if (client != m_clients.end())
                    playerPosition = client->authority->state().localPlayer.position;
            }
            const glm::vec3 regionCenter{
                (static_cast<float>(coordinate.x) + 0.5f) * kRegionEdge,
                (static_cast<float>(coordinate.y) + 0.5f) * kRegionEdge,
                (static_cast<float>(coordinate.z) + 0.5f) * kRegionEdge};
            const float distance = glm::distance(regionCenter, playerPosition);
            const std::uint8_t depth =
                distance < 2048.0f ? kRegionDepth :
                distance < 4096.0f ? static_cast<std::uint8_t>(kRegionDepth - 2) :
                distance < 8192.0f ? static_cast<std::uint8_t>(kRegionDepth - 4) :
                static_cast<std::uint8_t>(kRegionDepth - 6);
            RegionSnapshot snapshot{};
            snapshot.coordinate = coordinate;
            snapshot.depth = depth;
            snapshot.worldRevision = m_world->revision();
            const auto region = m_world->copyRegion(coordinate, depth);
            snapshot.revision = region.revision();
            snapshot.nodes = region.nodes();
            const auto encoded = encode(snapshot);
            if (!encoded.empty()) connection.sendFrame(encoded);
        }
    } catch (const std::runtime_error& error) {
        if (m_running) std::cerr << "Server: world stream stopped: "
                                 << error.what() << '\n';
    }
}
void Server::receive() {
    std::array<std::byte, 256> buffer{};
    for (;;) {
        Peer peer; const auto size = m_socket.receive(buffer, peer);
        if (size <= 0) return;
        const auto bytes = std::span<const std::byte>(
            buffer.data(), static_cast<std::size_t>(size));
        Registration registration{};
        if (decode(bytes, registration)) {
            std::scoped_lock lock(m_mutex);
            auto client = std::find_if(m_clients.begin(), m_clients.end(),
                [&](const ClientConnection& candidate) {
                    return candidate.uuid == registration.uuid;
                });
            if (client == m_clients.end()) {
                if (m_clients.size() >= AuthoritativeState{}.players.size()) continue;
                ClientConnection connection{};
                connection.uuid = registration.uuid;
                connection.peer = peer;
                connection.authority = std::make_unique<Authority>(
                    m_nextPlayerId++, m_world);
                m_clients.push_back(std::move(connection));
                client = std::prev(m_clients.end());
            } else {
                client->peer = peer;
            }
            m_socket.send(encode(RegistrationAck{
                registration.uuid, client->authority->state().localPlayer.id}), peer);
            continue;
        }
        InputIntent intent{};
        if (!decode(bytes, intent) ||
            glm::length(intent.horizontalDelta) > 0.25f ||
            std::abs(intent.look.x) > 90.0f || std::abs(intent.look.y) > 90.0f) continue;
        std::scoped_lock lock(m_mutex);
        const auto client = std::find_if(m_clients.begin(), m_clients.end(),
            [&](const ClientConnection& candidate) { return candidate.peer == peer; });
        if (client != m_clients.end() && intent.clientTick > client->lastClientTick) {
            client->jumpRequested = client->jumpRequested || intent.jumpPressed;
            client->latest = intent;
            client->lastClientTick = intent.clientTick;
        }
    }
}
void Server::run() {
    m_socket.bind(m_port);
    m_socket.nonBlocking();
    using Clock = std::chrono::steady_clock;
    auto next = Clock::now();
    while (m_running) {
        receive();
        if (Clock::now() >= next) {
            std::scoped_lock lock(m_mutex);
            ++m_tick;
            for (auto& client : m_clients) {
                InputIntent intent = client.latest;
                intent.jumpPressed = client.jumpRequested;
                client.jumpRequested = false;
                if (intent.clientTick <=
                    client.authority->state().acknowledgedInputTick) intent = {};
                client.authority->step(intent, kTickSeconds);

                std::vector<glm::vec3> playerPositions;
                playerPositions.reserve(m_clients.size());
                for (const auto& candidate : m_clients)
                    playerPositions.push_back(
                        candidate.authority->state().localPlayer.position);
                m_world->updateLod(playerPositions);

                AuthoritativeState snapshot = client.authority->state();
                snapshot.tick = m_tick;
                snapshot.playerCount = static_cast<std::uint8_t>(
                    std::min(m_clients.size(), snapshot.players.size()));
                for (std::size_t i = 0; i < snapshot.playerCount; ++i) {
                    snapshot.players[i] = m_clients[i].authority->state().localPlayer;
                }
                if (client.peer.port != 0) {
                if (!m_socket.send(encode(snapshot), client.peer))
                    std::cerr << "Server: snapshot send failed\n";
                }
            }
            next += std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<float>(kTickSeconds));
            if (Clock::now() > next + std::chrono::seconds(1)) next = Clock::now();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}
}
