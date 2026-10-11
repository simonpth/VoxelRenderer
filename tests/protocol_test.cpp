// Round-trip tests for the wire protocol.
//
// The point of these tests is the rejection path, not the happy path: a
// malformed payload must be refused before it reaches the simulation, and a
// client must never be able to inject a transform, a material table that lies
// about solidity, or a brick index that escapes the root layer.

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <vector>

#include "network/protocol.hpp"

using namespace voxel;
using namespace voxel::net;

namespace {

ClientUuid makeUuid(std::uint8_t seed) {
    ClientUuid uuid{};
    for (std::size_t i = 0; i < uuid.size(); ++i)
        uuid[i] = static_cast<std::uint8_t>(seed + i);
    return uuid;
}

} // namespace

int main() {
    // --- registration ---
    {
        const Registration message{makeUuid(3)};
        Registration decoded{};
        assert(decode(encode(message), decoded));
        assert(decoded.uuid == message.uuid);
    }

    // --- intent is the only thing a client may send about itself ---
    {
        const InputIntent message{7, {0.4f, -0.2f}, {0.01f, -0.02f}, true};
        InputIntent decoded{};
        assert(decode(encode(message), decoded));
        assert(decoded.clientTick == 7);
        assert(decoded.move == message.move);
        assert(decoded.look == message.look);
        assert(decoded.jumpPressed == message.jumpPressed);
    }
    {
        // Non-finite look deltas are not a valid intent.
        InputIntent message{1, {0.0f, 0.0f}, {std::numeric_limits<float>::infinity(), 0.0f}, false};
        InputIntent decoded{};
        assert(!decode(encode(message), decoded));
    }
    {
        InputIntent message{1, {9.0f, 0.0f}, {0.0f, 0.0f}, false};
        InputIntent decoded{};
        assert(!decode(encode(message), decoded));
    }

    // --- snapshot ---
    {
        AuthoritativeState message{};
        message.tick = 12;
        message.worldRevision = 4;
        message.localPlayer.id = 9;
        message.localPlayer.position = {1.5f, 2.5f, 3.5f};
        message.localPlayer.velocity = {0.1f, 0.2f, -0.3f};
        message.localPlayer.yaw = 90.0f;
        message.localPlayer.pitch = -12.0f;
        message.localPlayer.grounded = true;
        message.playerCount = 2;
        message.players[0] = message.localPlayer;
        message.players[1].id = 10;
        message.players[1].position = {9.0f, 9.0f, 9.0f};
        AuthoritativeState decoded{};
        assert(decode(encode(message), decoded));
        assert(decoded.tick == 12);
        assert(decoded.playerCount == 2);
        assert(decoded.localPlayer.position == message.localPlayer.position);
        assert(decoded.localPlayer.grounded);
        assert(decoded.players[1].id == 10);
        assert(decoded.players[1].position == message.players[1].position);
    }

    // --- world descriptor ---
    {
        WorldDescriptor message{};
        message.sizeX = 1024;
        message.sizeY = 1024;
        message.sizeZ = 256;
        message.brick = 128;
        message.rootDimX = 8;
        message.rootDimY = 8;
        message.rootDimZ = 2;
        message.rootCount = 128;
        message.seed = 0xABCDEF;
        message.materials = serverMaterials();
        WorldDescriptor decoded{};
        assert(decode(encode(message), decoded));
        assert(decoded.sizeX == 1024);
        assert(decoded.rootCount == 128);
        assert(decoded.materials.size() == message.materials.size());
        for (std::size_t i = 0; i < message.materials.size(); ++i)
            assert(decoded.materials[i].flags == message.materials[i].flags);

        // A root layer that cannot cover the world it claims is rejected.
        WorldDescriptor broken = message;
        broken.rootCount = 4;
        WorldDescriptor decodedBroken{};
        assert(!decode(encode(broken), decodedBroken));
    }

    // --- interest and ack ---
    {
        const WorldInterest message{makeUuid(1), 3, 128, 256, 64, 4};
        WorldInterest decoded{};
        assert(decode(encode(message), decoded));
        assert(decoded.playerId == 3);
        assert(decoded.centerX == 128);
        assert(decoded.radius == 4);
    }
    {
        const WorldAck message{makeUuid(1), 3, 77, 0xDEADBEEF};
        WorldAck decoded{};
        assert(decode(encode(message), decoded));
        assert(decoded.rootIndex == 77);
        assert(decoded.revision == 0xDEADBEEF);
    }

    // --- brick snapshot: the payload the renderer consumes ---
    {
        Brick a{7};
        a.fill(0, 0, 0, 128, 128, 10, 3);
        a.set(5, 6, 7, 4);
        std::vector<SvoNode> nodes;
        SvoNode root{};
        a.compact(nodes, root);

        const BrickSnapshot message{makeUuid(1), 3, 42, 0x1234, 5, 7, nodes};
        BrickSnapshot decoded{};
        assert(decode(encode(message), decoded));
        assert(decoded.rootIndex == 42);
        assert(decoded.revision == 0x1234);
        assert(decoded.nodes.size() == nodes.size());
        assert(decoded.nodes == nodes);
        assert(decoded.nodes.front().mask == root.mask);

        // A child block that runs off the end of the payload is not a brick.
        BrickSnapshot truncated = message;
        truncated.nodes.resize(std::max<std::size_t>(8, nodes.size() / 2));
        BrickSnapshot decodedTruncated{};
        assert(!decode(encode(truncated), decodedTruncated));
    }

    // --- unknown kinds are refused instead of being guessed at ---
    {
        std::vector<std::byte> bytes = encode(Registration{makeUuid(1)});
        bytes[7] = std::byte(0x77);
        Registration decoded{};
        assert(!decode(bytes, decoded));
    }
    {
        AuthoritativeState decoded{};
        assert(!decode({}, decoded));
    }

    std::printf("protocol tests passed\n");
    return 0;
}
