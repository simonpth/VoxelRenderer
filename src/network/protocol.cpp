#include "protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <bit>
#include <limits>

namespace voxel::net {
namespace {

void u16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(std::byte(value));
    out.push_back(std::byte(value >> 8));
}
void u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(std::byte(value >> shift));
}
void u64(std::vector<std::byte>& out, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        out.push_back(std::byte(value >> shift));
}
void f32(std::vector<std::byte>& out, float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    u32(out, bits);
}
void bytes(std::vector<std::byte>& out, const void* data, std::size_t size) {
    const auto* source = static_cast<const std::byte*>(data);
    out.insert(out.end(), source, source + size);
}
void header(std::vector<std::byte>& out, PacketKind kind) {
    u32(out, kMagic);
    u16(out, kProtocol);
    out.push_back(std::byte(kind));
}
void uuid(std::vector<std::byte>& out, const ClientUuid& value) {
    bytes(out, value.data(), value.size());
}

bool u16(std::span<const std::byte> in, std::size_t& at, std::uint16_t& value) {
    if (in.size() - at < 2) return false;
    value = std::to_integer<std::uint16_t>(in[at]) |
            (std::to_integer<std::uint16_t>(in[at + 1]) << 8);
    at += 2;
    return true;
}
bool u32(std::span<const std::byte> in, std::size_t& at, std::uint32_t& value) {
    if (in.size() - at < 4) return false;
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        value |= std::to_integer<std::uint32_t>(in[at++]) << shift;
    return true;
}
bool u64(std::span<const std::byte> in, std::size_t& at, std::uint64_t& value) {
    if (in.size() - at < 8) return false;
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8)
        value |= std::to_integer<std::uint64_t>(in[at++]) << shift;
    return true;
}
bool f32(std::span<const std::byte> in, std::size_t& at, float& value) {
    std::uint32_t bits;
    if (!u32(in, at, bits)) return false;
    std::memcpy(&value, &bits, sizeof value);
    return std::isfinite(value);
}
bool read(std::span<const std::byte> in, std::size_t& at, void* out,
          std::size_t size) {
    if (in.size() - at < size) return false;
    std::memcpy(out, in.data() + at, size);
    at += size;
    return true;
}
bool uuid(std::span<const std::byte> in, std::size_t& at, ClientUuid& value) {
    return read(in, at, value.data(), value.size());
}
bool begin(std::span<const std::byte> in, std::size_t& at, PacketKind expected) {
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    if (!u32(in, at, magic) || !u16(in, at, version) || at >= in.size())
        return false;
    return magic == kMagic && version == kProtocol &&
           in[at++] == std::byte(expected);
}
void vec2(std::vector<std::byte>& out, glm::vec2 value) {
    f32(out, value.x);
    f32(out, value.y);
}
void vec3(std::vector<std::byte>& out, glm::vec3 value) {
    f32(out, value.x);
    f32(out, value.y);
    f32(out, value.z);
}
void player(std::vector<std::byte>& out, const PlayerState& value) {
    u32(out, value.id);
    vec3(out, value.position);
    vec3(out, value.velocity);
    f32(out, value.yaw);
    f32(out, value.pitch);
    out.push_back(std::byte(value.grounded ? 1 : 0));
}
bool vec2(std::span<const std::byte> in, std::size_t& at, glm::vec2& value) {
    return f32(in, at, value.x) && f32(in, at, value.y);
}
bool vec3(std::span<const std::byte> in, std::size_t& at, glm::vec3& value) {
    return f32(in, at, value.x) && f32(in, at, value.y) && f32(in, at, value.z);
}
bool player(std::span<const std::byte> in, std::size_t& at, PlayerState& value) {
    std::byte grounded = std::byte(0);
    if (!u32(in, at, value.id) || !vec3(in, at, value.position) ||
        !vec3(in, at, value.velocity) || !f32(in, at, value.yaw) ||
        !f32(in, at, value.pitch) || !read(in, at, &grounded, 1))
        return false;
    value.grounded = std::to_integer<unsigned>(grounded) != 0;
    return true;
}

} // namespace

std::vector<std::byte> encode(const Registration& message) {
    std::vector<std::byte> out;
    header(out, PacketKind::Registration);
    uuid(out, message.uuid);
    return out;
}

std::vector<std::byte> encode(const RegistrationAck& message) {
    std::vector<std::byte> out;
    header(out, PacketKind::RegistrationAck);
    uuid(out, message.uuid);
    u32(out, message.playerId);
    return out;
}

std::vector<std::byte> encode(const InputIntent& message) {
    std::vector<std::byte> out;
    header(out, PacketKind::Intent);
    u32(out, message.clientTick);
    vec2(out, message.move);
    vec2(out, message.look);
    out.push_back(std::byte(message.jumpPressed ? 1 : 0));
    return out;
}

std::vector<std::byte> encode(const AuthoritativeState& message) {
    std::vector<std::byte> out;
    header(out, PacketKind::Snapshot);
    u32(out, message.tick);
    u32(out, message.acknowledgedInputTick);
    u32(out, message.worldRevision);
    player(out, message.localPlayer);
    out.push_back(std::byte(message.playerCount));
    for (std::uint8_t i = 0; i < message.playerCount; ++i)
        player(out, message.players[i]);
    return out;
}

std::vector<std::byte> encode(const WorldDescriptor& message) {
    std::vector<std::byte> out;
    header(out, PacketKind::WorldDescriptor);
    u32(out, message.sizeX);
    u32(out, message.sizeY);
    u32(out, message.sizeZ);
    u32(out, message.brick);
    u32(out, message.rootDimX);
    u32(out, message.rootDimY);
    u32(out, message.rootDimZ);
    u32(out, message.rootCount);
    u64(out, message.seed);
    const auto count = static_cast<std::uint16_t>(
        std::min<std::size_t>(message.materials.size(), 65535));
    u16(out, count);
    for (std::uint16_t i = 0; i < count; ++i) {
        f32(out, message.materials[i].r);
        f32(out, message.materials[i].g);
        f32(out, message.materials[i].b);
        f32(out, message.materials[i].a);
        u32(out, message.materials[i].flags);
    }
    return out;
}

std::vector<std::byte> encode(const WorldInterest& message) {
    std::vector<std::byte> out;
    header(out, PacketKind::WorldInterest);
    uuid(out, message.uuid);
    u32(out, message.playerId);
    u32(out, static_cast<std::uint32_t>(message.centerX));
    u32(out, static_cast<std::uint32_t>(message.centerY));
    u32(out, static_cast<std::uint32_t>(message.centerZ));
    out.push_back(std::byte(message.radius));
    return out;
}

std::vector<std::byte> encode(const WorldAck& message) {
    std::vector<std::byte> out;
    header(out, PacketKind::WorldAck);
    uuid(out, message.uuid);
    u32(out, message.playerId);
    u32(out, message.rootIndex);
    u32(out, message.revision);
    return out;
}

std::vector<std::byte> encode(const BrickSnapshot& message) {
    if (message.nodes.empty() || message.nodes.size() > kMaxBrickNodes) return {};
    std::vector<std::byte> out;
    header(out, PacketKind::BrickSnapshot);
    uuid(out, message.uuid);
    u32(out, message.playerId);
    u32(out, message.rootIndex);
    u32(out, message.revision);
    u32(out, message.worldRevision);
    out.push_back(std::byte(message.lodLevel));
    u32(out, static_cast<std::uint32_t>(message.nodes.size()));
    bytes(out, message.nodes.data(), message.nodes.size() * sizeof(SvoNode));
    return out;
}

// --- decode ---------------------------------------------------------------

bool decode(std::span<const std::byte> in, Registration& message) {
    std::size_t at = 0;
    return begin(in, at, PacketKind::Registration) &&
           uuid(in, at, message.uuid) && at == in.size();
}

bool decode(std::span<const std::byte> in, RegistrationAck& message) {
    std::size_t at = 0;
    return begin(in, at, PacketKind::RegistrationAck) &&
           uuid(in, at, message.uuid) && u32(in, at, message.playerId) &&
           at == in.size();
}

bool decode(std::span<const std::byte> in, InputIntent& message) {
    std::size_t at = 0;
    std::byte jump = std::byte(0);
    if (!begin(in, at, PacketKind::Intent) || !u32(in, at, message.clientTick) ||
        !vec2(in, at, message.move) || !vec2(in, at, message.look) ||
        !read(in, at, &jump, 1))
        return false;
    message.jumpPressed = std::to_integer<unsigned>(jump) != 0;
    return at == in.size() &&
           std::abs(message.move.x) <= 1.0f && std::abs(message.move.y) <= 1.0f &&
           std::abs(message.look.x) <= 0.5f && std::abs(message.look.y) <= 0.5f;
}

bool decode(std::span<const std::byte> in, AuthoritativeState& message) {
    std::size_t at = 0;
    if (!begin(in, at, PacketKind::Snapshot) ||
        !u32(in, at, message.tick) ||
        !u32(in, at, message.acknowledgedInputTick) ||
        !u32(in, at, message.worldRevision) ||
        !player(in, at, message.localPlayer) || at >= in.size())
        return false;
    const auto count = std::to_integer<std::uint8_t>(in[at++]);
    if (count > message.players.size()) return false;
    message.playerCount = count;
    for (std::uint8_t i = 0; i < count; ++i)
        if (!player(in, at, message.players[i])) return false;
    return at == in.size();
}

bool decode(std::span<const std::byte> in, WorldDescriptor& message) {
    std::size_t at = 0;
    std::uint16_t count = 0;
    if (!begin(in, at, PacketKind::WorldDescriptor) ||
        !u32(in, at, message.sizeX) || !u32(in, at, message.sizeY) ||
        !u32(in, at, message.sizeZ) || !u32(in, at, message.brick) ||
        !u32(in, at, message.rootDimX) || !u32(in, at, message.rootDimY) ||
        !u32(in, at, message.rootDimZ) || !u32(in, at, message.rootCount) ||
        !u64(in, at, message.seed) || !u16(in, at, count))
        return false;
    if (message.brick == 0 || (message.brick & (message.brick - 1)) != 0) return false;
    if (message.sizeX == 0 || message.sizeY == 0 || message.sizeZ == 0) return false;
    if (message.rootDimX * message.rootDimY * message.rootDimZ != message.rootCount)
        return false;
    if (message.rootDimX * message.brick < message.sizeX ||
        message.rootDimY * message.brick < message.sizeY ||
        message.rootDimZ * message.brick < message.sizeZ)
        return false;
    message.materials.resize(count);
    for (auto& material : message.materials) {
        if (!f32(in, at, material.r) || !f32(in, at, material.g) ||
            !f32(in, at, material.b) || !f32(in, at, material.a) ||
            !u32(in, at, material.flags))
            return false;
    }
    return at == in.size();
}

bool decode(std::span<const std::byte> in, WorldInterest& message) {
    std::size_t at = 0;
    std::uint32_t cx = 0, cy = 0, cz = 0;
    if (!begin(in, at, PacketKind::WorldInterest) ||
        !uuid(in, at, message.uuid) || !u32(in, at, message.playerId) ||
        !u32(in, at, cx) || !u32(in, at, cy) || !u32(in, at, cz))
        return false;
    if (at >= in.size()) return false;
    message.centerX = static_cast<std::int32_t>(cx);
    message.centerY = static_cast<std::int32_t>(cy);
    message.centerZ = static_cast<std::int32_t>(cz);
    message.radius = std::to_integer<std::uint8_t>(in[at++]);
    return at == in.size() && message.radius <= kMaxInterestRadius;
}

bool decode(std::span<const std::byte> in, WorldAck& message) {
    std::size_t at = 0;
    return begin(in, at, PacketKind::WorldAck) &&
           uuid(in, at, message.uuid) && u32(in, at, message.playerId) &&
           u32(in, at, message.rootIndex) && u32(in, at, message.revision) &&
           at == in.size();
}

bool decode(std::span<const std::byte> in, BrickSnapshot& message) {
    std::size_t at = 0;
    std::uint32_t count = 0;
    if (!begin(in, at, PacketKind::BrickSnapshot) ||
        !uuid(in, at, message.uuid) || !u32(in, at, message.playerId) ||
        !u32(in, at, message.rootIndex) || !u32(in, at, message.revision) ||
        !u32(in, at, message.worldRevision) || at >= in.size())
        return false;
    message.lodLevel = std::to_integer<std::uint8_t>(in[at++]);
    if (message.lodLevel > 31) return false;
    if (!u32(in, at, count) || count == 0 || count > kMaxBrickNodes) return false;
    if (static_cast<std::size_t>(count) * sizeof(SvoNode) != in.size() - at)
        return false;
    message.nodes.resize(count);
    if (!read(in, at, message.nodes.data(), count * sizeof(SvoNode)))
        return false;
    if (at != in.size()) return false;

    // The root lives at index 0, so every child index must land inside the
    // payload, and a node with children needs room for all of them.
    for (const SvoNode& node : message.nodes) {
        if (!node.hasChildren()) continue;
        const std::uint32_t first = node.firstChild();
        if (first == 0 || first >= count) return false;
        // Children are contiguous and stored in mask order, so the highest
        // stored child sits at first + popcount(mask) - 1.
        if (first + static_cast<std::uint32_t>(
                        std::popcount(static_cast<std::uint8_t>(node.mask))) >
            count)
            return false;
    }
    if (!message.nodes.front().hasChildren() &&
        message.nodes.front().firstChild() != 0)
        return false;
    return true;
}

} // namespace voxel::net
