#define VOLK_IMPLEMENTATION
#define VMA_IMPLEMENTATION
#include "vulkancontext.hpp"

#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <cmath>
#include <format>

namespace {
void checkVk(VkResult result, const char *operation) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string(operation) + " failed with VkResult " + std::to_string(result));
}

template <typename T>
T unwrap(vkb::Result<T> &&result, const char *operation) {
  if (!result)
    throw std::runtime_error(std::string(operation) + ": " + result.error().message());
  return result.value();
}

const char *presentModeName(VkPresentModeKHR mode) {
  switch (mode) {
  case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE (uncapped, may tear)";
  case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX (triple buffered, vsynced)";
  case VK_PRESENT_MODE_FIFO_KHR: return "FIFO (vsynced)";
  case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
  default: return "other";
  }
}
} // namespace

VulkanContext::VulkanContext(Window *window) : m_window(window) {
  if (!m_window)
    throw std::invalid_argument("VulkanContext requires a Window");
  if (volkInitialize() != VK_SUCCESS)
    throw std::runtime_error("volkInitialize failed");

  initInstance();
  initDevice();
  initAllocator();
  createSwapchain();
  createFrameResources();
  createDescriptorSetLayout();
  createPipelineLayout();
  createPipelines();
  createDescriptorPool();
  createWorldBuffer();
  createComputeImages();
}

VulkanContext::~VulkanContext() {
  if (m_device)
    vkDeviceWaitIdle(m_device);

  for (auto &frame : m_frames) {
    if (frame.overlayVertexBuffer) {
      vmaDestroyBuffer(m_allocator, frame.overlayVertexBuffer, frame.overlayVertexAllocation);
      frame.overlayVertexBuffer = VK_NULL_HANDLE;
      frame.overlayVertexAllocation = VK_NULL_HANDLE;
      frame.overlayMappedData = nullptr;
    }
  }

  if (m_computePipeline)
    vkDestroyPipeline(m_device, m_computePipeline, nullptr);
  if (m_pipeline)
    vkDestroyPipeline(m_device, m_pipeline, nullptr);
  if (m_overlayPipeline)
    vkDestroyPipeline(m_device, m_overlayPipeline, nullptr);
  if (m_pipelineLayout)
    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);

  destroyComputeImages();
  destroyWorldBuffer();

  if (m_descriptorPool)
    vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
  if (m_descriptorSetLayout)
    vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);

  for (auto &frame : m_frames) {
    if (frame.imageAvailable)
      vkDestroySemaphore(m_device, frame.imageAvailable, nullptr);
    if (frame.renderFinished)
      vkDestroySemaphore(m_device, frame.renderFinished, nullptr);
    if (frame.inFlight)
      vkDestroyFence(m_device, frame.inFlight, nullptr);
    if (frame.pool)
      vkDestroyCommandPool(m_device, frame.pool, nullptr);
  }

  cleanupSwapchain();

  if (m_swapchain.swapchain)
    vkb::destroy_swapchain(m_swapchain);
  if (m_allocator)
    vmaDestroyAllocator(m_allocator);
  if (m_surface)
    Window::destroySurface(m_instance, m_surface);
  if (m_device)
    vkb::destroy_device(m_vkbDevice);
  if (m_instance)
    vkb::destroy_instance(m_vkbInstance);
}

void VulkanContext::initInstance() {
  auto extensions = Window::requiredInstanceExtensions();
  auto instance = unwrap(vkb::InstanceBuilder(Window::getInstanceProcAddr())
                             .set_app_name("Voxel Renderer")
                             .set_engine_name("Voxel Renderer")
                             .require_api_version(1, 3)
                             .enable_extensions(extensions)
                             .build(),
                         "create Vulkan instance");
  m_vkbInstance = instance;
  m_instance = instance.instance;
  volkLoadInstance(m_instance);
  m_surface = m_window->createSurface(m_instance);
}

void VulkanContext::initDevice() {
  VkPhysicalDeviceDynamicRenderingFeatures dynamicRendering{};
  dynamicRendering.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
  dynamicRendering.dynamicRendering = VK_TRUE;

  VkPhysicalDeviceSynchronization2Features sync2{};
  sync2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
  sync2.synchronization2 = VK_TRUE;

  auto physicalDevice = unwrap(vkb::PhysicalDeviceSelector(m_vkbInstance, m_surface)
                                   .set_minimum_version(1, 3)
                                   .add_required_extension_features(dynamicRendering)
                                   .add_required_extension_features(sync2)
                                   .select(),
                               "select physical device");
  m_vkbPhysicalDevice = physicalDevice;
  m_physicalDevice = physicalDevice.physical_device;
  auto device = unwrap(vkb::DeviceBuilder(m_vkbPhysicalDevice).build(), "create logical device");
  m_vkbDevice = device;
  m_device = device.device;
  auto queue = unwrap(device.get_queue_and_index(vkb::QueueType::graphics), "get graphics queue");
  m_graphicsQueue = queue.first;
  m_graphicsFamily = queue.second;
  volkLoadDevice(m_device);
}

void VulkanContext::initAllocator() {
  VmaVulkanFunctions functions{};
  functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
  functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

  VmaAllocatorCreateInfo info{};
  info.physicalDevice = m_physicalDevice;
  info.device = m_device;
  info.instance = m_instance;
  info.pVulkanFunctions = &functions;
  info.vulkanApiVersion = VK_API_VERSION_1_3;
  checkVk(vmaCreateAllocator(&info, &m_allocator), "vmaCreateAllocator");
}

void VulkanContext::createSwapchain() {
  VkExtent2D extent = m_window->framebufferExtent();

  vkb::SwapchainBuilder builder(m_vkbDevice);
  if (m_swapchain.swapchain != VK_NULL_HANDLE) {
    builder.set_old_swapchain(m_swapchain);
  }

  auto swapchain_ret = builder.set_desired_extent(extent.width, extent.height)
                              .set_desired_present_mode(VK_PRESENT_MODE_IMMEDIATE_KHR)
                              .add_fallback_present_mode(VK_PRESENT_MODE_MAILBOX_KHR)
                              .add_fallback_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                              .build();
  auto swapchain = unwrap(std::move(swapchain_ret), "create swapchain");

  if (m_swapchain.swapchain != VK_NULL_HANDLE) {
    vkb::destroy_swapchain(m_swapchain);
  }

  m_swapchain = swapchain;
  std::cout << "Swapchain present mode: " << presentModeName(m_swapchain.present_mode)
            << " (VkPresentModeKHR=" << static_cast<int>(m_swapchain.present_mode) << ")\n";
  m_swapchainImages = unwrap(m_swapchain.get_images(), "get swapchain images");
  m_swapchainImageViews = unwrap(m_swapchain.get_image_views(), "get swapchain image views");
  m_imagesInFlight.assign(m_swapchainImages.size(), VK_NULL_HANDLE);
}

void VulkanContext::createFrameResources() {
  for (auto &frame : m_frames) {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_graphicsFamily;
    checkVk(vkCreateCommandPool(m_device, &poolInfo, nullptr, &frame.pool), "create command pool");

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = frame.pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    checkVk(vkAllocateCommandBuffers(m_device, &allocInfo, &frame.cmd), "allocate command buffer");

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    checkVk(vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &frame.imageAvailable), "create imageAvailable semaphore");
    checkVk(vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &frame.renderFinished), "create renderFinished semaphore");

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    checkVk(vkCreateFence(m_device, &fenceInfo, nullptr, &frame.inFlight), "create frame fence");
  }
}

void VulkanContext::createDescriptorSetLayout() {
  VkDescriptorSetLayoutBinding bindings[3]{};
  bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  bindings[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

  VkDescriptorSetLayoutCreateInfo setInfo{};
  setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  setInfo.bindingCount = 3;
  setInfo.pBindings = bindings;
  checkVk(vkCreateDescriptorSetLayout(m_device, &setInfo, nullptr, &m_descriptorSetLayout), "create descriptor set layout");
}

void VulkanContext::createPipelineLayout() {
  VkPushConstantRange pushConstantRange{};
  pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pushConstantRange.offset = 0;
  pushConstantRange.size = sizeof(ComputePushConstants);

  VkPipelineLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutInfo.setLayoutCount = 1;
  layoutInfo.pSetLayouts = &m_descriptorSetLayout;
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges = &pushConstantRange;

  checkVk(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout), "create pipeline layout");
}

void VulkanContext::createPipelines() {
  // 1. Compute pipeline
  {
    VkShaderModule shader = createShaderModule(computeShaderPath);
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stage;
    pipelineInfo.layout = m_pipelineLayout;
    VkResult result = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_computePipeline);
    vkDestroyShaderModule(m_device, shader, nullptr);
    checkVk(result, "create compute pipeline");
  }

  // 2. Fullscreen blit graphics pipeline
  {
    VkShaderModule vertexShader = createShaderModule(vertexShaderPath);
    VkShaderModule fragmentShader = createShaderModule(fragmentShaderPath);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertexShader, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragmentShader, "main", nullptr};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.lineWidth = 1.0f;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.polygonMode = VK_POLYGON_MODE_FILL;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = 0xF;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &m_swapchain.image_format;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &rendering;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = m_pipelineLayout;

    checkVk(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline), "create graphics pipeline");
    vkDestroyShaderModule(m_device, vertexShader, nullptr);
    vkDestroyShaderModule(m_device, fragmentShader, nullptr);

    // 3. UI overlay graphics pipeline
    VkShaderModule overlayVertex = createShaderModule(overlayVertexShaderPath);
    VkShaderModule overlayFragment = createShaderModule(overlayFragmentShaderPath);
    VkPipelineShaderStageCreateInfo overlayStages[2]{};
    overlayStages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, overlayVertex, "main", nullptr};
    overlayStages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, overlayFragment, "main", nullptr};

    VkVertexInputBindingDescription binding{0, sizeof(OverlayVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(OverlayVertex, pos)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(OverlayVertex, color)}
    };
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions = attributes;
    pipelineInfo.pStages = overlayStages;

    VkPipelineColorBlendAttachmentState overlayBlendAttachment{};
    overlayBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    overlayBlendAttachment.blendEnable = VK_TRUE;
    overlayBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    overlayBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    overlayBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    overlayBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    overlayBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    overlayBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo overlayBlend{};
    overlayBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    overlayBlend.attachmentCount = 1;
    overlayBlend.pAttachments = &overlayBlendAttachment;
    pipelineInfo.pColorBlendState = &overlayBlend;

    checkVk(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_overlayPipeline), "create overlay pipeline");
    vkDestroyShaderModule(m_device, overlayVertex, nullptr);
    vkDestroyShaderModule(m_device, overlayFragment, nullptr);
  }
}

void VulkanContext::createDescriptorPool() {
  VkDescriptorPoolSize poolSizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kFramesInFlight},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kFramesInFlight},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFramesInFlight}
  };

  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = kFramesInFlight;
  poolInfo.poolSizeCount = 3;
  poolInfo.pPoolSizes = poolSizes;
  checkVk(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "create descriptor pool");

  std::array<VkDescriptorSetLayout, kFramesInFlight> layouts{};
  layouts.fill(m_descriptorSetLayout);
  std::array<VkDescriptorSet, kFramesInFlight> descriptorSets{};
  VkDescriptorSetAllocateInfo setAlloc{};
  setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  setAlloc.descriptorPool = m_descriptorPool;
  setAlloc.descriptorSetCount = kFramesInFlight;
  setAlloc.pSetLayouts = layouts.data();
  checkVk(vkAllocateDescriptorSets(m_device, &setAlloc, descriptorSets.data()), "allocate compute descriptor sets");

  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    m_frames[i].computeDescriptorSet = descriptorSets[i];
  }
}

void VulkanContext::createWorldBuffer() {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = sizeof(m_uploadedWorld);
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                           VMA_ALLOCATION_CREATE_MAPPED_BIT;
    checkVk(vmaCreateBuffer(m_allocator, &bufferInfo, &allocationInfo, &m_worldBuffer,
                            &m_worldAllocation, nullptr), "create world buffer");
    VmaAllocationInfo allocation{};
    vmaGetAllocationInfo(m_allocator, m_worldAllocation, &allocation);
    m_worldMappedData = allocation.pMappedData;

    for (auto& frame : m_frames) {
      VkDescriptorBufferInfo worldInfo{m_worldBuffer, 0, sizeof(m_uploadedWorld)};
      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = frame.computeDescriptorSet;
      write.dstBinding = 2;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write.pBufferInfo = &worldInfo;
      vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }
  }

void VulkanContext::destroyWorldBuffer() {
    if (m_worldBuffer) {
      vmaDestroyBuffer(m_allocator, m_worldBuffer, m_worldAllocation);
      m_worldBuffer = VK_NULL_HANDLE;
      m_worldAllocation = VK_NULL_HANDLE;
      m_worldMappedData = nullptr;
    }
  }

void VulkanContext::uploadWorld(const voxel::RenderState& state) {
    std::array<std::uint32_t, voxel::kVoxelCount * 2> packed{};
    for (std::size_t i = 0; i < voxel::kVoxelCount; ++i) {
      const auto& voxel = state.world[i];
      packed[i * 2] = voxel.material;
      packed[i * 2 + 1] = static_cast<std::uint32_t>(voxel.color.r) |
                          (static_cast<std::uint32_t>(voxel.color.g) << 8u) |
                          (static_cast<std::uint32_t>(voxel.color.b) << 16u) |
                          (static_cast<std::uint32_t>(voxel.color.a) << 24u);
    }
    if (!m_worldUploaded || packed != m_uploadedWorld) {
      std::memcpy(m_worldMappedData, packed.data(), sizeof(packed));
      m_uploadedWorld = packed;
      m_worldUploaded = true;
      vmaFlushAllocation(m_allocator, m_worldAllocation, 0, sizeof(packed));
  }
}

void VulkanContext::createComputeImages() {
  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = m_computeImageFormat;
  imageInfo.extent = {m_swapchain.extent.width, m_swapchain.extent.height, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VmaAllocationCreateInfo allocationInfo{};
  allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

  for (uint32_t index = 0; index < kFramesInFlight; ++index) {
    auto &frame = m_frames[index];
    checkVk(vmaCreateImage(m_allocator, &imageInfo, &allocationInfo, &frame.computeImage, &frame.computeAllocation, nullptr), "create compute image");

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = frame.computeImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_computeImageFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    checkVk(vkCreateImageView(m_device, &viewInfo, nullptr, &frame.computeImageView), "create compute image view");

    VkDescriptorImageInfo storageImage{VK_NULL_HANDLE, frame.computeImageView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo sampledImage{VK_NULL_HANDLE, frame.computeImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    VkWriteDescriptorSet writes[2]{};
    for (auto &write : writes) write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = frame.computeDescriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &storageImage;

    writes[1].dstSet = frame.computeDescriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[1].pImageInfo = &sampledImage;

    vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
    frame.computeImageInitialized = false;
  }
}

void VulkanContext::destroyComputeImages() {
  for (auto &frame : m_frames) {
    if (frame.computeImageView) {
      vkDestroyImageView(m_device, frame.computeImageView, nullptr);
      frame.computeImageView = VK_NULL_HANDLE;
    }
    if (frame.computeImage) {
      vmaDestroyImage(m_allocator, frame.computeImage, frame.computeAllocation);
      frame.computeImage = VK_NULL_HANDLE;
      frame.computeAllocation = VK_NULL_HANDLE;
    }
    frame.computeImageInitialized = false;
  }
}

void VulkanContext::updateOverlayBuffer(FrameData& frame, const std::vector<OverlayVertex>& vertices) {
  if (vertices.empty()) {
    frame.overlayVertexCount = 0;
    return;
  }

  const size_t requiredBytes = vertices.size() * sizeof(OverlayVertex);
  if (frame.overlayBufferCapacity < requiredBytes) {
    if (frame.overlayVertexBuffer) {
      vmaDestroyBuffer(m_allocator, frame.overlayVertexBuffer, frame.overlayVertexAllocation);
      frame.overlayVertexBuffer = VK_NULL_HANDLE;
      frame.overlayVertexAllocation = VK_NULL_HANDLE;
      frame.overlayMappedData = nullptr;
    }

    const size_t newCapacity = std::max(requiredBytes * 2, size_t{16384 * sizeof(OverlayVertex)});

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = newCapacity;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo mapped{};
    checkVk(vmaCreateBuffer(m_allocator, &bufferInfo, &allocationInfo, &frame.overlayVertexBuffer, &frame.overlayVertexAllocation, &mapped), "create overlay vertex buffer");
    frame.overlayMappedData = mapped.pMappedData;
    frame.overlayBufferCapacity = newCapacity;
  }

  std::memcpy(frame.overlayMappedData, vertices.data(), requiredBytes);
  vmaFlushAllocation(m_allocator, frame.overlayVertexAllocation, 0, requiredBytes);
  frame.overlayVertexCount = static_cast<uint32_t>(vertices.size());
}

void VulkanContext::cleanupSwapchain() {
  for (auto view : m_swapchainImageViews)
    vkDestroyImageView(m_device, view, nullptr);
  m_swapchainImageViews.clear();
}

void VulkanContext::recreateSwapchain() {
  VkExtent2D extent = m_window->framebufferExtent();
  if (extent.width == 0 || extent.height == 0)
    return;

  vkDeviceWaitIdle(m_device);
  cleanupSwapchain();
  createSwapchain();
  destroyComputeImages();
  createComputeImages();
  m_framebufferResized = false;
}

void VulkanContext::recordCommands(FrameData &frame, uint32_t imageIndex, float deltaTime,
                                   float totalTime, const voxel::RenderState& state) {
  VkCommandBuffer cmd = frame.cmd;
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  checkVk(vkBeginCommandBuffer(cmd, &begin), "begin command buffer");

  // 1. Transition compute image for storage writes
  VkImageMemoryBarrier2 preComputeBarrier{};
  preComputeBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  preComputeBarrier.srcStageMask = frame.computeImageInitialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
  preComputeBarrier.srcAccessMask = frame.computeImageInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0;
  preComputeBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  preComputeBarrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  preComputeBarrier.oldLayout = frame.computeImageInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  preComputeBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  preComputeBarrier.image = frame.computeImage;
  preComputeBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  VkDependencyInfo preComputeDep{};
  preComputeDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  preComputeDep.imageMemoryBarrierCount = 1;
  preComputeDep.pImageMemoryBarriers = &preComputeBarrier;
  vkCmdPipelineBarrier2(cmd, &preComputeDep);
  frame.computeImageInitialized = true;

  // 2. Dispatch compute shader with push constants
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &frame.computeDescriptorSet, 0, nullptr);

  ComputePushConstants pushConstants{
      m_swapchain.extent.width,
      m_swapchain.extent.height,
      totalTime,
      deltaTime,
      // Authoritative position is the player's feet; render from eye height.
      glm::vec4(state.localPlayer.position + glm::vec3(0.0f, 0.0f, 1.6f), 1.0f),
      glm::vec4(glm::normalize(glm::vec3(
          std::cos(glm::radians(state.localPlayer.yaw)) *
              std::cos(glm::radians(state.localPlayer.pitch)),
          std::sin(glm::radians(state.localPlayer.yaw)) *
              std::cos(glm::radians(state.localPlayer.pitch)),
          std::sin(glm::radians(state.localPlayer.pitch)))), 0.0f),
      voxel::kWorldEdge
  };
  vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants), &pushConstants);
  vkCmdDispatch(cmd, (m_swapchain.extent.width + 7) / 8, (m_swapchain.extent.height + 7) / 8, 1);

  // 3. Batched barrier:
  //    - Transition computeImage from GENERAL to SHADER_READ_ONLY_OPTIMAL (ready for fragment shader read)
  //    - Transition swapchain image from UNDEFINED to COLOR_ATTACHMENT_OPTIMAL (ready for render pass)
  VkImageMemoryBarrier2 postComputeBarriers[2]{};
  postComputeBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  postComputeBarriers[0].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  postComputeBarriers[0].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  postComputeBarriers[0].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  postComputeBarriers[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  postComputeBarriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  postComputeBarriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  postComputeBarriers[0].image = frame.computeImage;
  postComputeBarriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  postComputeBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  postComputeBarriers[1].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
  postComputeBarriers[1].srcAccessMask = 0;
  postComputeBarriers[1].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  postComputeBarriers[1].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  postComputeBarriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  postComputeBarriers[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  postComputeBarriers[1].image = m_swapchainImages[imageIndex];
  postComputeBarriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  VkDependencyInfo postComputeDep{};
  postComputeDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  postComputeDep.imageMemoryBarrierCount = 2;
  postComputeDep.pImageMemoryBarriers = postComputeBarriers;
  vkCmdPipelineBarrier2(cmd, &postComputeDep);

  // 4. Begin Dynamic Rendering with DONT_CARE load op (fullscreen quad immediately covers all pixels)
  VkRenderingAttachmentInfo attachment{};
  attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  attachment.imageView = m_swapchainImageViews[imageIndex];
  attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  VkRenderingInfo rendering{};
  rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  rendering.renderArea.extent = m_swapchain.extent;
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &attachment;

  vkCmdBeginRendering(cmd, &rendering);

  // 5. Fullscreen blit
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &frame.computeDescriptorSet, 0, nullptr);
  VkViewport vp{0.0f, 0.0f, static_cast<float>(m_swapchain.extent.width), static_cast<float>(m_swapchain.extent.height), 0.0f, 1.0f};
  VkRect2D scissor{{0, 0}, m_swapchain.extent};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdDraw(cmd, 3, 1, 0, 0);

  // 6. UI Overlay
  if (frame.overlayVertexCount > 0) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_overlayPipeline);
    VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &frame.overlayVertexBuffer, &vertexOffset);
    vkCmdDraw(cmd, frame.overlayVertexCount, 1, 0, 0);
  }

  vkCmdEndRendering(cmd);

  // 7. Transition swapchain image for presentation
  VkImageMemoryBarrier2 presentBarrier{};
  presentBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  presentBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  presentBarrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  presentBarrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
  presentBarrier.dstAccessMask = 0;
  presentBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  presentBarrier.image = m_swapchainImages[imageIndex];
  presentBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  VkDependencyInfo presentDep{};
  presentDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  presentDep.imageMemoryBarrierCount = 1;
  presentDep.pImageMemoryBarriers = &presentBarrier;
  vkCmdPipelineBarrier2(cmd, &presentDep);

  checkVk(vkEndCommandBuffer(cmd), "end command buffer");
}

void VulkanContext::drawFrame(float deltaTime, float totalTime,
                              const voxel::RenderState& state) {
  if (m_framebufferResized || m_window->isMinimized()) {
    recreateSwapchain();
    if (m_window->isMinimized() || m_swapchain.extent.width == 0 || m_swapchain.extent.height == 0)
      return;
  }

  auto &frame = m_frames[m_frameIndex];
  checkVk(vkWaitForFences(m_device, 1, &frame.inFlight, VK_TRUE, std::numeric_limits<uint64_t>::max()), "wait for frame fence");

  uint32_t imageIndex = 0;
  VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain.swapchain, std::numeric_limits<uint64_t>::max(),
                                          frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    recreateSwapchain();
    return;
  }
  const bool acquireSuboptimal = (result == VK_SUBOPTIMAL_KHR);
  if (result != VK_SUCCESS && !acquireSuboptimal)
    checkVk(result, "acquire swapchain image");

  if (m_imagesInFlight[imageIndex] != VK_NULL_HANDLE && m_imagesInFlight[imageIndex] != frame.inFlight)
    checkVk(vkWaitForFences(m_device, 1, &m_imagesInFlight[imageIndex], VK_TRUE, std::numeric_limits<uint64_t>::max()), "wait for swapchain image fence");
  m_imagesInFlight[imageIndex] = frame.inFlight;

  checkVk(vkResetFences(m_device, 1, &frame.inFlight), "reset frame fence");
  checkVk(vkResetCommandBuffer(frame.cmd, 0), "reset command buffer");

  if (deltaTime > 0.00001f) {
    float instantFps = 1.0f / deltaTime;
    m_rollingFps = 0.95f * m_rollingFps + 0.05f * instantFps;
  }
  const glm::vec3 viewDirection(
      std::cos(glm::radians(state.localPlayer.yaw)) *
          std::cos(glm::radians(state.localPlayer.pitch)),
      std::sin(glm::radians(state.localPlayer.yaw)) *
          std::cos(glm::radians(state.localPlayer.pitch)),
      std::sin(glm::radians(state.localPlayer.pitch)));
  const float health = state.localPlayer.grounded ? 0.85f : 0.80f;
  m_ui.update(
      {health,
       m_rollingFps,
       std::format("Position: {:.2f}, {:.2f}, {:.2f}",
                   state.localPlayer.position.x,
                   state.localPlayer.position.y,
                   state.localPlayer.position.z),
       std::format("Velocity: {:.2f}, {:.2f}, {:.2f}",
                   state.localPlayer.velocity.x,
                   state.localPlayer.velocity.y,
                   state.localPlayer.velocity.z),
       std::format("View: {:.2f}, {:.2f}, {:.2f}",
                   viewDirection.x, viewDirection.y, viewDirection.z)},
      m_swapchain.extent.width, m_swapchain.extent.height);
  updateOverlayBuffer(frame, m_ui.getVertices());

  uploadWorld(state);
  recordCommands(frame, imageIndex, deltaTime, totalTime, state);

  // Modern Vulkan 1.3 synchronization2 submission
  VkSemaphoreSubmitInfo waitInfo{};
  waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
  waitInfo.semaphore = frame.imageAvailable;
  waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

  VkCommandBufferSubmitInfo cmdInfo{};
  cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  cmdInfo.commandBuffer = frame.cmd;

  VkSemaphoreSubmitInfo signalInfo{};
  signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
  signalInfo.semaphore = frame.renderFinished;
  signalInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

  VkSubmitInfo2 submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
  submitInfo.waitSemaphoreInfoCount = 1;
  submitInfo.pWaitSemaphoreInfos = &waitInfo;
  submitInfo.commandBufferInfoCount = 1;
  submitInfo.pCommandBufferInfos = &cmdInfo;
  submitInfo.signalSemaphoreInfoCount = 1;
  submitInfo.pSignalSemaphoreInfos = &signalInfo;

  checkVk(vkQueueSubmit2(m_graphicsQueue, 1, &submitInfo, frame.inFlight), "submit frame");

  VkPresentInfoKHR present{};
  present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &frame.renderFinished;
  present.swapchainCount = 1;
  present.pSwapchains = &m_swapchain.swapchain;
  present.pImageIndices = &imageIndex;

  result = vkQueuePresentKHR(m_graphicsQueue, &present);
  if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
      acquireSuboptimal || m_framebufferResized) {
    recreateSwapchain();
  } else {
    checkVk(result, "present frame");
  }

  m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;
}

VkShaderModule VulkanContext::createShaderModule(const std::string &path) {
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file)
    throw std::runtime_error("cannot open compiled shader: " + path);
  const std::streamsize fileSize = file.tellg();
  if (fileSize <= 0 || fileSize % sizeof(uint32_t) != 0)
    throw std::runtime_error("compiled shader has invalid size: " + path);
  std::vector<uint32_t> code(static_cast<size_t>(fileSize) / sizeof(uint32_t));
  file.seekg(0);
  if (!file.read(reinterpret_cast<char *>(code.data()), fileSize))
    throw std::runtime_error("cannot read compiled shader: " + path);
  VkShaderModuleCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = code.size() * sizeof(uint32_t);
  info.pCode = code.data();
  VkShaderModule shader = VK_NULL_HANDLE;
  checkVk(vkCreateShaderModule(m_device, &info, nullptr, &shader), "create shader module");
  return shader;
}
