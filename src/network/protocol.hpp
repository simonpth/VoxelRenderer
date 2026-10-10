#pragma once

#include <cstring>
#include <span>
#include <vector>
#include "address.hpp"
#include "../simulation/state.hpp"

namespace voxel::net {

inline constexpr std::uint32_t kMagic = 0x56584C31;
inline constexpr std::uint16_t kProtocol = 1;
enum class PacketKind : std::uint8_t { Intent = 1, Snapshot = 2, Voxel = 3, WorldChunk = 4 };

std::vector<std::byte> encode(const InputIntent& intent);
std::vector<std::byte> encode(const AuthoritativeState& state);
std::vector<std::byte> encode(const VoxelChange& change);
std::vector<std::byte> encode(const WorldChunk& chunk);
bool decode(std::span<const std::byte> bytes, InputIntent& intent);
bool decode(std::span<const std::byte> bytes, AuthoritativeState& state);
bool decode(std::span<const std::byte> bytes, VoxelChange& change);
bool decode(std::span<const std::byte> bytes, WorldChunk& chunk);

} // namespace voxel::net
