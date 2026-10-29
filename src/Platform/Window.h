// ===========================================================================
//  Platform/Window.h
//
//  职责边界（沿用 ZHC 的划分）：
//    管：GLFW 生命周期、窗口创建/销毁、framebuffer 尺寸、原始事件轮询
//    不管：主循环、任何 Vk* 调用、渲染状态、输入逻辑
//
//  依赖规则：
//    本文件允许出现 GLFW 类型，但不允许出现 Vulkan 类型。
//    窗口与 Vulkan 的唯一交点由 Application 层桥接（GetNativeHandle）。
//
//  与 ZHC 版 Window 的差异：
//    ZHC 强制定义 GLFW_INCLUDE_NONE（因为要避开 glad 的 GL 头）。
//    本版改用 GLFW_INCLUDE_VULKAN —— Vulkan 没有 glad 冲突问题，
//    反而需要 GLFW 提供 vulkan 相关的类型声明。
//
//  生命周期约束：
//    构造即完成 glfwInit + 窗口创建；析构完成窗口销毁 + glfwTerminate。
//    【全局只应存在一个 Window】glfwInit/glfwTerminate 是进程级调用，
//    多实例会重复初始化/提前终止。将来要支持多窗口再改这里。
// ===========================================================================
#pragma once

// 必须写在 glfw3.h 之前：让 GLFW 引入 Vulkan 头而不是 GL 头。
// 定义在这里而不是每个 include 处，保证谁先 include 都生效。
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

class Window
{
public:
    struct WindowProps
    {
        std::string Title = "vulkan_Renderer";
        uint32_t    Width = 1280;
        uint32_t    Height =720;
    };

    explicit Window(const WindowProps& props);
    ~Window();

    // 窗口对象持有 OS 资源且不可拷贝（删了就没有了），禁拷
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // ---- 每帧驱动：主循环需要的全部零件 --------------------------------
    bool ShouldClose() const;
    void PollEvents();
    bool RefreshFramebufferSize();
    // ---- 窗口信息 --------------------------------------------------------
    //  返回的是 framebuffer 尺寸（像素），不是窗口的逻辑尺寸。
    //  高 DPI 屏上后者可能是 800 而前者是 1600。
    //  Vulkan 交换链要用的是 framebuffer 尺寸 —— 拿错会让画面拉伸。
    uint32_t GetWidth()  const { return m_Width; }
    uint32_t GetHeight() const { return m_Height; }

    // ---- Vulkan 桥接 -----------------------------------------------------
    //  创建 VkSurfaceKHR 需要 GLFWwindow*。
    //  放这里而不是让 Application 直接碰 GLFW：窗口实现细节不外泄。
    GLFWwindow* GetNativeHandle() const { return m_Window; }
    // ---- Win32 桥接 -------------------------------------------------------
    //  返回 Win32 窗口句柄（HWND），Vulkan 创建 surface 需要它。
    //
    //  【为什么返回 void* 而不是 HWND】HWND 需要 <windows.h>，
    //    那个头文件会带进几千个宏（min/max 会被它改成宏，很麻烦）。
    //    放在 .cpp 里转换，头文件保持干净。
    //
    //  【平台相关性】这只在 Windows 上有意义。换平台时换成对应句柄类型
    //    （X11 是 Window，Wayland 是 wl_surface*）。
    void* GetNativeWin32Handle() const;
    //  返回 GLFW 要求的实例扩展列表。
    //
    //  【为什么要问它】创建 VkInstance 必须显式列出扩展。
    //    GLFW 要的窗口系统接口（Windows 下 VK_KHR_surface + VK_KHR_win32_surface）
    //    是它自己决定的，只能问出来，不能猜。
    //
    //  【生命周期警告】返回的指针数组由 GLFW 持有，在 glfwTerminate() 之后失效。
    //    调用方必须在 Window 析构前用完（Application 的 Init 里正好满足）。
    std::vector<const char*> GetRequiredInstanceExtensions() const;
    static double GetTime();
    // ---- 输入轮询：用于"当前是否按住"这类持续状态 -----------------------
    //  WASD 移动、右键是否按住，都用这两个，不要用回调 + bool 数组
    bool IsKeyPressed(int key) const;
    bool IsMouseButtonPressed(int button) const;

    // ---- 事件注册：用于"移动/滚动"这类离散事件 -------------------------
    void SetCursorPosCallback(std::function<void(double x, double y)> callback);
    void SetScrollCallback(std::function<void(double xOffset, double yOffset)> callback);

private:
    void InstallCallbacks();// 只在构造函数里调一次
private:
    GLFWwindow* m_Window = nullptr;
    uint32_t    m_Width = 0;
    uint32_t    m_Height = 0;
    WindowProps m_Data;
    std::function<void(double, double)> m_CursorPosCallback;
    std::function<void(double, double)> m_ScrollCallback;

};