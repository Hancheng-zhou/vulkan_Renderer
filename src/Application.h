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
#include <vector>
#include <vulkan/vulkan.h>
#include "Platform/Window.h"
#include "VkBackend/FrameResources.h"  
#include "Renderer/Camera.h"

class VulkanContext;
class VulkanAllocator;
class VulkanSwapchain;
class VulkanCommandPool;
class Mesh;
struct FrameResources;
class VulkanPipeline;
class VulkanBuffer;
class VulkanDescriptor;
class CameraController;
class Scene;
class VulkanImage;

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
    //渲染一帧 等fence-> acquire -> record command -> submit -> present 
    void RenderFrame();
    //交换链图像用途转换(画前画后各一次)
    void TransitionImageToColorAttachment(VkCommandBuffer cmd, uint32_t imageIndex);
    void TransitionImageToPresent(VkCommandBuffer cmd, uint32_t imageIndex);

    // 重建交换链，并同步重建依赖它图像数量的资源。
    //
    // 【为什么要包一层】交换链重建后 GetImageCount() 可能变化，
    //   m_RenderFinishedSemaphores 必须跟着重建，否则下一帧按
    //   新的 imageIndex 取信号量会越界。所有重建点统一走这里，
    //   就不会有人只改了一半。
    void RecreateSwapchain();

    // 按【交换链图像数量】创建/销毁 renderFinished 信号量。
    void CreateRenderFinishedSemaphores();
    void DestroyRenderFinishedSemaphores();
    // 按当前交换链尺寸创建深度图。
    // 【必须和 swapchain 同步重建】尺寸不匹配时校验层会报
    //   "depthAttachment extent is smaller than renderArea extent"，
    //   表现为画面边缘出现未定义的条纹。
    void CreateDepthImage();
    // 按当前交换链尺寸创建 MSAA 颜色图。m_SampleCount == 1 时不创建。
    void CreateMsaaColorImage();
private:
    // ---- 多帧并行深度 ----------------------------------------------------
    //
    //  2 表示"同时有 2 帧在 GPU 上排队"。好处：
    //    CPU 录第 N+1 帧时，GPU 还在跑第 N 帧，两者不互相等待。
    //
    //  为什么不是 1：
    //    单帧时 CPU 每帧都要等 GPU 彻底空闲，帧率会被 CPU/GPU 中较慢的一方
    //    直接卡住，且 resize、present 都会引入停顿。
    //
    //  为什么不是 3：
    //    2 已经能填满流水线；3 只增加延迟和显存，收益很小。
    //    这个值是性能调优点，不是正确性要求，2 是安全默认。
    static constexpr uint32_t k_MaxFramesInFlight = 2;
    // 【为什么用 D32_SFLOAT】它是规范保证支持的深度格式里最简单的一种：
    //   32 位浮点，无 stencil。精度足够，且 aspect 只有 DEPTH_BIT，
    //   省掉 stencil 那一堆配置。
    //   需要模板（比如描边、延迟渲染）时换成 D24_UNORM_S8_UINT。
    static constexpr VkFormat k_DepthFormat = VK_FORMAT_D32_SFLOAT;

    // ---- 成员顺序即析构顺序（逆序）----
    //
    //  m_Allocator 必须声明在 m_Context 之前：它持有的 VkDeviceMemory
    //  属于 device，device 一死全部失效。所以 allocator 要先死。
    //
    //  链式依赖：Window -> Context -> Allocator -> Buffer
    //  销毁必须完全反过来：Buffer -> Allocator -> Context -> Window
    Window                        m_Window;    // 先构造，后析构
    std::unique_ptr<VulkanContext> m_Context;  // 后构造，先析构
    std::unique_ptr<VulkanAllocator> m_Allocator;
    std::unique_ptr <VulkanSwapchain> m_Swapchain;
    std::unique_ptr<VulkanCommandPool> m_CommandPool;
    std::unique_ptr<VulkanPipeline> m_Pipeline;
    std::unique_ptr<VulkanImage> m_DepthImage;
    // ---- MSAA ----------------------------------------------------------
    //
    //  实际使用的采样数。Init() 里查一次，之后不变。
    //  【为什么存成员而不是每次查】它决定了三张图的创建参数和
    //    pipeline 的配置，散在多个地方各查一次容易不一致。
    VkSampleCountFlagBits m_SampleCount = VK_SAMPLE_COUNT_1_BIT;

    // 4x 颜色图。仅当 m_SampleCount > 1 时存在。
    //   【必须和 m_DepthImage 同尺寸同采样数】
    std::unique_ptr<VulkanImage> m_MsaaColorImage;
    std::vector<FrameResources> m_Frames;
    // 每帧一份相机 UBO。数量 = k_MaxFramesInFlight。
    // 【为什么不能共用一份】CPU 写第 N 帧时，GPU 可能还在读第 N-1 帧的。
    std::vector<std::unique_ptr<VulkanBuffer>> m_CameraUniformBuffers;
    std::unique_ptr<VulkanDescriptor> m_Descriptor;
    // ---- 按【交换链图像】索引的 renderFinished 信号量 --------------------
    //
    //  【为什么不能放进 FrameResources（按帧索引）】
    //    vkQueuePresentKHR 返回后，present 引擎何时真正消耗掉这个信号量
    //    是不确定的。而 fence 只跟踪 vkQueueSubmit，不跟踪 present。
    //
    //    于是会出现：帧数(2) < 图像数(3)，绕一圈回到同一帧槽位时，
    //    上一轮的信号量可能还被 present 占着 —— 再次 signal 就违反
    //    VUID-vkQueueSubmit-pSignalSemaphores-00067。
    //
    //  【为什么按图像索引就安全】
    //    每次 signal 它之前，那张图像必须重新被 acquire；
    //    而 acquire 成功本身就意味着它上一次的 present 已经结束。
    //
    //  【数量必须随交换链图像数变化】
    //    重建 swapchain 时调用 CreateRenderFinishedSemaphores() 同步重建。
    std::vector<VkSemaphore> m_RenderFinishedSemaphores;
    uint32_t m_FrameIndex = 0;
    std::unique_ptr<Scene> m_Scene;
    std::unique_ptr<CameraController> m_CameraController;
    
    bool m_Running = false;
};