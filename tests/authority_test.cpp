// Authority tests: the server is the only thing that may move a player.
//
// The point is the basis, not the plumbing. The renderer builds its camera
// basis as right = cross(forward, up) with up = +Z, so the authority has to
// resolve movement in exactly that basis or W walks somewhere other than where
// the client is looking. Yaw is degrees on the wire, so feeding degrees into
// trigonometry is the classic way to break that agreement, and a diagonal that
// outruns an axis is the classic way to break the speed budget.

#include <cmath>
#include <cstdio>

#include "simulation/authority.hpp"

using namespace voxel;

namespace {

constexpr int kFloorZ = 6;          // cells z < 6 are stone, z >= 6 are air
constexpr float kCentre = 24.0f;    // middle of the carved platform
constexpr float kWalkSpeed = 5.6f;  // mirrors the authority's constants
constexpr float kFlySpeed = 16.0f;
constexpr float kTick = kTickSeconds;

// A flat platform of air with open space above it, carved out of terrain.
struct Arena {
    World world;
    explicit Arena() : world(WorldSpec{64, 64, 64, 32}, 1) {
        world.fill(8, 8, 0, 40, 40, kFloorZ, kStoneId);
        world.fill(8, 8, kFloorZ, 40, 40, 64, 0);
    }

    // Feet height the player starts at: on the floor, or hovering well above.
    //
    // `position` is the centre of the 3x3x7 collision box, not the feet, so the
    // feet sit half a box lower. Placing the centre at the feet height would
    // bury the bottom two thirds of the player in the floor and leave it unable
    // to move -- which is what this used to do once the box grew from a
    // cylinder to something with real width and height.
    Authority authority(float z, float yaw = 0.0f) {
        Authority authority(1, world);
        authority.state().localPlayer.yaw = yaw;
        authority.state().localPlayer.pitch = 0.0f;
        authority.state().localPlayer.position = {kCentre, kCentre, z + kHalfBox};
        return authority;
    }
    static constexpr float kHalfBox = 3.5f;    // half of the 7-tall box
    static constexpr float kHalfWidth = 1.5f;  // half of the 3-wide box
    static constexpr std::uint16_t kStoneId = 1;
};

// Steps `ticks` ticks with one intent. Every step carries a fresh tick number,
// because the server drops an intent whose tick it has already acknowledged.
void run(Authority& authority, const InputIntent& intent, int ticks) {
    std::uint32_t tick = 1;
    for (int i = 0; i < ticks; ++i) {
        InputIntent frame = intent;
        frame.clientTick = tick++;
        authority.step(frame);
    }
}

void runReplayed(Authority& authority, int ticks) {
    // A default intent has clientTick 0: that is how the server marks a tick
    // for which no fresh intent arrived.
    for (int i = 0; i < ticks; ++i) authority.step(InputIntent{});
}

float travelled(const glm::vec3& from, const glm::vec3& to) {
    return glm::length(to - from);
}

int failures = 0;

// RelWithDebInfo defines NDEBUG, which compiles assert() into nothing: the
// suite would then pass without asserting anything. Every case reports and
// keeps going, so one run shows everything that is wrong rather than the first
// failure.
#define CHECK(condition)                                                     \
    do {                                                                      \
        if (!(condition)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                       \
        }                                                                     \
    } while (false)

} // namespace

int main() {
    // Nothing in these tests writes to the world, so every case can run on
    // one shared arena and each still gets its own authority to drive.
    Arena arena;

    // --- movement follows the requested direction, not the keys -------------
    //
    // The authority no longer builds a camera basis from `yaw`: the client
    // resolves its keys against the view it is drawing and sends a world-space
    // direction. So these cases are about the direction being obeyed exactly,
    // and about yaw having no influence at all.
    {
        Authority authority = arena.authority(static_cast<float>(kFloorZ));
        const glm::vec3 start = authority.state().localPlayer.position;
        // Facing +X at yaw 0, but asking for +Y directly. A yaw-dependent
        // implementation would walk +X here and fail this.
        run(authority, InputIntent{1, {0.0f, 1.0f}, {}}, 10);
        const glm::vec3 moved = authority.state().localPlayer.position - start;
        CHECK(std::abs(moved.y - kWalkSpeed * 10 * kTick) < 1e-3f);
        CHECK(std::abs(moved.x) < 1e-5f);
        CHECK(std::abs(moved.z) < 1e-5f);
    }
    {
        // The same direction must be walked the same way at any yaw. This is
        // the property the old yaw-relative path could not have.
        for (const float yaw : {0.0f, 90.0f, 180.0f, -45.0f}) {
            Authority authority = arena.authority(static_cast<float>(kFloorZ),
                                                 yaw);
            const glm::vec3 start = authority.state().localPlayer.position;
            run(authority, InputIntent{1, {-1.0f, 0.0f}, {}}, 10);
            const glm::vec3 moved =
                authority.state().localPlayer.position - start;
            CHECK(std::abs(moved.x + kWalkSpeed * 10 * kTick) < 1e-3f);
            CHECK(std::abs(moved.y) < 1e-5f);
        }
    }
    {
        // Reversing the direction reverses the movement, exactly.
        Authority forward = arena.authority(static_cast<float>(kFloorZ));
        const glm::vec3 forwardStart = forward.state().localPlayer.position;
        run(forward, InputIntent{1, {0.7f, -0.7f}, {}}, 10);

        Authority back = arena.authority(static_cast<float>(kFloorZ));
        const glm::vec3 backStart = back.state().localPlayer.position;
        run(back, InputIntent{1, {-0.7f, 0.7f}, {}}, 10);

        const glm::vec3 a = forward.state().localPlayer.position - forwardStart;
        const glm::vec3 b = back.state().localPlayer.position - backStart;
        CHECK(std::abs(a.x + b.x) < 1e-5f);
        CHECK(std::abs(a.y + b.y) < 1e-5f);
    }

    // --- a diagonal must not outrun a single axis --------------------------
    {
        Authority straight = arena.authority(static_cast<float>(kFloorZ));
        const glm::vec3 straightStart = straight.state().localPlayer.position;
        run(straight, InputIntent{1, {0.0f, 1.0f}, {}}, 30);

        // A 45 degree diagonal, sent as the (unnormalised) direction a client
        // resolving W+D would produce.
        Authority diagonal = arena.authority(static_cast<float>(kFloorZ));
        const glm::vec3 diagonalStart = diagonal.state().localPlayer.position;
        run(diagonal, InputIntent{1, {1.0f, 1.0f}, {}}, 30);

        const float straightDistance =
            travelled(straightStart, straight.state().localPlayer.position);
        const float diagonalDistance =
            travelled(diagonalStart, diagonal.state().localPlayer.position);
        CHECK(std::abs(diagonalDistance - straightDistance) < 1e-3f);
        const glm::vec3 delta =
            diagonal.state().localPlayer.position - diagonalStart;
        CHECK(std::abs(std::abs(delta.x) - std::abs(delta.y)) < 1e-3f);
        CHECK(straightDistance > 0.5f * kWalkSpeed * 30 * kTick);
    }
    {
        // A hostile client must not buy speed with a longer vector. The
        // decoder clamps, but the authority cannot rely on that alone.
        Authority nominal = arena.authority(static_cast<float>(kFloorZ));
        const glm::vec3 nominalStart = nominal.state().localPlayer.position;
        run(nominal, InputIntent{1, {0.0f, 1.0f}, {}}, 30);

        Authority boosted = arena.authority(static_cast<float>(kFloorZ));
        const glm::vec3 boostedStart = boosted.state().localPlayer.position;
        run(boosted, InputIntent{1, {0.0f, 50.0f}, {}}, 30);

        const float nominalDistance =
            travelled(nominalStart, nominal.state().localPlayer.position);
        const float boostedDistance =
            travelled(boostedStart, boosted.state().localPlayer.position);
        CHECK(boostedDistance <= nominalDistance + 1e-3f);
    }

    // --- collision still stops the player -----------------------------------
    {
        // The arena floor and air stop at x = 8; west of that is terrain.
        Authority authority = arena.authority(static_cast<float>(kFloorZ),
                                             180.0f);
        authority.state().localPlayer.position = {10.5f, kCentre, 6.0f};
        // The player is west-facing (yaw 180) but movement no longer consults yaw, so
        // the intent has to ask for -X directly. That is the point of the
        // change: the wire carries "walk that way", never "press W".
        run(authority, InputIntent{1, {-1.0f, 0.0f}, {}}, 120);
        // The sweep stops the box against the wall. The box is 3 wide, so its
        // -x face is 1.5 behind the centre, and the first solid cell is x = 7,
        // which the face can just touch: the centre cannot pass x = 8.5. The
        // sub-step the sweep moves in is 0.093 wide, so allow for that.
        // The invariant is about the box, not the centre: the -x face must not
        // enter the solid region, which starts at x = 8. Asserting a magic
        // centre value instead would just re-encode the box width, and the
        // whole point of the assertion is that the width is applied at all.
        CHECK(authority.state().localPlayer.position.x - Arena::kHalfWidth >=
              7.9f);
        // And it must actually have stopped, rather than walked the whole way.
        CHECK(authority.state().localPlayer.position.x < 11.0f);
    }

    // --- hovering: gravity is off while flying -----------------------------
    {
        Authority authority = arena.authority(20.0f);
        const glm::vec3 start = authority.state().localPlayer.position;

        InputIntent asking{1, {0.0f, 0.0f}, {}};
        asking.flying = true;
        run(authority, asking, 30);
        CHECK(authority.state().localPlayer.flying);
        CHECK(std::abs(authority.state().localPlayer.position.z - start.z) <
               1e-5f);
        // Not standing while hovering either way.
        CHECK(!authority.state().localPlayer.grounded);
    }

    // --- flight is faster, space lifts, shift drops -------------------------
    {
        Authority authority = arena.authority(20.0f);
        InputIntent flying{1, {0.0f, 1.0f}, {}};
        flying.flying = true;
        run(authority, flying, 1);
        CHECK(authority.state().localPlayer.flying);

        // Horizontal: three times the walk speed, same basis.
        const glm::vec3 flat = authority.state().localPlayer.position;
        run(authority, flying, 10);
        const float flown = travelled(flat, authority.state().localPlayer.position);
        CHECK(std::abs(flown - kFlySpeed * 10 * kTick) < 1e-3f);

        const glm::vec3 level = authority.state().localPlayer.position;
        InputIntent down{1, {0.0f, 0.0f}, {}};
        down.flying = true;
        down.descendPressed = true;
        run(authority, down, 10);
        const glm::vec3 dropped = authority.state().localPlayer.position;
        CHECK(dropped.z < level.z - 0.98f * kFlySpeed * 10 * kTick);

        InputIntent up{};
        up.clientTick = 1;
        up.moveDirection = {0.0f, 0.0f};
        up.flying = true;
        up.jumpPressed = true;
        run(authority, up, 10);
        // Space lifts again from wherever the shift drop left the player.
        CHECK(authority.state().localPlayer.position.z >
               dropped.z + 0.98f * kFlySpeed * 10 * kTick);
        // Symmetric: it climbs back to where it was hovering.
        CHECK(std::abs(authority.state().localPlayer.position.z - level.z) <
               1e-3f);

        // Releasing both keys stops the vertical axis dead: flight has no
        // momentum of its own.
        InputIntent hold{1, {0.0f, 0.0f}, {}};
        hold.flying = true;
        run(authority, hold, 1);
        const glm::vec3 hovering = authority.state().localPlayer.position;
        CHECK(std::abs(hovering.z - level.z) < 1e-5f);

        // Turning flight back off hands the player back to gravity.
        run(authority, InputIntent{1, {0.0f, 0.0f}, {}}, 10);
        CHECK(!authority.state().localPlayer.flying);
        // Falling rather than hovering: the authority integrates with
        // semi-implicit Euler, so ten ticks from rest drop ~0.3 cells.
        const float fell = hovering.z - authority.state().localPlayer.position.z;
        CHECK(fell > 0.2f && fell < 0.4f);
        // And the floor still catches them, only slower than flying.
        run(authority, InputIntent{1, {0.0f, 0.0f}, {}}, 400);
        CHECK(authority.state().localPlayer.grounded);
        CHECK(authority.state().localPlayer.position.z > kFloorZ);
    }

    // --- a replayed tick must not cancel a held flight key ------------------
    {
        Authority authority = arena.authority(20.0f);
        InputIntent intent{1, {0.0f, 0.0f}, {}};
        intent.flying = true;
        intent.descendPressed = true;
        run(authority, intent, 5);
        const glm::vec3 level = authority.state().localPlayer.position;
        runReplayed(authority, 10);
        CHECK(authority.state().localPlayer.flying);
        CHECK(authority.state().localPlayer.position.z <
               level.z - 0.9f * kFlySpeed * 10 * kTick);
    }

    // --- gravity and the floor are untouched on the ground ------------------
    {
        Authority authority = arena.authority(static_cast<float>(kFloorZ));
        run(authority, InputIntent{1, {0.0f, 0.0f}, {}}, 60);
        CHECK(authority.state().localPlayer.grounded);
        // Resting on the floor puts the *feet* at kFloorZ; `position` is the
        // centre of the 7-tall box, so it rests half a box higher.
        CHECK(std::abs(authority.state().localPlayer.position.z -
                       (static_cast<float>(kFloorZ) + Arena::kHalfBox)) < 1e-4f);
        CHECK(std::abs(authority.state().localPlayer.velocity.z) < 1e-5f);
    }

    // --- a spawned player starts in open air and can actually walk ---------
    //
    // The player is 3 wide, so the surface it lands on is the highest column
    // under its whole footprint. Spawning on the height of the *centre* column
    // instead put the feet inside any neighbour standing a cell higher, and a
    // buried player is not merely ugly: the sweep finds every direction
    // blocked, so it cannot move at all. That failure is invisible until
    // something walks the player and measures nothing happening, so it is
    // pinned here across several world sizes -- including the production one.
    {
        for (const WorldSpec& spec :
             {WorldSpec{1024, 1024, 256, 128}, WorldSpec{512, 512, 128, 128},
              WorldSpec{65536, 65536, 8192, 128}}) {
            World world(spec, 7);
            Authority authority(1, world);
            const glm::vec3 spawn = authority.state().localPlayer.position;

            int buried = 0;
            for (int z = static_cast<int>(std::floor(spawn.z - Arena::kHalfBox));
                 z <= static_cast<int>(std::floor(spawn.z + Arena::kHalfBox)); ++z)
                for (int y = static_cast<int>(std::floor(spawn.y - Arena::kHalfWidth));
                     y <= static_cast<int>(std::floor(spawn.y + Arena::kHalfWidth)); ++y)
                    for (int x = static_cast<int>(std::floor(spawn.x - Arena::kHalfWidth));
                         x <= static_cast<int>(std::floor(spawn.x + Arena::kHalfWidth)); ++x)
                        buried += world.isSolid(x, y, z) ? 1 : 0;
            if (buried != 0) {
                std::printf("FAIL spawn in %ux%ux%u is buried in %d cells\n",
                            spec.sizeX, spec.sizeY, spec.sizeZ, buried);
                ++failures;
            }

            run(authority, InputIntent{1, {0.0f, 1.0f}, {}}, 120);
            const float moved = std::abs(authority.state().localPlayer.position.y -
                                         spawn.y);
            if (moved < 1.0f) {
                std::printf("FAIL spawn in %ux%ux%u could not walk (%.2f)\n",
                            spec.sizeX, spec.sizeY, spec.sizeZ,
                            static_cast<double>(moved));
                ++failures;
            }
        }
    }

    if (failures != 0) {
        std::printf("authority tests FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("authority tests passed\n");
    return 0;
}
