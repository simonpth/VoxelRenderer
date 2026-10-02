#include <iostream>
#include "renderer/window.hpp"
#include "renderer/vulkancontext.hpp"

int main() {
    std::cout << "Voxel Renderer started." << std::endl;

    Window window("Voxel Renderer", 800, 600);
    VulkanContext renderer(&window);

    // Main Rendering loop
    while (!window.shouldClose()) {
        window.pollEvents();

        if (window.wasResized()) {
            window.waitWhileMinimized();
            renderer.notifyResized();
        }

        renderer.drawFrame();
    }

    return 0;
}