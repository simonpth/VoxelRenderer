#include <iostream>
#include <chrono>
#include <iomanip>
#include "renderer/window.hpp"
#include "renderer/vulkancontext.hpp"
#include "engine/player.hpp"

int main() {
    std::cout << "Voxel Renderer started." << std::endl;

    Window window("Voxel Renderer", 800, 600);
    VulkanContext renderer(&window);
    Player player;

    using Clock = std::chrono::steady_clock;
    auto lastFrameTime = Clock::now();
    auto fpsWindowStart = lastFrameTime;
    uint32_t fpsFrameCount = 0;
    float totalTime = 0.0f;

    // Main Rendering loop
    while (!window.shouldClose()) {
        const auto currentFrameTime = Clock::now();
        const std::chrono::duration<float> deltaDuration = currentFrameTime - lastFrameTime;
        lastFrameTime = currentFrameTime;
        const float deltaTime = deltaDuration.count();
        totalTime += deltaTime;

        window.pollEvents();

        if (window.wasResized()) {
            window.waitWhileMinimized();
            renderer.notifyResized();
        }

        player.update(deltaTime);
        renderer.drawFrame(deltaTime, totalTime, player.getHealth() / player.getMaxHealth());

        ++fpsFrameCount;
        const std::chrono::duration<float> fpsElapsed = currentFrameTime - fpsWindowStart;
        if (fpsElapsed.count() >= 1.0f) {
            std::cout << "FPS: " << std::fixed << std::setprecision(1)
                      << (fpsFrameCount / fpsElapsed.count()) << '\n';
            fpsFrameCount = 0;
            fpsWindowStart = currentFrameTime;
        }
    }

    return 0;
}
