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

// Look deltas are carried in degrees, because yaw and pitch are degrees
// everywhere on the wire and in the renderer. The bound is per intent: a fast
// mouse flick has to survive the tick, but a corrupt value must not.
inline constexpr float kMaxLookDegrees = 45.0f;

// Raw player intent. It never contains a position, a velocity, or a collision
// result: those belong to the server authority.
// Client-local raw keys, as read from the keyboard. This never reaches the
// wire: `Client::submit` resolves it into an `InputIntent::moveDirection`
// against the camera the player is actually looking through. Kept separate so
// the distinction between "what is pressed" and "what was asked of the
// authority" is visible in the type rather than in a comment.
struct RawMoveInput {
    glm::vec2 axes{0.0f};   // x = strafe right, y = forward, both -1..1
};

struct InputIntent {
    std::uint32_t clientTick = 0;
    // Desired direction of travel, in world units, on the x/y ground plane.
    //
    // This is a resolved direction, not raw WASD. The client is the only party
    // that knows which way the screen is facing, so it resolves the two there
    // and sends the answer. The authority used to rebuild the same camera basis
    // from `yaw` and resolve the keys itself, which put two copies of that
    // basis in the codebase -- one here, one in the renderer -- and made the
    // keys resolve against whichever yaw the server last replicated. Between
    // snapshots that is not the yaw the player is looking along, so W walked
    // off in a direction the screen disagreed with. Sending the direction
    // removes the duplication entirely: there is now one basis, the one the
    // renderer draws with.
    //
    // Magnitude is the analogue of a key being held harder or softer: 1 is full
    // speed, 0 is standing still. It is clamped on arrival, so a client cannot
    // buy speed by sending a longer vector.
    glm::vec2 moveDirection{0.0f};
    // Raw keys, set only by the local caller and never encoded. Resolved into
    // `moveDirection` on the way out.
    RawMoveInput rawMove{};
    glm::vec2 look{0.0f};    // degrees applied for this tick
    bool jumpPressed = false;
    // Flight: shift descends, and `flying` is the mode the client is asking
    // for. The authority owns the applied state and replicates it back, so a
    // client can request a mode but can never place itself anywhere.
    bool descendPressed = false;
    bool flying = false;
};

// Feet position; one voxel is one world unit. Yaw and pitch are degrees.
struct PlayerState {
    std::uint32_t id = 0;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool grounded = false;
    // Authority-owned flight mode, replicated so the client HUD cannot lie
    // about a state it does not own.
    bool flying = false;
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

// Which brick, at which level of its pyramid.
//
// A brick is resident *at a level*, not as a whole. The visible detail under the
// player's feet is the exact octree at level 0; the terrain at the horizon is
// an averaged grid at level 5 or 6. Those are different pieces of data that
// arrive separately, are evicted separately, and must be acknowledged
// separately -- keying them on the root index alone would let a level-0
// delivery be read as covering the coarse one, and the coarse one would then be
// either never acknowledged or never refined.
struct BrickRef {
    std::uint32_t rootIndex = 0;
    std::uint8_t level = 0;
};

struct BrickUpdate {
    // Index into the world's N-ary root layer. The renderer addresses the
    // root table directly with it and needs nothing else to place a brick.
    std::uint32_t rootIndex = 0;
    std::uint32_t revision = 0;
    // Which level of this brick the payload holds. Level 0 is the exact
    // octree -- a brick's own full resolution is log2(brick edge) -- so
    // anything above 0 is an averaged grid and `cells` is the payload.
    std::uint8_t lodLevel = 0;
    // Transient payload for level 0: index 0 is the brick root, the rest its
    // subtree.
    //
    // Ownership is transferred, never shared. See Client::drainBricks() for
    // why this vector must have exactly one owner at a time.
    std::vector<SvoNode> nodes;
    // Transient payload for levels 1 and above: a regular grid of averaged
    // colour, `edge` cells per side, x fastest. There is no alpha -- a cell
    // either has a colour or is empty -- so this needs no blending and no
    // ordering against the octree below it.
    //
    // `edge == 0` is the single-colour case and `cells` holds one cell; see
    // BrickLevel in protocol.hpp. Carried separately rather than derived from
    // the cell count, because two different levels can both reduce to a single
    // cell and the receiver has no way to tell them apart from the size.
    std::uint32_t edge = 0;
    std::vector<AvgCell> cells;
    bool remove = false;
    bool uploaded = false;
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
    // The renderer's upload queue for this frame. Filled by
    // Client::drainBricks() immediately before the draw and emptied by
    // VulkanContext::uploadWorld() during it, so it is main-thread only for
    // its whole lifetime. The world stream never touches this vector; it has
    // its own, which is what makes the handover a swap rather than a race.
    std::vector<BrickUpdate> bricks;

    // The neighbourhood the client wants resident, in world-cell coordinates,
    // plus the streaming radius. Published so the renderer can order eviction:
    // a brick inside this window is never the first thing thrown away to make
    // room for a brick from across the world.
    //
    // Without it the arena fills by recency alone, and a progressive fill
    // walking outward slowly replaces the ground under the player's feet with
    // scenery they cannot see. The window is the client's decision, not the
    // renderer's, so it is stated here rather than recomputed.
    std::int32_t interestX = 0;
    std::int32_t interestY = 0;
    std::int32_t interestZ = 0;
    std::int32_t interestRadius = 0;

    // Bricks the renderer placed in the GPU pool this frame, one entry per
    // level. Drained and cleared by the client each frame, and this is the
    // *only* thing that produces an acknowledgement: a brick that was merely
    // received, or that the pool refused, never appears here and so stays
    // outstanding.
    std::vector<BrickRef> uploadedRefs;

    // Every brick level the renderer finished with this frame, uploaded or not.
    // The client needs this to tell "still queued" from "handed over and
    // dropped": a refused brick must leave the client's outstanding set so the
    // server's next resend is accepted rather than mistaken for a duplicate.
    // Without it, refusing a brick would wedge it permanently -- the client
    // would wait for an upload that can never arrive and the server would wait
    // for an ack.
    std::vector<BrickRef> processedRefs;

    // Bricks the renderer dropped from the pool to make room for something
    // else. Filled by the renderer, drained by the client. A brick here was
    // uploaded, acknowledged, and then thrown away, so it is *not* an
    // outstanding upload -- and it is also no longer resident, which is the
    // fact that matters: the client has to stop considering it held and tell
    // the server to forget it, or the server's "already delivered" record
    // makes it skip every future resend and the terrain is missing for the
    // rest of the session.
    //
    // This is the difference between a bounded arena and a broken one. An
    // eviction the client never learns about is indistinguishable from a brick
    // that never arrived, except that nothing will ever bring it back.
    //
    // Only level 0 is dropped by name. The coarse levels cost a few hundred
    // bytes each and the renderer keeps every one of them rather than evicting,
    // so an eviction with no level on it can only be the detail brick.
    std::vector<BrickRef> evictedRefs;
};

} // namespace voxel
