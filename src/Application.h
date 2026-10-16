#pragma once
// ===========================================================================
//  Application.h
//
//  职责边界：
//    管：装配顺序、Vulkan 资源的创建与销毁、主循环骨架
//    不管：场景内容、相机逻辑、UI（那些是后续阶段的 SceneRenderer / EditorUI）
//
//  为什么要有这个类（而不是把主循环写在 main.cpp 里）：
//    main.cpp 只该回答"程序从哪进"。装配顺序、生命周期规则、
//    每帧做什么，都属于引擎自己的知识。混在 main 里，main 会长到
//    五六百行，且无法被复用（比如将来写单元测试想建一个离屏 Application）。
//
//  生命周期（本类最要紧的部分）：
//    m_Window 声明在 m_Context 之前，析构顺序因此是 m_Context 先死、
//    m_Window 后死。这不是巧合，是刻意安排的：
//      Vulkan 资源（Device 及其派生物）必须先于窗口消失。
//      反过来会让 Vulkan 对象引用一个已销毁的窗口表面。
//
//    【这条规则在 ZHC 里靠注释提醒，在这里由编译器保证】
//    把顺序写进成员声明次序，就没人能不小心写反。
// ===========================================================================

#pragma once

#include <memory>
#include "Platform/Window.h"
class VulkanContext;
class Application
{
public:
    Application();
    ~Application();

    // 运行主循环，返回进程退出码（0 = 正常，1 = 初始化失败）。
    // 不自己调 exit()：退出码交回 main，让调用方决定怎么处理。
    int Run();

private:
    void Init();
    void Shutdown();

    Window                        m_Window;    // 先构造，后析构
    std::unique_ptr<VulkanContext> m_Context;  // 后构造，先析构

    bool m_Running = false;
};