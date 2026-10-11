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
    WorldSpec world{1024, 1024, 256, 128};
    std::uint64_t seed = 0xC0FFEE1234ULL;
    std::uint8_t interestRadius = 6;
    // GPU node pool capacity, in SVO nodes (8 bytes each).
    std::uint32_t poolNodes = 4u * 1024u * 1024u;
    std::uint32_t maxRayDistance = 512;
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
