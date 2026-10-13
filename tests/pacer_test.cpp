// The token bucket, directly: does it pace, cap bursts, and respect unmetered?
//
// This used to be asserted end to end, through a real client and server with a
// shaped bandwidth -- and it kept failing for reasons to do with the world,
// not the bucket. The pyramid made a 128-brick world cost 80 KB against a
// 300 KB burst cap, so the whole world legally left in the opening burst and
// the "paced, not a burst" assertion failed on a correct server; with a larger
// cap the same thing happened slower. Pacing is a property of the bucket, so
// it is tested as one: deterministic, no sockets, no terrain, milliseconds.
#include <cstdint>
#include <cstdio>

#include "network/pacer.hpp"

using namespace voxel::net;

namespace {

int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                      \
        if (!(condition)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            ++failures;                                                       \
        }                                                                     \
    } while (false)

} // namespace

int main() {
    // Unmetered: everything fits, always, and charging is a no-op rather than
    // an underflow. Zero meaning "nothing is ever sent" would be the quietest
    // possible way to break every test that does not set a bandwidth.
    {
        Pacer free(0, 1000);
        CHECK(free.canSpend(1000, 0));
        CHECK(free.canSpend(1000, 1u << 30));
        free.charge(1000, 1u << 30);
        CHECK(free.canSpend(2000, 1u << 30));
    }

    // Metered: the opening burst is exactly one second of credit, no more.
    {
        Pacer pacer(400 * 1024, 0);
        CHECK(pacer.canSpend(0, 400 * 1024));
        CHECK(!pacer.canSpend(0, 400 * 1024 + 1));
        // Spent credit is gone until time passes: the same second cannot pay
        // twice.
        pacer.charge(0, 400 * 1024);
        CHECK(!pacer.canSpend(0, 1));
    }

    // Refill is linear in elapsed milliseconds: half a second earns half a
    // second of bytes, and the bucket never exceeds its one-second cap however
    // long it idles -- which is what stops a long pause banking a whole world
    // of burst.
    {
        Pacer pacer(1000, 0);
        pacer.charge(0, 1000);
        CHECK(!pacer.canSpend(0, 1));
        CHECK(pacer.canSpend(500, 500));
        CHECK(!pacer.canSpend(500, 501));
        pacer.charge(500, 500);
        // Ten idle seconds still bank only one second of credit.
        CHECK(pacer.canSpend(10500, 1000));
        CHECK(!pacer.canSpend(10500, 1001));
    }

    // Charging more than is banked saturates at zero rather than wrapping: a
    // wrapped u64 would read as ~18 exabytes of credit and the ceiling would
    // be decorative.
    {
        Pacer pacer(1000, 0);
        pacer.charge(0, 1u << 30);
        CHECK(!pacer.canSpend(0, 1));
        CHECK(pacer.canSpend(1000, 1000));
    }

    // A clock that stands still earns nothing, and one that runs backward
    // earns nothing either rather than rewinding the bucket.
    {
        Pacer pacer(1000, 5000);
        pacer.charge(5000, 1000);
        CHECK(!pacer.canSpend(5000, 1));
        CHECK(!pacer.canSpend(4000, 1));
        CHECK(pacer.canSpend(6000, 1000));
    }

    if (failures == 0) std::printf("pacer tests passed\n");
    return failures ? 1 : 0;
}
