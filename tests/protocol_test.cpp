// Round-trip tests for the wire protocol.
//
// The point of these tests is the rejection path, not the happy path: a
// malformed payload must be refused before it reaches the simulation, and a
// client must never be able to inject a transform, a material table that lies
// about solidity, or a brick index that escapes the root layer.

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

// SvoNode has no operator==: the node vector is compared field by field.
bool sameNodes(const std::vector<SvoNode>& a, const std::vector<SvoNode>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].word0 != b[i].word0 || a[i].word1() != b[i].word1())
            return false;
    return true;
}

int failures = 0;

// RelWithDebInfo defines NDEBUG, which compiles CHECK() into nothing: the
// suite would then pass without asserting anything.
#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                      \
        }                                                                    \
    } while (false)

} // namespace

int main() {
    // --- registration ---
    {
        const Registration message{makeUuid(3)};
        Registration decoded{};
        CHECK(decode(encode(message), decoded));
        CHECK(decoded.uuid == message.uuid);
    }

    // --- intent is the only thing a client may send about itself ---
    {
        InputIntent message{};
        message.clientTick = 7;
        message.moveDirection = {0.4f, -0.2f};
        message.look = {0.01f, -0.02f};
        message.jumpPressed = true;
        message.descendPressed = true;
        message.flying = true;
        InputIntent decoded{};
        CHECK(decode(encode(message), decoded));
        CHECK(decoded.clientTick == 7);
        CHECK(decoded.moveDirection == message.moveDirection);
        CHECK(decoded.look == message.look);
        CHECK(decoded.jumpPressed == message.jumpPressed);
        CHECK(decoded.descendPressed == message.descendPressed);
        CHECK(decoded.flying == message.flying);
        // Raw keys are a client-local concept and must not appear on the wire.
        CHECK(decoded.rawMove.axes == glm::vec2(0.0f));
    }
    {
        // An oversized direction is clamped to unit length rather than
        // refused: it is a valid "full speed" request, just asked for wrongly.
        // Anything longer would be free movement, so it cannot survive decode.
        InputIntent message{};
        message.clientTick = 3;
        message.moveDirection = {100.0f, 0.0f};
        InputIntent decoded{};
        CHECK(decode(encode(message), decoded));
        CHECK(glm::length(decoded.moveDirection) <= 1.0f);
        CHECK(glm::abs(glm::length(decoded.moveDirection) - 1.0f) < 1e-5f);
    }
    {
        // A diagonal direction is longer than an axis one and must keep its
        // length, so the authority can tell "walking forward and right at full
        // tilt" from "walking forward".
        InputIntent message{};
        message.moveDirection = {0.6f, 0.8f};
        InputIntent decoded{};
        CHECK(decode(encode(message), decoded));
        CHECK(glm::abs(glm::length(decoded.moveDirection) - 1.0f) < 1e-5f);
    }
    {
        // Non-finite look deltas are not a valid intent.
        InputIntent message{};
        message.clientTick = 1;
        message.look = {std::numeric_limits<float>::infinity(), 0.0f};
        InputIntent decoded{};
        CHECK(!decode(encode(message), decoded));
    }
    {
        // A wildly oversized direction is clamped, not refused: "walk that way" is
        // a legitimate request and the server only cares that the magnitude
        // does not exceed a full step. Refusing it would also work, but
        // clamping keeps a malformed or hostile peer from being able to stall a
        // client by simply disconnecting it from its own input.
        InputIntent message{};
        message.clientTick = 1;
        message.moveDirection = {9.0f, 0.0f};
        InputIntent decoded{};
        CHECK(decode(encode(message), decoded));
        CHECK(glm::abs(glm::length(decoded.moveDirection) - 1.0f) < 1e-5f);
    }
    {
        // Look is degrees per intent: a fast flick is legal, a wild value is
        // not. The old 0.5 bound would have clipped a real mouse flick.
        InputIntent flick{};
        flick.clientTick = 1;
        flick.look = {kMaxLookDegrees, -kMaxLookDegrees};
        InputIntent decodedFlick{};
        CHECK(decode(encode(flick), decodedFlick));
        CHECK(decodedFlick.look == flick.look);

        InputIntent tooFast{};
        tooFast.clientTick = 1;
        tooFast.look = {kMaxLookDegrees + 1.0f, 0.0f};
        InputIntent decodedTooFast{};
        CHECK(!decode(encode(tooFast), decodedTooFast));
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
        message.localPlayer.flying = true;
        message.playerCount = 2;
        message.players[0] = message.localPlayer;
        message.players[1].id = 10;
        message.players[1].position = {9.0f, 9.0f, 9.0f};
        AuthoritativeState decoded{};
        CHECK(decode(encode(message), decoded));
        CHECK(decoded.tick == 12);
        CHECK(decoded.playerCount == 2);
        CHECK(decoded.localPlayer.position == message.localPlayer.position);
        CHECK(decoded.localPlayer.grounded);
        CHECK(decoded.localPlayer.flying);
        CHECK(decoded.players[1].id == 10);
        CHECK(decoded.players[1].position == message.players[1].position);
        CHECK(!decoded.players[1].flying);
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
        CHECK(decode(encode(message), decoded));
        CHECK(decoded.sizeX == 1024);
        CHECK(decoded.rootCount == 128);
        CHECK(decoded.materials.size() == message.materials.size());
        for (std::size_t i = 0; i < message.materials.size(); ++i)
            CHECK(decoded.materials[i].flags == message.materials[i].flags);

        // A root layer that cannot cover the world it claims is rejected.
        WorldDescriptor broken = message;
        broken.rootCount = 4;
        WorldDescriptor decodedBroken{};
        CHECK(!decode(encode(broken), decodedBroken));
    }

    // --- interest and ack ---
    {
        const WorldInterest message{makeUuid(1), 3, 128, 256, 64, 4};
        WorldInterest decoded{};
        CHECK(decode(encode(message), decoded));
        CHECK(decoded.playerId == 3);
        CHECK(decoded.centerX == 128);
        CHECK(decoded.radius == 4);
    }
    {
        const WorldAck message{makeUuid(1), 3, 77, 0xDEADBEEF};
        WorldAck decoded{};
        CHECK(decode(encode(message), decoded));
        CHECK(decoded.rootIndex == 77);
        CHECK(decoded.revision == 0xDEADBEEF);
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
        CHECK(decode(encode(message), decoded));
        CHECK(decoded.rootIndex == 42);
        CHECK(decoded.revision == 0x1234);
        CHECK(decoded.nodes.size() == nodes.size());
        CHECK(sameNodes(decoded.nodes, nodes));
        CHECK(decoded.nodes.front().mask == root.mask);

        // A child block that runs off the end of the payload is not a brick.
        BrickSnapshot truncated = message;
        truncated.nodes.resize(std::max<std::size_t>(8, nodes.size() / 2));
        BrickSnapshot decodedTruncated{};
        CHECK(!decode(encode(truncated), decodedTruncated));
    }

    // --- unknown kinds are refused instead of being guessed at ---
    {
        std::vector<std::byte> bytes = encode(Registration{makeUuid(1)});
        // Magic (4) + version (2) leaves the kind at offset 6. Anything else
        // in the payload is data, not the kind.
        bytes[6] = std::byte(0x77);
        Registration decoded{};
        CHECK(!decode(bytes, decoded));
    }
    {
        AuthoritativeState decoded{};
        CHECK(!decode({}, decoded));
    }

    if (failures != 0) {
        std::printf("protocol tests FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("protocol tests passed\n");
    return 0;
}
