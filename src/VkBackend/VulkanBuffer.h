// ===========================================================================
//  VkBackend/VulkanBuffer.h
//
//  职责边界：
//    管：VkBuffer 对象的创建/销毁、向 allocator 申请 backing memory、bind
//    不管：显存怎么分（那是 VulkanAllocator 的事）
//
//  与 ZHC 的 VertexBuffer / IndexBuffer 的关系：
//    ZHC 分成两个类，因为 GL 里 VAO/VBO/EBO 是三套不同对象。
//    Vulkan 里两者都是 VkBuffer，只靠 usage 位区分 —— 所以合并成一个类，
//    由调用方在 CreateInfo 里指定 usage（VERTEX / INDEX / UNIFORM ...）。
// ===========================================================================
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

#include "VkBackend/VulkanAllocator.h"

class VulkanBuffer
{
public:
	struct CreateInfo
	{
		VkDeviceSize size = 0;
		VkBufferUsageFlags usage = 0;
		VulkanAllocator::Usage memoryUsage = VulkanAllocator::Usage::CPUToGPU;
		// 顶点数据通常要求 4 字节对齐，UBO 要求更严（见下）。
		// 传 0 表示"用 buffer 自己要求的最小对齐"（构造时从
		// VkMemoryRequirements::alignment 取）。
		VkDeviceSize alignment = 0;
	};
	VulkanBuffer(VkDevice device, VulkanAllocator& allocator, const CreateInfo& info);
	~VulkanBuffer();

	VulkanBuffer(const VulkanBuffer&) = delete;
	VulkanBuffer& operator=(const VulkanBuffer&) = delete;
	// ---- 数据上传 --------------------------------------------------------
	//
	//  仅对 CpuToGpu 有效。GpuOnly 内存 CPU 不可见，调它等于写野指针 ——
	//  所以这里显式抛异常，而不是静默失败。
	//  （GpuOnly 的上传要走 staging buffer，阶段 5 实现。）
	void Upload(const void* data, VkDeviceSize size, VkDeviceSize offset = 0);
	// ---- 访问 ------------------------------------------------------------
	VkBuffer     GetHandle() const { return m_Buffer; }
	VkDeviceSize GetSize()   const { return m_Size; }
	void* GetMappedPointer() const { return m_Allocation.mapped; }
	// 供 Pipeline 描述顶点布局用（阶段 5）。
	const VkMemoryRequirements& GetRequirements() const { return m_Requirements; }
private:
	VkDevice m_Device = VK_NULL_HANDLE;
	VulkanAllocator& m_Allocator;
	VkBuffer m_Buffer = VK_NULL_HANDLE;
	VkDeviceSize m_Size = 0;
	VkMemoryRequirements m_Requirements{};
	VulkanAllocator::Allocation m_Allocation;
	bool m_IsHostVisible = false;
};