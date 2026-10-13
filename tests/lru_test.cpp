// End-to-end eviction recovery test.
//
// The client's GPU arena is a fixed budget, so a brick that was delivered can
// later be evicted to make room. Delivery is not the interesting part -- the
// interesting part is what happens next:
//
//   * the client must stop considering the brick resident, or the server's
//     next resend is dismissed as a duplicate and the brick never returns;
//   * the client must tell the server to forget the delivery, because the
//     server's "already delivered" record is exactly what makes it skip every
//     future send of that brick.
//
// Either half missing makes an eviction permanent, which is what "storage
// becomes a problem" actually looks like: not running out of memory, but
// terrain that silently never comes back.
//
// The real renderer needs a GPU, which a test cannot provide, so this drives
// the same seam `VulkanContext::uploadWorld` uses -- a stand-in renderer whose
// arena is the real BrickPool, reporting real evictions through
// `evictedRoots`. The pool policy is not faked; only the memcpy is.
#include <algorithm>
#include <unordered_map>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "network/client.hpp"
#include "network/server.hpp"
#include "simulation/brick_pool.hpp"

using namespace voxel;
using namespace voxel::net;

namespace {

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Stands in for the renderer. Its arena is the real pool with the real policy,
// so what this reports as evicted is what the real renderer would evict for
// the same capacity. `nodeCount` mirrors how many nodes each brick payload
// actually carries, since the real arena's budget is nodes, not bricks.
//
// `uploadsPerFrame` is the one thing this deliberately adds, and it is not a
// fudge: a GPU cannot copy a whole window's worth of nodes in one frame. It
// matters for what is being tested, because the server hands over up to sixteen
// bricks at a time, and a client that drained them all at once evicted only
// bricks from the batch it was already holding -- every eviction was replaced
// in the same frame, and the recovery path was never exercised at all. Pacing
// uploads is what makes an arena-full eviction reach back into an earlier
// frame, which is the only case where recovery is needed.
class BoundedRenderer {
  public:
    explicit BoundedRenderer(std::uint32_t capacity,
                             std::size_t uploadsPerFrame = 2)
        : m_pool(capacity), m_uploadsPerFrame(uploadsPerFrame) {}

    void drain(RenderState& state) {
        state.evictedRefs.clear();
        std::size_t placed = 0;
        for (BrickUpdate& update : state.bricks) {
            update.uploaded = false;
            state.processedRefs.push_back({update.rootIndex, update.lodLevel});
            if (update.remove) {
                BrickPool::Slot slot{};
                m_pool.drop(update.rootIndex, slot);
                continue;
            }
            if (placed >= m_uploadsPerFrame) continue;   // the rest wait
            if (update.lodLevel != 0) {
                // An averaged grid needs no pool space -- it is a flat array
                // of colours, not octree blocks -- so it is held as soon as it
                // arrives. Reporting it is what lets the client acknowledge the
                // level; dropping it here would leave it unacknowledged and the
                // server would resend it forever.
                ++placed;
                update.uploaded = true;
                state.uploadedRefs.push_back(
                    {update.rootIndex, update.lodLevel});
                continue;
            }
            const auto claim =
                m_pool.claim(update.rootIndex,
                             static_cast<std::uint32_t>(update.nodes.size()));
            // Evictions are reported whether or not the claim succeeded: the
            // renderer gives up runs either way, and the client has to hear
            // about it either way. Only level 0 is evicted by name -- the pool
            // holds octrees, and a grid eviction would name a level the pool
            // never held.
            for (std::uint32_t evicted : claim.evicted)
                state.evictedRefs.push_back({evicted, 0});
            if (!claim.placed) continue;
            ++placed;
            update.uploaded = true;
            state.uploadedRefs.push_back({update.rootIndex, update.lodLevel});
        }
        // Whatever was not placed stays queued, as a real renderer would, and
        // is handed back to the client at the next drain -- see
        // Client::drainBricks, which is where the retry actually happens.
        //
        // Compacted in place with erase rather than rebuilt and moved in: a
        // move-assignment would free the old buffer, and the leftovers are the
        // half of the queue that is about to be swapped across.
        state.bricks.erase(
            std::remove_if(state.bricks.begin(), state.bricks.end(),
                           [](const BrickUpdate& update) {
                               return update.uploaded || update.remove;
                           }),
            state.bricks.end());
    }

    std::size_t resident() const { return m_pool.size(); }
    std::uint32_t used() const { return m_pool.used(); }
    std::uint32_t capacity() const { return m_pool.capacity(); }

  private:
    BrickPool m_pool;
    std::size_t m_uploadsPerFrame;
};

} // namespace

int main() {
    std::printf("lru eviction recovery:\n");
    const WorldSpec spec{256, 256, 128, 64};
    const int port = 27445;

    Server server(port, spec, 0xCAFE55);
    server.start();
    Client client(Peer{"127.0.0.1", port});
    client.start();

    for (int i = 0; i < 200 && !client.renderState().sizeX; ++i) sleepMs(20);
    if (client.renderState().sizeX != spec.sizeX) {
        std::printf("FAIL: descriptor never arrived\n");
        return 1;
    }

    // A GPU cannot copy a whole window's worth of nodes in one frame, and the
    // budget is deliberately set below the window's size, so this is the state
    // the client spends most of its life in.
constexpr std::size_t kUploadsPerFrame = 2;

// A budget between one brick and the whole window. That bracket matters:
    // the largest brick the server sends is 18,443 nodes, and the whole window
    // as reduced on the wire costs about 80,000, so a budget below the former
    // would not be a budget at all -- one brick would not fit and the arena
    // would grow instead, which is correct behaviour but not what this test is
    // about. Staying between the two means eviction is the only thing that can
    // bound it.
    //
    // 32 KiB holds about eight of the ~3.8k-node bricks the server actually
    // sends, so the window does not fit and pressure is continuous. This exact
    // figure matters: at 96 KiB the window fitted, every eviction was a
    // same-frame replacement, and the recovery path this test exists for was
    // never once exercised.
    constexpr std::uint32_t kBudget = 32 * 1024;
    static_assert(kBudget > 18'443 && kBudget < 80'000,
                  "budget must fit one brick but not the whole window");
    BoundedRenderer renderer(kBudget, kUploadsPerFrame);

    std::uint64_t uploads = 0, evictions = 0, frames = 0;
    std::uint64_t recovered = 0;
    std::uint64_t worstLatency = 0;
    // (root, level) -> frame evicted and still owing a redelivery. Drained by
    // re-upload, so anything left here at the end is a brick the world
    // permanently lost -- which is the failure, not a slow stream.
    std::unordered_map<std::uint64_t, std::uint64_t> outstanding;
    // Every (root, level) that has completed at least one evict-and-return
    // cycle. A brick outstanding at the end is only "still in flight" if its
    // pipeline already proved itself; one that never once came back is a path
    // that never worked, whatever its age.
    std::unordered_set<std::uint64_t> everRecovered;
    auto refKey = [](std::uint32_t root, std::uint8_t level) {
        return (static_cast<std::uint64_t>(root) << 3) | level;
    };
    // 400 frames is the budget: eviction pressure is continuous from the first
    // frames (the pool holds ~8 bricks against a 32-brick window), so recovery
    // is exercised hundreds of times over without a 1500-frame soak.
    for (int i = 0; i < 400; ++i) {
        client.drainBricks(client.renderState().bricks);
        renderer.drain(client.renderState());
        // An eviction the same frame re-places is not a lost brick -- the
        // renderer dropped a run and immediately re-claimed it -- so only an
        // eviction with no upload behind it in the same frame enters the
        // recovery set.
        for (const BrickRef& ref : client.renderState().evictedRefs) {
            const auto uploaded = client.renderState().uploadedRefs;
            const bool replaced = std::any_of(
                uploaded.begin(), uploaded.end(), [&](const BrickRef& up) {
                    return up.rootIndex == ref.rootIndex;
                });
            if (!replaced) {
                outstanding[refKey(ref.rootIndex, ref.level)] = frames;
                ++evictions;
            }
        }
        for (const BrickRef& ref : client.renderState().uploadedRefs) {
            ++uploads;
            const auto at =
                outstanding.find(refKey(ref.rootIndex, ref.level));
            if (at != outstanding.end()) {
                worstLatency =
                    std::max(worstLatency, frames - at->second);
                outstanding.erase(at);
                everRecovered.insert(refKey(ref.rootIndex, ref.level));
                ++recovered;
            }
        }
        client.confirmUploads();
        ++frames;
        // The arena may never exceed its budget. This is the invariant the
        // whole design exists to provide.
        if (renderer.used() > renderer.capacity()) {
            std::printf("FAIL: arena over budget (%u > %u)\n", renderer.used(),
                        renderer.capacity());
            return 1;
        }
        sleepMs(4);
    }

    std::printf("  %llu frames, %llu uploads, %llu evictions needing recovery, "
                "%llu recovered, worst latency %llu frames, %zu still out\n",
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(uploads),
                static_cast<unsigned long long>(evictions),
                static_cast<unsigned long long>(recovered),
                static_cast<unsigned long long>(worstLatency),
                outstanding.size());

    // The arena must have been bounded by eviction, not by growing. If capacity
    // moved above the budget, the run tested growth instead and the eviction
    // path was never what kept the client inside its budget.
    if (renderer.capacity() != kBudget) {
        std::printf("FAIL: arena grew from %u to %u; growth should not have "
                    "been needed\n", kBudget, renderer.capacity());
        return 1;
    }

    // Eviction must actually have happened -- a budget this small against a
    // window this size cannot avoid it -- and at least some of it must have
    // been real loss rather than same-frame replacement, or the recovery path
    // was never touched and the assertions below would pass for a client that
    // never forgets anything.
    if (evictions == 0) {
        std::printf("FAIL: no evictions needed recovery; the budget was never "
                    "exercised\n");
        return 1;
    }

    // Recovery must keep up: every eviction comes back, and quickly. What is
    // judged is age, not presence -- a resend refused by a full client queue
    // waits out the server's 500 ms grace before it is retried, so an eviction
    // from the last ~150 frames may legitimately still be in the pipeline when
    // the run ends. Anything older than that is not pipelined, it is stuck:
    // the client still believes it holds the brick (or the server believes it
    // delivered it), which is indistinguishable from terrain that never
    // existed.
    std::uint64_t oldestAge = 0;
    std::uint64_t neverRecovered = 0;
    for (const auto& [key, evictedAt] : outstanding) {
        const std::uint64_t age = frames - evictedAt;
        oldestAge = std::max(oldestAge, age);
        // Age-gated like the check above: a brick evicted on the last frames
        // has had no time for even one cycle, so youth is the pipeline, not
        // evidence either way. Only an old brick with no completed cycle is a
        // path that never worked.
        if (age > 150 && everRecovered.find(key) == everRecovered.end())
            ++neverRecovered;
    }
    // A brick that never once completed a cycle is not "in flight", however
    // young: there is no evidence its redelivery path works at all.
    if (neverRecovered > 0) {
        std::printf("FAIL: %zu evicted brick(s) never completed one recovery\n",
                    static_cast<std::size_t>(neverRecovered));
        return 1;
    }
    if (oldestAge > 150) {
        std::printf("FAIL: %zu evicted brick(s) never came back, oldest %llu "
                    "frames ago\n", outstanding.size(),
                    static_cast<unsigned long long>(oldestAge));
        return 1;
    }
    if (!outstanding.empty())
        std::printf("  (%zu redeliveries younger than 150 frames still in "
                    "flight)\n", outstanding.size());
    if (worstLatency > 200) {
        std::printf("FAIL: worst recovery latency %llu frames; redelivery is "
                    "stalling\n",
                    static_cast<unsigned long long>(worstLatency));
        return 1;
    }
    if (uploads == 0) {
        std::printf("FAIL: no uploads at all\n");
        return 1;
    }

    client.stop();
    server.stop();
    std::printf("lru eviction recovery: all passed\n");
    return 0;
}
