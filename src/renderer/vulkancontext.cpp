#define VOLK_IMPLEMENTATION
#define VMA_IMPLEMENTATION
#include "vulkancontext.hpp"

#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

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
  createPipeline();
  createVertexBuffer();
}

VulkanContext::~VulkanContext() {
  if (m_device)
    vkDeviceWaitIdle(m_device);
  if (m_allocator && m_vertexBuffer)
    vmaDestroyBuffer(m_allocator, m_vertexBuffer, m_vertexAllocation);
  if (m_pipeline)
    vkDestroyPipeline(m_device, m_pipeline, nullptr);
  if (m_pipelineLayout)
    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
  for (auto &frame : m_frames) {
    if (frame.imageAvailable)
      vkDestroySemaphore(m_device, frame.imageAvailable, nullptr);
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
  auto instance = unwrap(vkb::InstanceBuilder(Window::getInstanceProcAddr()).set_app_name("Voxel Renderer").set_engine_name("Voxel Renderer").require_api_version(1, 3).enable_extensions(extensions).build(), "create Vulkan instance");
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

  auto swapchain_ret = builder.set_desired_extent(extent.width, extent.height).build();
  auto swapchain = unwrap(std::move(swapchain_ret), "create swapchain");

  // After creating the new one, we can safely destroy the old one's handle
  if (m_swapchain.swapchain != VK_NULL_HANDLE) {
    vkb::destroy_swapchain(m_swapchain);
  }

  m_swapchain = swapchain;
  m_swapchainImages = unwrap(m_swapchain.get_images(), "get swapchain images");
  m_swapchainImageViews = unwrap(m_swapchain.get_image_views(), "get swapchain image views");
  m_renderFinished.resize(m_swapchainImages.size());
  m_imagesInFlight.assign(m_swapchainImages.size(), VK_NULL_HANDLE);
  for (auto &semaphore : m_renderFinished) {
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    checkVk(vkCreateSemaphore(m_device, &info, nullptr, &semaphore), "create render semaphore");
  }
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
    checkVk(vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &frame.imageAvailable), "create image semaphore");
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    checkVk(vkCreateFence(m_device, &fenceInfo, nullptr, &frame.inFlight), "create frame fence");
  }
}

void VulkanContext::createPipeline() {
  VkShaderModule vertexShader = createShaderModule(vertexShaderPath);
  VkShaderModule fragmentShader = createShaderModule(fragmentShaderPath);
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertexShader, "main", nullptr};
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragmentShader, "main", nullptr};
  VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attributes[] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, pos)}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)}};
  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount = 2;
  vertexInput.pVertexAttributeDescriptions = attributes;
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
  VkPipelineLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  if (m_pipelineLayout == VK_NULL_HANDLE)
    checkVk(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout), "create pipeline layout");

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
}

void VulkanContext::createVertexBuffer() {
  const Vertex vertices[] = {{{0.0f, -0.5f}, {1.0f, 0.0f, 0.0f}}, {{0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}}, {{-0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}}};
  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = sizeof(vertices);
  bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  VmaAllocationCreateInfo allocationInfo{};
  allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
  allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VmaAllocationInfo mapped{};
  checkVk(vmaCreateBuffer(m_allocator, &bufferInfo, &allocationInfo, &m_vertexBuffer, &m_vertexAllocation, &mapped), "create vertex buffer");
  std::memcpy(mapped.pMappedData, vertices, sizeof(vertices));
}

void VulkanContext::cleanupSwapchain() {
  for (auto semaphore : m_renderFinished)
    vkDestroySemaphore(m_device, semaphore, nullptr);
  m_renderFinished.clear();
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
  m_framebufferResized = false;
}

void VulkanContext::recordCommands(VkCommandBuffer cmd, uint32_t imageIndex) {
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  checkVk(vkBeginCommandBuffer(cmd, &begin), "begin command buffer");

  VkImageMemoryBarrier2 barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  barrier.image = m_swapchainImages[imageIndex];
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
  barrier.srcAccessMask = 0;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkDependencyInfo dependency{};
  dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;

  vkCmdPipelineBarrier2(cmd, &dependency);

  VkClearValue clear{};
  clear.color = {{0.04f, 0.06f, 0.1f, 1.0f}};
  VkRenderingAttachmentInfo attachment{};
  attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  attachment.imageView = m_swapchainImageViews[imageIndex];
  attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.clearValue = clear;
  VkRenderingInfo rendering{};
  rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  rendering.renderArea.extent = m_swapchain.extent;
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &attachment;
  vkCmdBeginRendering(cmd, &rendering);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
  VkViewport vp{0, 0, static_cast<float>(m_swapchain.extent.width), static_cast<float>(m_swapchain.extent.height), 0, 1};
  VkRect2D scissor{{0, 0}, m_swapchain.extent};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexBuffer, &offset);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);

  barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
  barrier.dstAccessMask = 0;
  barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  vkCmdPipelineBarrier2(cmd, &dependency);

  checkVk(vkEndCommandBuffer(cmd), "end command buffer");
}

void VulkanContext::drawFrame() {
  auto &frame = m_frames[m_frameIndex];
  checkVk(vkWaitForFences(m_device, 1, &frame.inFlight, VK_TRUE, std::numeric_limits<uint64_t>::max()), "wait for frame fence");
  uint32_t imageIndex = 0;
  VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain.swapchain, std::numeric_limits<uint64_t>::max(), frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    recreateSwapchain();
    return;
  }
  if (result == VK_SUBOPTIMAL_KHR)
    result = VK_SUCCESS;
  checkVk(result, "acquire swapchain image");
  if (m_imagesInFlight[imageIndex] != VK_NULL_HANDLE && m_imagesInFlight[imageIndex] != frame.inFlight)
    checkVk(vkWaitForFences(m_device, 1, &m_imagesInFlight[imageIndex], VK_TRUE, std::numeric_limits<uint64_t>::max()), "wait for swapchain image fence");
  m_imagesInFlight[imageIndex] = frame.inFlight;
  checkVk(vkResetFences(m_device, 1, &frame.inFlight), "reset frame fence");
  checkVk(vkResetCommandBuffer(frame.cmd, 0), "reset command buffer");
  recordCommands(frame.cmd, imageIndex);
  VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &frame.imageAvailable;
  submit.pWaitDstStageMask = &waitStage;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &frame.cmd;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &m_renderFinished[imageIndex];
  checkVk(vkQueueSubmit(m_graphicsQueue, 1, &submit, frame.inFlight), "submit frame");
  VkPresentInfoKHR present{};
  present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &m_renderFinished[imageIndex];
  present.swapchainCount = 1;
  present.pSwapchains = &m_swapchain.swapchain;
  present.pImageIndices = &imageIndex;
  result = vkQueuePresentKHR(m_graphicsQueue, &present);
  if (result == VK_ERROR_OUT_OF_DATE_KHR || m_framebufferResized)
    recreateSwapchain();
  else if (result != VK_SUBOPTIMAL_KHR)
    checkVk(result, "present frame");
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