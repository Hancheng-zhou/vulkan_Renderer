// ===========================================================================
//  VkBackend/VulkanPipeline.h
//
//  职责边界：
//    管：VkPipelineLayout + VkGraphicsPipeline 的创建与销毁、SPIR-V 加载
//    不管：什么时候 bind、画多少个顶点（那是调用方录制的事）
//
//  管线是什么（OpenGL 里没有对应物）：
//    OpenGL 的渲染状态是【全局可变】的 —— glUseProgram、glDepthFunc、
//    glBlendFunc 改一次，后续绘制都按新状态走。
//
//    Vulkan 把这些状态【在任何绘制之前就固定下来】，打包成一个不可变的
//    VkPipeline。之后只能整块切换，不能改其中一项。
//
//    代价：改一个混合模式就要重建整条管线（可以派生，但本质仍是新对象）。
//    收益：驱动可以在创建时就把所有状态编译进 GPU 指令，
//          绘制时零状态校验，这是 Vulkan 高效的核心原因之一。
//
//  Dynamic Rendering 的特殊之处：
//    管线不再需要 VkRenderPass，但必须通过 pNext 告诉它
//    "我以后会渲染到什么格式的附件上"（VkPipelineRenderingCreateInfo）。
//    写错或不写，vkCreateGraphicsPipelines 会直接失败。
//
//  为什么视口/裁剪用 dynamic state：
//    它们依赖窗口尺寸，而窗口会 resize。做成动态的话 resize 只需在
//    命令里重设两个值；做成静态的话每次 resize 都要重建整条管线。
// ===========================================================================
#pragma once
#include<vulkan/vulkan.h>
#include<cstdint>
#include<string>
#include<vector>

class VulkanPipeline
{
public:
	struct CreateInfo
	{
		VkDevice device = VK_NULL_HANDLE;
		VkFormat colorFormat = VK_FORMAT_UNDEFINED;
		std::string vertexSpvPath;
		std::string fragSpvPath;
	};
	explicit VulkanPipeline(const CreateInfo& info);
	~VulkanPipeline();
	VulkanPipeline(const VulkanPipeline&) = delete;
	VulkanPipeline& operator=(const VulkanPipeline&) = delete;
	VkPipeline GetHandle() const { return m_Pipeline; }
	VkPipelineLayout GetLayout() const { return m_Layout; }

private:
	// 把 .spv 文件读成字节数组
	static std::vector<char> ReadFile(const std::string path);
	// 从字节码创建 shader module。
	// 【为什么不在构造后立刻销毁 module】module 在管线创建时被"编译吸收"，
	//   创建完就可以销毁。但如果将来要做多套管线复用同一批 shader，
	//   保留句柄更灵活。这里选择创建后立即销毁，减少资源占用。
	VkShaderModule CreateShaderModule(const std::vector<char>& code);
private:
	VkDevice m_Device = VK_NULL_HANDLE;
	VkPipelineLayout m_Layout = VK_NULL_HANDLE;
	VkPipeline m_Pipeline = VK_NULL_HANDLE;
};

