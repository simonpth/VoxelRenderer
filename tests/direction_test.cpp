// W travels where the camera points.
//
// The wire format used to carry raw WASD, and the server rebuilt the camera
// basis from its own replicated `yaw` to resolve it. Between snapshots that
// yaw is not the one the player is looking along, so W walked in a direction
// the screen did not show -- and worse, when the two disagreed by a lot (a
// fast flick, or any lag) it walked somewhere else entirely.
//
// The client now resolves its keys against the same yaw it renders with and
// sends a world-space direction, so this is testable: turn the camera to a
// known yaw, hold W, and check the authority travels along the direction the
// renderer's basis says it should. It is measured end to end, through the real
// socket, because the property being tested is "the direction the client sends
// is the direction the renderer draws" -- nothing less exercises it.
//
// Verified against the old behaviour: with the keys sent raw, three of the four
// yaws below walked in a completely different direction (alignment 0.0, -0.0,
// -1.0), because "forward" had collapsed to a single fixed world axis.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include "network/client.hpp"
#include "network/server.hpp"
#include "network/protocol.hpp"
using namespace voxel;
using namespace voxel::net;
int main() {
    const std::uint16_t port = 28961;
    const WorldSpec spec{1024, 1024, 256, 128};
    Server server(port, spec, 7);
    server.start();
    Client client(Peer{"127.0.0.1", port});
    client.start();
    for (int i = 0; i < 400 && !client.renderState().sizeX; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // The authority replicates its own state over UDP, separately from the
    // world descriptor. Until the first snapshot has been consumed,
    // authoritativeState() reports a zeroed player -- so measuring from it
    // before that point yields nonsense. Wait for a non-zero position.
    for (int i = 0; i < 400; ++i) {
        client.updateRender(1.0f / 60.0f);
        if (client.authoritativeState().localPlayer.position != glm::vec3(0.0f))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // The basis the RENDERER uses, copied from VulkanContext::drawFrame. If the
    // authority now agrees with it, W travels where the screen points.
    const auto rendererForward = [](float yawDegrees) {
        const float yaw = glm::radians(yawDegrees);
        return glm::vec2{std::cos(yaw), std::sin(yaw)};
    };

    int failures = 0;
    std::uint32_t tick = 1;
    for (const float yaw : {0.0f, 90.0f, 180.0f, -90.0f}) {
        // Turn the camera to `yaw`, then hold W and let the authority move.
        // The tick must keep climbing: the server discards any intent it has
        // already acknowledged, so restarting the counter would make every
        // later case a no-op.
        // Re-read the tick BEFORE the loop: a stale counter makes every later
        // case a no-op, which is what happened the first time this was written.
        // Phase 1: turn the camera to `yaw`. Read back the rendered yaw rather
        // than assuming, so each case starts from wherever the last left off.
        for (int frame = 0; frame < 800; ++frame) {
            const float current = client.renderState().localPlayer.yaw;
            float difference = yaw - current;
            while (difference > 180.0f) difference -= 360.0f;
            while (difference < -180.0f) difference += 360.0f;
            if (std::abs(difference) < 0.5f) break;
            voxel::InputIntent intent{};
            intent.clientTick = ++tick;
            // A small step per frame, the way a real mouse would deliver it.
            intent.look = {glm::clamp(difference, -4.0f, 4.0f), 0.0f};
            intent.rawMove.axes = {0.0f, 0.0f};   // not moving while turning
            client.submit(intent, 1.0f / 60.0f);
            client.updateRender(1.0f / 60.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }

        // Phase 2: hold W and see which way the authority walks. `before` is
        // taken after the turn and a settle, so it cannot read a zero that came
        // from before the first snapshot arrived.
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const glm::vec3 before =
            client.authoritativeState().localPlayer.position;
        for (int frame = 0; frame < 400; ++frame) {
            voxel::InputIntent intent{};
            intent.clientTick = ++tick;
            intent.rawMove.axes = {0.0f, 1.0f};
            client.submit(intent, 1.0f / 60.0f);
            client.updateRender(1.0f / 60.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        const glm::vec3 after =
            client.authoritativeState().localPlayer.position;
        const glm::vec3 delta = after - before;
        // Ground plane: world x and y. The authority's moveDirection is in that
        // same plane, with y as "forward" at yaw 0 and x as "right".
        const glm::vec2 ground{delta.x, delta.y};
        const float length = glm::length(ground);
        const glm::vec2 normalised = length > 1e-4f ? ground / length
                                                    : glm::vec2{0.0f, 0.0f};
        const glm::vec2 expected = rendererForward(yaw);

        const float dot = glm::dot(normalised, expected);
        const bool aligned = dot > 0.99f;
        if (!aligned) ++failures;
        std::printf("yaw %7.1f : travelled (%6.2f, %6.2f) over %6.1f blocks, "
                    "expected (%5.2f, %5.2f), alignment %.4f  %s\n",
                    static_cast<double>(yaw),
                    static_cast<double>(ground.x),
                    static_cast<double>(ground.y),
                    static_cast<double>(length),
                    static_cast<double>(expected.x),
                    static_cast<double>(expected.y),
                    static_cast<double>(dot),
                    aligned ? "OK" : "MISALIGNED");
    }

    client.stop();
    server.stop();
    if (failures) {
        std::printf("%d yaw(s) walked the wrong way\n", failures);
        return 1;
    }
    std::printf("W travels where the camera points at every yaw\n");
    return 0;
}