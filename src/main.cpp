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
    voxel::net::Client client(voxel::net::Peer{config.serverHost, config.serverPort});
    server.start();
    client.start();

    Window window("Voxel Renderer", 960, 600);
    VulkanContext renderer(&window, config.poolNodes);
    renderer.setRayParameters(config.maxRayDistance, {-0.55f, -0.35f, 0.75f});

    bool mouseLookActive = true;
    SDL_SetWindowRelativeMouseMode(window.handle(), mouseLookActive);

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
        intent.move.x = static_cast<float>(keyboard[SDL_SCANCODE_D]) -
                        static_cast<float>(keyboard[SDL_SCANCODE_A]);
        intent.move.y = static_cast<float>(keyboard[SDL_SCANCODE_W]) -
                        static_cast<float>(keyboard[SDL_SCANCODE_S]);
        constexpr float mouseSensitivity = 0.0015f;
        intent.look.x = mouseLookActive ? mouseDelta.x * mouseSensitivity : 0.0f;
        intent.look.y = mouseLookActive ? -mouseDelta.y * mouseSensitivity : 0.0f;
        intent.jumpPressed = keyboard[SDL_SCANCODE_SPACE] != 0;

        // Everything except look goes to the server first.
        client.submit(intent, deltaTime);
        client.updateRender(deltaTime);
        voxel::RenderState& renderState = client.renderState();
        renderer.drawFrame(deltaTime, totalTime, renderState);

        if (!server.running() || !client.running()) {
            std::cerr << "Server or client stopped; exiting\n";
            break;
        }
    }

    client.stop();
    server.stop();
    return 0;
}
