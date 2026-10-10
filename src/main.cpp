#include <iostream>
#include <chrono>
#include "renderer/window.hpp"
#include "renderer/vulkancontext.hpp"
#include "network/client.hpp"
#include "network/server.hpp"

int main() {
    std::cout << "Voxel Renderer started." << std::endl;

    Window window("Voxel Renderer", 800, 600);
    VulkanContext renderer(&window);
    voxel::net::Server server;
    voxel::net::Client client;
    server.start();
    client.start();
    bool mouseLookActive = true;
    SDL_SetWindowRelativeMouseMode(window.handle(), mouseLookActive);

    using Clock = std::chrono::steady_clock;
    auto lastFrameTime = Clock::now();
    float totalTime = 0.0f;
    std::uint32_t inputTick = 1;
    glm::vec2 mouseDelta{0.0f};

    // Main Rendering loop
    while (!window.shouldClose()) {
        const auto currentFrameTime = Clock::now();
        const std::chrono::duration<float> deltaDuration = currentFrameTime - lastFrameTime;
        lastFrameTime = currentFrameTime;
        const float deltaTime = deltaDuration.count();
        totalTime += deltaTime;

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
        voxel::InputIntent input{};
        input.clientTick = inputTick++;
        input.move.x = static_cast<float>(keyboard[SDL_SCANCODE_D]) -
                       static_cast<float>(keyboard[SDL_SCANCODE_A]);
        input.move.y = static_cast<float>(keyboard[SDL_SCANCODE_W]) -
                       static_cast<float>(keyboard[SDL_SCANCODE_S]);
        constexpr float mouseSensitivity = 0.12f;
        if (mouseLookActive) {
            input.look.x = mouseDelta.x * mouseSensitivity;
            input.look.y = -mouseDelta.y * mouseSensitivity;
        }
        const bool jumpDown = keyboard[SDL_SCANCODE_SPACE];
        input.jumpPressed = jumpDown;
        client.submit(input, deltaTime);
        client.updateRender(deltaTime);
        voxel::RenderState& renderState = client.renderState();
        renderer.drawFrame(deltaTime, totalTime, renderState);
    }

    return 0;
}
