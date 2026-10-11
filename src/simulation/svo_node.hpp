#pragma once

#include <cstdint>

namespace voxel {

// ---------------------------------------------------------------------------
// Sparse voxel octree node
//
// Eight bytes, trivially copyable, and identical on the CPU and on the GPU.
// The layout is mirrored by `shaders/svo.slang`; do not change one without
// the other.
//
//   word0  u32   bits  0..23  firstChild   node index of child block + 0
//                        24..31  flags
//   word1  u16   bits  0..15  material     16-bit material id
//          u8    bits 16..23  mask         bit i => child i is stored
//          u8    bits 24..31  lodLevel     log2 of this node's edge
//
// `firstChild` is an absolute index into a pool of eight-node blocks, so a
// node's children live at `firstChild + i`. Index 0 is reserved, which lets
// `firstChild == 0` mean "no children" without an extra flag.
// ---------------------------------------------------------------------------

inline constexpr std::uint8_t kSvoFlagSolid = 1u << 0;
inline constexpr std::uint8_t kSvoFlagResident = 1u << 1;

struct alignas(8) SvoNode {
    std::uint32_t word0 = 0;
    std::uint16_t material = 0;
    std::uint8_t mask = 0;
    std::uint8_t lodLevel = 0;

    static constexpr std::uint32_t kFirstChildMask = 0x00FFFFFFu;

    std::uint32_t firstChild() const { return word0 & kFirstChildMask; }
    void setFirstChild(std::uint32_t value) {
        word0 = (word0 & ~kFirstChildMask) | (value & kFirstChildMask);
    }

    std::uint8_t flags() const { return static_cast<std::uint8_t>(word0 >> 24); }
    void setFlags(std::uint8_t value) {
        word0 = (word0 & kFirstChildMask) |
                (static_cast<std::uint32_t>(value) << 24);
    }

    // The packed second word, for GPU packing / unpacking.
    std::uint32_t word1() const {
        return static_cast<std::uint32_t>(material) |
               (static_cast<std::uint32_t>(mask) << 16) |
               (static_cast<std::uint32_t>(lodLevel) << 24);
    }
    static SvoNode fromWords(std::uint32_t w0, std::uint32_t w1) {
        SvoNode node;
        node.word0 = w0;
        node.material = static_cast<std::uint16_t>(w1 & 0xFFFF);
        node.mask = static_cast<std::uint8_t>((w1 >> 16) & 0xFF);
        node.lodLevel = static_cast<std::uint8_t>((w1 >> 24) & 0xFF);
        return node;
    }

    bool hasChildren() const { return mask != 0; }
    bool solid() const { return (flags() & kSvoFlagSolid) != 0; }
};

static_assert(sizeof(SvoNode) == 8);

} // namespace voxel
