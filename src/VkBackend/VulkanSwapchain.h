#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>


class VulkanSwapchain
{
public:
	struct CreateInfo
	{
		VkPhysicalDevice PhysicalDevice;
		VkDevice Device;
		VkSurfaceKHR Surface;
		uint32_t GraphicsQueueFamily;
		uint32_t PresentQueueFamily;
		uint32_t Width;
		uint32_t Height;
	};
	explicit VulkanSwapchain(const CreateInfo& info);
	~VulkanSwapchain();

	VulkanSwapchain(const VulkanSwapchain&) = delete;
	VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;
	bool Recreate(uint32_t width, uint32_t height);   // 返回 false = 本次无法重建（尺寸为0）
	//-------------查询-------------------------
	VkSwapchainKHR GetHandle()     const { return m_Swapchain; }
	VkFormat       GetFormat()     const { return m_ImageFormat; }
	VkExtent2D     GetExtent()     const { return m_Extent; }
	std::vector<VkImage> GetImages() const { return m_Images; }
	uint32_t       GetImageCount() const { return static_cast<uint32_t>(m_Images.size()); }
	const std::vector<VkImageView>& GetImageViews() const { return m_ImageViews; }
	void PrintInfo() const;
private:
	void Create(bool useOldSwapchain);// 真正的创建逻辑，构造和 Recreate 共用
	void Destroy();// 只销毁 imageView + swapchain，不碰 device/surface
	//-------------内部工具----------------------
	VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
	VkPresentModeKHR   ChoosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) const;
	VkExtent2D         ChooseExtent(const VkSurfaceCapabilitiesKHR& surfaceCaps, uint32_t w, uint32_t h) const;
private:
	//创建参数
	VkPhysicalDevice m_PhysicalDevice;
	VkDevice m_Device;
	VkSurfaceKHR m_Surface;
	uint32_t m_GraphicsQueueFamily;
	uint32_t m_PresentQueueFamily;
	uint32_t m_RequestedWidth;    // 请求尺寸，不是协商后的 extent
	uint32_t m_RequestedHeight;
	//当前这一代交换链产物
	VkSwapchainKHR           m_Swapchain = VK_NULL_HANDLE;    // VK_NULL_HANDLE 初始化
	std::vector<VkImage>     m_Images;       // 交换链创建，我们【不能】销毁
	std::vector<VkImageView> m_ImageViews;   // 我们创建，【必须】销毁
	VkFormat         m_ImageFormat = VK_FORMAT_UNDEFINED;          // 初始 VK_FORMAT_UNDEFINED
	VkPresentModeKHR m_PresentMode = VK_PRESENT_MODE_FIFO_KHR;          // 初始 VK_PRESENT_MODE_FIFO_KHR
	VkExtent2D       m_Extent = { 0,0 };               // 初始 {0, 0}


};

