#include "Application.h"

#include "Platform/Window.h"
#include "VkBackend/VulkanContext.h"
#include "VkBackend/VulkanAllocator.h"
#include "VkBackend/VulkanBuffer.h"
#include "VkBackend/VulkanSwapchain.h"
#include "VkBackend/VulkanCommandPool.h"
#include "VkBackend/FrameResources.h"
#include "VkBackend/VulkanPipeline.h"

#include "Core/Vertex.h"
#include "Renderer/Mesh.h"

#include <cstdio>
#include <exception>
#include <vector>
#include <stdexcept>
#include <array>
#include <cstddef>

#include <glm/glm.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
static const VkClearValue k_ClearColor = { { { 0.05f, 0.06f, 0.10f, 1.0f } } };
namespace {
    void ThrowOnFailed(VkResult result, const char* whatFailed)
    {
        if (result != VK_SUCCESS)
        {
            throw std::runtime_error(std::string("[Vulkan] ")
                + whatFailed
                + "failed, VkResult = "
                + std::to_string(static_cast<int>(result)));
        }
    }

    // -----------------------------------------------------------------------
    //  分配器自测：写进去 -> 读出来 -> 逐字节比对。
    //
    //  设计成"故意用多种大小/对齐/释放顺序"，覆盖三类最容易错的地方：
    //    1. 对齐：alignment 不是 2 的幂倍数时 offset 算错
    //    2. 碎片：分配-释放交错后，合并失败的 free list 会拒绝本可满足的请求
    //    3. 越界：offset + size 超过块尾
    // -----------------------------------------------------------------------
    void RunAllocatorSelfTest(VulkanAllocator& allocator)
    {
        constexpr VkBufferUsageFlags k_TestUsage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        // ---- 测试 1：单块 buffer 的写读往返 -----------------------------
        constexpr VkDeviceSize kSize = 1024;
        constexpr VkDeviceSize kAlign = 256;

        VulkanAllocator::Allocation a = allocator.Allocate(
            kSize, kAlign, 0xFFFFFFFF, VulkanAllocator::Usage::CPUToGPU);

        if (!a.IsValid())
        {
            throw std::runtime_error("[self-test] allocation invalid");
        }

        // 对齐检查：这是最值得单独验的一条。offset 不满足 alignment 时
        // 后续 bind 可能成功但行为未定义 —— 那种 bug 极难查。
        if (a.offset % kAlign != 0)
        {
            throw std::runtime_error("[self-test] offset not aligned to requested alignment");
        }

        // 写入已知模式：第 i 字节填 i & 0xFF。用位置相关的值而不是全 0，
        // 否则"没拷进去"和"拷进去全是 0"两种情况输出一样，测不出问题。
        auto* bytes = static_cast<uint8_t*>(a.mapped);
        for (VkDeviceSize i = 0; i < kSize; ++i)
        {
            bytes[i] = static_cast<uint8_t>(i & 0xFF);
        }

        for (VkDeviceSize i = 0; i < kSize; ++i)
        {
            if (bytes[i] != static_cast<uint8_t>(i & 0xFF))
            {
                throw std::runtime_error("[self-test] read-back mismatch at single buffer");
            }
        }

        std::printf("[self-test] single buffer  : OK (offset=%llu size=%llu)\n",
            static_cast<unsigned long long>(a.offset),
            static_cast<unsigned long long>(a.size));

        // ---- 测试 2：交错分配/释放，验证 free list 合并 -------------------
        //
        //  手法：先要三块，释放首尾两块，再要一块【跨越原来两个空洞】的大块。
        //  如果 Coalesce 没把相邻空洞合并，这次分配会失败 —— 而三块的总空闲
        //  明明足够。这就是"内存够用却分配失败"的典型症状。
        VulkanAllocator::Allocation b1 = allocator.Allocate(
             512, 16, 0xFFFFFFFF, VulkanAllocator::Usage::CPUToGPU);
        VulkanAllocator::Allocation b2 = allocator.Allocate(
            512, 16, 0xFFFFFFFF, VulkanAllocator::Usage::CPUToGPU);
        VulkanAllocator::Allocation b3 = allocator.Allocate(
             512, 16, 0xFFFFFFFF, VulkanAllocator::Usage::CPUToGPU);

        allocator.Free(a);    // a 是 1024，在最前面
        allocator.Free(b3);   // b3 是最后一块

        // 这一次要能吃掉 a 的空洞 + b2/b3 之间被合并出来的空间
        VulkanAllocator::Allocation big = allocator.Allocate(
            1024, 16, 0xFFFFFFFF, VulkanAllocator::Usage::CPUToGPU);

        if (!big.IsValid())
        {
            throw std::runtime_error(
                "[self-test] coalescing failed: free list did not merge adjacent ranges");
        }

        std::printf("[self-test] coalescing     : OK (reused offset=%llu)\n",
            static_cast<unsigned long long>(big.offset));

        allocator.Free(b1);
        allocator.Free(b2);
        allocator.Free(big);

        std::printf("[self-test] allocator      : total %llu bytes in %llu block(s)\n",
            static_cast<unsigned long long>(allocator.GetTotalAllocateBytes()),
            static_cast<unsigned long long>(allocator.GetBlockCount()));
    }
    // -----------------------------------------------------------------------
    //  Buffer 自测：验证"建 buffer -> 绑显存 -> 上传 -> 读回 -> 销毁"闭环。
    //
    //  三个检查点分别对应三类只有跑起来才暴露的问题：
    //    1. 上传/读回  —— vkBindBufferMemory 的 offset 是否正确
    //    2. 越界拒绝   —— Upload 的边界检查是活的，不是死代码
    //    3. GpuOnly 拒绝 —— 防止将来误把 Upload 用在 CPU 不可见的内存上
    // -----------------------------------------------------------------------
    void RunBufferSelfTest(VkDevice device, VulkanAllocator& allocator)
    {
        constexpr VkDeviceSize kBufferSize = 1024;

        VulkanBuffer::CreateInfo info{};
        info.size = kBufferSize;
        info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        info.memoryUsage = VulkanAllocator::Usage::CPUToGPU;
        info.alignment = 256;   // 故意大于默认要求，验证 alignment 取 max 的逻辑

        VulkanBuffer buffer(device, allocator, info);

        // ---- 检查 1：写入 -> 读回 -----------------------------------------
        const uint8_t pattern[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        buffer.Upload(pattern, sizeof(pattern), 0);

        const auto* mapped = static_cast<const uint8_t*>(buffer.GetMappedPointer());
        if (mapped == nullptr)
        {
            throw std::runtime_error("[self-test] CpuToGpu buffer has no mapped pointer");
        }

        for (int i = 0; i < 8; ++i)
        {
            if (mapped[i] != pattern[i])
            {
                throw std::runtime_error("[self-test] Upload/read-back mismatch");
            }
        }

        // 对齐也顺带验一次：bind 时的 offset 必须满足 alignment
        if (reinterpret_cast<std::uintptr_t>(mapped) % 256 != 0)
        {
            throw std::runtime_error("[self-test] buffer mapping not 256-byte aligned");
        }

        std::printf("[self-test] buffer upload   : OK (size=%llu)\n",
            static_cast<unsigned long long>(buffer.GetSize()));

        // ---- 检查 2：越界必须抛 -------------------------------------------
        //
        //  用标志位而不是 try-catch 里直接抛：否则会把自己刚抛出的异常
        //  当成"检查通过"，测试永远绿。
        bool rejectedOutOfRange = false;
        try
        {
            buffer.Upload(pattern, sizeof(pattern), kBufferSize - 4);
        }
        catch (const std::exception&)
        {
            rejectedOutOfRange = true;
        }

        if (!rejectedOutOfRange)
        {
            throw std::runtime_error("[self-test] Upload did not reject out-of-range write");
        }

        // ---- 检查 3：GpuOnly buffer 的 Upload 必须抛 -----------------------
        //
        //  DEVICE_LOCAL 内存 CPU 碰不到。这个检查防的是"将来有人把
        //  Upload 用在顶点 buffer 上"——那会写到野指针，且不一定立刻崩。
        VulkanBuffer::CreateInfo gpuInfo{};
        gpuInfo.size = 512;
        gpuInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        gpuInfo.memoryUsage = VulkanAllocator::Usage::GPUOnly;

        VulkanBuffer gpuBuffer(device, allocator, gpuInfo);

        if (gpuBuffer.GetMappedPointer() != nullptr)
        {
            throw std::runtime_error("[self-test] GpuOnly buffer should not be mapped");
        }

        bool rejectedGpuUpload = false;
        try
        {
            gpuBuffer.Upload(pattern, sizeof(pattern), 0);
        }
        catch (const std::exception&)
        {
            rejectedGpuUpload = true;
        }

        if (!rejectedGpuUpload)
        {
            throw std::runtime_error("[self-test] Upload on GpuOnly buffer was not rejected");
        }

        std::printf("[self-test] buffer guards  : OK (out-of-range + GpuOnly rejected)\n");
    }


}
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
            break;
        }
        if (m_Window.RefreshFramebufferSize())
        {
            RecreateSwapchain();

            // 重建后之前可能已 acquire 的 imageIndex 已失效，
            // 这一帧不再录命令，下一帧从新交换链重新开始。
            continue;
        }
        RenderFrame();
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
    spec.NativeWindowHandle = m_Window.GetNativeWin32Handle();
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
    // ---- 4. 显存分配器自测 ----------------------------------------------
    //
    //  【为什么放在这里】分配器的正确性无法靠"看起来能跑"判断 ——
    //  偏移算错、对齐漏掉、free list 没合并，都会静默地让数据错乱。
    //  所以这里做一次"写进去、读出来、逐字节比"的闭环验证。
    m_Allocator = std::make_unique<VulkanAllocator>(m_Context->GetDevice(), m_Context->GetPhysicalDevice());
    // ---- 5. 交换链 -------------------------------------------------------
    VulkanSwapchain::CreateInfo swapchainInfo{};
    swapchainInfo.PhysicalDevice = m_Context->GetPhysicalDevice();
    swapchainInfo.Device = m_Context->GetDevice();
    swapchainInfo.Surface = m_Context->GetSurface();
    swapchainInfo.GraphicsQueueFamily = m_Context->GetGraphicsFamily();
    swapchainInfo.PresentQueueFamily = m_Context->GetPresentFamily();
    swapchainInfo.Width = m_Window.GetWidth();
    swapchainInfo.Height = m_Window.GetHeight();
    
    m_Swapchain = std::make_unique<VulkanSwapchain>(swapchainInfo);
    m_Swapchain->PrintInfo();

    // ---- 5.5 按图像数量建 renderFinished 信号量 --------------------------
    //  必须在 swapchain 之后：数量取决于 GetImageCount()
    CreateRenderFinishedSemaphores();
    // ---- 6. 命令池（全局一份，按帧数分配缓冲）----------------------------
    m_CommandPool = std::make_unique<VulkanCommandPool>(m_Context->GetDevice(), m_Context->GetGraphicsFamily(), k_MaxFramesInFlight);
    // ---- 7. 每帧的同步对象 -----------------------------------------------
    //
    //  【命令缓冲从这里取】pool 已经按 k_MaxFramesInFlight 分配好了，
    //    这里只是把句柄填进各自的 FrameResources，并建好配套的
    //    两个信号量 + 一个围栏。
    m_Frames.resize(k_MaxFramesInFlight);
    for (uint32_t i = 0; i < k_MaxFramesInFlight; i++)
    {
        m_Frames[i].CommandBuffer = m_CommandPool->GetBuffers()[i];
        m_Frames[i].CreateSyncObjects(m_Context->GetDevice());
    }
    // ---- 7.5 几何体 ------------------------------------------------------
    //
    //  MeshContext 把创建缓冲需要的四样环境打包，
    //  这样 Mesh 的生成函数不用一路传 device / allocator / pool / queue。
    MeshContext meshContext{};
    meshContext.Device        = m_Context->GetDevice();
    meshContext.Allocator     = m_Allocator.get();
    meshContext.CommandPool   = m_CommandPool.get();
    meshContext.GraphicsQueue = m_Context->GetGraphicsQueue();

    PrimitiveDesc cubeDesc;
    cubeDesc.Type = PrimitiveType::Cube;
    cubeDesc.Size = 1.0f;

    m_Mesh = Mesh::Create(meshContext, cubeDesc);

    std::printf("[Application] mesh : %u vertices, %u indices\n",
                m_Mesh->GetVertexCount(), m_Mesh->GetIndexCount());
    // --------8. 图形管线 --------------------------------------------------
    // ---- 顶点输入布局 ----------------------------------------------------
    //和shader里一致
    //
    //
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);                       // 48 字节
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 4> attributes{};

    attributes[0].binding = 0;
    attributes[0].location = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;      // vec3 pos
    attributes[0].offset = offsetof(Vertex, Position);

    attributes[1].binding = 0;
    attributes[1].location = 1;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;      // vec3 normal
    attributes[1].offset = offsetof(Vertex, Normal);

    attributes[2].binding = 0;
    attributes[2].location = 2;
    attributes[2].format = VK_FORMAT_R32G32_SFLOAT;         // vec2 texcoord
    attributes[2].offset = offsetof(Vertex, Texcoord);

    attributes[3].binding = 0;
    attributes[3].location = 3;
    attributes[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;   // vec4 tangent
    attributes[3].offset = offsetof(Vertex, Tangent);

    VulkanPipeline::CreateInfo pipelineInfo{};
    pipelineInfo.device = m_Context->GetDevice();
    pipelineInfo.colorFormat = m_Swapchain->GetFormat();
    pipelineInfo.vertexSpvPath = "assets/shaders/Mesh.vert.spv";
    pipelineInfo.fragSpvPath = "assets/shaders/Mesh.frag.spv";
    pipelineInfo.vertexBindings = { binding };
    pipelineInfo.vertexAttributes = { attributes.begin(),attributes.end() };
    pipelineInfo.pushConstantSize = sizeof(float);   // 只推一个宽高比
    m_Pipeline = std::make_unique<VulkanPipeline>(pipelineInfo);
    // ---------------debug test----------------------------------------------------
    RunAllocatorSelfTest(*m_Allocator);
    RunBufferSelfTest(m_Context->GetDevice(),*m_Allocator);
    m_Running = true;
    std::printf("[Application] frames in flight : %u\n", k_MaxFramesInFlight);
}

void Application::Shutdown()
{
    if (!m_Context || m_Context->GetDevice() == VK_NULL_HANDLE)
    {
        // 【这里不能调任何依赖 m_Context 的函数】
        //   能进这个分支，就说明 m_Context 已经是空指针（第二次 Shutdown），
        //   或者 device 根本没建成。这两种情况下都没有 GPU 工作要等，
        //   也就不需要 vkDeviceWaitIdle —— 硬调只会解引用空指针。
        //
        //   曾经的写法在这里加了一句 vkDeviceWaitIdle(m_Context->GetDevice())，
        //   表现为关窗口时崩在 VulkanContext::GetDevice() 里（this == nullptr）。
        m_Frames.clear();
        m_RenderFinishedSemaphores.clear();   // 此时必然是空的，防御性清理
        m_Mesh.reset();
        m_Pipeline.reset();
        m_CommandPool.reset();
        m_Swapchain.reset();
        m_Allocator.reset();
        m_Context.reset();
        return;
    }

    VkDevice device = m_Context->GetDevice();

    // ---- 1. 先让 GPU 完全停下来 ------------------------------------------
    //
    //  【必须第一步】下面销毁的 VkFence / VkSemaphore / VkCommandPool /
    //    VkSwapchainKHR 全都要求对应资源不在 pending 状态。
    //    退出时最后提交的那帧可能还在跑，不等就会崩在驱动内部
    //    （表现为 nvoglv64.dll 里读地址 0）。
    vkDeviceWaitIdle(device);

    // ---- 1.5 销毁按图像索引的 renderFinished 信号量 -----------------------
    //  必须在 vkDeviceWaitIdle 之后：最后一帧的 present 可能还没消耗完它们。
    DestroyRenderFinishedSemaphores();

    // ---- 2. 逐帧销毁同步对象 ---------------------------------------------
    //
    //  命令缓冲归 pool 所有，不在这里释放；只销毁我们自己建的信号量和围栏。
    for (FrameResources& frame : m_Frames)
    {
        frame.DestroySyncObjects(device);
        frame.CommandBuffer = VK_NULL_HANDLE;
    }
    m_Frames.clear();

    // ---- 3. 按逆序销毁其余资源 -------------------------------------------
    //
    //  【每一个 GPU 资源都必须在这里显式 reset，不能指望成员自动析构】
    //    因为下面会把 m_Context（device）reset 掉。等到 ~Application
    //    的成员析构阶段，任何还活着的 VkBuffer / VkPipeline 再去调
    //    vkDestroy*，拿到的就是一个已销毁的 device ——
    //    表现为 vulkan-1.dll 里 abort（"请求了严重的程序退出"），
    //    而不是自己代码里的空指针崩溃，极难定位。
    //
    //  销毁顺序 = 依赖链逆序：
    //    VertexBuffer（用 allocator）-> Pipeline -> CommandPool
    //    -> Swapchain -> Allocator -> Context
    m_Mesh.reset();  // ← 曾经漏了这一行，导致退出时崩在 vkDestroyBuffer
    m_Pipeline.reset();
    m_CommandPool.reset();   // pool 一并回收所有 command buffer
    m_Swapchain.reset();     // imageView + swapchain
    m_Allocator.reset();     // VkDeviceMemory
    m_Context.reset();       // device + surface + instance

    // ---- 4. 防御性检查 ---------------------------------------------------
    //
    //  以后再加 GPU 资源时如果忘了在上面清理，这里会在 Debug 下立刻断言，
    //  而不是等到退出时崩在驱动内部。断言信息比 "访问冲突" 有用得多。
    if (m_Mesh || m_Pipeline || m_CommandPool ||
        m_Swapchain || m_Allocator || m_Context)
    {
        std::fprintf(stderr,
                     "[Application] WARNING: some GPU resources were not released in Shutdown()\n");
    }

    m_FrameIndex = 0;
    m_Running = false;
}

// ===========================================================================
//  交换链重建（统一入口）
// ===========================================================================

void Application::RecreateSwapchain()
{
    m_Swapchain->Recreate(m_Window.GetWidth(), m_Window.GetHeight());

    // 【图像数量可能变化】重建后必须按新的数量重建信号量。
    //
    //  Recreate 内部有 vkDeviceWaitIdle，所以此刻旧的 renderFinished
    //  信号量一定已不在使用中，销毁是安全的。
    //
    //  【为什么所有重建点都走这个函数】
    //    重建 swapchain 有两件事必须同时做：换链 + 换信号量数组。
    //    只做前一件，下一帧按新 imageIndex 取信号量就会越界。
    //    把两件事绑在一个入口里，就没人能只改一半。
    CreateRenderFinishedSemaphores();
}

void Application::CreateRenderFinishedSemaphores()
{
    // 先清掉旧的（首次调用时是空数组，无害）
    DestroyRenderFinishedSemaphores();

    VkDevice device = m_Context->GetDevice();
    const uint32_t imageCount = m_Swapchain->GetImageCount();

    m_RenderFinishedSemaphores.assign(imageCount, VK_NULL_HANDLE);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (uint32_t i = 0; i < imageCount; ++i)
    {
        ThrowOnFailed(vkCreateSemaphore(device, &semaphoreInfo, nullptr,
                                        &m_RenderFinishedSemaphores[i]),
                      "vkCreateSemaphore(renderFinished)");
    }

    std::printf("[Application] renderFinished semaphores : %u (per swapchain image)\n",
                imageCount);
}

void Application::DestroyRenderFinishedSemaphores()
{
    // 没有 device 时不能调 vkDestroySemaphore，直接清空数组
    if (!m_RenderFinishedSemaphores.empty() &&
        m_Context && m_Context->GetDevice() != VK_NULL_HANDLE)
    {
        VkDevice device = m_Context->GetDevice();

        for (VkSemaphore& semaphore : m_RenderFinishedSemaphores)
        {
            if (semaphore != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(device, semaphore, nullptr);
                semaphore = VK_NULL_HANDLE;
            }
        }
    }

    m_RenderFinishedSemaphores.clear();
}

// ===========================================================================
//  单帧渲染
// ===========================================================================
void Application::RenderFrame()
{
    VkDevice device = m_Context->GetDevice();
    // ---- 取出这一帧要用的槽位 --------------------------------------------
    //
    //  【关键认知】多帧并行时，等的是"当前槽位"的围栏，不是"上一帧"的。
    //    槽位 0 和槽位 1 分别记录各自那一帧是否执行完。
    //    m_FrameIndex 每帧推进，正好轮流使用两套资源。
    FrameResources& frame = m_Frames[m_FrameIndex];
    // ---- 1. 等这一帧上次提交的命令执行完 ---------------------------------
    //
    //  为什么必须等：
    //    要重置并重录 frame.CommandBuffer。如果 GPU 还在读它，
    //    重录会写入正在被执行的命令流 —— 画面随机错乱或驱动崩溃。
    ThrowOnFailed(vkWaitForFences(device, 1, &frame.InFlight, VK_TRUE, UINT64_MAX),
        "vkWaitForFences");
    // ---- 2. 取一张可写的交换链图像 ---------------------------------------
    uint32_t imageIndex = 0;
    VkResult acquireResult = vkAcquireNextImageKHR(device, m_Swapchain->GetHandle(), UINT64_MAX, frame.ImageAvailable, VK_NULL_HANDLE, &imageIndex);
    // ---- 3. 交换链失效：重建后直接返回 -----------------------------------
    //
    //  【这里绝对不能在前面 reset 围栏】
    //    如果 vkResetFences 放在 wait 之后、acquire 之前，
    //    这条 return 路径会让围栏停在"未触发"状态，
    //    而这一帧没有 submit、永远不会 signal —— 下一帧
    //    vkWaitForFences 将永久阻塞，程序卡死。
    //    窗口 resize 必然走这条路径，所以这个坑一定会踩到。
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        RecreateSwapchain();
        return;
    }
    // SUBOPTIMAL 表示"能用但不是最优"：继续画完这一帧，最后再重建。
    // 把它当错误抛出是常见误写。
    const bool isSuboptimal = (acquireResult == VK_SUBOPTIMAL_KHR);
    if (!isSuboptimal && acquireResult != VK_SUCCESS)
    {
        throw std::runtime_error("[Application] vkAcquireNextImageKHR failed");
    }
    // ---- 3.5 取这张图像专用的 renderFinished 信号量 -----------------------
    //
    //  【索引来自 imageIndex，不是 m_FrameIndex】
    //    present 的生命周期跟"图像"绑定，不跟"帧槽位"绑定。
    //    用错了就会回到"信号量还在被 present 占用时又被 signal"的违规状态。
    //
    //  数量检查：重建 swapchain 后如果忘了同步重建信号量，这里会立刻
    //  抛出一句人话，而不是等驱动读到越界内存。
    if (imageIndex >= static_cast<uint32_t>(m_RenderFinishedSemaphores.size()))
    {
        throw std::runtime_error(
            "[Application] renderFinished semaphores out of sync with swapchain");
    }
    VkSemaphore renderFinished = m_RenderFinishedSemaphores[imageIndex];

    // ---- 4. 确认要提交了，这时候才 reset 围栏和命令缓冲 -------------------
    ThrowOnFailed(vkResetFences(device, 1, &frame.InFlight), "vkResetFences");

    // 单独重置这条缓冲（依赖 pool 的 RESET_COMMAND_BUFFER_BIT 标志）
    ThrowOnFailed(vkResetCommandBuffer(frame.CommandBuffer, 0), "vkResetCommandBuffer");
    // ---- 5. 录制命令 -----------------------------------------------------
   //
   //  【用作用域包住】CommandRecording 析构时自动 vkEndCommandBuffer，
   //    中途抛异常也不会漏。手写 Begin/End 的版本在异常路径上会漏掉 End，
   //    导致缓冲卡在 recording 状态。
    {
        CommandRecording recording(frame.CommandBuffer);
        VkCommandBuffer cmd = recording.Get();

        // 5a. 图像用途：呈现 -> 颜色附件
        TransitionImageToColorAttachment(cmd, imageIndex);

        // 5b. 清屏（Dynamic Rendering，无 RenderPass 对象）
        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = m_Swapchain->GetImageViews()[imageIndex];
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;   // 结果留给 present
        colorAttachment.clearValue = k_ClearColor;

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea.offset = { 0, 0 };
        renderingInfo.renderArea.extent = m_Swapchain->GetExtent();
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;
        // 无深度/模板附件，pDepthAttachment / pStencilAttachment 保持 nullptr

        vkCmdBeginRendering(cmd, &renderingInfo);

        // ---- 绑定管线 ----------------------------------------------------
        //
        // 【必须在 BeginRendering 之后】管线包含颜色附件格式等状态，
        //   只有在 rend rendering scope 内绑定才是合法的。
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline->GetHandle());
        // ---- 设置动态视口与裁剪 -------------------------------------------
        //
        // 这两项在管线里声明为 dynamic，必须每帧设置。
        // 【忘了设的后果】视口是未定义值，三角形可能完全不可见，
        //   而且校验层只在部分驱动上报错。
        //
        // 用 swapchain 的 extent，而不是窗口尺寸 —— 两者在边界情况下可能差一像素，
        // 用错会有 1px 的边缝或溢出。
        const VkExtent2D extent = m_Swapchain->GetExtent();
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(extent.width);
        viewport.height = static_cast<float>(extent.height);
        viewport.minDepth = 0.0f;   // Vulkan 的深度范围是 [0,1]，不是 GL 的 [-1,1]
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = { 0, 0 };
        scissor.extent = extent;
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // ---- 推送宽高比 ----------------------------------------------------
        //
        // 【必须在 bindPipeline 之后、draw 之前】
        //   推送常量的布局属于管线布局；没绑管线时调用是未定义行为。
        //
        //   这个值用来抵消 NDC -> 视口的非等比缩放：
        //   800x600 的窗口下，x 的 1 个 NDC 单位 = 400 像素，
        //   y 的 1 个单位 = 300 像素，不修正就会横向拉伸 1.33 倍。
        const float aspect = static_cast<float>(extent.width) /
                             static_cast<float>(extent.height);

        vkCmdPushConstants(cmd, m_Pipeline->GetLayout(),
                           VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float), &aspect);

        //Mesh 内部完成绑定和绘制
        m_Mesh->Bind(cmd);
        m_Mesh->Draw(cmd);

        vkCmdEndRendering(cmd);

        // 5c. 图像用途：颜色附件 -> 呈现
        TransitionImageToPresent(cmd, imageIndex);

    }
    // ---- 6. 提交 ---------------------------------------------------------
    //
    //  pWaitDstStageMask 的含义：在到达这个管线阶段之前，先等 imageAvailable。
    //    COLOR_ATTACHMENT_OUTPUT = "开始写颜色之前等"，这是最精确的时机。
    //    填 TOP_OF_PIPE 也能跑，但会让等待发生得更早，某些驱动上多一帧延迟。
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &frame.ImageAvailable;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.CommandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &renderFinished;

    // 提交时把这一帧的围栏挂上，GPU 执行完会自动 signal 它
    ThrowOnFailed(vkQueueSubmit(m_Context->GetGraphicsQueue(), 1, &submitInfo, frame.InFlight),
        "vkQueueSubmit");
    // ---- 7. 呈现 ---------------------------------------------------------
    VkSwapchainKHR swapchain = m_Swapchain->GetHandle();

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &imageIndex;

    VkResult presentResult = vkQueuePresentKHR(m_Context->GetPresentQueue(), &presentInfo);
    // 三种情况都要重建：present 明确失效、present 次优、acquire 时已经次优
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR ||
        presentResult == VK_SUBOPTIMAL_KHR ||
        isSuboptimal)
    {
        RecreateSwapchain();
    }
    else if (presentResult != VK_SUCCESS)
    {
        throw std::runtime_error("[Application] vkQueuePresentKHR failed");
    }

    // ---- 8. 推进到下一个槽位 ---------------------------------------------
    //
    //  【必须在函数末尾】如果放在开头，acquire 失败提前 return 时
    //    索引不会推进，下一帧还是同一个槽位 —— 那个槽位的围栏可能
    //    处于未触发状态，直接死锁。
    m_FrameIndex = (m_FrameIndex + 1) % k_MaxFramesInFlight;
}
// ===========================================================================
//  图像用途转换
// ===========================================================================

void Application::TransitionImageToColorAttachment(VkCommandBuffer cmd, uint32_t imageIndex)
{
    // 【什么是 image layout】
    //   Vulkan 要求每张图像在任何时刻都有明确的"用途标签"，它决定
    //   显存在物理上的排布（tiling）。同一张图不能同时被当"呈现目标"
    //   和"渲染目标"用，切换必须显式转换并插屏障，让驱动有机会重排。
    //
    //   OpenGL 没有这个概念 —— 这是 Vulkan 最常见黑屏原因之一。
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

    // 【为什么用 UNDEFINED 作起点】
    //   每帧都清屏，不需要保留上一帧内容。声明 UNDEFINED 等于告诉驱动
    //   "这里的数据可以丢"，驱动会跳过复制，更快。
    //   若将来改成"在上一帧基础上叠加"，必须换成 PRESENT_SRC_KHR。
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

    barrier.image = m_Swapchain->GetImages()[imageIndex];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    // 访问掩码：转换前没有读操作，转换后是颜色写入
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,              // 起点：命令流最开头
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,  // 终点：开始写颜色
        0,
        0, nullptr,      // 无全局内存屏障
        0, nullptr,      // 无缓冲屏障
        1, &barrier);
}
void Application::TransitionImageToPresent(VkCommandBuffer cmd, uint32_t imageIndex)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

    barrier.image = m_Swapchain->GetImages()[imageIndex];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    // 颜色写入完成 -> 交给显示引擎读取
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,  // 起点：颜色写入结束
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,           // 终点：命令流末尾
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier);
}

