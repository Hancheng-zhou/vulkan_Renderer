#include "FrameResources.h"
#include <stdexcept>
void FrameResources::CreateSyncObjects(VkDevice device)
{
	VkSemaphoreCreateInfo semaphoreInfo{};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &ImageAvailable) != VK_SUCCESS)
	{
		throw std::runtime_error("[FrameResources] vkCreateSemaphore failed");
	}

	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

	// 初始即 signaled，第一帧的等待才会立即通过。
	// 忘了这个标志 -> 启动即卡死在 vkWaitForFences。
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

	if (vkCreateFence(device, &fenceInfo, nullptr, &InFlight) != VK_SUCCESS)
	{
		throw std::runtime_error("[FrameResources] vkCreateFence failed");
	}

}

void FrameResources::DestroySyncObjects(VkDevice device)
{
	if (InFlight != VK_NULL_HANDLE)
	{
		vkDestroyFence(device, InFlight, nullptr);
		InFlight = VK_NULL_HANDLE;
	}
	if (ImageAvailable != VK_NULL_HANDLE)
	{
		vkDestroySemaphore(device, ImageAvailable, nullptr);
		ImageAvailable = VK_NULL_HANDLE;
	}
}
