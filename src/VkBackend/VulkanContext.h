// ===========================================================================
//  VkBackend/VulkanContext.h
//
//  职责边界：
//    管：Vulkan 实例的创建与销毁、物理设备的挑选、逻辑设备与队列的建立、
//        校验层（Validation Layer）的接入
//    不管：窗口（Platform/Window）、交换链、命令缓冲、任何绘制行为
//
//  依赖规则：
//    本文件允许出现 Vk* 类型与 Vulkan API 调用 —— 这是 VkBackend/ 层内部的特权。
//    Renderer/ 与 Scene/ 里的类不许直接碰 Vulkan API，只能经由本层提供的封装。
//
//  什么是 Instance / Device（对照 OpenGL 帮助理解）：
//    OpenGL 里只有一个隐式的"全局上下文"，glad 加载完就能直接调 gl*。
//    Vulkan 把它拆成了三级：
//
//      VkInstance        —— 整个进程级别。代表"你的程序连接到了 Vulkan 运行库"，
//                           记录应用名、要用哪些扩展、要不要开校验层。
//                           它不知道世上有哪些显卡。
//      VkPhysicalDevice  —— 一台真实的 GPU（显卡）。只能【只读查询】：
//                           它的名字、显存多大、支持什么。不能拿它下命令。
//      VkDevice          —— 从某台 PhysicalDevice 上"开启"出来的逻辑连接。
//                           所有真正干活的对象（Buffer / Image / Pipeline ...）
//                           都属于某一个 Device。
//
//    为什么要拆这么细：一台机器可以有两张显卡（核显 + 独显），
//    Instance 让你先枚举出来再挑，OpenGL 做不到这件事。
//
//  什么是 Queue / QueueFamily（OpenGL 里完全没有对应物）：
//    GPU 上的活是分"族"的：有的队列族能画三角形、有的只能做拷贝、有的能显示到窗口。
//    创建逻辑设备时必须明确申请"我要从某某族要几条队列"。
//    命令不是调用即执行，而是录进缓冲后 Submit 给某条队列 —— 这是阶段 3 的内容。
//
//  生命周期约束（调用方必须遵守）：
//    VulkanContext 必须先于所有其他 VkBackend 对象构造，
//    因为它们的创建都依赖这里的 m_Device。
//    析构顺序反过来：本对象最后销毁。
//    在 main 里把它声明成窗口之后的第一个局部变量即可。
// ===========================================================================
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>



struct QueueFamilyIndices
{
	std::optional<uint32_t> GraphicsFamily;
	bool IsComplete() const
	{
		return GraphicsFamily.has_value();
	}
};

//为什么要从VulkanContext开始写？
//不像opengl 调用 glCreateShader时 驱动自动创建了上下文 管理内存 处理状态
//而vulkan 需要我们自己创建上下文 管理内存 处理状态
// 要严格按照顺序进行初始化
//1、创建 VkInstance（连接到 Vulkan 运行时）
//2、创建 VkSurfaceKHR（连接到窗口系统）
//3、枚举并选择 VkPhysicalDevice（选显卡）
//4、创建 VkDevice 和 VkQueue（创建逻辑设备和队列）
//5、创建 VkSwapchainKHR（创建交换链）
//VulkanContext 是整个渲染器的基石，它把 Vulkan 最底层、最繁琐、最容易出错的部分封装起来，
// 为上层提供了一个干净、安全、易用的接口。这就是为什么几乎所有 Vulkan 项目都会从它开始写。
class VulkanContext
{

public:
	struct ContextSpec
	{

		std::string ApplicationName = "Vulkan_Renderer";
		// 校验层是一组可选的调试代码，会在每次 API 调用时检查参数是否合法。
		// 开着它，绝大多数 Vulkan 新手错误会以一段人话的形式打印到控制台，
		// 而不是表现成黑屏或者驱动崩溃。
		//
		// 【开发期务必为 true】Vulkan 的默认行为是"什么都不检查"，
		// 用错参数往往直接崩溃，且没有任何提示。
		// Release 打包时可以关掉以减少开销。
		bool EnableValidationLayers = true;
		// 创建 Instance 时必须显式列出要用的扩展。
		// GLFW 需要其中的窗口系统接口（Windows 下是 VK_KHR_win32_surface）
		// 才能在后续阶段创建 Surface —— 所以这里由调用方把
		// glfwGetRequiredInstanceExtensions() 的结果传进来。
		//
		// 【为什么不在这里直接调用 GLFW】本层不应该知道窗口是谁提供的。
		// 由 main 装配时注入，将来换成 SDL 或自己建窗口都不用改这里。
		std::vector<const char*> RequiredInstanceExtensions;

	};
	explicit VulkanContext(const ContextSpec& spec);
	~VulkanContext();
	//单例模式
	VulkanContext(const VulkanContext&) = delete;
	VulkanContext& operator=(const VulkanContext&) = delete;

	//-----------访问器------------------------
	VkInstance			GetInstance()		const { return m_Instance; }
	VkPhysicalDevice	GetPhysicalDevice() const { return m_PhysicalDevice; }
	VkDevice			GetDevice()			const { return m_Device; }
    VkQueue				GetGraphicsQueue()	const { return m_GraphicsQueue; }
	const VkPhysicalDeviceProperties& GetDeviceProperties() const { return m_DeviceProperties; }
	const QueueFamilyIndices& GetQueueFamilies() const { return m_QueueFamilies; }

	//打印函数
	void PrintDeviceInfo() const;

private:
	//------初始化函数 顺序很重要---------
	void CreateInstance();
	void SetupDebugMessenger();  //依赖CreateInstance
	void PickPhysicalDevice();	//依赖CreateInstance
	void CreateLogicalDevice(); //依赖PickPhysicalDevice
	//-------查询helpers------------------
	QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device) const;
	bool               IsDeviceSuitable(VkPhysicalDevice device) const;
	int                RateDeviceSuitability(VkPhysicalDevice device) const;
	// 必须在 instance 和 device 层重复的两件事，封装成静态方法避免写错
	static bool CheckValidationLayerSupport();
	static std::vector<const char*> GetRequiredDeviceExtensions();
	// 校验层的消息回调。必须是 static —— Vulkan 拿的是裸函数指针，
	// 成员函数隐含 this 参数，签名对不上
	static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
		VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
		VkDebugUtilsMessageTypeFlagsEXT             type,
		const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
		void* userData);
private:
	ContextSpec					m_Spec;
	VkInstance					m_Instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT	m_DebugMessenger = VK_NULL_HANDLE;
	VkPhysicalDevice			m_PhysicalDevice = VK_NULL_HANDLE;
	VkDevice					m_Device = VK_NULL_HANDLE;
	VkQueue						m_GraphicsQueue = VK_NULL_HANDLE;

	// GPU 的只读快照（名字、厂商、驱动版本、各项上限）。
    // 缓存下来而不是每次查询：后续创建 Buffer / Image 时要反复读它。
	VkPhysicalDeviceProperties	m_DeviceProperties{};
	QueueFamilyIndices			m_QueueFamilies;

};
