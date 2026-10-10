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
        InputIntent intent{};
        if (!decode({buffer.data(), static_cast<std::size_t>(size)}, intent) ||
            glm::length(intent.horizontalDelta) > 0.25f ||
            std::abs(intent.look.x) > 90.0f || std::abs(intent.look.y) > 90.0f) continue;
        std::scoped_lock lock(m_mutex);
        if (intent.clientTick > m_lastClientTick || peer != m_peer) {
            if (peer != m_peer) m_worldSent = false;
            m_jumpRequested = m_jumpRequested || intent.jumpPressed;
            m_latest = intent; m_lastClientTick = intent.clientTick; m_peer = std::move(peer);
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
            InputIntent intent{};
            Peer peer;
            {
                std::scoped_lock lock(m_mutex);
                intent = m_latest; peer = m_peer;
                intent.jumpPressed = m_jumpRequested;
                m_jumpRequested = false;
            }
            if (intent.clientTick <= m_authority.state().acknowledgedInputTick) {
                intent = {};
            }
            m_authority.step(intent, kTickSeconds);
            if (peer.port != 0) {
                if (!m_worldSent || peer != m_peer) {
                    constexpr std::size_t kChunkSize = 64;
                    for (std::size_t offset = 0; offset < kVoxelCount; offset += kChunkSize) {
                        const std::size_t count = std::min(kChunkSize, kVoxelCount - offset);
                        WorldChunk chunk{m_authority.state().worldRevision,
                                         static_cast<std::uint32_t>(offset),
                                         {m_authority.state().world.begin() + offset,
                                          m_authority.state().world.begin() + offset + count}};
                        if (!m_socket.send(encode(chunk), peer))
                            std::cerr << "Server: world chunk send failed\n";
                    }
                    m_worldSent = true;
                }
                const auto packet = encode(m_authority.state());
                if (!m_socket.send(packet, peer)) std::cerr << "Server: snapshot send failed\n";
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
