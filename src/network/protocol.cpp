#include "protocol.hpp"
#include <cmath>
#include <iostream>
#include <limits>

namespace voxel::net {
namespace {
void u16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(std::byte(value)); out.push_back(std::byte(value >> 8));
}
void u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(std::byte(value >> shift));
}
void node(std::vector<std::byte>& out, const SvoNode& value) {
    u32(out, value.childMask);
    u32(out, value.firstChild);
    u16(out, value.material);
    u16(out, value.flags);
}
void uuid(std::vector<std::byte>& out, const ClientUuid& value) {
    for (const auto byte : value) out.push_back(std::byte(byte));
}
void f32(std::vector<std::byte>& out, float value) {
    std::uint32_t bits; std::memcpy(&bits, &value, sizeof bits); u32(out, bits);
}
void header(std::vector<std::byte>& out, PacketKind kind) {
    u32(out, kMagic); u16(out, kProtocol); out.push_back(std::byte(kind));
}
bool u16(std::span<const std::byte> in, std::size_t& at, std::uint16_t& value) {
    if (in.size() - at < 2) return false;
    value = std::to_integer<std::uint16_t>(in[at]) |
            (std::to_integer<std::uint16_t>(in[at + 1]) << 8); at += 2; return true;
}
bool u32(std::span<const std::byte> in, std::size_t& at, std::uint32_t& value);
bool node(std::span<const std::byte> in, std::size_t& at, SvoNode& value) {
    return u32(in, at, value.childMask) &&
           u32(in, at, value.firstChild) &&
           u16(in, at, value.material) &&
           u16(in, at, value.flags);
}
bool u32(std::span<const std::byte> in, std::size_t& at, std::uint32_t& value) {
    if (in.size() - at < 4) return false; value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) value |=
        std::to_integer<std::uint32_t>(in[at++]) << shift;
    return true;
}
bool uuid(std::span<const std::byte> in, std::size_t& at, ClientUuid& value) {
    if (in.size() - at < value.size()) return false;
    for (auto& byte : value) byte = std::to_integer<std::uint8_t>(in[at++]);
    return true;
}
bool f32(std::span<const std::byte> in, std::size_t& at, float& value) {
    std::uint32_t bits; if (!u32(in, at, bits)) return false;
    std::memcpy(&value, &bits, sizeof value);
    return std::isfinite(value);
}
bool begin(std::span<const std::byte> in, std::size_t& at, PacketKind expected) {
    std::uint32_t magic; std::uint16_t version;
    if (!u32(in, at, magic) || !u16(in, at, version) || at >= in.size()) return false;
    return magic == kMagic && version == kProtocol && in[at++] == std::byte(expected);
}
void vec2(std::vector<std::byte>& out, glm::vec2 v) { f32(out, v.x); f32(out, v.y); }
void vec3(std::vector<std::byte>& out, glm::vec3 v) { f32(out, v.x); f32(out, v.y); f32(out, v.z); }
void player(std::vector<std::byte>& out, const PlayerState& p) {
    u32(out, p.id); vec3(out, p.position); vec3(out, p.velocity);
    f32(out, p.yaw); f32(out, p.pitch); out.push_back(std::byte(p.grounded));
}
bool vec2(std::span<const std::byte> in, std::size_t& at, glm::vec2& v) { return f32(in, at, v.x) && f32(in, at, v.y); }
bool vec3(std::span<const std::byte> in, std::size_t& at, glm::vec3& v) { return f32(in, at, v.x) && f32(in, at, v.y) && f32(in, at, v.z); }
bool player(std::span<const std::byte> in, std::size_t& at, PlayerState& p) {
    return u32(in, at, p.id) && vec3(in, at, p.position) && vec3(in, at, p.velocity) &&
           f32(in, at, p.yaw) && f32(in, at, p.pitch) && at < in.size() &&
           ((p.grounded = std::to_integer<unsigned>(in[at++]) != 0), true);
}
}

std::vector<std::byte> encode(const InputIntent& i) {
    std::vector<std::byte> out; header(out, PacketKind::Intent); u32(out, i.clientTick);
    vec2(out, i.horizontalDelta); vec2(out, i.look);
    out.push_back(std::byte(i.jumpPressed)); return out;
}
std::vector<std::byte> encode(const Registration& registration) {
    std::vector<std::byte> out; header(out, PacketKind::Registration);
    uuid(out, registration.uuid); return out;
}
std::vector<std::byte> encode(const RegistrationAck& acknowledgement) {
    std::vector<std::byte> out; header(out, PacketKind::RegistrationAck);
    uuid(out, acknowledgement.uuid); u32(out, acknowledgement.playerId);
    return out;
}
std::vector<std::byte> encode(const AuthoritativeState& s) {
    std::vector<std::byte> out; header(out, PacketKind::Snapshot);
    u32(out, s.tick); u32(out, s.worldRevision); u32(out, s.acknowledgedInputTick);
    player(out, s.localPlayer);
    out.push_back(std::byte(s.playerCount));
    for (std::uint8_t i = 0; i < s.playerCount; ++i) player(out, s.players[i]);
    return out;
}
std::vector<std::byte> encode(const VoxelChange& c) {
    std::vector<std::byte> out; header(out, PacketKind::Voxel); u32(out, c.tick);
    u32(out, c.revision); u32(out, c.index); u16(out, c.value.material);
    out.insert(out.end(), reinterpret_cast<const std::byte*>(&c.value.color),
               reinterpret_cast<const std::byte*>(&c.value.color) + sizeof c.value.color);
    return out;
}
std::vector<std::byte> encode(const WorldChunk& c) {
    std::vector<std::byte> out; header(out, PacketKind::WorldChunk);
    u32(out, c.revision); u32(out, c.offset); u16(out, static_cast<std::uint16_t>(c.values.size()));
    for (const Voxel& voxel : c.values) {
        u16(out, voxel.material);
        out.insert(out.end(), reinterpret_cast<const std::byte*>(&voxel.color),
                   reinterpret_cast<const std::byte*>(&voxel.color) + sizeof voxel.color);
    }
        return out;
    }
    std::vector<std::byte> encode(const RegionSnapshot& s) {
        // Three packed GPU words per node must fit inside the TCP frame
        // budget. The old one-million-node limit silently dropped valid
        // full-resolution 1024^3 regions.
        constexpr std::size_t kMaxNodes = 5'000'000;
        if (s.nodes.empty() || s.nodes.size() > kMaxNodes) {
            std::cerr << "Network: refusing SVO snapshot with "
                      << s.nodes.size() << " nodes\n";
            return {};
        }
        std::vector<std::byte> out;
        header(out, PacketKind::RegionSnapshot);
        u16(out, s.coordinate.x);
        u16(out, s.coordinate.y);
        u16(out, s.coordinate.z);
        out.push_back(std::byte(s.depth));
        u32(out, s.revision);
        u32(out, s.worldRevision);
        u32(out, static_cast<std::uint32_t>(s.nodes.size()));
        for (const auto& value : s.nodes) node(out, value);
        return out;
    }
    std::vector<std::byte> encode(const RegionInterestRequest& request) {
        std::vector<std::byte> out;
        header(out, PacketKind::RegionInterest);
        uuid(out, request.uuid);
        u16(out, request.center.x);
        u16(out, request.center.y);
        u16(out, request.center.z);
        u16(out, request.region.x);
        u16(out, request.region.y);
        u16(out, request.region.z);
        out.push_back(std::byte(request.radius));
        return out;
    }
bool decode(std::span<const std::byte> in, InputIntent& i) {
    std::size_t at = 0;
    return begin(in, at, PacketKind::Intent) && u32(in, at, i.clientTick) &&
           vec2(in, at, i.horizontalDelta) && vec2(in, at, i.look) && at < in.size() &&
           ((i.jumpPressed = std::to_integer<unsigned>(in[at++]) != 0), at == in.size());
}
bool decode(std::span<const std::byte> in, Registration& registration) {
    std::size_t at = 0;
    return begin(in, at, PacketKind::Registration) &&
           uuid(in, at, registration.uuid) && at == in.size();
}
bool decode(std::span<const std::byte> in, RegistrationAck& acknowledgement) {
    std::size_t at = 0;
    return begin(in, at, PacketKind::RegistrationAck) &&
           uuid(in, at, acknowledgement.uuid) &&
           u32(in, at, acknowledgement.playerId) && at == in.size();
}
bool decode(std::span<const std::byte> in, AuthoritativeState& s) {
    std::size_t at = 0; if (!begin(in, at, PacketKind::Snapshot) ||
        !u32(in, at, s.tick) || !u32(in, at, s.worldRevision) ||
        !u32(in, at, s.acknowledgedInputTick) ||
        !player(in, at, s.localPlayer) || at >= in.size()) return false;
    s.playerCount = std::to_integer<std::uint8_t>(in[at++]);
    if (s.playerCount > s.players.size()) return false;
    for (std::uint8_t i = 0; i < s.playerCount; ++i) if (!player(in, at, s.players[i])) return false;
    return at == in.size();
}
bool decode(std::span<const std::byte> in, VoxelChange& c) {
    std::size_t at = 0; if (!begin(in, at, PacketKind::Voxel) ||
        !u32(in, at, c.tick) || !u32(in, at, c.revision) || !u32(in, at, c.index) ||
        c.index >= kVoxelCount || !u16(in, at, c.value.material) || in.size() - at != sizeof c.value.color) return false;
    std::memcpy(&c.value.color, in.data() + at, sizeof c.value.color); return true;
}
bool decode(std::span<const std::byte> in, WorldChunk& c) {
    std::size_t at = 0; std::uint16_t count = 0;
    if (!begin(in, at, PacketKind::WorldChunk) ||
        !u32(in, at, c.revision) || !u32(in, at, c.offset) ||
        !u16(in, at, count) || count == 0 || count > 64 ||
        c.offset >= kVoxelCount || c.offset + count > kVoxelCount) return false;
    c.values.resize(count);
    for (Voxel& voxel : c.values) {
        if (!u16(in, at, voxel.material) || in.size() - at < sizeof voxel.color) return false;
        std::memcpy(&voxel.color, in.data() + at, sizeof voxel.color);
        at += sizeof voxel.color;
    }
    return at == in.size();
}
bool decode(std::span<const std::byte> in, RegionSnapshot& s) {
    constexpr std::uint32_t kMaxNodes = 5'000'000;
    std::size_t at = 0;
    std::uint16_t x = 0, y = 0, z = 0;
    std::uint32_t count = 0;
    if (!begin(in, at, PacketKind::RegionSnapshot) ||
        !u16(in, at, x) || !u16(in, at, y) || !u16(in, at, z) ||
        at >= in.size() || (s.depth = std::to_integer<std::uint8_t>(in[at++])) == 0 ||
        s.depth > kRegionDepth ||
        !u32(in, at, s.revision) || !u32(in, at, s.worldRevision) ||
        !u32(in, at, count) || count == 0 || count > kMaxNodes ||
        count > (in.size() - at) / 12) return false;
    if (x >= kRegionCountX || y >= kRegionCountY || z >= kRegionCountZ)
        return false;
    s.coordinate = RegionCoord{x, y, z};
    s.nodes.resize(count);
    for (auto& value : s.nodes)
        if (!node(in, at, value)) return false;
    if (s.nodes.front().firstChild >= s.nodes.size() && s.nodes.front().childMask != 0)
        return false;
    for (const auto& value : s.nodes) {
        if (value.childMask != 0 &&
            (s.nodes.size() < 8 || value.firstChild > s.nodes.size() - 8))
            return false;
    }
    return at == in.size();
}
bool decode(std::span<const std::byte> in, RegionInterestRequest& request) {
    std::size_t at = 0;
    if (!begin(in, at, PacketKind::RegionInterest) ||
        !uuid(in, at, request.uuid) ||
        !u16(in, at, request.center.x) ||
        !u16(in, at, request.center.y) ||
        !u16(in, at, request.center.z) ||
        !u16(in, at, request.region.x) ||
        !u16(in, at, request.region.y) ||
        !u16(in, at, request.region.z) ||
        at >= in.size()) return false;
    request.radius = std::to_integer<std::uint8_t>(in[at++]);
    return at == in.size() &&
           request.center.x < kRegionCountX &&
           request.center.y < kRegionCountY &&
           request.center.z < kRegionCountZ &&
           request.region.x < kRegionCountX &&
           request.region.y < kRegionCountY &&
           request.region.z < kRegionCountZ &&
           request.radius <= 5;
}
}
