#include <chrono>
#include <iostream>
#include <string>

#include "config.hpp"
#include "renderer/window.hpp"
#include "renderer/vulkancontext.hpp"
#include "network/client.hpp"
#include "network/server.hpp"

int main(int argc, char** argv) {
    const voxel::Config config = voxel::parseConfig(argc, argv);
    if (config.parseError && !config.error.empty()) {
        std::cerr << "Configuration error: " << config.error << "\n\n"
                  << voxel::Config::usage();
        return 1;
    }
    if (config.helpRequested) {
        std::cout << voxel::Config::usage();
        return 0;
    }
    std::cout << "World " << config.world.sizeX << " x " << config.world.sizeY
              << " x " << config.world.sizeZ << ", brick " << config.world.brick
              << ", root " << config.world.rootDimX() << " x "
              << config.world.rootDimY() << " x " << config.world.rootDimZ()
              << " = " << config.world.rootCount() << " bricks\n";

    // The server owns the world; the client only renders what it is sent.
    voxel::net::Server server(config.serverPort, config.world, config.seed);
    server.setBandwidth(config.streamBytesPerSecond);

    // The world's real cost against the RAM budget it is supposed to fit in.
    // Computed up front because a world that does not fit is a design error,
    // and finding out from the allocator gives you nothing to act on.
    {
        const std::uint64_t bytesPerNode = sizeof(voxel::SvoNode);
        // The world's node count is not known until it is generated, but the
        // bound is: every cell could be a leaf. Reported as a bound, not an
        // estimate, so it is never wrong in the optimistic direction.
        const std::uint64_t cells =
            static_cast<std::uint64_t>(config.world.sizeX) * config.world.sizeY *
            config.world.sizeZ;
        const std::uint64_t bound = cells * bytesPerNode;
        std::cout << "Server world budget: " << (config.serverWorldBytes >> 30)
                  << " GiB (worst case for this world is "
                  << (bound >> 30) << " GiB)\n";
        if (bound > config.serverWorldBytes)
            std::cout << "  warning: the world cannot exceed the budget; "
                         "bricks will be generated lazily\n";
    }

    voxel::net::Client client(voxel::net::Peer{config.serverHost, config.serverPort});
    server.start();
    client.start();

    Window window("Voxel Renderer", 960, 600);
    VulkanContext renderer(&window, config.poolNodes);
    renderer.setRayParameters(config.maxRayDistance, {-0.55f, -0.35f, 0.75f});

    bool mouseLookActive = true;
    SDL_SetWindowRelativeMouseMode(window.handle(), mouseLookActive);
    // Flight mode is a requested state in the intent: the authority owns the
    // applied state, so this only records what the player asked for.
    bool flyMode = false;

    using Clock = std::chrono::steady_clock;
    auto lastFrameTime = Clock::now();
    float totalTime = 0.0f;
    std::uint32_t inputTick = 1;
    glm::vec2 mouseDelta{0.0f};

    while (!window.shouldClose()) {
        const auto currentFrameTime = Clock::now();
        const std::chrono::duration<float> deltaDuration =
            currentFrameTime - lastFrameTime;
        lastFrameTime = currentFrameTime;
        const float deltaTime = deltaDuration.count();
        totalTime += deltaTime;

        // Mouse movement is the only input the client acts on locally: the
        // look angle must respond within one frame to feel responsive.
        mouseDelta = {0.0f, 0.0f};
        window.pollEvents([&](const SDL_Event& event) {
            if (event.type == SDL_EVENT_KEY_DOWN &&
                event.key.scancode == SDL_SCANCODE_ESCAPE &&
                !event.key.repeat) {
                mouseLookActive = false;
                SDL_SetWindowRelativeMouseMode(window.handle(), false);
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                       event.button.button == SDL_BUTTON_LEFT) {
                mouseLookActive = true;
                SDL_SetWindowRelativeMouseMode(window.handle(), true);
            }
            if (event.type == SDL_EVENT_KEY_DOWN &&
                event.key.scancode == SDL_SCANCODE_F && !event.key.repeat) {
                flyMode = !flyMode;
            }
            if (event.type == SDL_EVENT_MOUSE_MOTION) {
                mouseDelta.x += event.motion.xrel;
                mouseDelta.y += event.motion.yrel;
            }
        });

        if (window.wasResized()) {
            window.waitWhileMinimized();
            renderer.notifyResized();
        }

        const bool* keyboard = SDL_GetKeyboardState(nullptr);
        voxel::InputIntent intent{};
        intent.clientTick = inputTick++;
        intent.rawMove.axes.x =
            static_cast<float>(keyboard[SDL_SCANCODE_D]) -
            static_cast<float>(keyboard[SDL_SCANCODE_A]);
        intent.rawMove.axes.y =
            static_cast<float>(keyboard[SDL_SCANCODE_W]) -
            static_cast<float>(keyboard[SDL_SCANCODE_S]);
        // Degrees per pixel of mouse travel. The authority stores yaw in
        // degrees and turns left as yaw grows, so a rightward sweep is a
        // negative delta; pitch already matched the vertical direction.
        constexpr float mouseSensitivity = 0.1f;
        const glm::vec2 lookDelta = mouseDelta * mouseSensitivity;
        intent.look.x = mouseLookActive ? -lookDelta.x : 0.0f;
        intent.look.y = mouseLookActive ? -lookDelta.y : 0.0f;
        intent.jumpPressed = keyboard[SDL_SCANCODE_SPACE] != 0;
        // Shift drops out of the air, and is only meaningful while flying.
        intent.descendPressed =
            keyboard[SDL_SCANCODE_LSHIFT] != 0 || keyboard[SDL_SCANCODE_RSHIFT] != 0;
        intent.flying = flyMode;

        // Everything except look goes to the server first.
        client.submit(intent, deltaTime);
        client.updateRender(deltaTime);
        voxel::RenderState& renderState = client.renderState();
        renderer.drawFrame(deltaTime, totalTime, renderState);
        // Acknowledge only what the renderer managed to place in the GPU
        // pool. Doing this before the draw, or on receipt, is what let the
        // server believe a brick was delivered while the client still had no
        // data for it.
        client.confirmUploads();

        if (!server.running() || !client.running()) {
            std::cerr << "Server or client stopped; exiting\n";
            break;
        }
    }

    client.stop();
    server.stop();
    return 0;
}
