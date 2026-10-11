#include "config.hpp"

#include <cstdlib>

namespace voxel {
namespace {

// Reads the next argv token as an unsigned decimal. Returns false when the
// token is missing or not a number, leaving `target` untouched.
bool readUnsigned(int& index, int argc, char** argv, std::uint32_t& target) {
    if (index + 1 >= argc) return false;
    try {
        const auto value = std::stoul(argv[index + 1]);
        if (value > 0xFFFFFFFFu) return false;
        ++index;
        target = static_cast<std::uint32_t>(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

Config fail(Config config, std::string message) {
    config.parseError = true;
    config.helpRequested = true;
    config.error = std::move(message);
    return config;
}

} // namespace

const char* Config::usage() {
    return "VoxelRenderer\n"
           "  --port   <n>      UDP gameplay port (world stream uses port + 1)\n"
           "  --host   <ip>     server address\n"
           "  --world  X Y Z    world size in cells (default 1024 1024 256)\n"
           "  --brick  <n>      power-of-two edge of one octree brick (default 128)\n"
           "  --seed   <n>      terrain seed\n"
           "  --radius <n>      brick streaming radius (default 6)\n"
           "  --pool   <n>      GPU node pool capacity in nodes (default 4M)\n"
           "  --ray    <n>      maximum primary ray distance in cells\n";
}

Config parseConfig(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            config.helpRequested = true;
            config.error.clear();
            return config;
        }
        if (argument == "--port") {
            std::uint32_t port = config.serverPort;
            if (!readUnsigned(i, argc, argv, port)) return fail(config, "port");
            config.serverPort = static_cast<std::uint16_t>(port);
        } else if (argument == "--host") {
            if (i + 1 >= argc) return fail(config, "host");
            config.serverHost = argv[++i];
        } else if (argument == "--world") {
            std::uint32_t x = 0, y = 0, z = 0;
            if (!readUnsigned(i, argc, argv, x) ||
                !readUnsigned(i, argc, argv, y) ||
                !readUnsigned(i, argc, argv, z))
                return fail(config, "world");
            if (x == 0 || y == 0 || z == 0) return fail(config, "world size");
            config.world.sizeX = x;
            config.world.sizeY = y;
            config.world.sizeZ = z;
        } else if (argument == "--brick") {
            std::uint32_t brick = 0;
            if (!readUnsigned(i, argc, argv, brick)) return fail(config, "brick");
            if (brick == 0 || (brick & (brick - 1)) != 0)
                return fail(config, "brick power of two");
            config.world.brick = brick;
        } else if (argument == "--seed") {
            std::uint32_t seed = static_cast<std::uint32_t>(config.seed);
            if (!readUnsigned(i, argc, argv, seed)) return fail(config, "seed");
            config.seed = seed;
        } else if (argument == "--radius") {
            std::uint32_t radius = 0;
            if (!readUnsigned(i, argc, argv, radius)) return fail(config, "radius");
            if (radius > 16) return fail(config, "radius <= 16");
            config.interestRadius = static_cast<std::uint8_t>(radius);
        } else if (argument == "--pool") {
            if (!readUnsigned(i, argc, argv, config.poolNodes))
                return fail(config, "pool");
        } else if (argument == "--ray") {
            if (!readUnsigned(i, argc, argv, config.maxRayDistance))
                return fail(config, "ray");
        } else {
            return fail(config, "unknown argument " + argument);
        }
    }
    return config;
}

} // namespace voxel
