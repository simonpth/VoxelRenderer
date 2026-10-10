#include <cassert>
#include "../src/simulation/svo.hpp"
#include "../src/network/protocol.hpp"

int main() {
    voxel::SvoRegion region;
    assert(region.get(3, 4, 5).material == 0);
    region.set(3, 4, 5, 7);
    assert(region.get(3, 4, 5).material == 7);
    assert(region.get(3, 4, 6).material == 0);
    region.set(3, 4, 5, 0);
    assert(region.get(3, 4, 5).material == 0);

    region.fill(0, 0, 0, 128, 128, 1, 3);
    assert(region.get(0, 0, 0).material == 3);
    assert(region.get(127, 127, 0).material == 3);
    assert(region.get(0, 0, 1).material == 0);

    voxel::SvoWorld world(1234);
    world.set(-1, -1, 100, 9);
    assert(world.get(static_cast<std::int64_t>(voxel::kWorldWidth) - 1,
                     static_cast<std::int64_t>(voxel::kWorldWidth) - 1, 100).material == 9);
    assert(world.isSolid(0, 0, -1));
    assert(!world.isSolid(0, 0, static_cast<std::int32_t>(voxel::kWorldDepth)));

    voxel::net::RegionSnapshot snapshot{
        {1, 2, 3}, region.revision(), world.revision(), region.nodes()};
    const auto encoded = voxel::net::encode(snapshot);
    voxel::net::RegionSnapshot decoded{};
    assert(!encoded.empty());
    assert(voxel::net::decode(encoded, decoded));
    assert(decoded.coordinate == snapshot.coordinate);
    assert(decoded.nodes.size() == snapshot.nodes.size());
    assert(decoded.revision == snapshot.revision);
    assert(decoded.worldRevision == snapshot.worldRevision);

    voxel::net::RegionInterestRequest interest{{}, {255, 255, 10},
                                               {254, 0, 10}, 5};
    const auto interestBytes = voxel::net::encode(interest);
    voxel::net::RegionInterestRequest decodedInterest{};
    assert(voxel::net::decode(interestBytes, decodedInterest));
    assert(decodedInterest.center == interest.center);
    assert(decodedInterest.radius == interest.radius);
    return 0;
}
