#pragma once

#include <atomic>
#include <random>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "protocol.hpp"
#include "udp_socket.hpp"
#include "tcp_socket.hpp"

namespace voxel::net {
class Client {
public:
    explicit Client(Peer server = {"127.0.0.1", 27015});
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    void start();
    void stop();
    void submit(InputIntent intent, float frameSeconds);
    void updateRender(float frameSeconds);
    RenderState& renderState() { return m_render; }
    const RenderState& renderState() const { return m_render; }
    const AuthoritativeState& authoritativeState() const { return m_authoritative; }
private:
    struct RegionKeyHash {
        std::size_t operator()(const RegionCoord& coordinate) const noexcept {
            return (static_cast<std::size_t>(coordinate.x) << 32) ^
                   (static_cast<std::size_t>(coordinate.y) << 16) ^
                   coordinate.z;
        }
    };
    void receiveLoop();
    void worldReceiveLoop();
    void acceptSnapshot(AuthoritativeState snapshot);
    Peer m_server;
    std::atomic_bool m_running{false};
    std::thread m_thread;
    std::thread m_worldThread;
    UdpSocket m_socket;
    TcpSocket m_worldSocket;
    mutable std::mutex m_mutex;
    std::deque<AuthoritativeState> m_snapshots;
    std::deque<VoxelChange> m_changes;
    std::deque<RegionSnapshot> m_regionSnapshots;
    std::unordered_map<RegionCoord, std::uint32_t, RegionKeyHash> m_regionRevisions;
    AuthoritativeState m_previous{};
    AuthoritativeState m_next{};
    bool m_hasSnapshots = false;
    AuthoritativeState m_authoritative{};
    RenderState m_render{};
    float m_clock = 0.0f;
    float m_localYaw = -90.0f;
    float m_localPitch = 0.0f;
    glm::vec2 m_accumulatedHorizontalMovement{0.0f};
    glm::vec2 m_accumulatedLook{0.0f};
    std::uint32_t m_latestClientTick = 0;
    bool m_jumpRequested = false;
    ClientUuid m_uuid{};
    bool m_registered = false;
    float m_registrationRetry = 0.0f;
    float m_inputSendAccumulator = kTickSeconds;
};
}
