#include "Application.h"

#include "Platform/Window.h"
#include "VkBackend/VulkanContext.h"

#include <cstdio>
#include <exception>
#include <vector>

#include <glm/glm.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>

Application::Application()
    : m_Window(Window::WindowProps{})   // 用 WindowProps 的全部默认值
{
    // 构造函数只建窗口。
    // Vulkan 资源的创建放在 Init()：窗口是 Vulkan 的前置条件，
    // 把顺序摊开比藏在初始化列表里更容易读懂。
}

Application::~Application()
{
    // 兜底：正常路径在 Run() 末尾已经调过一次 Shutdown()。
    // 这里再调一次是防"Run() 没被调用就直接析构"的情况。
    // Shutdown() 是幂等的，重复调用安全。
    Shutdown();
}

int Application::Run()
{
    try
    {
        Init();
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[fatal] %s\n", e.what());

        // 【Init 失败也要清理】Init 可能建到一半才抛异常
        // （比如 Window 好了、VulkanContext 失败）。
        // 不在这里收尾，那半个 Context 就泄漏到进程结束。
        Shutdown();
        return 1;
    }

    // ---- 主循环 ----------------------------------------------------------
    //
    // 当前阶段是空转：只轮询事件、检查关闭请求。
    // 后续阶段会在这里依次填入 输入 -> 更新世界 -> 渲染 -> 提交。
    while (m_Running)
    {
        m_Window.PollEvents();

        if (m_Window.ShouldClose())
        {
            m_Running = false;
        }
    }

    Shutdown();
    return 0;
}

void Application::Init()
{
    // ---- 1. 向 GLFW 要实例扩展 ------------------------------------------
    //
    // 必须在 glfwTerminate 之前调用 —— GetRequiredInstanceExtensions
    // 返回的指针由 GLFW 持有。当前时机（Window 已建、尚未销毁）满足。
    std::vector<const char*> instanceExtensions = m_Window.GetRequiredInstanceExtensions();

    // ---- 2. 创建 Vulkan 上下文 -------------------------------------------
    VulkanContext::ContextSpec spec;
    spec.ApplicationName = "vulkan_Renderer";
    spec.EnableValidationLayers = true;
    spec.RequiredInstanceExtensions = std::move(instanceExtensions);

    m_Context = std::make_unique<VulkanContext>(spec);

    // 验收判据 1：确认程序跑在哪张卡上。
    // 双显卡笔记本很容易误选集成显卡，这行输出是第一道检查。
    m_Context->PrintDeviceInfo();

    // ---- 3. 验收判据 2：glm 可用 ----------------------------------------
    //
    //  ZHC 里 glm 是数学基础，先确认它被正确链接。
    //  宏 GLM_FORCE_DEPTH_ZERO_TO_ONE 让裁剪空间深度落在 [0,1]，
    //  这是 Vulkan 的约定（OpenGL 是 [-1,1]）。
    const glm::mat4 identity(1.0f);
    const glm::vec4 origin(0.0f, 0.0f, 0.0f, 1.0f);
    const glm::vec4 transformed = identity * origin;

    std::printf("[glm] mat4 * vec4 -> (%.2f, %.2f, %.2f)\n",
        transformed.x, transformed.y, transformed.z);

    m_Running = true;
}

void Application::Shutdown()
{
    // 显式销毁 Context，不等 Application 析构。
    //
    // 【为什么不等自动析构】Shutdown() 可能在 Init() 中途失败时被调用，
    //   那时需要立刻把已建成的资源清掉；也可能被 Run() 和析构函数各调一次。
    //   集中在这里，销毁时机就只有一个入口。
    //
    // unique_ptr::reset(nullptr) 对空指针再次调用是安全的，
    // 所以这个函数天然幂等。
    m_Context.reset();
}