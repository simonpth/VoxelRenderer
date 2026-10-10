#pragma once

#include <cstdint>
#include <string>

namespace voxel::net {
struct Peer {
    std::string host;
    std::uint16_t port = 0;
    bool operator==(const Peer&) const = default;
};
}
