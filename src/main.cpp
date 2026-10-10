#include <iostream>
#include <chrono>
#include <iomanip>
#include "renderer/window.hpp"
#include "renderer/vulkancontext.hpp"

int main() {
    std::cout << "Voxel Renderer started." << std::endl;

    Window window("Voxel Renderer", 800, 600);
    VulkanContext renderer(&window);
    using Clock = std::chrono::steady_clock;
    auto fpsWindowStart = Clock::now();
    uint32_t fpsFrameCount = 0;

    // Main Rendering loop
    while (!window.shouldClose()) {
        window.pollEvents();

        if (window.wasResized()) {
            window.waitWhileMinimized();
            renderer.notifyResized();
        }

        renderer.drawFrame();

        ++fpsFrameCount;
        const auto now = Clock::now();
        const std::chrono::duration<float> elapsed = now - fpsWindowStart;
        if (elapsed.count() >= 1.0f) {
            std::cout << "FPS: " << std::fixed << std::setprecision(1)
                      << (fpsFrameCount / elapsed.count()) << '\n';
            fpsFrameCount = 0;
            fpsWindowStart = now;
        }
    }

    return 0;
}
