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
inline constexpr std::uint16_t kProtocol = 2;

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
};

std::vector<std::byte> encode(const Registration& message);
std::vector<std::byte> encode(const RegistrationAck& message);
std::vector<std::byte> encode(const InputIntent& message);
std::vector<std::byte> encode(const AuthoritativeState& message);
std::vector<std::byte> encode(const WorldDescriptor& message);
std::vector<std::byte> encode(const WorldInterest& message);
std::vector<std::byte> encode(const WorldAck& message);
std::vector<std::byte> encode(const BrickSnapshot& message);

bool decode(std::span<const std::byte> bytes, Registration& message);
bool decode(std::span<const std::byte> bytes, RegistrationAck& message);
bool decode(std::span<const std::byte> bytes, InputIntent& message);
bool decode(std::span<const std::byte> bytes, AuthoritativeState& message);
bool decode(std::span<const std::byte> bytes, WorldDescriptor& message);
bool decode(std::span<const std::byte> bytes, WorldInterest& message);
bool decode(std::span<const std::byte> bytes, WorldAck& message);
bool decode(std::span<const std::byte> bytes, BrickSnapshot& message);

} // namespace voxel::net
