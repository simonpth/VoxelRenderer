#pragma once

#include <cstring>
#include <array>
#include <span>
#include <vector>
#include "address.hpp"
#include "../simulation/state.hpp"
#include "../simulation/svo.hpp"

namespace voxel::net {

inline constexpr std::uint32_t kMagic = 0x56584C31;
inline constexpr std::uint16_t kProtocol = 1;
using ClientUuid = std::array<std::uint8_t, 16>;
struct Registration {
    ClientUuid uuid{};
};
struct RegistrationAck {
    ClientUuid uuid{};
    std::uint32_t playerId = 0;
};
struct RegionSnapshot {
    RegionCoord coordinate{};
    std::uint32_t revision = 0;
    std::uint32_t worldRevision = 0;
    std::vector<SvoNode> nodes;
    std::uint8_t depth = kRegionDepth;
};
struct RegionInterestRequest {
    ClientUuid uuid{};
    RegionCoord center{};
    RegionCoord region{};
    std::uint8_t radius = 1;
};

enum class PacketKind : std::uint8_t {
    Intent = 1, Snapshot = 2, Voxel = 3, WorldChunk = 4,
    Registration = 5, RegistrationAck = 6, RegionSnapshot = 7,
    RegionInterest = 8
};

std::vector<std::byte> encode(const Registration& registration);
std::vector<std::byte> encode(const RegistrationAck& acknowledgement);
std::vector<std::byte> encode(const InputIntent& intent);
std::vector<std::byte> encode(const AuthoritativeState& state);
std::vector<std::byte> encode(const VoxelChange& change);
std::vector<std::byte> encode(const WorldChunk& chunk);
std::vector<std::byte> encode(const RegionSnapshot& snapshot);
std::vector<std::byte> encode(const RegionInterestRequest& request);
bool decode(std::span<const std::byte> bytes, Registration& registration);
bool decode(std::span<const std::byte> bytes, RegistrationAck& acknowledgement);
bool decode(std::span<const std::byte> bytes, InputIntent& intent);
bool decode(std::span<const std::byte> bytes, AuthoritativeState& state);
bool decode(std::span<const std::byte> bytes, VoxelChange& change);
bool decode(std::span<const std::byte> bytes, WorldChunk& chunk);
bool decode(std::span<const std::byte> bytes, RegionSnapshot& snapshot);
bool decode(std::span<const std::byte> bytes, RegionInterestRequest& request);

} // namespace voxel::net
