#include "authority.hpp"

#include <algorithm>
#include <cmath>

namespace voxel {
namespace {
constexpr float kGravity = 20.0f;
constexpr float kJumpSpeed = 7.0f;
constexpr float kPlayerRadius = 0.35f;
constexpr float kPlayerHeight = 2.0f;

float wrapDegrees(float value) {
    while (value > 180.0f) value -= 360.0f;
    while (value < -180.0f) value += 360.0f;
    return value;
}
}

Authority::Authority(std::uint32_t playerId) {
    m_state.localPlayer.id = playerId;
    m_state.world = makeTerrain();
    m_state.localPlayer.position = {16.0f, 16.0f, 12.0f};
    m_state.localPlayer.grounded = false;
    m_state.playerCount = 1;
    m_state.players[0] = m_state.localPlayer;
}

void Authority::step(const InputIntent& intent, float seconds) {
    m_intent = intent;
    auto& player = m_state.localPlayer;

    player.yaw = wrapDegrees(player.yaw + intent.look.x);
    player.pitch = std::clamp(player.pitch + intent.look.y, -89.0f, 89.0f);

    player.velocity.x = intent.horizontalDelta.x / seconds;
    player.velocity.y = intent.horizontalDelta.y / seconds;

    const bool wasGrounded = player.grounded;
    if (intent.jumpPressed && wasGrounded) {
        player.velocity.z = kJumpSpeed;
        player.grounded = false;
    }
    player.velocity.z -= kGravity * seconds;
    movePlayer(seconds);

    m_state.players[0] = player;
    m_state.acknowledgedInputTick = std::max(
        m_state.acknowledgedInputTick, intent.clientTick);
    ++m_state.tick;
}

void Authority::movePlayer(float seconds) {
    auto& player = m_state.localPlayer;
    player.position += player.velocity * seconds;
    player.position.x = std::clamp(player.position.x, kPlayerRadius,
                                   static_cast<float>(kWorldEdge) - kPlayerRadius);
    player.position.y = std::clamp(player.position.y, kPlayerRadius,
                                   static_cast<float>(kWorldEdge) - kPlayerRadius);
    const auto solid = [&](int x, int y, int z) {
        if (x < 0 || y < 0 || z < 0 || x >= static_cast<int>(kWorldEdge) ||
            y >= static_cast<int>(kWorldEdge) || z >= static_cast<int>(kWorldEdge)) return true;
        return m_state.world[static_cast<std::size_t>(x) +
                             kWorldEdge * (static_cast<std::size_t>(y) +
                             kWorldEdge * static_cast<std::size_t>(z))].material != 0;
    };
    const auto collides = [&](glm::vec3 position) {
        const int minX = static_cast<int>(std::floor(position.x - kPlayerRadius));
        const int maxX = static_cast<int>(std::floor(position.x + kPlayerRadius));
        const int minY = static_cast<int>(std::floor(position.y - kPlayerRadius));
        const int maxY = static_cast<int>(std::floor(position.y + kPlayerRadius));
        const int minZ = static_cast<int>(std::floor(position.z));
        const int maxZ = static_cast<int>(
            std::ceil(position.z + kPlayerHeight) - 1.0f);
        for (int z = minZ; z <= maxZ; ++z)
            for (int y = minY; y <= maxY; ++y)
                for (int x = minX; x <= maxX; ++x)
                    if (solid(x, y, z)) return true;
        return false;
    };
    const glm::vec3 previous = player.position - player.velocity * seconds;
    glm::vec3 resolved = previous;
    resolved.x = player.position.x;
    if (collides(resolved)) {
        resolved.x = previous.x;
        player.velocity.x = 0.0f;
    }
    resolved.y = player.position.y;
    if (collides(resolved)) {
        resolved.y = previous.y;
        player.velocity.y = 0.0f;
    }
    resolved.z = player.position.z;
    if (collides(resolved)) {
        resolved.z = previous.z;
        player.velocity.z = 0.0f;
        player.grounded = player.position.z < previous.z;
    } else {
        player.grounded = false;
    }
    player.position = resolved;
}

} // namespace voxel
