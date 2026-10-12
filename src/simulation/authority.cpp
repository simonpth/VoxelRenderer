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
constexpr float kPlayerRadius = 0.3f;
constexpr float kPlayerHeight = 1.8f;
constexpr float kEyeHeight = 1.6f;
constexpr float kMaxStep = 0.25f;   // sub-step size for swept collision
} // namespace

glm::vec3 Authority::spawnPosition(const World& world) {
    const auto centreX = static_cast<float>(world.spec().sizeX) * 0.5f;
    const auto centreY = static_cast<float>(world.spec().sizeY) * 0.5f;
    const auto cx = static_cast<std::int32_t>(std::floor(centreX));
    const auto cy = static_cast<std::int32_t>(std::floor(centreY));
    const auto top = terrainTop(cx, cy, static_cast<std::int32_t>(world.spec().sizeZ));
    return glm::vec3(centreX, centreY, static_cast<float>(top) + 6.0f);
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
    const int minX = static_cast<int>(std::floor(position.x - kPlayerRadius));
    const int maxX = static_cast<int>(std::floor(position.x + kPlayerRadius));
    const int minY = static_cast<int>(std::floor(position.y - kPlayerRadius));
    const int maxY = static_cast<int>(std::floor(position.y + kPlayerRadius));
    const int minZ = static_cast<int>(std::floor(position.z));
    const int maxZ =
        static_cast<int>(std::ceil(position.z + kPlayerHeight - 1e-4f)) - 1;
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
    target.x = std::clamp(target.x, kPlayerRadius,
                          static_cast<float>(spec.sizeX) - kPlayerRadius);
    target.y = std::clamp(target.y, kPlayerRadius,
                          static_cast<float>(spec.sizeY) - kPlayerRadius);

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

    // Grounded probe: the 2x2 footprint of cells directly below the feet.
    const int fx = static_cast<int>(std::floor(player.position.x));
    const int fy = static_cast<int>(std::floor(player.position.y));
    const int fz = static_cast<int>(std::floor(player.position.z));
    const int r = static_cast<int>(std::floor(kPlayerRadius));
    bool grounded = false;
    for (int y = fy - r; !grounded && y <= fy + r; ++y)
        for (int x = fx - r; x <= fx + r; ++x)
            if (m_world.isSolid(x, y, fz - 1)) { grounded = true; break; }
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
