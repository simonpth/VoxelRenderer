#pragma once

// Volk must come before anything that pulls in vulkan.h (CMake also defines VK_NO_PROTOTYPES).
#include <volk.h>

// VMA is told to resolve all Vulkan functions through vkGetInstanceProcAddr / vkGetDeviceProcAddr,
// which is what we get from Volk. VMA_IMPLEMENTATION is defined in VulkanContext.cpp only.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-braces"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wnullability-extension"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-variable"
#endif
#include <vk_mem_alloc.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <VkBootstrap.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "window.hpp"

#ifndef VOXEL_SHADER_DIR
#define VOXEL_SHADER_DIR "/Users/simonpth/proj/VoxelRenderer/build/shaders/"
#endif

class VulkanContext {
public:
    explicit VulkanContext(Window* window);
    ~VulkanContext();

    VulkanContext(const VulkanContext&)            = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    // Call once per frame. Renders everything and presents.
    void drawFrame();

    // Call from the GLFW framebuffer-size callback (optional; out-of-date swapchains are also detected).
    void notifyResized() { m_framebufferResized = true; }

private:
    static constexpr uint32_t kFramesInFlight = 2;

    static constexpr const char* vertexShaderPath   = VOXEL_SHADER_DIR "triangle.vert.spv";
    static constexpr const char* fragmentShaderPath = VOXEL_SHADER_DIR "triangle.frag.spv";
    static constexpr const char* computeShaderPath = VOXEL_SHADER_DIR "pixel.comp.spv";
    static constexpr const char* overlayVertexShaderPath = VOXEL_SHADER_DIR "overlay.vert.spv";
    static constexpr const char* overlayFragmentShaderPath = VOXEL_SHADER_DIR "overlay.frag.spv";

    struct Vertex { float pos[2]; float color[3]; };

    struct FrameData {
        VkCommandPool   pool           = VK_NULL_HANDLE;
        VkCommandBuffer cmd            = VK_NULL_HANDLE;
        VkSemaphore     imageAvailable = VK_NULL_HANDLE;
        VkFence         inFlight       = VK_NULL_HANDLE;
        VkImage computeImage = VK_NULL_HANDLE;
        VmaAllocation computeAllocation = VK_NULL_HANDLE;
        VkImageView computeImageView = VK_NULL_HANDLE;
        VkDescriptorSet computeDescriptorSet = VK_NULL_HANDLE;
        bool computeImageInitialized = false;
    };

    // --- init ---
    void initInstance();
    void initDevice();
    void initAllocator();
    void createSwapchain();
    void createFrameResources();
    void createPipeline();
    void createComputeResources();
    void destroyComputeResources();
    void createTriangleBuffer();

    // --- runtime ---
    void cleanupSwapchain();
    void recreateSwapchain();
    void recordCommands(FrameData& frame, uint32_t imageIndex);

    // --- helpers ---
    VkShaderModule createShaderModule(const std::string& path);

    Window* m_window = nullptr;

    // vk-bootstrap objects
    vkb::Instance       m_vkbInstance;
    vkb::PhysicalDevice m_vkbPhysicalDevice;
    vkb::Device         m_vkbDevice;
    vkb::Swapchain      m_swapchain;

    VkInstance       m_instance       = VK_NULL_HANDLE;
    VkSurfaceKHR     m_surface        = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice         m_device         = VK_NULL_HANDLE;
    VkQueue          m_graphicsQueue  = VK_NULL_HANDLE;
    uint32_t         m_graphicsFamily = 0;

    VmaAllocator m_allocator = VK_NULL_HANDLE;

    // Swapchain-dependent
    std::vector<VkImage>     m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;
    std::vector<VkSemaphore> m_renderFinished; // one per swapchain image
    std::vector<VkFence>     m_imagesInFlight; // fence currently using each image

    // Per-frame
    std::array<FrameData, kFramesInFlight> m_frames{};
    uint32_t m_frameIndex = 0;

    // Pipeline
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_pipeline       = VK_NULL_HANDLE;
    VkPipeline m_trianglePipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_computePipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_computePipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    // A compute target per in-flight frame prevents read/write hazards across submissions.
    VkFormat m_computeImageFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkBuffer m_triangleBuffer = VK_NULL_HANDLE;
    VmaAllocation m_triangleAllocation = VK_NULL_HANDLE;

    bool m_framebufferResized = false;
};
