#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "protocol.hpp"
#include "tcp_socket.hpp"
#include "udp_socket.hpp"
#include "../simulation/svo.hpp"
#include "../simulation/state.hpp"

namespace voxel::net {

// ---------------------------------------------------------------------------
// Client
//
// Keeps no voxel data on the CPU. It forwards intent over UDP, receives
// authoritative snapshots over UDP, streams bricks over TCP, hands the renderer
// a transient upload queue, and interpolates transform state for display.
// ---------------------------------------------------------------------------

class Client {
  public:
    Client(Peer server, std::string host = "127.0.0.1");
    ~Client() noexcept;

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void start();
    // Streaming window radius, in bricks. Must cover the renderer's view
    // distance; see the note on m_interestRadius.
    void setInterestRadius(std::uint8_t bricks) {
        m_interestRadius = bricks;
    }
    void stop();
    bool running() const { return m_running.load(); }

    // Called once per render frame with the raw local input.
    void submit(const InputIntent& intent, float frameSeconds);
    void updateRender(float frameSeconds);
    // Call after the renderer has drawn, so acknowledgements reflect bricks
    // that really reached the GPU pool rather than bricks merely received.
    void confirmUploads();
    // Hands the renderer the bricks received since the last frame.
    //
    // The queue lives here and is passed across by swapping, never shared.
    // It used to live in RenderState, which the world stream appended to while
    // the main thread iterated it and cleared it: two threads mutating one
    // std::vector, which is how the renderer came to free a pointer that was
    // never allocated and abort. Swapping means each side owns its vector
    // outright for the whole time it holds it.
    void drainBricks(std::vector<BrickUpdate>& out);
    // How many bricks are waiting for the renderer right now. Exposed so a
    // test can verify the queue bound from the outside: draining moves bricks
    // into the caller's vector (and unplaced ones back), so the drain itself
    // cannot observe the bound it relieves.
    std::size_t pendingQueueSize() const;
    RenderState& renderState() { return m_render; }
    const RenderState& renderState() const { return m_render; }
    const AuthoritativeState& authoritativeState() const {
        return m_authoritative;
    }

  private:
    struct RegionKeyHash {
        std::size_t operator()(std::uint32_t value) const noexcept {
            return std::hash<std::uint32_t>{}(value);
        }
    };
    struct LevelKeyHash {
        std::size_t operator()(std::uint64_t value) const noexcept {
            return std::hash<std::uint64_t>{}(value);
        }
    };
    // Packs a root index and a pyramid level into one key.
    //
    // A brick is resident at a level, not as a whole, and each level is
    // delivered and acknowledged on its own. Keying delivery on the root index
    // alone is what would make progressive refinement impossible: a brick
    // streamed at level 5 as distant scenery could never be sent again at level
    // 3 as the player walks toward it, because the first delivery is already
    // recorded as "sent".
    static std::uint64_t levelKey(std::uint32_t rootIndex,
                                 std::uint8_t level) noexcept {
        return (static_cast<std::uint64_t>(rootIndex) << 3) |
               static_cast<std::uint64_t>(level);
    }
    // Bookkeeping only: which levels of a brick the client believes it holds,
    // and which neighbours it still wants. No voxel data is stored here.
    struct Resident {
        std::uint32_t revision = 0;
        // Bitmask of pyramid levels delivered and confirmed. Level 0 is the
        // octree underfoot; the bits above it are the averaged grids the far
        // field is drawn from. They are tracked together because they belong to
        // one brick and are evicted together.
        std::uint8_t levels = 0;
    };
    // Bound on bricks waiting for the GPU. Beyond this the client stops
    // accepting frames rather than dropping ones it has already recorded.
    static constexpr std::size_t kMaxPendingBricks = 256;
    // Headroom above the upload bound for pending frees, which carry no voxel
    // data. Eviction has to be lossless -- an unprocessed free leaks the GPU
    // slice the entry still owns -- so a big window shift is allowed to queue
    // a little past the limit rather than dropping any of it.
    static constexpr std::size_t kMaxEvictionSlack = 256;    void receiveLoop();
    void worldReceiveLoop();
    void acceptSnapshot(AuthoritativeState snapshot);
    void enqueueBrick(BrickSnapshot snapshot);
    void updateInterest();

    Peer m_server;
    std::string m_host;
    voxel::WorldSpec m_spec{};
    // Copy owned by the world stream thread only.
    voxel::WorldSpec m_streamSpec{};
    bool m_hasSpec = false;
    std::vector<Material> m_materials;

    std::atomic_bool m_running{false};
    std::thread m_thread;
    std::thread m_worldThread;

    UdpSocket m_socket;
    TcpSocket m_worldSocket;

    mutable std::mutex m_mutex;
    // Bricks received and not yet handed to the renderer. Guarded by m_mutex
    // and only ever transferred by swap -- see drainBricks().
    std::vector<BrickUpdate> m_pendingBricks;
    std::deque<AuthoritativeState> m_snapshots;
    std::deque<BrickSnapshot> m_worldQueue;
    std::vector<WorldAck> m_pendingAcks;
    // (rootIndex, level) -> revision waiting for the renderer to confirm the
    // upload. An ack is only queued from here, once the renderer reports
    // success.
    std::unordered_map<std::uint64_t, std::uint32_t, LevelKeyHash>
        m_awaitingUpload;
    bool m_uploadOverflow = false;
    // How many entries in the render queue are pending frees rather than
    // uploads. Tracked so the upload bound can be applied to uploads alone.
    std::size_t m_evictionBacklog = 0;
    // Pending frees by root index, so the backlog can be retired exactly as
    // the renderer reports them rather than guessed at.
    std::unordered_set<std::uint32_t> m_pendingFrees;
    // Roots the renderer evicted from its bounded arena. Each becomes a
    // WorldForget on the next stream pass, which is the only thing that makes
    // the server resend an evicted brick: without it, the delivery record
    // says "already sent" and the brick is missing for the rest of the
    // session -- an eviction would be permanent.
    std::vector<std::uint32_t> m_pendingForgets;
    std::unordered_map<std::uint32_t, Resident, RegionKeyHash> m_resident;
    std::int32_t m_interestX = 0;
    std::int32_t m_interestY = 0;
    std::int32_t m_interestZ = 0;
    bool m_hasInterest = false;

    AuthoritativeState m_previous{};
    AuthoritativeState m_next{};
    bool m_hasSnapshots = false;
    AuthoritativeState m_authoritative{};
    RenderState m_render{};

    float m_clock = 0.0f;
    float m_localYaw = 0.0f;
    float m_localPitch = 0.0f;
    glm::vec2 m_accumulatedLook{0.0f};
    std::uint32_t m_latestClientTick = 0;
    // Last intent the server has been sent, used to tell whether a snapshot
    // already acknowledges every look the client applied locally.
    std::uint32_t m_sentClientTick = 0;
    bool m_jumpRequested = false;
    ClientUuid m_uuid{};
    bool m_registered = false;
    std::uint32_t m_playerId = 0;
    float m_registrationRetry = 0.0f;
    float m_inputAccumulator = voxel::kTickSeconds;
    // How far, in bricks, the client asks the server to keep it stocked.
    //
    // This must cover the view distance, or the renderer walks rays into bricks
    // the client was never asked to hold and there is nothing to draw there.
    // It used to be pinned at 6 while the default ray distance is 2048 cells --
    // 16 bricks at a 128-cell brick -- so ten of every sixteen bricks a sky ray
    // crossed were outside the window and could never arrive. That mismatch,
    // not a streaming stall, is what filled the sky with purple cubes. It is
    // derived from the view distance in main.cpp rather than written down
    // here, so the two cannot drift apart again.
    std::uint8_t m_interestRadius = 6;
};

} // namespace voxel::net
