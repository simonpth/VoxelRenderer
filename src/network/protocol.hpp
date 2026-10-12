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
inline constexpr std::uint16_t kProtocol = 4;

inline constexpr std::uint32_t kMaxBrickNodes = 1u << 24;   // firstChild width
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
struct WorldAck {
    ClientUuid uuid{};
    std::uint32_t playerId = 0;
    std::uint32_t rootIndex = 0;
    std::uint32_t revision = 0;
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

// nodes[0] is the brick root and becomes the client's root table entry.
struct BrickSnapshot {
    ClientUuid uuid{};
    std::uint32_t playerId = 0;
    std::uint32_t rootIndex = 0;
    std::uint32_t revision = 0;
    std::uint32_t worldRevision = 0;
    std::uint8_t lodLevel = 0;
    std::vector<SvoNode> nodes;
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
