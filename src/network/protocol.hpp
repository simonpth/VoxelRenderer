#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "address.hpp"
#include "../simulation/svo_node.hpp"
#include "../simulation/state.hpp"
#include "../simulation/svo.hpp"

namespace voxel::net {

// Wire format. Little-endian, fixed-width, versioned. Everything that carries
// authority or payload is bounds checked on decode.
inline constexpr std::uint32_t kMagic = 0x56584C32u;   // "VXL2"
// 3: InputIntent::move (WASD, yaw-relative) became InputIntent::moveDirection
//    (a world-space direction resolved by the client). The two are the same
//    width on the wire, so an unversioned peer would decode WASD as a
//    direction and walk off at a nonsense angle with no error anywhere -- the
//    version check is the only thing that stops it.
// 4: WorldForget. The client's GPU arena is a fixed budget, so a brick that was
//    delivered and acknowledged can later be evicted to make room. The server
//    must be told, because its "already delivered" record is exactly what makes
//    it skip every future resend of that brick: without this message an
//    eviction is permanent, and the terrain it held is missing for the rest of
//    the session. A v3 peer would silently ignore the frame and never resend,
//    so the version check is load-bearing here too.
// 5: The averaged pyramid. A brick is now streamed at a *level* rather than as
//    one blob: level 0 is the exact voxel octree, levels 1 and above are
//    regular grids of averaged colour. A grid is a fixed, tiny size at coarse
//    levels -- level 5 of a 128-cell brick is 64 cells, 256 bytes, whatever the
//    terrain does -- where a reduced octree of the same volume can be orders of
//    magnitude bigger. The client therefore has to track what it holds per
//    (rootIndex, level), because it can hold several levels of one brick at
//    once and they arrive separately.
//
//    WorldAck carries the level for that reason, and it is the part that makes
//    the version check load-bearing rather than cosmetic: a v4 peer sends an
//    ack keyed on the root index alone, the v5 server books it against level 0,
//    and the coarse grid for that brick is never acknowledged -- so the server
//    resends it forever, and the client's "never a duplicate" rule silently
//    drops every one of those resends.
inline constexpr std::uint16_t kProtocol = 5;

inline constexpr std::uint32_t kMaxBrickNodes = 1u << 24;   // firstChild width
// One averaged grid for the largest brick: 128 cells at level 1 is 262144.
inline constexpr std::uint32_t kMaxLevelCells = 1u << 18;
inline constexpr std::uint8_t kMaxInterestRadius = 16;

using ClientUuid = std::array<std::uint8_t, 16>;

// --- UDP: registration and gameplay ---------------------------------------
struct Registration {
    ClientUuid uuid{};
};
struct RegistrationAck {
    ClientUuid uuid{};
    std::uint32_t playerId = 0;
};

// --- TCP: world stream ----------------------------------------------------
// Sent once per connection. The client cannot generate terrain, only render.
struct WorldDescriptor {
    std::uint32_t sizeX = 0;
    std::uint32_t sizeY = 0;
    std::uint32_t sizeZ = 0;
    std::uint32_t brick = 0;
    std::uint32_t rootDimX = 0;
    std::uint32_t rootDimY = 0;
    std::uint32_t rootDimZ = 0;
    std::uint32_t rootCount = 0;
    std::uint64_t seed = 0;
    std::vector<Material> materials;
};

// Doubles as the stream hello. Says which root-layer neighbourhood the client
// wants and which cell it is standing in.
struct WorldInterest {
    ClientUuid uuid{};
    std::uint32_t playerId = 0;
    std::int32_t centerX = 0;
    std::int32_t centerY = 0;
    std::int32_t centerZ = 0;
    std::uint8_t radius = 1;
};

// Confirms a brick reached the GPU slice, so the server stops resending it.
//
// Carries the level as well as the root index, because a brick can hold several
// pyramid levels at once and they are acknowledged independently -- see the
// kProtocol 5 note.
struct WorldAck {
    ClientUuid uuid{};
    std::uint32_t playerId = 0;
    std::uint32_t rootIndex = 0;
    std::uint32_t revision = 0;
    std::uint8_t lodLevel = 0;
};

// Retracts a delivery the server has already counted. Sent for a brick that
// was evicted from the client's bounded GPU arena while still inside the
// interest window -- a brick that left the window needs no message, because the
// server forgets those on its own when the window moves.
//
// No revision: the whole record goes, whatever revision it names. The server
// resends on its next pass and the client re-uploads, which is the whole point.
struct WorldForget {
    ClientUuid uuid{};
    std::uint32_t playerId = 0;
    std::uint32_t rootIndex = 0;
};

// One averaged pyramid level of a brick: a regular grid, x fastest, holding
// edge^3 cells. Level k of a 128-cell brick is (128 >> k)^3, so level 1 is
// 262144 cells and level 7 is one.
//
// `edge == 0` is a fast path meaning "one colour, repeated to fill the level",
// and `cells` then holds exactly one cell. It exists for the 96% of the world
// that is a uniform brick -- open sky and deep rock. Such a brick stores its
// cell only at the top level, because every level below it is the same value by
// definition, so broadcasting it would otherwise mean fanning a megabyte of one
// colour across level 1. With this, a uniform brick at any level costs four
// bytes.
//
// There is no alpha. A cell either has a colour or is empty, which is what
// makes an averaged level a sampling of the volume above it rather than a lossy
// compression with holes in it: the renderer never has to blend or sort two
// half-transparent levels against each other.
//
// The size is the point. Level 5 of a 128-cell brick is 64 cells, 256 bytes on
// the wire, for any terrain whatsoever -- where the same volume as a reduced
// octree depends on how much detail happens to be in it and is routinely tens
// of kilobytes.
struct BrickLevel {
    std::uint8_t level = 0;
    std::uint32_t edge = 0;
    std::vector<AvgCell> cells;
};

// nodes[0] is the brick root and becomes the client's root table entry.
//
// `nodes` and `levels` are alternatives rather than an addition: a brick sent at
// full detail carries the exact octree, and anything sent reduced carries the
// averaged grid for the level named in `lodLevel`. Neither repeats the other,
// because the shader asks for one at a time.
struct BrickSnapshot {
    ClientUuid uuid{};
    std::uint32_t playerId = 0;
    std::uint32_t rootIndex = 0;
    std::uint32_t revision = 0;
    std::uint32_t worldRevision = 0;
    std::uint8_t lodLevel = 0;
    std::vector<SvoNode> nodes;
    std::vector<BrickLevel> levels;
};

enum class PacketKind : std::uint8_t {
    Registration = 1,
    RegistrationAck = 2,
    Intent = 3,
    Snapshot = 4,
    WorldDescriptor = 5,
    WorldInterest = 6,
    BrickSnapshot = 7,
    WorldAck = 8,
    WorldForget = 9,
};

std::vector<std::byte> encode(const Registration& message);
std::vector<std::byte> encode(const RegistrationAck& message);
std::vector<std::byte> encode(const InputIntent& message);
std::vector<std::byte> encode(const AuthoritativeState& message);
std::vector<std::byte> encode(const WorldDescriptor& message);
std::vector<std::byte> encode(const WorldInterest& message);
std::vector<std::byte> encode(const WorldAck& message);
std::vector<std::byte> encode(const WorldForget& message);
std::vector<std::byte> encode(const BrickSnapshot& message);

bool decode(std::span<const std::byte> bytes, Registration& message);
bool decode(std::span<const std::byte> bytes, RegistrationAck& message);
bool decode(std::span<const std::byte> bytes, InputIntent& message);
bool decode(std::span<const std::byte> bytes, AuthoritativeState& message);
bool decode(std::span<const std::byte> bytes, WorldDescriptor& message);
bool decode(std::span<const std::byte> bytes, WorldInterest& message);
bool decode(std::span<const std::byte> bytes, WorldAck& message);
bool decode(std::span<const std::byte> bytes, WorldForget& message);
bool decode(std::span<const std::byte> bytes, BrickSnapshot& message);

} // namespace voxel::net
