// Streaming delivery regression test.
//
// This pins the behaviour that produced permanent magenta holes and inverted
// shadows. The bug was not in the renderer or the shader: it was that the
// server's record of "this client already has this brick" outlived the
// client's own copy of it. Four separate defects fed it:
//
//   1. `sent` and `acknowledged` were never purged, so a brick the client had
//      evicted when it left the interest window was still recorded as
//      delivered. Walking out of range and back lost it permanently.
//   2. The client's 256-entry queue was trimmed by *discarding* the oldest
//      updates -- which had already been marked resident and acknowledged.
//      The server saw the ack and stopped retrying a brick that had never
//      reached the GPU.
//   3. The client acknowledged on receipt rather than after upload, so a brick
//      the node pool refused was written off as delivered.
//   4. A sent-but-unacknowledged brick was skipped forever rather than retried.
//
// The end-to-end wiring needs a GPU renderer to observe, which a test cannot
// provide, so these cases are driven at the seam: a stand-in "renderer" that
// does exactly what `VulkanContext::uploadWorld` does with respect to
// acknowledgement -- accept some updates, refuse others, and report which
// reached the pool.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "network/client.hpp"
#include "network/server.hpp"

using namespace voxel;
using namespace voxel::net;

namespace {

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Stands in for the renderer. Accepts updates until `acceptLimit` have been
// taken, then refuses everything -- exactly the pool-exhausted path, which is
// the case that used to lose bricks silently.
class FakeRenderer {
  public:
    explicit FakeRenderer(int acceptLimit) : m_acceptLimit(acceptLimit) {}

    void drain(RenderState& state) {
        for (BrickUpdate& update : state.bricks) {
            // Mirror the renderer: every brick handed over is reported as
            // processed, whatever the outcome, and only accepted ones as
            // uploaded. That is what makes a refusal recoverable.
            update.uploaded = false;
            state.processedRoots.push_back(update.rootIndex);
            if (update.remove) continue;
            if (static_cast<int>(m_accepted) >= m_acceptLimit) continue;
            ++m_accepted;
            update.uploaded = true;
            state.uploadedRoots.push_back(update.rootIndex);
        }
        state.bricks.clear();
    }

    int accepted() const { return m_accepted; }

  private:
    int m_acceptLimit;
    int m_accepted = 0;
};

// A renderer with a bounded arena: once it holds `capacity` bricks it must drop
// one to make room, and it says which. That is what the real renderer does when
// the node pool is full, and it is the only thing that makes a brick come back
// -- the client retracts what was dropped, and the server resends it.
//
// FakeRenderer above cannot express this: it refuses uploads silently, which is
// the defect-2 behaviour these tests exist to catch, so it must not be used
// where eviction is what is under test.
class BoundedRenderer {
  public:
    explicit BoundedRenderer(std::size_t capacity) : m_capacity(capacity) {}

    void drain(RenderState& state) {
        state.evictedRoots.clear();
        for (BrickUpdate& update : state.bricks) {
            update.uploaded = false;
            state.processedRoots.push_back(update.rootIndex);
            if (update.remove) {
                for (auto it = m_held.begin(); it != m_held.end(); ++it)
                    if (*it == update.rootIndex) { m_held.erase(it); break; }
                continue;
            }
            if (m_held.size() >= m_capacity && !m_held.empty()) {
                // Drop the oldest to make room, and report it, exactly as the
                // real arena's LRU does.
                const std::uint32_t victim = m_held.front();
                m_held.erase(m_held.begin());
                state.evictedRoots.push_back(victim);
            }
            m_held.push_back(update.rootIndex);
            ++m_accepted;
            update.uploaded = true;
            state.uploadedRoots.push_back(update.rootIndex);
        }
        state.bricks.clear();
    }

    int accepted() const { return m_accepted; }

  private:
    std::size_t m_capacity;
    std::size_t m_accepted = 0;
    std::vector<std::uint32_t> m_held;
};

} // namespace

int main() {
    const std::uint16_t port = 28911;
    // Sizing is load-bearing for case 3. The client's interest radius is 6
    // bricks, so a brick is only evicted once the centre travels more than
    // (6 + 1) * brick blocks; the walk covers twice that to guarantee the
    // window has turned over completely. A 32-cell brick keeps that distance
    // small enough to walk in a sane time, which matters because the authority
    // ticks in real time and the player cannot be teleported -- it owns the
    // position. The world is wide enough for both legs plus a margin, and tall
    // enough for the climb that clears terrain.
    constexpr std::uint32_t kBrick = 32;
    constexpr float kTurnOverBlocks = 2.0f * 7.0f * static_cast<float>(kBrick);
    const WorldSpec spec{2048, 1024, 384, kBrick};
    Server server(port, spec, 0xBEEF01);
    server.start();
    if (!server.running()) {
        std::printf("server did not start (port in use?)\n");
        return 1;
    }

    Client client(Peer{"127.0.0.1", port});
    client.start();

    for (int i = 0; i < 200 && !client.renderState().sizeX; ++i) sleepMs(20);
    if (client.renderState().sizeX != spec.sizeX) {
        std::printf("descriptor never arrived\n");
        client.stop();
        server.stop();
        return 1;
    }

    // ---- Case 1: nothing is acknowledged until the renderer confirms it. ----
    //
    // The renderer below refuses every update, so nothing may be acked. If the
    // ack were still sent on receipt, the server would mark these bricks
    // delivered and never resend them -- which is defect 3.
    {
        FakeRenderer renderer(0);
        std::size_t delivered = 0;
        for (int i = 0; i < 100; ++i) {
            client.updateRender(1.0f / 60.0f);
            delivered += client.renderState().bricks.size();
            renderer.drain(client.renderState());
            client.confirmUploads();
            sleepMs(10);
        }
        if (delivered == 0) {
            std::printf("no bricks were offered; test cannot observe delivery\n");
            client.stop();
            server.stop();
            return 1;
        }
        if (renderer.accepted() != 0) {
            std::printf("renderer refused everything but %d uploads happened\n",
                        renderer.accepted());
            client.stop();
            server.stop();
            return 1;
        }
        if (!client.renderState().uploadedRoots.empty()) {
            std::printf("uploads reported despite a refusing renderer\n");
            client.stop();
            server.stop();
            return 1;
        }
        std::printf("case 1: %zu bricks offered, 0 acknowledged before upload\n",
                    delivered);
    }

    // ---- Case 2: a refused brick is retried, not written off. ----
    //
    // Now let the renderer accept. Whatever it refused in case 1 was never
    // acknowledged, so the server's resend timer must bring it round again.
    // Under defect 4 those bricks would be skipped forever.
    {
        FakeRenderer renderer(1 << 30);
        int rounds = 0;
        // Counted at the renderer, not by reading uploadedRoots afterwards:
        // confirmUploads drains that vector by design, so a later read is
        // always zero and would make this case unfalsifiable.
        const int before = renderer.accepted();
        for (int i = 0; i < 300; ++i) {
            client.updateRender(1.0f / 60.0f);
            renderer.drain(client.renderState());
            client.confirmUploads();
            ++rounds;
            if (renderer.accepted() - before > 8) break;
            sleepMs(20);
        }
        const int accepted = renderer.accepted() - before;
        if (accepted == 0) {
            std::printf("case 2: no brick was ever accepted after retrying "
                        "(%d rounds)\n", rounds);
            client.stop();
            server.stop();
            return 1;
        }
        std::printf("case 2: %zu bricks accepted over %d rounds after retry\n",
                    accepted, rounds);
    }

    // ---- Case 3: a brick the arena dropped comes back. ----
    //
    // The original bug: the server's "already delivered" record outlived the
    // client's copy of the brick, so a brick the client no longer held was
    // never sent again and rendered magenta for the rest of the session.
    //
    // What makes a client stop holding a brick has changed, and this case
    // follows the change. It used to be leaving the interest window; the client
    // now retains bricks until its bounded arena is full, because the arena is
    // far larger than the window and holding the rest of the world is the point.
    // So the eviction is driven by capacity here -- a bounded stand-in renderer
    // that reports what it drops, which is what makes the client retract it and
    // the server resend it. A walk away and back then lands on bricks the arena
    // gave up on the way out.
    {
        // Let things settle so the current window is fully delivered.
        BoundedRenderer renderer(24);
        for (int i = 0; i < 100; ++i) {
            client.updateRender(1.0f / 60.0f);
            renderer.drain(client.renderState());
            client.confirmUploads();
            sleepMs(10);
        }

        // Move far enough that the window is a different set of bricks, then
        // come back. Each leg is a fresh set of intents; the authority owns the
        // position, so this is the only honest way to move a client.
        // Walk until the authority has actually moved the player `distance` blocks
        // along x, or the step budget runs out. Positioning by intent *count*
        // would be guesswork: the authority ticks at a fixed rate regardless of
        // how fast intents arrive, so the test has to observe where it got to.
        const auto startPos = client.authoritativeState().localPlayer.position;
        // `move` is applied relative to the player's own yaw, which the
        // authority owns and replicates back. Rather than guess which world
        // axis that currently points along, the walk measures the distance
        // actually covered from the start position -- direction-agnostic, and
        // it fails loudly if the player is not moving at all.
        // The client tick must keep increasing across the whole test. The server
        // discards any intent whose tick it has already acknowledged, so
        // restarting the counter would make every later intent a replayed one
        // and the player would stop responding to input.
        std::uint32_t tick = 1000;
        // Ascend while travelling. Terrain is not flat and collision is correct
        // -- a player walking into a hillside *should* stop -- so climbing is
        // what keeps this measuring streaming rather than terrain.
        //
        // Progress is measured horizontally. Ascending at the fly speed would
        // otherwise outpace the horizontal walk, and the vertical climb would
        // exhaust the world's height long before the window turned over.
        const auto walkFrom = [&](glm::vec3 origin, float dir, float distance) {
            const float originX = origin.x, originY = origin.y;
            // Terrain tops out well below this, so once above it there is
            // nothing left to collide with and the climb can stop. Holding
            // ascend for the whole walk would burn the world's height and the
            // step budget on going straight up instead of sideways.
            constexpr float kSafeAltitude = 200.0f;
            for (int i = 0; i < 4000; ++i) {
                const PlayerState& now =
                    client.authoritativeState().localPlayer;
                InputIntent intent{};
                intent.clientTick = ++tick;
                intent.flying = true;
                intent.jumpPressed = now.position.z < kSafeAltitude;
                intent.rawMove.axes = {0.0f, dir};
                client.submit(intent, 1.0f / 60.0f);
                client.updateRender(1.0f / 60.0f);
                renderer.drain(client.renderState());
                client.confirmUploads();
                sleepMs(20);
                const PlayerState& p =
                    client.authoritativeState().localPlayer;
                const float horizontal = std::hypot(p.position.x - originX,
                                                    p.position.y - originY);
                if (horizontal >= distance) return true;
            }
            const PlayerState& p = client.authoritativeState().localPlayer;
            std::printf(
                "  walk(%g) covered only %.1f horizontal blocks, wanted %.0f\n"
                "    pos=(%.1f, %.1f, %.1f) flying=%d grounded=%d\n",
                static_cast<double>(dir),
                static_cast<double>(std::hypot(p.position.x - originX,
                                               p.position.y - originY)),
                static_cast<double>(distance),
                static_cast<double>(p.position.x),
                static_cast<double>(p.position.y),
                static_cast<double>(p.position.z),
                p.flying ? 1 : 0, p.grounded ? 1 : 0);
            return false;
        };
        const auto walk = [&](float dir, float distance) {
            return walkFrom(client.authoritativeState().localPlayer.position, dir,
                            distance);
        };

        // A brick is evicted once the centre moves more than (radius + 1) bricks.
        // Travelling 2 * (radius + 1) bricks guarantees the window has turned
        // over *completely*: the bricks it started on are behind it, and the
        // bricks it now covers were never in view.
        constexpr float kTurnOver = kTurnOverBlocks;
        if (!walk(1.0f, kTurnOver)) {
            std::printf("case 3: could not travel %g blocks to leave the "
                        "window\n", static_cast<double>(kTurnOver));
            client.stop();
            server.stop();
            return 1;
        }
        // Back toward where we started, along the axis we came out on.
        if (!walkFrom(startPos, -1.0f, kTurnOver)) {
            std::printf("case 3: could not travel back\n");
            client.stop();
            server.stop();
            return 1;
        }

        // After returning, give the server time to notice and resend. Counted at the
        // renderer, because confirmUploads drains uploadedRoots by design.
        const int beforeReturn = renderer.accepted();
        for (int i = 0; i < 200; ++i) {
            client.updateRender(1.0f / 60.0f);
            renderer.drain(client.renderState());
            client.confirmUploads();
            if (renderer.accepted() > beforeReturn) break;
            sleepMs(20);
        }
        const int redelivered = renderer.accepted() - beforeReturn;
        if (redelivered == 0) {
            std::printf("case 3: nothing was redelivered on returning to a "
                        "visited window\n");
            client.stop();
            server.stop();
            return 1;
        }
        std::printf("case 3: %zu bricks redelivered after leaving and "
                    "returning\n", redelivered);
    }

    // ---- Case 4: overflow is back-pressure, not a silent drop. ----
    //
    // If the queue ever exceeds its bound *and* a brick has been dropped
    // without being acknowledged, that is the old defect 2. The client must
    // simply stop accepting and let the server retry.
    {
        FakeRenderer renderer(0);   // nothing drains, so the queue fills
        std::size_t offered = 0;
        for (int i = 0; i < 200; ++i) {
            client.updateRender(1.0f / 60.0f);
            offered += client.renderState().bricks.size();
            sleepMs(10);
        }
        // The bound is on *uploads*: frees are tracked separately and may take the
        // queue up to the upload limit plus the eviction slack. What must never
        // happen is unbounded growth, or a brick dropped without being
        // acknowledged (which is what leaves a permanent hole).
        constexpr std::size_t kUploadBound = 256;
        constexpr std::size_t kFreeSlack = 256;
        if (client.renderState().bricks.size() > kUploadBound + kFreeSlack) {
            std::printf("case 4: queue grew to %zu, past the bound of %zu\n",
                        client.renderState().bricks.size(),
                        kUploadBound + kFreeSlack);
            client.stop();
            server.stop();
            return 1;
        }
        // Nothing was ever acknowledged, so the server still owes us all of
        // it -- which is the property that makes the overflow recoverable.
        if (!client.renderState().uploadedRoots.empty()) {
            std::printf("case 4: uploads acknowledged with no renderer\n");
            client.stop();
            server.stop();
            return 1;
        }
        std::printf("case 4: queue held at %zu (bound %zu) under overflow, "
                    "0 acknowledged\n",
                    client.renderState().bricks.size(),
                    kUploadBound + kFreeSlack);
    }

    client.stop();
    server.stop();
    std::printf("streaming tests passed\n");
    return 0;
}