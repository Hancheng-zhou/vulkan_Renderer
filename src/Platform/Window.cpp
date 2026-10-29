#include "Platform/Window.h"
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <stdexcept>
#include <utility>
Window::Window(const WindowProps& props)
    : m_Width(props.Width)
    , m_Height(props.Height)
    , m_Data(props)
{
    // glfwInit() 返回 int 而不是 bool（历史原因），0 = 失败
    if (glfwInit() == GLFW_FALSE)
    {
        throw std::runtime_error("[Window] glfwInit failed");
    }

    // GLFW_NO_API：不创建任何 OpenGL 上下文。
    // 这正是"只用 GLFW 管窗口、图形 API 走 Vulkan"的开关。
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    //允许resize
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    m_Window = glfwCreateWindow(static_cast<int>(props.Width),
        static_cast<int>(props.Height),
        props.Title.c_str(),
        nullptr,   // 不用 monitor 参数 -> 窗口化
        nullptr);  // 不与其它窗口共享上下文

    if (m_Window == nullptr)
    {
        // 失败时也要收尾：glfwInit 已经调用过，
        // 不配对 glfwTerminate 就抛异常，会泄漏 GLFW 的进程级资源
        glfwTerminate();
        throw std::runtime_error("[Window] glfwCreateWindow failed");
    }

    // ---- 取 framebuffer 尺寸 -------------------------------------------
    //
    // 覆盖掉 props 里的逻辑尺寸。
    // 缩放 100% 时两者相等，但高 DPI 屏上不等，而 Vulkan 只认像素。
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(m_Window, &framebufferWidth, &framebufferHeight);

    m_Width = static_cast<uint32_t>(framebufferWidth);
    m_Height = static_cast<uint32_t>(framebufferHeight);
    //初始化回调函数
    InstallCallbacks();
}

Window::~Window()
{
    std::printf("[~Window] destroying window\n");
    if (m_Window != nullptr)
    {
        glfwDestroyWindow(m_Window);
        m_Window = nullptr;
    }

    // 【必须与构造里的 glfwInit 配对】GLFW 是进程级状态，
    // 漏掉这一次 terminate，某些平台下退出时会卡住或报错
    glfwTerminate();
}

bool Window::ShouldClose() const
{
    // glfwWindowShouldClose 返回 int（GLFW_TRUE/GLFW_FALSE）
    return glfwWindowShouldClose(m_Window) == GLFW_TRUE;
}

void Window::PollEvents()
{
    glfwPollEvents();
}

bool Window::RefreshFramebufferSize()
{
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(m_Window, &width, &height);
    if (width != m_Width || height != m_Height)
    {
        m_Width = static_cast<uint32_t>(width);
        m_Height = static_cast<uint32_t>(height);
        return true;
    }
    return false;
}

void* Window::GetNativeWin32Handle() const
{
    void* handle = reinterpret_cast<void*> (glfwGetWin32Window(m_Window));
    if (handle == nullptr)
    {
        throw std::runtime_error("[Window] glfwGetWin32Window returned nullptr");
    }
    return handle;
}

std::vector<const char*> Window::GetRequiredInstanceExtensions() const
{
    uint32_t extensionCount = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&extensionCount);

    if (extensions == nullptr || extensionCount == 0)
    {
        throw std::runtime_error("[Window] glfwGetRequiredInstanceExtensions returned nothing");
    }

    // 拷贝一份。GLFW 内部那块数组的生存期不属于我们，
    // 直接存指针的话，之后每次访问都要担心它有没有被 GLFW 释放。
    return std::vector<const char*>(extensions, extensions + extensionCount);
}

double Window::GetTime()
{
    return glfwGetTime();
}

bool Window::IsKeyPressed(int key) const
{
    return glfwGetKey(m_Window, key) == GLFW_PRESS;
}

bool Window::IsMouseButtonPressed(int button) const
{
    return glfwGetMouseButton(m_Window, button) == GLFW_PRESS;
}

void Window::SetCursorPosCallback(std::function<void(double x, double y)> callback)
{
    m_CursorPosCallback = std::move(callback);
}

void Window::SetScrollCallback(std::function<void(double xOffset, double yOffset)> callback)
{
    m_ScrollCallback = std::move(callback);
}

void Window::InstallCallbacks()
{
    glfwSetWindowUserPointer(m_Window, this);
    glfwSetCursorPosCallback(m_Window, [](GLFWwindow* window, double x, double y) 
        {
            auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
            if (self && self->m_CursorPosCallback)
            {
                self->m_CursorPosCallback(x, y);
            }
        });
    glfwSetScrollCallback(m_Window, [](GLFWwindow* window, double xOffset, double yOffset)
        {
            auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
            if (self && self->m_ScrollCallback)
            {
                self->m_ScrollCallback(xOffset, yOffset);
            }
        });
}
