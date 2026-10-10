#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include "protocol.hpp"
#include "udp_socket.hpp"

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
    const RenderState& renderState() const { return m_render; }
    const AuthoritativeState& authoritativeState() const { return m_authoritative; }
private:
    void receiveLoop();
    void acceptSnapshot(AuthoritativeState snapshot);
    Peer m_server;
    std::atomic_bool m_running{false};
    std::thread m_thread;
    UdpSocket m_socket;
    mutable std::mutex m_mutex;
    std::deque<AuthoritativeState> m_snapshots;
    std::deque<VoxelChange> m_changes;
    std::deque<WorldChunk> m_worldChunks;
    AuthoritativeState m_previous{};
    AuthoritativeState m_next{};
    bool m_hasSnapshots = false;
    AuthoritativeState m_authoritative{};
    RenderState m_render{};
    float m_clock = 0.0f;
    float m_localYaw = -90.0f;
    float m_localPitch = 0.0f;
    glm::vec3 m_localHorizontalMovement{0.0f};
    glm::vec3 m_interpolatingHorizontalMovement{0.0f};
    glm::vec2 m_accumulatedHorizontalMovement{0.0f};
    glm::vec2 m_accumulatedLook{0.0f};
    std::uint32_t m_latestClientTick = 0;
    bool m_jumpRequested = false;
    bool m_initialInputSent = false;
    struct PendingMovement {
        std::uint32_t clientTick = 0;
        glm::vec3 displacement{0.0f};
    };
    std::deque<PendingMovement> m_pendingMovement;
};
}
