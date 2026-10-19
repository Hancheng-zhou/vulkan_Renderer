#include "VkBackend/VulkanContext.h"
#include <vulkan/vulkan_win32.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace 
{
    // 标准的 Khronos 校验层。SDK 安装后自带。
    // 它是"壳"，内部把历史上各家厂商的调试层
    //（threading / parameter_validation / object_tracker / core_validation）
    // 合并成了一层，所以只需要列这一个名字。
    const char* const k_ValidationLayerName = "VK_LAYER_KHRONOS_validation";

    // ---------------------------------------------------------------------------
    //  vkCreateDebugUtilsMessengerEXT / vkDestroyDebugUtilsMessengerEXT
    //
    //  【为什么必须手动取地址】vulkan-1.lib 只导出核心 API 的符号。
    //    VK_EXT_debug_utils 这类扩展函数的实现装在驱动里，
    //    必须先用 vkGetInstanceProcAddr 按名字向实例问出函数指针。
    //    忘记这一步的表现是链接期报 unresolved external，
    //    且报的总是 Create-Destroy 这一对。
    //
    //  【为什么是自由函数不是成员】它们是给 Vulkan 回调用的工具，
    //    不访问对象状态；放进匿名命名空间避免污染全局符号表。
    // ---------------------------------------------------------------------------
    VkResult CreateDebugUtilsMessengerEXT(
        VkInstance instance,
        const VkDebugUtilsMessengerCreateInfoEXT* createInfo,
        const VkAllocationCallbacks* allocator,
        VkDebugUtilsMessengerEXT* messenger)
    {
        auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT")
            );
        if (func != nullptr)
        {
            return func(instance, createInfo, allocator, messenger);
        }
        else
            return VK_ERROR_EXTENSION_NOT_PRESENT;
    }   
    void DestroyDebugUtilsMessengerEXT(
        VkInstance instance,
        VkDebugUtilsMessengerEXT messenger,
        const VkAllocationCallbacks* allocator)
    {
        auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT")
            );
        if (func!= nullptr)
        {
            func(instance, messenger, allocator);
        }
    }

    void ThrowOnFailed(VkResult result, const char* whatFailed)
    {
        if (result != VK_SUCCESS)
        {
            throw std::runtime_error(std::string("[Vulkan] ") 
                +whatFailed 
                + "failed, VkResult = " 
                + std::to_string(static_cast<int>(result)));
        }
    }
}


// ==================================================
// 构造函数 +析构函数
// ==================================================

VulkanContext::VulkanContext(const ContextSpec& spec):m_Spec(spec)
{
    //按照顺序执行初始化
    CreateInstance();
    CreateSurface();
    SetupDebugMessenger();
    PickPhysicalDevice();
    CreateLogicalDevice();
}

VulkanContext::~VulkanContext()
{
    // -----------------------------------------------------------------------
   //  销毁顺序严格逆序于创建：Device -> DebugMessenger -> Instance
   //
   //  【Device 必须先于 Instance】Device 是从 Instance 派生出来的资源，
   //    Instance 一销毁，属于它的 Device / Buffer / Image 会全部失效。
   //    Vulkan 不替你自动清理也不报错，泄漏会一直拖到进程退出由 OS 回收。
   //
   //  【DebugMessenger 放在 Instance 之前】它挂在 Instance 上，
   //    这样安排能让它捕获到"instance 销毁期间"发出的校验消息。
   // -----------------------------------------------------------------------
    std::printf("[~VulkanContext] destroying device\n");
    if (m_Device != VK_NULL_HANDLE)
    {
        vkDestroyDevice(m_Device, nullptr);
    }
    if (m_Surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
        m_Surface = VK_NULL_HANDLE;
    }

    if (m_DebugMessenger != VK_NULL_HANDLE)
    {
        DestroyDebugUtilsMessengerEXT(m_Instance, m_DebugMessenger, nullptr);
    }
    if (m_Instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(m_Instance, nullptr);
    }

}

void VulkanContext::PrintDeviceInfo() const
{
    // %s 直接可用：deviceName 是固定长度的 char 数组，保证以 \0 结尾。
    std::printf("[Vulkan] API version   : %u.%u.%u\n",
        VK_API_VERSION_MAJOR(m_DeviceProperties.apiVersion),
        VK_API_VERSION_MINOR(m_DeviceProperties.apiVersion),
        VK_API_VERSION_PATCH(m_DeviceProperties.apiVersion));
    std::printf("[Vulkan] Device name    : %s\n", m_DeviceProperties.deviceName);
    std::printf("[Vulkan] Device type    : %s\n",
        m_DeviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete GPU"
        : m_DeviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated GPU"
        : m_DeviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? "software CPU" : "other");
    std::printf("[Vulkan] Graphics queue : family %u\n", m_QueueFamilies.GraphicsFamily.value_or(UINT32_MAX));
    std::printf("[Vulkan] Present queue  : family %u\n", GetPresentFamily());
}
// ===========================================================================
//  1. Instance —— "本程序连接到了 Vulkan 运行库"
// ===========================================================================
bool VulkanContext::CheckValidationLayerSupport()
{
    // 先看本机装了哪些层。不查直接用会得到
    // vkCreateInstance 返回 VK_ERROR_LAYER_NOT_PRESENT。
    //经典二次调用 一次知道个数 二次根据个数分配内存
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
    for (const VkLayerProperties& layerProperties : availableLayers)
    {
        if (std::strcmp(layerProperties.layerName, k_ValidationLayerName) == 0)
        {
            return true;
        }
    }
    return false;
}

VkBool32 VKAPI_CALL VulkanContext::DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, 
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData, 
    void* /*userData*/)
{
    const bool isError = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;

    std::fprintf(isError ? stderr : stdout,
        "[validation%s] %s\n",
        isError ? " ERROR" : "",
        callbackData->pMessage);

    // 【返回值是给驱动看的】VK_FALSE = "这个错误没有导致回调失败，继续执行"。
    // 若返回 VK_TRUE，出错的 vkXxx 调用会被中断并返回
    // VK_ERROR_VALIDATION_FAILED_EXT。一般做法都是保持 FALSE ——
    // 让日志打出来，而不是让程序当场戛然而止。
    return VK_FALSE;
}

void VulkanContext::CreateInstance()
{
    if (m_Spec.EnableValidationLayers && !CheckValidationLayerSupport()) {
        throw std::runtime_error("[Vulkan] validation layer requested but not available: "
            + std::string(k_ValidationLayerName));
    }
    // ---- applicationInfo：告诉驱动"我是谁、我要哪个版本的 Vulkan" --------
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = m_Spec.ApplicationName.c_str();
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);

    // 【这里必须是 1.3】整个项目依赖 Dynamic Rendering，
    //   它是 Vulkan 1.3 的核心特性（1.2 时代要额外加载 VK_KHR_dynamic_rendering）。
    //   填成 1.2 会让后续的 vkCmdBeginRendering 无函数可用。
    appInfo.apiVersion = VK_API_VERSION_1_3;

    // ---- instanceCreateInfo ----------------------------------------------
    VkInstanceCreateInfo instanceCreateInfo{};
    instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceCreateInfo.pApplicationInfo = &appInfo;
    // 扩展列表 = 调用方要的（GLFW 的 window 系统接口）+ 调试用扩展
    std::vector<const char*> extensions = m_Spec.RequiredInstanceExtensions;
    if (m_Spec.EnableValidationLayers)
    {
        // VK_EXT_debug_utils：把校验层产生的消息投递回我们的回调函数。
        // 没有它，校验层仍然在检查，但你什么也看不到。
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    instanceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instanceCreateInfo.ppEnabledExtensionNames = extensions.data();
    // ---- 调试 Messenger 的创建信息 ----------------------------------------
    //
    // 【为什么要在这里再准备一份】
    //   正常流程是 Instance 建好后再 SetupDebugMessenger()。
    //   但那样只能捕获"instance 创建之后"的消息 ——
    //   而 vkCreateInstance / vkDestroyInstance 自身的报错正是最想看到的一类。
    //   把它挂到 instanceCreateInfo.pNext 上，这两个调用期间的消息也能被捕获。
    //
    // 【这里只订阅 WARNING/ERROR，不收窄也行但极吵】
    //   Loader 在 vkCreateInstance 前后会向本回调倾倒大量 VERBOSE/INFO 消息
    //   （遍历注册表、登记机器上每一个层、打印层链 callstack），
    //   这些是 Loader 自己的行为，与代码对错无关，会把真正要看的消息淹掉。
    //   代价是 instance 创建/销毁期间的 VERBOSE/INFO 也看不到 —— 但那些
    //   几乎全是上面说的登记信息，没有诊断价值。
    //   与 SetupDebugMessenger 保持同一套 severity，行为更一致。
    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debugCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debugCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debugCreateInfo.pfnUserCallback = DebugCallback;
    debugCreateInfo.pUserData = nullptr;
    if (m_Spec.EnableValidationLayers)
    {
        const char* const layers[] = { k_ValidationLayerName };
        instanceCreateInfo.enabledLayerCount = 1;
        instanceCreateInfo.ppEnabledLayerNames = layers;
        instanceCreateInfo.pNext = &debugCreateInfo;
    }

    ThrowOnFailed(vkCreateInstance(&instanceCreateInfo, nullptr, &m_Instance),
        "vkCreateInstance");

}

void VulkanContext::CreateSurface()
{
    if (m_Spec.NativeWindowHandle == nullptr)
    {
        //离屏模式：没有窗口可以呈现 不要求present队列
        m_QueueFamilies.RequiresPresent = false;
        return;
    }
    // 【为什么用 VK_KHR_win32_surface 而不是 glfwCreateWindowSurface】
    //   GLFW 那个函数需要 GLFWwindow*，会逼得 VkBackend 层依赖 GLFW ——
    //   违反"VkBackend 不碰窗口库"的铁律。
    //   改用原生 Win32 surface：这里只需要 HWND，由 Application 从
    //   Window 取出后经 ContextSpec 传进来。依赖方向保持干净。
    //
    // 【hwnd + hinstance】Win32 surface 的两个必要参数。
    //   hinstance 传当前模块句柄即可（GetModuleHandle(nullptr)），
    //   Vulkan 只用它做内部标识，不用于创建窗口。
    VkWin32SurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.hwnd = reinterpret_cast<HWND>(m_Spec.NativeWindowHandle);
    surfaceInfo.hinstance = GetModuleHandleW(nullptr);
    ThrowOnFailed(vkCreateWin32SurfaceKHR(m_Instance, &surfaceInfo, nullptr, &m_Surface),"vkCreateWin32SurfaceKHR");
    m_QueueFamilies.RequiresPresent = true;
}

void VulkanContext::SetupDebugMessenger()
{
    if (!m_Spec.EnableValidationLayers)
    {
        return;
    }
    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = DebugCallback;
    createInfo.pUserData = nullptr;
    ThrowOnFailed(CreateDebugUtilsMessengerEXT(m_Instance, &createInfo, nullptr, &m_DebugMessenger),
        "vkCreateDebugUtilsMessengerEXT");

}


// ===========================================================================
//  2. PhysicalDevice —— 一台真实的 GPU，只能【只读查询】
// ===========================================================================

QueueFamilyIndices VulkanContext::FindQueueFamilies(VkPhysicalDevice device,VkSurfaceKHR surface) const
{
    QueueFamilyIndices indices;
    indices.RequiresPresent = (surface != VK_NULL_HANDLE);
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device,&queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++)
    {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            indices.GraphicsFamily = i;
        }
        // present 支持是【按 surface 问】的，不是设备的固有属性：
        // 同一个队列族可能对某个 surface 能呈现、对另一个不能。
        if (surface != VK_NULL_HANDLE)
        {
            VkBool32 canPresent = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &canPresent);
            if(canPresent == VK_TRUE)
            {
                indices.PresentFamily = i;
            }
        }
        if (indices.IsComplete())
        {
            break;
        }
    }
    return indices;
}

bool VulkanContext::IsDeviceSuitable(VkPhysicalDevice device) const
{
    // ---- 硬性门槛 --------------------------------------------------------
    VkPhysicalDeviceProperties deviceProperties;
    vkGetPhysicalDeviceProperties(device, &deviceProperties);

    // 整个项目的 Pass 都建立在 Dynamic Rendering 之上，它是 Vulkan 1.3 核心特性。
    // 驱动报告低于 1.3 就排除这张卡。
    //
    // 【实测容易踩的点】很多机器上有两张 Vulkan 设备（集显 + 独显），
    //   其中一张可能停在 1.2。这里显式过滤，程序才会自动选到可用的那张，
    //   而不是在 vkCmdBeginRendering 上莫名崩溃。
    if (deviceProperties.apiVersion < VK_API_VERSION_1_3)
    {
        return false;
    }
    // ---- 查询 1.3 特性支持情况 -------------------------------------------
    //
    //  【为什么要单独查】apiVersion >= 1.3 只说明"驱动实现了 1.3 的函数"，
    //  不代表"这个特定特性被硬件支持"。两者是独立的两层：
    //    版本  = 有哪些 API 可以调
    //    特性  = 硬件能不能真的做这件事
    //
    //  查询需要两层结构：Features2 是外壳，pNext 挂具体版本的结构。
    //  这是 Vulkan 1.1 之后所有特性查询的统一模式。
    VkPhysicalDeviceVulkan13Features supported13{};
    supported13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &supported13;

    vkGetPhysicalDeviceFeatures2(device, &features2);

    // 整个渲染管线都建立在 Dynamic Rendering 上，硬件不支持就没法跑
    if (supported13.dynamicRendering != VK_TRUE)
    {
        return false;
    }


    return FindQueueFamilies(device,m_Surface).IsComplete();

}

int VulkanContext::RateDeviceSuitability(VkPhysicalDevice device) const
{
    // 用"打分排序"而不是"取第一个符合的"：
    //   双显卡机器上 vkEnumeratePhysicalDevices 的返回顺序完全由驱动决定，
    //   第一个常常是集成显卡。评分能让独显稳定排到前面。
    VkPhysicalDeviceProperties deviceProperties;
    vkGetPhysicalDeviceProperties(device, &deviceProperties);

    int score = 0;

    // 独显 +1000，这个权重足以压过下面所有加分项。
    // 核显通常远低于 1000，所以两者同时存在时独显必胜。
    if (deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
    {
        score += 1000;
    }

    // 最大纹理尺寸反映能力下限。粗略按 4K 单位取占比，
    // 让差距悬殊时也能拉开档次。
    const int maxTextureSize = static_cast<int>(deviceProperties.limits.maxImageDimension2D);
    score += maxTextureSize > 0 ? (maxTextureSize / 4096) : 0;

    return score;
}

void VulkanContext::PickPhysicalDevice()
{
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr);
    if (deviceCount == 0)
    {
        throw std::runtime_error("[Vulkan] failed to find GPUs with Vulkan support");
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data());
    VkPhysicalDevice bestDevice = VK_NULL_HANDLE;
    int bestScore = -1;
    for (auto device : devices)
    {
        if (!IsDeviceSuitable(device))
        {
            continue;
        }
        const int score = RateDeviceSuitability(device);
        if (score > bestScore)
        {
            bestScore = score;
            bestDevice = device;
        }
    }
    if (bestDevice == VK_NULL_HANDLE)
    {
        throw std::runtime_error("[Vulkan] failed to find a suitable GPU "
            "(requires Vulkan 1.3 support for dynamic rendering)");
    }

    m_PhysicalDevice = bestDevice;
    m_QueueFamilies = FindQueueFamilies(m_PhysicalDevice,m_Surface);

    // 顺手缓存只读属性。后续创建 Buffer / Image 时要反复读它
    //（对齐要求、支持哪些格式...），不想每次都来一次枚举。
    vkGetPhysicalDeviceProperties(m_PhysicalDevice, &m_DeviceProperties);
}
// ===========================================================================
//  3. LogicalDevice —— 从 PhysicalDevice "开启"出来的逻辑连接
// ===========================================================================

std::vector<const char*> VulkanContext::GetRequiredDeviceExtensions()
{
    // 只列一项，但它必须在这里出现：
    //   VK_KHR_swapchain 让设备能创建交换链，没有它就无法把画面送到窗口。
    //   阶段 3 会用到，现在先开启（开启未使用的扩展是安全的）。
    //
    // 【为什么它不属于核心】Vulkan 规范把"窗口系统相关"的扩展
    //   一律定义为 KHR 级 —— Vulkan 本身只定义怎么画，不定义画到哪。
    return { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
}

void VulkanContext::CreateLogicalDevice()
{   
    // ---- 队列申请 --------------------------------------------------------
    //
    // 【必须显式申请】OpenGL 的上下文天生带着一切；Vulkan 里
    //   "这个设备上有哪几条队列"是你申请出来的，不申请就拿不到句柄。
    //
    // 【priority 是必须的，不能传 nullptr】它决定同族多条队列间的调度权重。
    //   本阶段只申请 1 条，填 1.0 即可。
    QueueFamilyIndices indices = m_QueueFamilies;
    std::vector<uint32_t> uniqueFamilies;
    uniqueFamilies.push_back(indices.GraphicsFamily.value());
    if (indices.PresentFamily.has_value() && indices.PresentFamily.value() != indices.GraphicsFamily.value())
    {
        uniqueFamilies.push_back(indices.PresentFamily.value());
    }
    const float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    for(uint32_t family:uniqueFamilies)
    { 
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = family;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }
    // ---- 设备特性 --------------------------------------------------------
   //
   // 现在什么都不启用。geometryShader / tessellation / wideLine 之类
   // 默认是关的 —— 【Vulkan 与 OpenGL 的重大差异】：
   //   OpenGL 里这些功能随时可用，Vulkan 里不在这里显式申请，
   //   后续在 Pipeline 里也用不了，且校验层会直接报错。
    VkPhysicalDeviceVulkan13Features enabled13{};
    enabled13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    enabled13.dynamicRendering = VK_TRUE;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &enabled13;



    // ---- 创建 ------------------------------------------------------------
    VkDeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
    deviceCreateInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    deviceCreateInfo.pNext = &features2;
    deviceCreateInfo.pEnabledFeatures = nullptr;
    // 【为什么设备层还要带一次校验层】新规范下 Instance 上启用就够了，
    //   但旧驱动/旧 SDK 会要求两层都列。为了兼容性，两边保持一致。
    std::vector<const char*> deviceExtensions = GetRequiredDeviceExtensions();
    deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions.data();
    if (m_Spec.EnableValidationLayers)
    {
        const char* const layers[] = { k_ValidationLayerName };
        deviceCreateInfo.enabledLayerCount = 1;
        deviceCreateInfo.ppEnabledLayerNames = layers;
    }

    ThrowOnFailed(vkCreateDevice(m_PhysicalDevice, &deviceCreateInfo, nullptr, &m_Device),
        "vkCreateDevice");
    // ---- 取出队列句柄 ----------------------------------------------------
    //
    // 队列是设备创建时【已经存在】的，这里只是拿到它的句柄。
    // 没有 vkCreateQueue —— 也不能单独销毁队列，它随 Device 一起消失。
    vkGetDeviceQueue(m_Device, indices.GraphicsFamily.value(), 0, &m_GraphicsQueue);
    const uint32_t presentFamily = indices.PresentFamily.value_or(indices.GraphicsFamily.value());
    vkGetDeviceQueue(m_Device, presentFamily, 0, &m_PresentQueue);
}



