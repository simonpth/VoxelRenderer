// Does the server stream the whole world, steadily, until there is nothing
// left?
//
// This is the property the arena being far larger than the interest window
// depends on. If only the window streams, a client that can hold the world has
// no reason to, and every step into unwatched terrain is a hole that fills in
// later. If the whole world streams with no pacing, it arrives as one burst on
// connect, which is no better.
//
// So three things are checked: the whole world arrives, it arrives spread over
// time rather than in one burst, and the sustained rate stays under the
// configured bandwidth.
#include <chrono>
#include <cstdio>
#include <thread>
#include <unordered_set>
#include <vector>

#include "network/client.hpp"
#include "network/server.hpp"

using namespace voxel;
using namespace voxel::net;

namespace {

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

} // namespace

int main() {
    const WorldSpec spec{512, 512, 128, 64};
    const int port = 27551;
    const std::uint32_t roots = spec.rootCount();

    // A deliberately small bandwidth, so "arrives over time" is observable in
    // a test rather than being indistinguishable from instant.
    constexpr std::uint64_t kBandwidth = 400 * 1024;

    Server server(port, spec, 0xB0BAFE);
    server.setBandwidth(kBandwidth);
    server.start();
    Client client(Peer{"127.0.0.1", port});
    client.start();

    std::unordered_set<std::uint32_t> seen;
    std::uint64_t bytes = 0;
    std::uint64_t firstAt = 0, lastAt = 0;
    bool complete = false;

    // Draining starts the instant the client connects, not after waiting for
    // the descriptor. Waiting first makes the measurement wrong: the client's
    // queue buffers up to 256 bricks, so by the time the loop begins the whole
    // world can already be sitting in it, and delivery then looks like an
    // instant burst however slowly the server produced it. The clock has to
    // start where the server does.
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 40'000 && !complete; ++i) {
        // Acknowledge every brick, as a renderer with room to spare would.
        // Skipping this is not a harmless simplification: the server treats an
        // unacknowledged brick as still in flight and declines to re-offer
        // anything already sent, so a client that never acknowledges stalls the
        // whole world after a single pass.
        for (BrickUpdate& update : client.renderState().bricks) {
            client.renderState().processedRoots.push_back(update.rootIndex);
            if (update.remove) continue;
            bytes += static_cast<std::uint64_t>(update.nodes.size()) *
                     sizeof(SvoNode);
            update.uploaded = true;
            client.renderState().uploadedRoots.push_back(update.rootIndex);
            if (seen.insert(update.rootIndex).second) {
                if (seen.size() == 1) firstAt = i;
                lastAt = i;
            }
        }
        client.renderState().bricks.clear();
        client.confirmUploads();
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
    std::printf("  delivery spanned iterations %llu..%llu\n",
                static_cast<unsigned long long>(firstAt),
                static_cast<unsigned long long>(lastAt));

    // The whole world, not just the neighbourhood.
    if (seen.size() < roots) {
        std::printf("FAIL: only %zu of %u bricks delivered; the server is "
                    "streaming the window, not the world\n",
                    seen.size(), roots);
        return 1;
    }

    // Paced, not a burst. A single-burst delivery lands entirely within a
    // handful of iterations no matter how fast the loop is; a paced one is
    // spread across a duration that reflects the bandwidth it was given.
    if (lastAt - firstAt < 20) {
        std::printf("FAIL: the whole world arrived within %llu iterations; "
                    "the stream is not paced\n",
                    static_cast<unsigned long long>(lastAt - firstAt));
        return 1;
    }

    // And the sustained rate respects the ceiling. The slack absorbs one
    // second of burst credit, which the bucket deliberately allows.
    if (elapsed > 0.0) {
        const double rate = static_cast<double>(bytes) / elapsed;
        std::printf("  sustained %.1f KiB/s against a %llu KiB/s ceiling\n",
                    rate / 1024.0,
                    static_cast<unsigned long long>(kBandwidth / 1024));
        if (rate > kBandwidth * 1.6) {
            std::printf("FAIL: sustained %.1f KiB/s exceeds the %llu KiB/s "
                        "ceiling; the pacer is not limiting anything\n",
                        rate / 1024.0,
                        static_cast<unsigned long long>(kBandwidth / 1024));
            return 1;
        }
    }

    client.stop();
    server.stop();
    std::printf("whole-world streaming: all passed\n");
    return 0;
}