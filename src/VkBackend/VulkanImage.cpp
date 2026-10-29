#include "VkBackend/VulkanImage.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace
{
    void ThrowOnFailed(VkResult result, const char* whatFailed)
    {
        if (result != VK_SUCCESS)
        {
            throw std::runtime_error(std::string("[VulkanImage] ") + whatFailed +
                " failed, VkResult = " +
                std::to_string(static_cast<int>(result)));
        }
    }
}

VulkanImage::VulkanImage(const CreateInfo& info)
    : m_Device(info.Device)
    , m_Allocator(info.Allocator)
    , m_Format(info.Format)
    , m_Extent{ info.Width, info.Height }
{
    if (m_Device == VK_NULL_HANDLE || m_Allocator == nullptr)
    {
        throw std::runtime_error("[VulkanImage] device or allocator is null");
    }
    if (info.Width == 0 || info.Height == 0)
    {
        throw std::runtime_error("[VulkanImage] width/height must not be 0");
    }
    if (info.Format == VK_FORMAT_UNDEFINED)
    {
        throw std::runtime_error("[VulkanImage] format must not be UNDEFINED");
    }

    // ---- 1. 创建 VkImage -------------------------------------------------
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = info.Format;
    imageInfo.extent = { info.Width, info.Height, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;

    // 采样数。1 = 不开 MSAA。
    // 【这一项必须和 Pipeline 的 rasterizationSamples、
    //   以及同一个 rendering 里其他附件的 samples 完全一致】
    imageInfo.samples = info.Samples;

    // 【OPTIMAL vs LINEAR】OPTIMAL 是驱动自己挑最好的排布（可能带压缩、
    //   可能重排像素顺序），GPU 读写最快，但 CPU 不能直接理解其内存布局。
    //   LINEAR 是逐行排列，CPU 能 mmap 后逐像素读写，但 GPU 侧慢。
    //   渲染附件一律用 OPTIMAL。
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;

    imageInfo.usage = info.Usage;

    // 只有一个队列族访问。跨族共享才需要 CONCURRENT。
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    // 【只有这两个值合法】UNDEFINED = "内容不可用，第一次用之前必须转换"。
    //   填成 COLOR_ATTACHMENT_OPTIMAL 会直接被校验层拒绝 ——
    //   创建时不可能已经处于那个 layout。
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    ThrowOnFailed(vkCreateImage(m_Device, &imageInfo, nullptr, &m_Image),
        "vkCreateImage");

    // ---- 2. 查内存要求 ---------------------------------------------------
    //
    // 【为什么不能自己算大小】tiling OPTIMAL 时，驱动可能加 padding、
    //   可能对宽高做对齐（比如宽度向上取整到 64 像素的倍数）。
    //   真实需要的字节数只有驱动知道。自己算 width*height*4 是不够的。
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(m_Device, m_Image, &requirements);

    // ---- 3. 分配并绑定 ---------------------------------------------------
    //
    // 【注意是 GPUOnly】图像永远在显存里。即使要做 staging 上传，
    //   也是"CPU 写中转 buffer -> GPU 拷贝进 image"，image 本身
    //   从不需要 HOST_VISIBLE。
    m_Allocation = m_Allocator->Allocate(requirements.size,
        requirements.alignment,
        requirements.memoryTypeBits,
        VulkanAllocator::Usage::GPUOnly);

    ThrowOnFailed(vkBindImageMemory(m_Device, m_Image,
        m_Allocation.memory, m_Allocation.offset),
        "vkBindImageMemory");

    // ---- 4. 创建 imageView -----------------------------------------------
    //
    // 【为什么必须要 view】VkImage 只是存储，Pipeline 和渲染附件要的是
    //   VkImageView —— 它描述"怎么看待这张图"：哪几个 mip、哪几层、
    //   哪些通道、什么 aspect。同一张图可以建多个 view 用在不同场合。
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_Image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = info.Format;

    viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

    // 【aspect 必须和格式匹配】D32_SFLOAT 只用 DEPTH_BIT。
    //   如果格式带 stencil（D24_UNORM_S8_UINT），这里要写 DEPTH_BIT | STENCIL_BIT。
    //   写错的典型表现：vkCreateImageView 返回 VK_ERROR_FORMAT_MISMATCH。
    viewInfo.subresourceRange.aspectMask = info.Aspect;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    ThrowOnFailed(vkCreateImageView(m_Device, &viewInfo, nullptr, &m_View),
        "vkCreateImageView");
}

VulkanImage::~VulkanImage()
{
    // 销毁顺序：view -> image -> 显存。
    //
    // 【view 必须最先】它是 image 的"解释器"，image 没了它就是悬垂句柄。
    // 【显存最后】image 还在引用那段内存时不能归还。
    //   Vulkan 不检查这个顺序，错了会在随机时刻崩在驱动内部。
    if (m_View != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_Device, m_View, nullptr);
        m_View = VK_NULL_HANDLE;
    }

    if (m_Image != VK_NULL_HANDLE)
    {
        vkDestroyImage(m_Device, m_Image, nullptr);
        m_Image = VK_NULL_HANDLE;
    }

    m_Allocator->Free(m_Allocation);
    m_Allocation = {};
}

void VulkanImage::TransitionLayout(VkCommandBuffer      cmd,
    VkImageLayout        newLayout,
    VkPipelineStageFlags srcStage,
    VkPipelineStageFlags dstStage,
    VkAccessFlags        srcAccess,
    VkAccessFlags        dstAccess)
{
    // 已经是目标布局：不需要转换。
    //
    // 【为什么不无脑每次都插屏障】同布局屏障虽然合法，但会让驱动
    //   多做一次无谓的排布校验。而"同布局"这个判断只有维护了
    //   m_CurrentLayout 才能做 —— 这正是把它放进本类的原因。
    if (m_CurrentLayout == newLayout)
    {
        return;
    }

    // 【UNDEFINED 起点要特殊处理】
    //   声明 oldLayout = UNDEFINED 等于告诉驱动"这里的旧内容可以丢"。
    //   此时 srcAccessMask 必须为 0、srcStage 用 TOP_OF_PIPE ——
    //   因为没有"之前"的操作需要等待。
    //   填了非零值不会崩，但校验层的 sync validation 会报
    //   SYNC-HAZARD-WRITE-AFTER-READ 之类的噪音，让人误以为有真 bug。
    if (m_CurrentLayout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        srcAccess = 0;
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = m_CurrentLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_Image;
    barrier.subresourceRange.aspectMask = m_Format == VK_FORMAT_D32_SFLOAT
        ? VK_IMAGE_ASPECT_DEPTH_BIT
        : VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;

    vkCmdPipelineBarrier(cmd,
        srcStage,
        dstStage,
        0,
        0, nullptr,   // 无全局内存屏障
        0, nullptr,   // 无 buffer 屏障
        1, &barrier);

    m_CurrentLayout = newLayout;
}
