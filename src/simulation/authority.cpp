#include "authority.hpp"

#include <algorithm>
#include <cmath>

namespace voxel {

namespace {
constexpr float kGravity = 24.0f;
constexpr float kJumpSpeed = 8.6f;
constexpr float kMoveSpeed = 5.6f;
constexpr float kFlySpeed = 16.0f;   // flight is roughly three times walking
constexpr float kTerminalSpeed = 60.0f;
// The player's collision box, in cells: 3 x 3 x 7 with z as height.
//
// x and y are half-extents about the position, so the box spans
// [pos - 1.5, pos + 1.5] -- three cells wide. z is treated as half-extents too
// (3.5), so the box spans seven cells with the position at its centre; this
// differs from the old cylinder-and-height form only in that it is now a box
// rather than a round footprint, which matters because the terrain is sampled
// on integer cells and a cylinder of radius 0.3 tested one cell on each side at
// most. The width is what makes the player large enough to matter to the
// collision code at all.
constexpr float kPlayerHalfWidth = 1.5f;
constexpr float kPlayerHalfHeight = 3.5f;
constexpr float kEyeHeight = 3.0f;
constexpr float kMaxStep = 0.25f;   // sub-step size for swept collision
} // namespace

glm::vec3 Authority::spawnPosition(const World& world) {
    const auto centreX = static_cast<float>(world.spec().sizeX) * 0.5f;
    const auto centreY = static_cast<float>(world.spec().sizeY) * 0.5f;
    const auto maxZ = static_cast<std::int32_t>(world.spec().sizeZ);
    // The player is 3 cells wide, so the surface it lands on is the *highest*
    // column under its whole footprint -- not the one under its centre. Taking
    // the centre alone put the feet level with the centre column's top, which
    // is inside any neighbour standing even one cell higher: on the default
    // spawn that is 15 of 16 footprint cells already solid, so the player
    // spawned buried, the sweep found every direction blocked, and it could
    // never move at all.
    const int minX = static_cast<int>(std::floor(centreX - kPlayerHalfWidth));
    const int maxX = static_cast<int>(std::floor(centreX + kPlayerHalfWidth));
    const int minY = static_cast<int>(std::floor(centreY - kPlayerHalfWidth));
    const int maxY = static_cast<int>(std::floor(centreY + kPlayerHalfWidth));
    std::uint32_t top = 0;
    for (int y = minY; y <= maxY; ++y)
        for (int x = minX; x <= maxX; ++x)
            top = std::max(top, terrainTop(x, y, maxZ, world.shape()));

    // `position` is the centre of the 7-tall box, so the feet are 3.5 below it.
    // Spawning at the surface itself would bury the lower half of the player in
    // the ground and let the collision resolver push them out sideways.
    return glm::vec3(centreX, centreY,
                     static_cast<float>(top) + kPlayerHalfHeight + 0.1f);
}

Authority::Authority(std::uint32_t playerId, World& world)
    : m_world(world) {
    m_state.localPlayer.id = playerId;
    m_state.localPlayer.position = spawnPosition(world);
    m_state.localPlayer.grounded = false;
    m_state.playerCount = 1;
    m_state.players[0] = m_state.localPlayer;
}

bool Authority::collides(glm::vec3 position) const {
    // Half-open on the +z side so a box whose top edge lands exactly on a cell
    // boundary does not test the cell above it. With a 3.5 half-height and a
    // position on a whole number this matters constantly -- the player's feet
    // sit on a surface that is almost always an exact integer.
    const int minX = static_cast<int>(std::floor(position.x - kPlayerHalfWidth));
    const int maxX = static_cast<int>(std::floor(position.x + kPlayerHalfWidth));
    const int minY = static_cast<int>(std::floor(position.y - kPlayerHalfWidth));
    const int maxY = static_cast<int>(std::floor(position.y + kPlayerHalfWidth));
    const int minZ =
        static_cast<int>(std::floor(position.z - kPlayerHalfHeight));
    const int maxZ =
        static_cast<int>(std::ceil(position.z + kPlayerHalfHeight - 1e-4f)) - 1;
    // 4x4x7 = 112 cells tested per query. At a 5.6 m/s walk and 0.25 m
    // sub-steps that is ~2,500 lookups a second per player -- each one a
    // generate-if-absent into the server's brick cache, which is why the
    // cache has to stay warm around the player rather than covering the world.
    for (int z = minZ; z <= maxZ; ++z)
        for (int y = minY; y <= maxY; ++y)
            for (int x = minX; x <= maxX; ++x)
                if (m_world.isSolid(x, y, z)) return true;
    return false;
}

void Authority::movePlayer(float seconds) {
    const auto& spec = m_world.spec();
    PlayerState& player = m_state.localPlayer;
    const glm::vec3 start = player.position;

    // No camera basis is built here any more. The client resolves its own keys
    // against the same basis the renderer draws with and sends the resulting
    // world direction, so there is nothing left to reconstruct from `yaw` and
    // therefore nothing that can disagree with what the player sees.
    //
    // `yaw` is still replicated -- other clients draw this player's facing --
    // it is simply no longer the thing movement is derived from.
    //
    // The direction is normalised before use so that a diagonal covers the same
    // distance per tick as an axis: the client may legitimately send a vector
    // longer than 1 for a diagonal combination, and without this, W+D would be
    // a sqrt(2) times faster than W.
    glm::vec2 wish = m_wishMove;
    const float wishLength = glm::length(wish);
    if (wishLength > 1.0f) wish /= wishLength;

    const float speed = player.flying ? kFlySpeed : kMoveSpeed;
    player.velocity.x = wish.x * speed;
    player.velocity.y = wish.y * speed;

    glm::vec3 target = start + player.velocity * seconds;
    // Clamped to keep the whole box inside the world, not just its centre: a
    // half-width of 1.5 at the edge would otherwise hang a cell and a half of
    // the player through the world boundary.
    const float boundX = static_cast<float>(spec.sizeX) - kPlayerHalfWidth;
    const float boundY = static_cast<float>(spec.sizeY) - kPlayerHalfWidth;
    const float boundZ = static_cast<float>(spec.sizeZ) - kPlayerHalfHeight;
    target.x = std::clamp(target.x, kPlayerHalfWidth, boundX);
    target.y = std::clamp(target.y, kPlayerHalfWidth, boundY);
    target.z = std::clamp(target.z, kPlayerHalfHeight, boundZ);

    glm::vec3 resolved = start;
    for (int axis = 0; axis < 3; ++axis) {
        const float delta = target[static_cast<glm::vec3::length_type>(axis)] -
                            resolved[static_cast<glm::vec3::length_type>(axis)];
        if (delta == 0.0f) continue;
        const int steps =
            std::max(1, static_cast<int>(std::ceil(std::abs(delta) / kMaxStep)));
        const float step = delta / static_cast<float>(steps);
        for (int s = 0; s < steps; ++s) {
            glm::vec3 trial = resolved;
            trial[static_cast<glm::vec3::length_type>(axis)] += step;
            if (collides(trial)) {
                player.velocity[static_cast<glm::vec3::length_type>(axis)] = 0.0f;
                break;
            }
            resolved = trial;
        }
    }
    player.position = resolved;

    // Grounded probe: the footprint directly below the *feet*, not below the
    // centre. The two are a full box-height apart, and the probe used to read
    // the centre, so a player resting on the floor was probing a cell of open
    // air and never registered as grounded at all.
    const int fx = static_cast<int>(std::floor(player.position.x));
    const int fy = static_cast<int>(std::floor(player.position.y));
    const int feet =
        static_cast<int>(std::floor(player.position.z - kPlayerHalfHeight));
    const int r = static_cast<int>(kPlayerHalfWidth);
    bool grounded = false;
    for (int y = fy - r; !grounded && y <= fy + r; ++y)
        for (int x = fx - r; x <= fx + r; ++x)
            if (m_world.isSolid(x, y, feet - 1)) { grounded = true; break; }
    // Flight is not standing, even when hovering just above the floor.
    player.grounded = player.flying ? false : grounded;
    (void)kEyeHeight;
}

void Authority::step(const InputIntent& intent) {
    PlayerState& player = m_state.localPlayer;

    // Look is accumulated on the server so every client sees the same
    // orientation, but the client applies it locally too for responsiveness.
    player.yaw += intent.look.x;
    while (player.yaw > 180.0f) player.yaw -= 360.0f;
    while (player.yaw < -180.0f) player.yaw += 360.0f;
    player.pitch = std::clamp(player.pitch + intent.look.y, -89.0f, 89.0f);

    m_wishMove = glm::clamp(intent.moveDirection, glm::vec2(-1.0f),
                          glm::vec2(1.0f));

    // A zero client tick marks an intent the server replayed because no fresh
    // one arrived this tick. Held flight keys keep their last known value so a
    // dropped packet cannot make a held key stutter, and a released key is
    // picked up on the very next fresh intent. The requested mode is latched
    // for the same reason: it is a desired state, not an event.
    if (intent.clientTick != 0) {
        m_ascendHeld = intent.jumpPressed;
        m_descendHeld = intent.descendPressed;
        m_flyingRequested = intent.flying;
    }
    player.flying = m_flyingRequested;

    // Nothing about flight is simulated on the client: this is the only place
    // gravity is switched off, and it happens here, next to the collision it
    // still respects.
    if (player.flying) {
        float vertical = 0.0f;
        if (m_ascendHeld) vertical += 1.0f;
        if (m_descendHeld) vertical -= 1.0f;
        player.velocity.z = vertical * kFlySpeed;
    } else {
        if (intent.jumpPressed && player.grounded) {
            player.velocity.z = kJumpSpeed;
            player.grounded = false;
        }
        player.velocity.z -= kGravity * kTickSeconds;
        player.velocity.z = std::max(player.velocity.z, -kTerminalSpeed);
    }

    movePlayer(kTickSeconds);

    m_state.players[0] = player;
    m_state.acknowledgedInputTick =
        std::max(m_state.acknowledgedInputTick, intent.clientTick);
    ++m_state.tick;
}

} // namespace voxel
