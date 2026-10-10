#include "server.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#include <cmath>
#include <algorithm>

namespace voxel::net {
Server::Server(std::uint16_t port) : m_port(port) {}
Server::~Server() { stop(); }
void Server::start() {
    if (m_running.exchange(true)) return;
    m_thread = std::thread(&Server::run, this);
}
void Server::stop() {
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();
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
                connection.authority = std::make_unique<Authority>(m_nextPlayerId++);
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

                AuthoritativeState snapshot = client.authority->state();
                snapshot.tick = m_tick;
                snapshot.playerCount = static_cast<std::uint8_t>(
                    std::min(m_clients.size(), snapshot.players.size()));
                for (std::size_t i = 0; i < snapshot.playerCount; ++i) {
                    snapshot.players[i] = m_clients[i].authority->state().localPlayer;
                }
                if (client.peer.port != 0) {
                    if (!client.worldSent) {
                    constexpr std::size_t kChunkSize = 64;
                    for (std::size_t offset = 0; offset < kVoxelCount; offset += kChunkSize) {
                        const std::size_t count = std::min(kChunkSize, kVoxelCount - offset);
                        WorldChunk chunk{snapshot.worldRevision,
                                         static_cast<std::uint32_t>(offset),
                                         {snapshot.world.begin() + offset,
                                          snapshot.world.begin() + offset + count}};
                        if (!m_socket.send(encode(chunk), client.peer))
                            std::cerr << "Server: world chunk send failed\n";
                    }
                    client.worldSent = true;
                }
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
