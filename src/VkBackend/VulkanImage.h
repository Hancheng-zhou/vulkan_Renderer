// ===========================================================================
//  VkBackend/VulkanImage.h
//
//  职责边界：
//    管：VkImage + VkImageView 的创建/销毁、向 allocator 申请 backing memory、
//        image layout 的转换
//    不管：显存怎么分（VulkanAllocator）、画什么（Renderer）
//
//  VkImage 是什么（OpenGL 里对应 glGenTextures 那一类对象）：
//    一块二维（或三维/数组）像素存储。它【不自带】任何"用途"——
//    是纹理、是渲染目标、是深度附件，取决于你创建时的 usage 位
//    和使用时转换到的 layout。
//
//  与 VulkanBuffer 的对称性：
//    Buffer = 线性内存，元素定长，GPU 按索引取
//    Image  = 带 tiling 的二维网格，GPU 按 (x,y) 采样
//    两者的创建流程完全一样：create 对象 -> 查内存要求 -> 分配 -> bind。
//    所以本类的结构和 VulkanBuffer 高度相似，这是刻意的。
//
//  【为什么 layout 要在本类里维护】
//    layout 不是"设一次就完"的状态，每次用途变化都要转换并更新记录。
//    让调用方各自记一份，早晚会不一致；收在 m_CurrentLayout 里，
//    转换时自动把上一次的目标当作这一次的起点，不可能对不上。
// ===========================================================================
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

#include "VkBackend/VulkanAllocator.h"
class VulkanImage
{
public:
	struct CreateInfo {
		VkDevice Device = VK_NULL_HANDLE;
		VulkanAllocator* Allocator = nullptr;
		uint32_t Width = 0;
		uint32_t Height = 0;
		// 像素格式。深度附件用 VK_FORMAT_D32_SFLOAT，颜色用 B8G8R8A8_SRGB 之类
		VkFormat Format = VK_FORMAT_UNDEFINED;
		// 用途位。深度附件是 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT，
		// 纹理是 SAMPLED_BIT | TRANSFER_DST_BIT，渲染目标候选还要 COLOR_ATTACHMENT_BIT。
		//
		// 【为什么必须显式给】驱动会根据 usage 决定 tiling 方式。
		//   少写一位，创建可能成功，但用到时校验层报 "image was not created with ..."。
		VkImageUsageFlags Usage = 0;
		// imageView 的 aspect。颜色 = COLOR_BIT，深度 = DEPTH_BIT。
		// 用错的表现是 imageView 创建失败或渲染时整块黑。
		VkImageAspectFlags Aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		// 采样数。默认 1 = 不开 MSAA。
		//
		// 【必须是 2 的幂对应的枚举值】VK_SAMPLE_COUNT_4_BIT 就是 4，
		//   不是位掩码（和 framebufferColorSampleCounts 完全不同的用法，
		//   这是 Vulkan 里最容易混的一对概念）。
		//
		// 【关键约束】同一个 rendering 里，颜色附件和深度附件的 samples
		//   必须完全相同，pipeline 的 rasterizationSamples 也必须相同。
		//   三个里错一个，vkCreateGraphicsPipelines 或 vkCmdBeginRendering
		//   就会失败。
		VkSampleCountFlagBits Samples = VK_SAMPLE_COUNT_1_BIT;
	};
	explicit VulkanImage(const CreateInfo& info);
	~VulkanImage();

	VulkanImage(const VulkanImage&) = delete;
	VulkanImage& operator=(const VulkanImage&) = delete;
	// ---- 访问 ------------------------------------------------------------
	VkImage     GetHandle() const { return m_Image; }
	VkImageView GetView()   const { return m_View; }
	VkFormat    GetFormat() const { return m_Format; }
	VkExtent2D  GetExtent() const { return m_Extent; }
	// 当前 layout。通常不需要读，但诊断"布局转换漏了哪一步"时很有用。
	VkImageLayout GetLayout() const { return m_CurrentLayout; }
	// ---- 用途转换 --------------------------------------------------------
	//
	// 【这是 Vulkan 独有的概念，OpenGL 里没有】
	//   同一张图在不同时刻的物理排布可能不同：
	//   作为 COLOR_ATTACHMENT 时驱动可能用某种压缩布局，
	//   作为 SAMPLED 时又换成适合采样的排布。
	//   切换必须显式声明，并插屏障让驱动有机会重排、让写入对后续读取可见。
	//
	// 【参数怎么填】四个参数是一组，描述"从哪个阶段/访问 -> 到哪个阶段/访问"：
	//   深度附件：EARLY_FRAGMENT_TESTS，DEPTH_STENCIL_ATTACHMENT_READ|WRITE
	//   颜色附件：COLOR_ATTACHMENT_OUTPUT，COLOR_ATTACHMENT_WRITE
	//   被采样：  FRAGMENT_SHADER，SHADER_READ
	//
	// 【为什么不自己算 srcStage/srcAccess】
	//   因为 m_CurrentLayout 记录了上一次的目标，但"上一次是谁写的"
	//   只有调用方知道（可能是渲染、可能是拷贝）。所以这里由调用方给，
	//   本类只负责 oldLayout 的正确性。
	void TransitionLayout(VkCommandBuffer    cmd,
		VkImageLayout      newLayout,
		VkPipelineStageFlags srcStage,
		VkPipelineStageFlags dstStage,
		VkAccessFlags      srcAccess,
		VkAccessFlags      dstAccess);


private:
	VkDevice         m_Device = VK_NULL_HANDLE;
	VulkanAllocator* m_Allocator = nullptr;

	VkImage     m_Image = VK_NULL_HANDLE;
	VkImageView m_View = VK_NULL_HANDLE;

	VkFormat   m_Format = VK_FORMAT_UNDEFINED;
	VkExtent2D m_Extent{ 0, 0 };

	VulkanAllocator::Allocation m_Allocation{};

	// 创建时必然是 UNDEFINED（VkImageCreateInfo::initialLayout 只允许
	// UNDEFINED / PREINITIALIZED，前者表示"内容不可用"）。
	VkImageLayout m_CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};