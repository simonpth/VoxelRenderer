#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "svo_node.hpp"

namespace voxel {

// ---------------------------------------------------------------------------
// Shared simulation state
//
// The server owns every voxel and every player transform. The client only
// ever forwards intent and consumes authoritative or interpolated state.
// ---------------------------------------------------------------------------

inline constexpr std::uint32_t kTickRate = 60;
inline constexpr float kTickSeconds = 1.0f / static_cast<float>(kTickRate);
inline constexpr std::uint32_t kMaxPlayers = 16;

// Raw player intent. It never contains a position, a velocity, or a collision
// result: those belong to the server authority.
struct InputIntent {
    std::uint32_t clientTick = 0;
    glm::vec2 move{0.0f};    // x = strafe right, y = forward, yaw relative
    glm::vec2 look{0.0f};    // radians applied for this tick
    bool jumpPressed = false;
};

// Feet position; one voxel is one world unit.
struct PlayerState {
    std::uint32_t id = 0;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool grounded = false;
};// Authoritative snapshot published by the server over UDP.
struct AuthoritativeState {
    std::uint32_t tick = 0;
    std::uint32_t acknowledgedInputTick = 0;
    std::uint32_t worldRevision = 0;
    PlayerState localPlayer{};
    std::array<PlayerState, kMaxPlayers> players{};
    std::uint8_t playerCount = 0;
};

// ---------------------------------------------------------------------------
// Material table
//
// Every voxel stores a 16-bit material id. Colour and behaviour live in the
// material table, which the server defines and streams once per connection.
// ---------------------------------------------------------------------------

inline constexpr std::uint32_t kMaterialSolid = 1u << 0;
inline constexpr std::uint32_t kMaterialEmissive = 1u << 1;

#pragma pack(push, 1)
struct Material {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
    std::uint32_t flags = 0;
};
#pragma pack(pop)
static_assert(sizeof(Material) == 20);

inline constexpr std::uint16_t kMaterialCount = 6;

// ---------------------------------------------------------------------------
// Client -> renderer data flow
//
// The client keeps no CPU-side voxel world. Everything the renderer needs is
// either interpolated transform state or a transient upload queue that the
// renderer drains exactly once and then discards.
// ---------------------------------------------------------------------------

struct BrickUpdate {
    // Index into the world's N-ary root layer. The renderer addresses the
    // root table directly with it and needs nothing else to place a brick.
    std::uint32_t rootIndex = 0;
    std::uint32_t revision = 0;
    // Transient payload: index 0 is the brick root, the rest its subtree.
    std::vector<SvoNode> nodes;
    bool remove = false;
};

struct RenderState {
    // Geometry of the world the server sent. Non-zero once the descriptor has
    // been received; the renderer draws nothing meaningful before that.
    std::uint32_t sizeX = 0;
    std::uint32_t sizeY = 0;
    std::uint32_t sizeZ = 0;
    std::uint32_t brick = 0;
    std::uint32_t rootDimX = 0;
    std::uint32_t rootDimY = 0;
    std::uint32_t rootDimZ = 0;
    std::uint32_t rootCount = 0;

    // Uploaded once from the server descriptor, then cleared: the renderer
    // owns the GPU copy from then on.
    std::vector<Material> materials;

    PlayerState localPlayer{};
    std::vector<BrickUpdate> bricks;
};

} // namespace voxel
