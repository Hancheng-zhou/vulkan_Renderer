// ===========================================================================
//  VkBackend/VulkanDescriptor.h
//
//  职责边界：
//    管：VkDescriptorSetLayout / VkDescriptorPool / VkDescriptorSet 的创建与销毁、
//        把 buffer 写进某个 set 的某个 binding
//    不管：buffer 里放什么数据（那是调用方的事）
//
//  Descriptor 是什么（OpenGL 里没有对应物）：
//    OpenGL 里 shader 可以直接读任意绑定的 buffer（glBindBufferBase），
//    驱动在背后做地址映射。
//
//    Vulkan 要求你【显式声明】：shader 会访问几个资源、每个是什么类型、
//    在哪个 set / binding 上。这套声明叫 DescriptorSetLayout，
//    由它创建的实例叫 DescriptorSet，里面装的才是真正的 buffer 句柄。
//
//    为什么要这么麻烦：这套声明在【管线创建时】就被编译进 GPU 指令了。
//    绘制时切换资源只是改一块显存里的指针表，零状态校验 —— 这是 Vulkan
//    高效的关键。代价是配置错了很难查，因为错误发生在创建期而不是绘制期。
//
//  set / binding 的编号规则：
//    shader 里写 layout(set = 0, binding = 0)，
//    C++ 里就必须用同样的 0 / 0 去写。两者对不上时，shader 读到的是
//    未定义数据（不是空数据），表现为画面完全错乱但不报错。
// ===========================================================================
#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

class VulkanDescriptor 
{
public:
	VulkanDescriptor(VkDevice device, uint32_t bindingCount, uint32_t frameCount);
	~VulkanDescriptor();

	VulkanDescriptor(const VulkanDescriptor&) = delete;
	VulkanDescriptor& operator=(const VulkanDescriptor&) = delete;
	VkDescriptorSetLayout GetLayout() const { return m_Layout; }
	VkDescriptorSet       GetSet(uint32_t frame) const { return m_Sets[frame]; }
	// 把 buffer 绑到第 frame 帧的 set 的第 binding 槽上。
	//
	// 【为什么每帧一个 set】CPU 写第 N 帧的 UBO 时，GPU 可能还在读第 N-1 帧的。
	//   共用一个 set + 一个 buffer 会让两帧的数据互相覆盖，画面表现为抖动。
	void BindUniformBuffer(uint32_t frame, uint32_t binding,
		VkBuffer buffer, VkDeviceSize size);

private:
	VkDevice m_Device = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_Layout = VK_NULL_HANDLE;
	VkDescriptorPool m_Pool = VK_NULL_HANDLE;
	std::vector<VkDescriptorSet> m_Sets;

	uint32_t m_BindingCount = 0;
};

