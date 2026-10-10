#pragma once

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include <cmath>

namespace voxel {

inline constexpr std::uint32_t kTickRate = 60;
inline constexpr float kTickSeconds = 1.0f / static_cast<float>(kTickRate);
inline constexpr std::uint32_t kWorldEdge = 32;
inline constexpr std::size_t kVoxelCount =
    static_cast<std::size_t>(kWorldEdge) * kWorldEdge * kWorldEdge;

struct Voxel {
    std::uint16_t material = 0;
    glm::u8vec4 color{0, 0, 0, 0};
};

using VoxelWorld = std::array<Voxel, kVoxelCount>;

inline VoxelWorld makeEmptyWorld() {
    return {};
}

inline VoxelWorld makeTerrain() {
    VoxelWorld world{};
    constexpr Voxel stone{1, {128, 128, 128, 255}};
    for (std::uint32_t y = 0; y < kWorldEdge; ++y) {
        for (std::uint32_t x = 0; x < kWorldEdge; ++x) {
            const float height = 5.0f
                + 2.5f * std::sin(static_cast<float>(x) * 0.42f)
                + 2.0f * std::sin(static_cast<float>(y) * 0.31f + 0.8f);
            const auto top = static_cast<std::uint32_t>(
                std::clamp(height, 1.0f, static_cast<float>(kWorldEdge - 1)));
            for (std::uint32_t z = 0; z <= top; ++z) {
                world[x + kWorldEdge * (y + kWorldEdge * z)] = stone;
            }
        }
    }
    return world;
}

struct InputIntent {
    std::uint32_t clientTick = 0;
    glm::vec2 move{0.0f};
    glm::vec2 horizontalDelta{0.0f};
    glm::vec2 look{0.0f};
    bool jumpPressed = false;
};

struct PlayerState {
    std::uint32_t id = 0;
    glm::vec3 position{16.0f, 16.0f, 8.0f};
    glm::vec3 velocity{0.0f};
    float yaw = -90.0f;
    float pitch = 0.0f;
    bool grounded = true;
};

struct AuthoritativeState {
    std::uint32_t tick = 0;
    std::uint32_t acknowledgedInputTick = 0;
    std::uint32_t worldRevision = 0;
    PlayerState localPlayer{};
    std::array<PlayerState, 16> players{};
    std::uint8_t playerCount = 0;
    VoxelWorld world = makeEmptyWorld();
};

struct RenderState {
    std::uint32_t tick = 0;
    std::uint32_t worldRevision = 0;
    PlayerState localPlayer{};
    VoxelWorld world = makeEmptyWorld();
};

struct VoxelChange {
    std::uint32_t tick = 0;
    std::uint32_t revision = 0;
    std::uint32_t index = 0;
    Voxel value{};
};

struct WorldChunk {
    std::uint32_t revision = 0;
    std::uint32_t offset = 0;
    std::vector<Voxel> values;
};

} // namespace voxel
