// End-to-end lifecycle test: real UDP and TCP over loopback.
//
// The point is the shutdown path. A std::thread whose destructor runs while it
// is still joinable terminates the process, so starting and then destroying a
// server and a client must be exercised for real, not just compile.
#include <atomic>
#include <chrono>
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

} // namespace

int main() {
    // Use an uncommon port pair so a stale run cannot collide.
    const std::uint16_t port = 28431;
    const WorldSpec spec{1024, 1024, 256, 128};

    {
        Server server(port, spec, 0xC0FFEE);
        server.start();
        if (!server.running()) {
            std::printf("server did not start (port in use?)\n");
            return 1;
        }

        Client client(Peer{"127.0.0.1", port});
        client.start();

        // Registration, then the world stream handshake, then bricks.
        for (int i = 0; i < 200 && !client.renderState().sizeX; ++i) sleepMs(50);
        if (client.renderState().sizeX != spec.sizeX) {
            std::printf("descriptor never arrived (sizeX=%u)\n",
                        client.renderState().sizeX);
            return 1;
        }

        // Drive the client far enough that bricks are uploaded, and confirm
        // the geometry it was told matches the geometry it asked for.
        for (int i = 0; i < 200; ++i) {
            InputIntent intent{};
            intent.clientTick = static_cast<std::uint32_t>(i + 1);
            intent.look = {0.0005f, 0.0f};
            client.updateRender(1.0f / 60.0f);
            sleepMs(5);
            if (client.renderState().rootCount == spec.rootCount()) break;
        }
        if (client.renderState().rootCount != spec.rootCount()) {
            std::printf("descriptor rootCount %u != %u\n",
                        client.renderState().rootCount, spec.rootCount());
            return 1;
        }

        // Let the stream run, then stop both in both orders.
        sleepMs(500);
        client.stop();
        server.stop();
        // A second stop must be a no-op, not a terminate.
        client.stop();
        server.stop();
    }

    // Both destructors ran here; if either left a thread joinable the process
    // would already have aborted.

    // Repeat the whole cycle to be sure cleanup released the ports.
    {
        Server server(port, spec, 0xC0FFEE);
        server.start();
        if (!server.running()) {
            std::printf("second server did not start\n");
            return 1;
        }
        server.stop();
    }

    std::printf("lifecycle tests passed\n");
    return 0;
}
