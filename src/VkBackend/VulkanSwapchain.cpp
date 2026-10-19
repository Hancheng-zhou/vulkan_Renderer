#include "VkBackend/VulkanSwapchain.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
namespace
{
    constexpr VkFormat       k_PreferredFormat = VK_FORMAT_B8G8R8A8_SRGB;
    constexpr VkPresentModeKHR k_PreferredPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
}
VulkanSwapchain::VulkanSwapchain(const CreateInfo& info)
    :m_PhysicalDevice(info.PhysicalDevice)
    ,m_Device(info.Device)
    , m_Surface(info.Surface)
    , m_GraphicsQueueFamily(info.GraphicsQueueFamily)
    , m_PresentQueueFamily(info.PresentQueueFamily)
    , m_RequestedWidth(info.Width)
    , m_RequestedHeight(info.Height)
{
    Create(false);
}

VulkanSwapchain::~VulkanSwapchain()
{
    Destroy();
}

void VulkanSwapchain::PrintInfo() const
{
    const char* formatName = "UNKNOWN";
    switch (m_ImageFormat)
    {
    case VK_FORMAT_B8G8R8A8_SRGB:  formatName = "B8G8R8A8_SRGB";  break;
    case VK_FORMAT_B8G8R8A8_UNORM: formatName = "B8G8R8A8_UNORM"; break;
    case VK_FORMAT_R8G8B8A8_SRGB:  formatName = "R8G8B8A8_SRGB";  break;
    case VK_FORMAT_R8G8B8A8_UNORM: formatName = "R8G8B8A8_UNORM"; break;
    default: break;
    }

    const char* modeName = "UNKNOWN";
    switch (m_PresentMode)
    {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:    modeName = "IMMEDIATE";    break;
    case VK_PRESENT_MODE_FIFO_KHR:         modeName = "FIFO";         break;
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: modeName = "FIFO_RELAXED"; break;
    case VK_PRESENT_MODE_MAILBOX_KHR:      modeName = "MAILBOX";      break;
    default: break;
    }

    std::printf("[Swapchain] images       : %u\n", GetImageCount());
    std::printf("[Swapchain] extent       : %u x %u\n", m_Extent.width, m_Extent.height);
    std::printf("[Swapchain] format       : %s\n", formatName);
    std::printf("[Swapchain] present mode : %s\n", modeName);
    std::printf("[Swapchain] sharing      : %s\n",
        m_GraphicsQueueFamily == m_PresentQueueFamily ? "EXCLUSIVE" : "CONCURRENT");



}

void VulkanSwapchain::Create(bool useOldSwapchain)
{
    vkDeviceWaitIdle(m_Device);
    //1、首先查询surface能力
    VkSurfaceCapabilitiesKHR caps;

    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_PhysicalDevice, m_Surface, &caps);
    if (r != VK_SUCCESS)
        throw std::runtime_error("[VulkanSwapchain] vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed");

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, m_Surface, &formatCount, nullptr);
    if (formatCount == 0)
        throw std::runtime_error("formatCount =0");
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, m_Surface, &formatCount, formats.data());

    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_PhysicalDevice, m_Surface, &modeCount, nullptr);
    if(modeCount ==0)
        throw std::runtime_error("modeCount =0");
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_PhysicalDevice, m_Surface, &modeCount, modes.data());

    //2、协商并记录
    const VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(formats);
    const VkPresentModeKHR presentMode = ChoosePresentMode(modes);
    const VkExtent2D extent = ChooseExtent(caps, m_RequestedWidth, m_RequestedHeight);
    if (extent.width == 0 || extent.height == 0)
        throw std::runtime_error("[VulkanSwapchain] extent is 0 (window minimized?)");
    //3、算图像数量
    uint32_t imageCount = caps.minImageCount + 1;
    // maxImageCount == 0 表示【无上限】，不是"最多 0 张"
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
    {
        imageCount = caps.maxImageCount;
    }
    //4、填info
    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = m_Surface;
    //图像数量
    createInfo.minImageCount = imageCount;
    //格式+色彩空间
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    //尺寸
    createInfo.imageExtent = extent;
    //层数
    createInfo.imageArrayLayers = 1;
    //用途 ：纯渲染 -> COLOR_ATTACHMENT
    // 若后续要后处理/截图，再加 SAMPLED_BIT / TRANSFER_SRC_BIT
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;

    //队列族共享模式
    const uint32_t queueFamilyIndices[] = { m_GraphicsQueueFamily,m_PresentQueueFamily };
    if (m_GraphicsQueueFamily != m_PresentQueueFamily)
    {
        // graphics 和 present 不是同一族 → 必须 CONCURRENT
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices; 
    }
    else
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.queueFamilyIndexCount = 0;
        createInfo.pQueueFamilyIndices = nullptr;
    }

    //预变换
    createInfo.preTransform = caps.currentTransform;
    //合成alpha
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    //呈现模式
    createInfo.presentMode = presentMode;
    //clipped
    createInfo.clipped = VK_TRUE;
    //oldSwapchain
    createInfo.oldSwapchain = useOldSwapchain ? m_Swapchain : VK_NULL_HANDLE;
    //5、先建后毁
    VkSwapchainKHR oldSwapchain = m_Swapchain;
    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    VkResult result = vkCreateSwapchainKHR(m_Device, &createInfo, nullptr, &newSwapchain);
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error("[VulkanSwapchain] vkCreateSwapchainKHR failed");
    }
    m_ImageFormat = surfaceFormat.format;
    m_PresentMode = presentMode;
    m_Extent = extent;
    for (auto view : m_ImageViews)
    {
        vkDestroyImageView(m_Device, view, nullptr);
    }
    m_ImageViews.clear();
    m_Images.clear();
    m_Swapchain = newSwapchain;
    if (oldSwapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(m_Device, oldSwapchain, nullptr);
    }
    //6、取图像
    uint32_t actualCount = 0;
    r = vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &actualCount, nullptr);
    if (r != VK_SUCCESS || actualCount == 0)
        throw std::runtime_error("[VulkanSwapchain] vkGetSwapchainImagesKHR(count) failed");
    m_Images.resize(actualCount);
    r = vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &actualCount, m_Images.data());
    if (r != VK_SUCCESS)
        throw std::runtime_error("[VulkanSwapchain] vkGetSwapchainImagesKHR(data) failed");
    //7、创建imageviews
    std::vector<VkImageView> newViews(actualCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < actualCount; ++i)
    {   
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_Images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = m_ImageFormat;
        viewInfo.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        VkResult r = vkCreateImageView(m_Device, &viewInfo, nullptr, &newViews[i]);
        if (r != VK_SUCCESS)
        {
            for (auto v : newViews) if (v) vkDestroyImageView(m_Device, v, nullptr);
            vkDestroySwapchainKHR(m_Device, newSwapchain, nullptr);
            m_Swapchain = VK_NULL_HANDLE;   // ★ 新 swapchain 已销毁，成员也清掉
            throw std::runtime_error("[VulkanSwapchain] vkCreateImageView failed");
        }

    }
    m_ImageViews = std::move(newViews);

}

VkSurfaceFormatKHR VulkanSwapchain::ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const
{
    // ① 特殊情况：surface 没有首选格式（老驱动 / 某些平台）
    //    规范：若只返回一个元素且 format == VK_FORMAT_UNDEFINED，
    //    表示可以自由选择，此时用你想要的格式即可。
    if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED)
    {
        return { k_PreferredFormat, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    }
    for (const auto& candidate : formats)
    {
        if (candidate.format == k_PreferredFormat && 
            candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        return candidate;
    }
    for (const auto& candidate : formats)
    {
        if (candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            return candidate;
        }
    }

    return formats[0];
}

VkPresentModeKHR VulkanSwapchain::ChoosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) const
{
    for (const auto& mode : presentModes)
        if (mode == k_PreferredPresentMode)
            return mode;
    // FIFO 是规范强制要求必须支持的，永远存在，且不撕裂
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanSwapchain::ChooseExtent(const VkSurfaceCapabilitiesKHR& surfaceCaps, uint32_t w, uint32_t h) const
{
    //驱动已经指定尺寸
    if (surfaceCaps.currentExtent.width != 0xFFFFFFFF)
        return surfaceCaps.currentExtent;
    //驱动允许自定尺寸 但是要clamp到 min-max
    VkExtent2D extent{};
    extent.width = std::clamp(w, surfaceCaps.minImageExtent.width, surfaceCaps.maxImageExtent.width);
    extent.height = std::clamp(h, surfaceCaps.minImageExtent.height, surfaceCaps.maxImageExtent.height);
    return extent;
}

void VulkanSwapchain::Destroy()
{
    for (auto view : m_ImageViews)
    {
        vkDestroyImageView(m_Device, view, nullptr);
    }
    m_ImageViews.clear();
    if (m_Swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
        m_Swapchain = VK_NULL_HANDLE;
        m_Images.clear();
    }
}
bool VulkanSwapchain::Recreate(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return false;

    }

    vkDeviceWaitIdle(m_Device);
    m_RequestedWidth = width;
    m_RequestedHeight = height;
    Create(true);
    std::printf("[Swapchain] recreated: requested %u x %u, extent %u x %u, %u images\n",
        width, height, m_Extent.width, m_Extent.height, GetImageCount());
    return true;
}