// Does the server stream the whole world until there is nothing left?
//
// This is the property the arena being far larger than the interest window
// depends on. If only the window streams, a client that can hold the world has
// no reason to, and every step into unwatched terrain is a hole that fills in
// later. So two things are checked: every root-layer brick arrives, and both
// tiers did their job -- the window tier (level 0 octrees near the player)
// and the background fill (top-level grids for the far world).
//
// Pacing used to be checked here too, through a shaped bandwidth -- and it
// kept failing for reasons to do with the world, not the bucket. The pyramid
// made 128 bricks cost ~80 KB against a 300 KB burst cap, so the whole world
// legally left in the opening burst; with a larger cap the same thing happened
// slower. Pacing is a property of the bucket and is tested as one, in
// pacer_test.cpp: deterministic, no sockets, no terrain.
#include <chrono>
#include <cstdio>
#include <thread>
#include <unordered_set>
#include <vector>

#include "network/client.hpp"
#include "network/server.hpp"
#include "simulation/lod_rings.hpp"

using namespace voxel;
using namespace voxel::net;

namespace {

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

} // namespace

int main() {
    const WorldSpec spec{1024, 1024, 256, 128};
    const int port = 27551;
    const std::uint32_t roots = spec.rootCount();

    // Unmetered: shaping would only slow a completeness check, and the
    // shaping itself is covered by the pacer unit test.

    Server server(port, spec, 0xB0BAFE);
    server.start();
    Client client(Peer{"127.0.0.1", port});
    client.start();

    std::unordered_set<std::uint32_t> seen;
    std::uint64_t bytes = 0;
    std::uint64_t levelCount[8] = {};
    std::uint64_t firstAt = 0, lastAt = 0;
    bool complete = false;

    // Draining starts the instant the client connects, not after waiting for
    // the descriptor. Waiting first makes the measurement wrong: the client's
    // queue buffers up to 256 bricks, so by the time the loop begins the whole
    // world can already be sitting in it, and delivery then looks like an
    // instant burst however slowly the server produced it. The clock has to
    // start where the server does.
    const auto start = std::chrono::steady_clock::now();
    // Bounded by wall clock, not by a loop count. A fixed iteration cap is a
    // deadline in disguise, and it is measured in *this machine's* idle speed:
    // the whole world lands in about 7,300 iterations when nothing else is
    // Thirty seconds. The world is ~3.5 MB against a 300 KB/s ceiling, so
    // ~12 s is the honest cost; beyond 30 s the stream is stalled, not slow,
    // and waiting longer only burns CI time to learn it again.
    constexpr double kDeadlineSeconds = 30.0;
    int iterations = 0;
    for (; !complete; ++iterations) {
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                           start)
                .count() > kDeadlineSeconds) {
            break;
        }
        // Acknowledge every brick, as a renderer with room to spare would.
        // Skipping this is not a harmless simplification: the server treats an
        // unacknowledged brick as still in flight and declines to re-offer
        // anything already sent, so a client that never acknowledges stalls the
        // whole world after a single pass.
        client.drainBricks(client.renderState().bricks);
        for (BrickUpdate& update : client.renderState().bricks) {
            client.renderState().processedRefs.push_back({update.rootIndex, update.lodLevel});
            if (update.remove) continue;
            // Both payloads: an octree costs 8 bytes a node and a grid 4 bytes
            // a cell, and rating only the octrees would let the grids travel
            // free -- which is most of the background fill.
            bytes += static_cast<std::uint64_t>(update.nodes.size()) *
                         sizeof(SvoNode) +
                     static_cast<std::uint64_t>(update.cells.size()) *
                         sizeof(AvgCell);
            update.uploaded = true;
            client.renderState().uploadedRefs.push_back({update.rootIndex, update.lodLevel});
            if (update.lodLevel < 8) ++levelCount[update.lodLevel];
            if (seen.insert(update.rootIndex).second) {
                if (seen.size() == 1) firstAt = static_cast<std::uint64_t>(iterations);
                lastAt = static_cast<std::uint64_t>(iterations);
            }
        }
        client.renderState().bricks.clear();
        client.confirmUploads();
        if (iterations % 100 == 0) {
            const glm::vec3 pp =
                client.authoritativeState().localPlayer.position;
            std::printf("  ... %zu/%u bricks after %.1fs at (%.0f,%.0f,%.0f)\n",
                        seen.size(), roots,
                        std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - start)
                            .count(), pp.x, pp.y, pp.z);
        }
        complete = client.renderState().sizeX != 0 && seen.size() >= roots;
        sleepMs(1);
    }
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();

    std::printf("  world has %u root-layer bricks\n", roots);
    std::printf("  delivered %zu distinct bricks (%.0f%%), %.1f KiB in %.2fs\n",
                seen.size(),
                100.0 * static_cast<double>(seen.size()) / roots,
                static_cast<double>(bytes) / 1024.0, elapsed);
    std::printf("  levels seen:");
    for (int l = 0; l < 8; ++l)
        if (levelCount[l] != 0)
            std::printf(" L%d:%llu", l, (unsigned long long)levelCount[l]);
    std::printf("\n");
    std::printf("  delivery spanned iterations %llu..%llu of %d\n",
                static_cast<unsigned long long>(firstAt),
                static_cast<unsigned long long>(lastAt), iterations);

    // The whole world, not just the neighbourhood.
    if (seen.size() < roots) {
        std::printf("FAIL: only %zu of %u bricks delivered; the server is "
                    "streaming the window, not the world\n",
                    seen.size(), roots);
        return 1;
    }

    // Both tiers did their job. Level 0 is the window tier -- the exact octree
    // near the player -- and the top level is the background fill, one cell
    // per far brick. A world of only level 0 means the fill never ran (every
    // step out of the window is a hole); a world with no level 0 means the
    // window never ran (the player stands in scenery). The top level here is
    // the brick root level, so it is derived, not written down.
    const std::uint8_t top =
        static_cast<std::uint8_t>(brickLodLevel(spec.brick));
    if (levelCount[0] == 0) {
        std::printf("FAIL: no full-detail brick arrived; the window tier "
                    "never delivered\n");
        return 1;
    }
    if (levelCount[top] == 0) {
        std::printf("FAIL: no top-level grid arrived; the background fill "
                    "never delivered\n");
        return 1;
    }

    client.stop();
    server.stop();
    std::printf("whole-world streaming: all passed\n");
    return 0;
}