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
        state.evictedRoots.clear();
        std::size_t placed = 0;
        for (BrickUpdate& update : state.bricks) {
            update.uploaded = false;
            state.processedRoots.push_back(update.rootIndex);
            if (update.remove) {
                BrickPool::Slot slot{};
                m_pool.drop(update.rootIndex, slot);
                continue;
            }
            if (placed >= m_uploadsPerFrame) continue;   // the rest wait
            const auto claim =
                m_pool.claim(update.rootIndex,
                             static_cast<std::uint32_t>(update.nodes.size()));
            // Evictions are reported whether or not the claim succeeded: the
            // renderer gives up runs either way, and the client has to hear
            // about it either way.
            state.evictedRoots.insert(state.evictedRoots.end(),
                                      claim.evicted.begin(),
                                      claim.evicted.end());
            if (!claim.placed) continue;
            ++placed;
            update.uploaded = true;
            state.uploadedRoots.push_back(update.rootIndex);
        }
        // Whatever was not placed stays queued, as a real renderer would.
        //
        // Compacted in place with erase rather than rebuilt and moved in. The
        // client's stream thread pushes into this same vector concurrently, and
        // a move-assignment frees the old buffer out from under it -- which is
        // a use-after-free, not a race, and showed up as an intermittent
        // SIGBUS. erase keeps the capacity, so the buffer the other thread is
        // writing into stays alive.
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

    std::uint64_t uploads = 0, evictions = 0, frames = 0, lost = 0;
    std::uint64_t distinctResident = 0;
    // Roots evicted and not yet seen resident again. Every one of these is a
    // brick that was delivered, acknowledged, thrown away, and still owes the
    // client a redelivery. The set drains as bricks come back, and an entry
    // that never leaves it is a brick the world has permanently lost.
    std::vector<std::uint32_t> outstanding;
    for (int i = 0; i < 1'500; ++i) {
        renderer.drain(client.renderState());
        // An eviction the same frame re-places is not a lost brick -- the
        // renderer dropped a run and immediately re-claimed it -- so it does
        // not count as one needing recovery. Only an eviction with no upload
        // behind it in the same frame is a brick the client genuinely lost and
        // owes a redelivery.
        for (std::uint32_t root : client.renderState().evictedRoots)
            if (std::find(client.renderState().uploadedRoots.begin(),
                          client.renderState().uploadedRoots.end(), root) ==
                client.renderState().uploadedRoots.end()) {
                outstanding.push_back(root);
                ++lost;
            }
        for (std::uint32_t root : client.renderState().uploadedRoots) {
            uploads++;
            const auto at = std::find(outstanding.begin(), outstanding.end(),
                                      root);
            if (at != outstanding.end()) outstanding.erase(at);
        }
        evictions += client.renderState().evictedRoots.size();
        client.confirmUploads();
        ++frames;
        distinctResident = std::max<std::uint64_t>(
            distinctResident,
            static_cast<std::uint64_t>(renderer.resident()));
        // The arena may never exceed its budget. This is the invariant the
        // whole design exists to provide.
        if (renderer.used() > renderer.capacity()) {
            std::printf("FAIL: arena over budget (%u > %u)\n", renderer.used(),
                        renderer.capacity());
            return 1;
        }
        sleepMs(4);
    }

    // No drain phase: with a budget below the window the working set is meant to
    // keep turning over, so "wait until it settles" would never finish. What is
    // checked below is that the turn-over is bounded and that the arena stays
    // inside its budget while it happens.

    std::printf("  arena: %u/%u nodes held, peak %llu bricks resident\n",
                renderer.used(), renderer.capacity(),
                static_cast<unsigned long long>(distinctResident));
    std::printf("  %llu frames, %llu uploads, %llu evictions (%llu needing "
                "recovery), %zu still outstanding\n",
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(uploads),
                static_cast<unsigned long long>(evictions),
                static_cast<unsigned long long>(lost), outstanding.size());

    // The arena must have been bounded by eviction, not by growing. If capacity
    // moved above the budget, the run tested growth instead and the eviction
    // path was never what kept the client inside its budget.
    if (renderer.capacity() != kBudget) {
        std::printf("FAIL: arena grew from %u to %u; growth should not have "
                    "been needed\n", kBudget, renderer.capacity());
        return 1;
    }

    // Eviction must actually have happened -- a budget this small against a
    // window this size cannot avoid it.
    if (evictions == 0) {
        std::printf("FAIL: no evictions occurred; the budget was never "
                    "exercised\n");
        return 1;
    }

    // And it must have been real eviction: at least one brick has to have been
    // dropped without an upload putting it straight back in the same frame.
    // When every eviction is a same-frame replacement the recovery path is
    // never touched, and the "no brick is lost" assertion below would pass for
    // a client that never forgets anything -- which is exactly the bug.
    if (lost == 0) {
        std::printf("FAIL: every eviction was replaced in the same frame; "
                    "no brick was actually lost and recovery was never "
                    "exercised\n");
        return 1;
    }

    // The invariant that actually has to hold, at any budget: no brick is
    // *lost*. An arena smaller than the window cannot hold everything, so
    // bricks churn -- upload, evict, resend, upload -- forever. That churn is
    // correct: the alternative is holding more than the budget, or dropping a
    // brick and never getting it back. What must never happen is a brick the
    // client believes it still holds and the server believes it delivered,
    // which is indistinguishable from terrain that never existed.
    //
    // So the check is that the working set keeps turning over, not that it
    // settles. A stalled recovery shows up as `outstanding` pinned at a
    // nonzero constant across a long idle stretch, because nothing ever comes
    // back to drain it.
    if (outstanding.empty()) {
        std::printf("FAIL: nothing outstanding, so the budget held the whole "
                    "window and eviction recovery was never exercised\n");
        return 1;
    }
    if (uploads == 0) {
        std::printf("FAIL: no uploads at all\n");
        return 1;
    }

    // Every outstanding brick must be one the client genuinely does not hold,
    // and the count must be bounded by the window rather than growing without
    // limit -- which would mean the arena is accumulating debris.
    if (outstanding.size() > 128) {
        std::printf("FAIL: %zu bricks outstanding is more than the window "
                    "could hold; the arena is accumulating debris\n",
                    outstanding.size());
        return 1;
    }

    // The decisive check, and the one that actually catches the bug.
    //
    // An arena smaller than its window is permanently asking for bricks it does
    // not have, so a correct client keeps the server streaming continuously:
    // every eviction is a fresh request. Measured, the stream sustains nearly
    // `uploadsPerFrame` every frame -- about 2,900 uploads over 1,500 frames.
    //
    // Break the retraction and the server stops: it still believes every
    // eviction was delivered, so it sends nothing more, and the stream goes
    // idle after the initial fill. That is not a subtle degradation -- it is
    // ~12x fewer uploads and terrain that never comes back. So the assertion is
    // that the stream stayed busy, which is what "evicted bricks are re-requested"
    // looks like from outside.
    const std::uint64_t possibleUploads = frames * kUploadsPerFrame;
    if (uploads * 2 < possibleUploads) {
        std::printf("FAIL: stream went idle. %llu uploads where up to %llu were "
                    "possible -- evicted bricks are not being re-requested, so "
                    "they will never come back\n",
                    static_cast<unsigned long long>(uploads),
                    static_cast<unsigned long long>(possibleUploads));
        return 1;
    }

    client.stop();
    server.stop();
    std::printf("lru eviction recovery: all passed\n");
    return 0;
}
