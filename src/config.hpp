#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "simulation/svo.hpp"

namespace voxel {

// ---------------------------------------------------------------------------
// Build-time defaults and command-line overrides.
//
// The world is deliberately small while the renderer is being brought up: it
// is the smallest volume that still exercises the non-cubic root layer, the
// octree bricks, streaming, and the GPU traversal. Everything here can be
// changed from the command line, so no rebuild is needed to resize the world.
// ---------------------------------------------------------------------------

struct Config {
    std::uint16_t serverPort = 27015;
    std::string serverHost = "127.0.0.1";
    // 2^16 x 2^16 x 2^13 cells. With 128-cell bricks that is a 512 x 512 x 64
    // root layer, or 16,777,216 bricks.
    //
    // The world is procedural and the terrain is a heightfield, so the server
    // stores none of it: any brick is regenerated from the seed on demand.
    // That is what makes this size reachable -- full-resolution storage would
    // be 4.4 TiB against an 8 GiB budget, and even a single-node-per-brick
    // floor would need 128 MiB just for root entries.
    WorldSpec world{65536, 65536, 8192, 128};
    std::uint64_t seed = 0xC0FFEE1234ULL;
    std::uint8_t interestRadius = 6;

    // GPU node pool: the client's entire budget for world storage, in SVO
    // nodes of 8 bytes each. This is the whole allowance -- no geometry, no
    // textures, no framebuffers -- and eviction keeps the client inside it no
    // matter how large the world is.
    //
    // 3 GiB is 402,653,184 nodes, which is 86x the entire default world at full
    // resolution. That headroom is the point: the arena can hold the whole
    // world, so the server can stream all of it and the client never has to
    // choose between the ground it is standing on and the ground it walked
    // past.
    std::uint32_t poolNodes = 402'653'184u;

    // Bytes per second the server may spend streaming world data. The whole
    // world is streamed, so this is what makes it arrive over time rather than
    // all at once. Zero means unmetered -- fine on loopback, unwise elsewhere.
    std::uint64_t streamBytesPerSecond = 32ull * 1024 * 1024;

    // Server RAM budget for the world, in bytes. Reported at startup against the
    // world's real cost, so an oversized world is visible before the allocator
    // finds out.
    std::uint64_t serverWorldBytes = 8ull * 1024 * 1024 * 1024;

    std::uint32_t maxRayDistance = 131072;
    // Set when a negotiation flag is malformed; reported at startup.
    bool helpRequested = false;
    bool parseError = false;
    std::string error;

    // Usage text printed for --help or a malformed argument list.
    static const char* usage();
};

// Parses argv. Unrecognised flags produce a parse error rather than a throw.
Config parseConfig(int argc, char** argv);

} // namespace voxel
