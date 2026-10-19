#include "VkBackend/VulkanCommandPool.h"
#include <stdexcept>
#include <string>

namespace
{
	void ThrowOnFailed(VkResult result, const char* whatFailed)
	{
		if (result != VK_SUCCESS)
		{
			throw std::runtime_error(std::string("[VulkanCommandPool] ") + whatFailed +
				" failed, VkResult = " +
				std::to_string(static_cast<int>(result)));
		}
	}
}
// ---------------------------------------------------------------------------
//  VulkanCommandPool
// ---------------------------------------------------------------------------

VulkanCommandPool::VulkanCommandPool(VkDevice device, uint32_t queueFamily, uint32_t bufferCount)
	:m_Device(device)
{
	if (bufferCount == 0)
	{
		throw std::runtime_error("[VulkanCommandPool] bufferCount must be > 0");

	}
	VkCommandPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = queueFamily;
	ThrowOnFailed(vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_Pool), "vkCreateCommandPool");
	m_Buffers.resize(bufferCount, VK_NULL_HANDLE);

	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = m_Pool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = bufferCount;
	ThrowOnFailed(vkAllocateCommandBuffers(m_Device, &allocInfo, m_Buffers.data()),
		"vkAllocateCommandBuffers");
}

VulkanCommandPool::~VulkanCommandPool()
{
	// buffer 归 pool 所有，销毁 pool 时一并回收，不需要单独 free。
	m_Buffers.clear();

	if (m_Pool != VK_NULL_HANDLE)
	{
		// 【要求 GPU 不在执行这些缓冲】由 Application::Shutdown 里的
		// vkDeviceWaitIdle 保证。
		vkDestroyCommandPool(m_Device, m_Pool, nullptr);
		m_Pool = VK_NULL_HANDLE;
	}
}
void VulkanCommandPool::ResetAll()
{
	// flags = 0：释放所有命令占用的资源，让缓冲回到初始状态。
	// 注意这里重置的是【整个 pool】，所以要求所有缓冲都不在 pending。
	ThrowOnFailed(vkResetCommandPool(m_Device, m_Pool, 0), "vkResetCommandPool");
}

// ---------------------------------------------------------------------------
//  CommandRecording
// ---------------------------------------------------------------------------

CommandRecording::CommandRecording(VkCommandBuffer commandBuffer)
	:m_CommandBuffer(commandBuffer)
{
	if (m_CommandBuffer == VK_NULL_HANDLE)
	{
		throw std::runtime_error("[CommandRecording] command buffer is null");
	}
	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	ThrowOnFailed(vkBeginCommandBuffer(m_CommandBuffer, &beginInfo), "vkBeginCommandBuffer");

}

CommandRecording::~CommandRecording()
{
	if (!m_Ended && m_CommandBuffer != VK_NULL_HANDLE)
	{
		vkEndCommandBuffer(m_CommandBuffer);
		m_Ended = true;
	}
}

void CommandRecording::End()
{
	if (m_Ended)
	{
		return;
	}
	ThrowOnFailed(vkEndCommandBuffer(m_CommandBuffer), "vkEndCommandBuffer");
}
