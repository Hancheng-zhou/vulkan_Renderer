#include "Application.h"

#include "Platform/Window.h"
#include "VkBackend/VulkanContext.h"
#include "VkBackend/VulkanAllocator.h"
#include "VkBackend/VulkanBuffer.h"
#include "VkBackend/VulkanSwapchain.h"
#include "VkBackend/VulkanCommandPool.h"
#include "VkBackend/FrameResources.h"
#include "VkBackend/VulkanPipeline.h"
#include "VkBackend/VulkanDescriptor.h"
#include "VkBackend/VulkanImage.h"

#include "Core/Vertex.h"
#include "Renderer/Mesh.h"
#include "Renderer/CameraController.h"
#include "Renderer/RenderObject.h"
#include "Renderer/Transform.h"

#include "Scene/Scene.h"

#include <cstdio>
#include <exception>
#include <vector>
#include <stdexcept>
#include <array>
#include <cstddef>

#include <glm/glm.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
#include <glm/gtc/matrix_transform.hpp>   // perspective / lookAt / rotate
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
    InputState PollInput(const Window& window)
    {
        InputState input;
        input.ViewportHovered = true;
        input.AcceptCameraKeys = true;

        // 三轴顺序 (前后, 上下, 左右)，与 Camera::MoveForward/MoveUp/MoveRight 对应
        if (window.IsKeyPressed(GLFW_KEY_W)) { input.MoveAxis.z += 1.0f; }
        if (window.IsKeyPressed(GLFW_KEY_S)) { input.MoveAxis.z -= 1.0f; }
        if (window.IsKeyPressed(GLFW_KEY_D)) { input.MoveAxis.x += 1.0f; }
        if (window.IsKeyPressed(GLFW_KEY_A)) { input.MoveAxis.x -= 1.0f; }
        if (window.IsKeyPressed(GLFW_KEY_SPACE)) { input.MoveAxis.y += 1.0f; }
        if (window.IsKeyPressed(GLFW_KEY_LEFT_CONTROL)) { input.MoveAxis.y -= 1.0f; }

        input.Fast = window.IsKeyPressed(GLFW_KEY_LEFT_SHIFT) ||
            window.IsKeyPressed(GLFW_KEY_RIGHT_SHIFT);
        input.RotateHold = window.IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT);

        return input;
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
    double lastTime = Window::GetTime();

    // ---- 主循环 ----------------------------------------------------------
    //
    // 当前阶段是空转：只轮询事件、检查关闭请求。
    // 后续阶段会在这里依次填入 输入 -> 更新世界 -> 渲染 -> 提交。
    while (m_Running)
    {
        const double now = Window::GetTime();
        const float deltaTime = static_cast<float>(now - lastTime);
        lastTime = now;
        m_Window.PollEvents();
        const InputState input = PollInput(m_Window);
        m_CameraController->OnUpdate(input, deltaTime);
        if (m_Window.ShouldClose())
        {
            m_Running = false;
            break;
        }
        if (m_Window.RefreshFramebufferSize())
        {
            RecreateSwapchain();
            m_Scene->GetCamera().SetViewportSize(m_Window.GetWidth(), m_Window.GetHeight());
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
    // ---- 4. 显存分配器 ----------------------------------------------
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

    // ---- 5.35 决定 MSAA 采样数 -------------------------------------------
    //
    // 【必须在建图之前】三张图的采样数都依赖它。
    // 【只在 Init 里查一次】设备不换、能力不变，重建交换链时沿用即可。
    m_SampleCount = m_Context->GetMaxUsableSampleCount();

    std::printf("[Application] MSAA         : %ux%s\n",
        static_cast<uint32_t>(m_SampleCount),
        m_SampleCount == VK_SAMPLE_COUNT_1_BIT ? " (disabled)" : "");
    // ---- 5.4 深度图像 ----------------------------------------------------
    //
    // 【为什么和 swapchain 绑在一起】深度附件必须覆盖整个渲染区域。
    //   尺寸取交换链的 extent，而不是窗口尺寸 —— 两者可能差一像素。
    CreateDepthImage();
    CreateMsaaColorImage();
    // ---- 5.5 按图像数量建 renderFinished 信号量 --------------------------
    //  必须在 swapchain 之后：数量取决于 GetImageCount()
    CreateRenderFinishedSemaphores();
    // ---- 5.6 场景 --------------------------------------------------------
    //
    //  Scene 持有相机与物体列表。相机参数在这里一次性确定：
    //   位置 (0, 1.5, 6) 看向原点，能看全沿 X 轴排开的一排立方体。
    //   宽高比用交换链的 extent，不是窗口逻辑尺寸。
    const VkExtent2D extent = m_Swapchain->GetExtent();
    const float aspect = static_cast<float>(extent.width) /
        static_cast<float>(extent.height);

    m_Scene = std::make_unique<Scene>(glm::vec3(0.0f, 1.5f, 6.0f),   // 相机位置
        glm::vec3(0.0f, 0.0f, 0.0f),   // 看向原点
        45.0f,                          // FOV
        aspect,
        0.1f, 100.0f);

    m_CameraController = std::make_unique<CameraController>(m_Scene->GetCamera());

    // 输入回调：转发给控制器（只记录，不决策）
    m_Window.SetCursorPosCallback([this](double x, double y)
        {
            m_CameraController->OnCursorMove(x, y);
        });
    m_Window.SetScrollCallback([this](double, double yOffset)
        {
            m_CameraController->OnScroll(yOffset);
        });
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
    // ---- 7.5 几何与场景内容 ----------------------------------------------
    MeshContext meshContext{};
    meshContext.Device = m_Context->GetDevice();
    meshContext.Allocator = m_Allocator.get();
    meshContext.CommandPool = m_CommandPool.get();
    meshContext.GraphicsQueue = m_Context->GetGraphicsQueue();

    PrimitiveDesc cubeDesc;
    cubeDesc.Type = PrimitiveType::Cube;
    cubeDesc.Size = 1.0f;

    // 【共享几何】5 个立方体用同一份顶点/索引缓冲 ——
    //   只有 Transform 不同。这正体现了"几何"与"位姿"分离的价值。
    std::shared_ptr<Mesh> cubeMesh(Mesh::Create(meshContext, cubeDesc));

    // 沿 X 轴排开，间距 1.2（小于边长 1？不，大于边长才有间隙）。
    // 【故意选 1.2】> 1.0，所以立方体之间没有像素重叠，
    //   但透视会让远端看起来贴近 —— 稍后放大间距或调整 Z 能造出重叠。
    for (int i = 0; i < 5; ++i)
    {
        RenderObject object;
        object.Name = "Cube" + std::to_string(i);
        object.SetSharedGeometry(cubeMesh, cubeDesc);

        object.Xform.Position = glm::vec3(
            static_cast<float>(i - 2) * 0.9f,                        // X：0.9 < 1.0，相邻重叠
            0.0f,
            static_cast<float>((i % 2) * 2 - 1) * 0.7f);             // Z：±0.7 交替

        object.Xform.Rotation = glm::vec3(0.0f, static_cast<float>(i) * 0.35f, 0.0f);

        m_Scene->AddObject(object);
    }

    std::printf("[Application] scene : %zu object(s), %u vertices shared\n",
        m_Scene->GetObjectCount(), cubeMesh->GetVertexCount());
    // ---- 7.6 相机 UBO + descriptor ---------------------------------------
    //
    //  顺序约束：descriptor 的 layout 要先建，才能传给 pipeline。
    //  所以这一段必须排在 m_Pipeline 创建之前。
    constexpr uint32_t k_CameraUboBindingCount = 1;
    constexpr VkDeviceSize k_CameraUboSize = sizeof(glm::mat4) + sizeof(glm::vec4);

    m_Descriptor = std::make_unique<VulkanDescriptor>(
        m_Context->GetDevice(), k_CameraUboBindingCount, k_MaxFramesInFlight);

    m_CameraUniformBuffers.resize(k_MaxFramesInFlight);

    for (uint32_t i = 0; i < k_MaxFramesInFlight; ++i)
    {
        // UBO 用 CpuToGpu：CPU 每帧都要写，必须是持久映射的
        VulkanBuffer::CreateInfo uboInfo{};
        uboInfo.size = k_CameraUboSize;
        uboInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        uboInfo.memoryUsage = VulkanAllocator::Usage::CPUToGPU;

        m_CameraUniformBuffers[i] = std::make_unique<VulkanBuffer>(
            m_Context->GetDevice(), *m_Allocator, uboInfo);

        // 把这份 buffer 绑到第 i 帧的 descriptor set 上
        m_Descriptor->BindUniformBuffer(i, 0,
            m_CameraUniformBuffers[i]->GetHandle(),
            k_CameraUboSize);
    }
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
    pipelineInfo.depthFormat = k_DepthFormat;
    pipelineInfo.vertexSpvPath = "assets/shaders/Mesh.vert.spv";
    pipelineInfo.fragSpvPath = "assets/shaders/Mesh.frag.spv";
    pipelineInfo.vertexBindings = { binding };
    pipelineInfo.vertexAttributes = { attributes.begin(),attributes.end() };
    pipelineInfo.descriptorSetLayout = m_Descriptor->GetLayout();
    pipelineInfo.pushConstantSize = sizeof(glm::mat4);   // 只推一个宽高比
    pipelineInfo.samples = m_SampleCount;
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
        m_Descriptor.reset();
        m_CameraUniformBuffers.clear();
        m_Scene.reset();
        m_CameraController.reset();
        m_Pipeline.reset();
        m_MsaaColorImage.reset();
        m_DepthImage.reset();
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
    m_Descriptor.reset();
    m_CameraUniformBuffers.clear();
    m_Scene.reset();
    m_CameraController.reset();
    m_Pipeline.reset();
    m_MsaaColorImage.reset();
    m_DepthImage.reset();
    m_CommandPool.reset();   // pool 一并回收所有 command buffer
    m_Swapchain.reset();     // imageView + swapchain
    m_Allocator.reset();     // VkDeviceMemory
    m_Context.reset();       // device + surface + instance

    // ---- 4. 防御性检查 ---------------------------------------------------
    //
    //  以后再加 GPU 资源时如果忘了在上面清理，这里会在 Debug 下立刻断言，
    //  而不是等到退出时崩在驱动内部。断言信息比 "访问冲突" 有用得多。
    if (m_Scene||m_CameraController || m_Pipeline|| m_MsaaColorImage ||m_DepthImage || m_CommandPool ||
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
    // 【新增】交换链尺寸变了，深度图必须跟着变。
    //   Recreate 内部有 vkDeviceWaitIdle，所以此刻销毁旧深度图是安全的。
    CreateDepthImage();
    CreateMsaaColorImage();
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

void Application::CreateDepthImage()
{
    const VkExtent2D extent = m_Swapchain->GetExtent();

    VulkanImage::CreateInfo info{};
    info.Device = m_Context->GetDevice();
    info.Allocator = m_Allocator.get();
    info.Width = extent.width;
    info.Height = extent.height;
    info.Format = k_DepthFormat;

    // 【usage 必须是 DEPTH_STENCIL_ATTACHMENT_BIT】
    //   只写 COLOR_ATTACHMENT_BIT 的话，vkCreateImage 会成功，
    //   但把它当深度附件渲染时校验层报 "image was not created with usage ..."，
    //   而画面表现是深度完全不起作用。
    info.Usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    // D32_SFLOAT 没有 stencil，只能写 DEPTH_BIT。
    info.Aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    // 【新增】深度图必须和颜色附件同采样数
    info.Samples = m_SampleCount;
    std::printf("[Application] depth image : %u x %u, D32_SFLOAT, %ux\n",
        extent.width, extent.height,
        static_cast<uint32_t>(m_SampleCount));
    // make_unique 赋值时旧的会先被销毁，但显式 reset 让意图更清楚
    m_DepthImage.reset();
    m_DepthImage = std::make_unique<VulkanImage>(info);

    std::printf("[Application] depth image : %u x %u, D32_SFLOAT\n",
        extent.width, extent.height);
}

void Application::CreateMsaaColorImage()
{
    // 采样数为 1 时不需要这张图 —— 直接渲染到交换链。
    // 这就是对照实验的开关：把 m_SampleCount 强制设成 _1_BIT，
    // 这条路径会走"无 MSAA"，锯齿立刻回来。
    if (m_SampleCount == VK_SAMPLE_COUNT_1_BIT)
    {
        m_MsaaColorImage.reset();
        return;
    }

    const VkExtent2D extent = m_Swapchain->GetExtent();

    VulkanImage::CreateInfo info{};
    info.Device = m_Context->GetDevice();
    info.Allocator = m_Allocator.get();
    info.Width = extent.width;
    info.Height = extent.height;

    // 【格式必须和交换链完全一致】
    //   resolve 要求源和目标的格式相同。用 B8G8R8A8_SRGB vs UNORM
    //   这种细微差别也不行 —— 校验层会报 VUID-VkRenderingAttachmentInfo
    //   里的 format mismatch。
    info.Format = m_Swapchain->GetFormat();

    // 【只需要 COLOR_ATTACHMENT_BIT】
    //   这张图从不被采样、从不被拷贝，只作为渲染目标存在。
    //   少写 SAMPLED_BIT 反而更好：usage 越少，驱动可能选更优的 tiling。
    info.Usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.Aspect = VK_IMAGE_ASPECT_COLOR_BIT;

    info.Samples = m_SampleCount;

    m_MsaaColorImage.reset();
    m_MsaaColorImage = std::make_unique<VulkanImage>(info);

    std::printf("[Application] msaa color  : %u x %u, %ux, format %d\n",
        extent.width, extent.height,
        static_cast<uint32_t>(m_SampleCount),
        static_cast<int>(info.Format));
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
    // ---- 2.5 更新这一帧的相机 UBO ----------------------------------------
    //
    // 【必须在这一帧的 fence 通过之后】否则会覆盖 GPU 正在读的数据。
    const float aspect = static_cast<float>(m_Swapchain->GetExtent().width) /
        static_cast<float>(m_Swapchain->GetExtent().height);

    struct CameraData
    {
        glm::mat4 ViewProjection;
        glm::vec4 CameraPosition;
    };

    const Camera& camera = m_Scene->GetCamera();

    CameraData cameraData{};
    cameraData.ViewProjection = camera.GetViewProjection();
    cameraData.CameraPosition = glm::vec4(camera.GetPosition(), 0.0f);

    m_CameraUniformBuffers[m_FrameIndex]->Upload(&cameraData, sizeof(cameraData), 0);
    
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
        // 5a.2 MSAA 颜色图：UNDEFINED -> 颜色附件
        //
        // 【何时做真转换】首次（布局是 UNDEFINED），以及每次重建 swapchain 后
        //   （新对象布局又是 UNDEFINED）。之后每帧同布局，内部直接返回。
        if (m_MsaaColorImage)
        {
            m_MsaaColorImage->TransitionLayout(
                cmd,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        }
        // 5a.5 深度图用途：切到"深度附件"布局
        //
        // 【为什么每帧都调】方法内部会比对 m_CurrentLayout，
        //   第一帧 UNDEFINED -> DEPTH_ATTACHMENT_OPTIMAL 做真转换，
        //   之后每帧都是同布局，直接返回。
        //
        // 【为什么在 beginRendering 之前】layout 转换是命令流里的屏障，
        //   必须发生在"开始使用这个附件"之前。放进 rendering scope 里
        //   是违规的（校验层会报 VUID-vkCmdPipelineBarrier...）。
        m_DepthImage->TransitionLayout(
            cmd,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        // ---- 颜色附件（MSAA 时带 resolve）---------------------------------
        //
        // 【结构与 6d 的差别只有三行】
        //   6d：imageView 指向交换链图，画完就是最终结果。
        //   现在：imageView 指向 4x 图，画完由 resolve 写进交换链图。
        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.clearValue = k_ClearColor;

        if (m_MsaaColorImage)
        {
            // 渲染目标 = 4x 图
            colorAttachment.imageView = m_MsaaColorImage->GetView();

            // 【DONT_CARE 是 MSAA 的标准写法】
            //   4x 图的内容在 resolve 之后就没用了，下一帧反正要 clear。
            //   设成 STORE 会让驱动把 4 倍大小的图白白回写一遍 ——
            //   在移动端上这是最典型的 MSAA 性能杀手。
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

            // ---- resolve 配置：把 4x 平均成 1x 写进交换链图 ----
            //
            // 【AVERAGE 以外的模式】还有 SAMPLE_ZERO（只取第 0 个子样本）
            //   和 MIN/MAX（取极值）。标准 MSAA 一律用 AVERAGE。
            //   后两种用于特殊技巧（比如廉价阴影、深度可视化）。
            colorAttachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            colorAttachment.resolveImageView = m_Swapchain->GetImageViews()[imageIndex];

            // 【必须是 COLOR_ATTACHMENT_OPTIMAL】
            //   resolve 写入本质上是"把目标当作颜色附件写"，
            //   所以目标布局要和普通颜色附件一样。
            //   这里正好和上面 TransitionImageToColorAttachment 转到的布局一致。
            colorAttachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        else
        {
            // 无 MSAA：直接渲染到交换链图，结果要保留给 present
            colorAttachment.imageView = m_Swapchain->GetImageViews()[imageIndex];
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }

        // ---- 深度附件 ----------------------------------------------------
        //
        // 【clearValue.depthStencil.depth 必须是 1.0】配合 COMPARE_OP_LESS，
        //   意思是"初始时所有位置都最远，任何几何都能写进去"。
        //   填 0.0 的话整个深度图一开始就是最近的，什么都画不上 —— 黑屏。
        VkRenderingAttachmentInfo depthAttachment{};
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView = m_DepthImage->GetView();
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;

        // 【DONT_CARE 是刻意的】深度值这一帧用完就丢，下一帧反正要 clear。
        //   声明不保留可以让驱动跳过回写整张深度图（移动端收益很明显）。
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue.depthStencil = { 1.0f, 0 };

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea.offset = { 0, 0 };
        renderingInfo.renderArea.extent = m_Swapchain->GetExtent();
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;
        // 现在有深度图了 应该挂上
        // 
        renderingInfo.pDepthAttachment = &depthAttachment;
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

        // ---- 绑定 descriptor set ------------------------------------------
        //   set 0 与 shader 里的 layout(set = 0) 对应
        VkDescriptorSet descriptorSet = m_Descriptor->GetSet(m_FrameIndex);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_Pipeline->GetLayout(),
            0, 1, &descriptorSet, 0, nullptr);

        // ---- 多物体绘制 ------------------------------------------------
       //
       // 每个物体：push 自己的 Model -> 绑几何 -> draw。
       //
       // 【为什么 Model 走 push constant 而不是 UBO】
       //   UBO 每帧只能有"一份"被 shader 读到。
       //   如果把 5 个 Model 都塞进一个 UBO 数组，shader 里要靠索引选，
       //   需要额外传入实例号 —— 比"每画一个推一次"复杂得多。
       //   push constant 就是为这种"每次绘制都变的少量数据"设计的。
        for (const RenderObject& object : m_Scene->GetObjects())
        {
            if (!object.IsVisible || object.Geometry == nullptr)
            {
                continue;
            }

            const glm::mat4 model = object.Xform.GetModelMatrix();

            vkCmdPushConstants(cmd, m_Pipeline->GetLayout(),
                VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &model);

            object.Geometry->Bind(cmd);
            object.Geometry->Draw(cmd);
        }

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

