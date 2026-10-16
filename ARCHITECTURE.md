# vulkan_Renderer 架构与迁移计划

> 目标：把 OpenGL 渲染器 [`../ZHC`](../ZHC) 迁移到 Vulkan。
> **ZHC 保持只读**，作为参考实现与新项目并存在同一个 workspace。

---

## 0. 已确认的技术选型

| 项 | 选择 | 说明 |
|---|---|---|
| Vulkan 入口 | 原生 `vulkan.h` | 不引入 volk / VMA |
| 显存分配 | **手写分配器** | 目标 ~300–500 行，见 §5.2 |
| Pass 组织 | **Dynamic Rendering** | `VK_KHR_dynamic_rendering`，无 `VkRenderPass` / `VkFramebuffer` |
| 实例版本 | Vulkan 1.3 | SDK 头文件已到 1.3，Dynamic Rendering 已进核心，无需按扩展加载 |
| 着色器编译 | 构建期 `glslangValidator` | SDK 自带 `E:\vulkan\Bin\glslangValidator.exe`，产出 `.spv` |
| 数学库 | glm | 沿用 ZHC |
| 窗口 | GLFW | 自建 `Platform/Window` 封装，**宏必须是 `GLFW_INCLUDE_VULKAN`** |
| 构建 | premake5 | 沿用，`premake5.lua` 需加 **Lib 目录** 与 **着色器预编译** 两步 |

依赖约束：新项目**不新增第三方库**，仅 `glfw` / `glm` / `stb_image`（已在 `vendor/`）。ImGui 在第 10 阶段引入。

---

## 1. 目标目录结构

```
src/
  Core/                     ← 直接复制 ZHC，零改动
    Vertex.h
    PrimitiveDesc.h
    Sampling.h
    ObjLoader.h / .cpp
  Platform/
    Window.h / .cpp         ← GLFW 封装（ZHC 精简版 + GLFW_INCLUDE_VULKAN）
  Application.h / .cpp      ← 新增：装配顺序 + 主循环骨架
  VkBackend/                ← 新增：Vulkan 对象封装层
    VulkanContext.*         Instance / PhysicalDevice / LogicalDevice / Queue / 校验层
    VulkanAllocator.*       手写显存分配器
    VulkanBuffer.*          VkBuffer + 上传/销毁
    VulkanImage.*           VkImage / VkImageView / VkSampler
    VulkanShader.*          SPIR-V 加载 → VkShaderModule
    VulkanPipeline.*        GraphicsPipeline（Dynamic Rendering 版）
    VulkanDescriptor.*      DescriptorPool / SetLayout / 写入
    VulkanCommand.*         CommandPool / CommandBuffer 录制辅助
    VulkanRenderTarget.*    替代 ZHC 的 FrameBuffer
  Scene/
    Scene.h / .cpp          ← 直接复制 ZHC
  Renderer/
    Transform.h  Camera.*  CameraController.*
    Light.h  Material.h  RenderObject.h  RenderSettings.h
    Mesh.h / .cpp           ← 接口保留，实现重写
  PostProcess.h / .cpp      ← 接口保留，实现重写
  SceneRenderer.h / .cpp    ← 接口保留，实现重写
  main.cpp                  ← 只剩 3 行：构造 Application 并 Run
```

**分层铁律**（沿用 ZHC，并向 Vulkan 加一条）：

1. `Scene` 不依赖 `Renderer` —— 渲染器读场景，场景不知道后端存在。
2. `Platform/` 不允许出现图形 API 调用。
3. **新增**：只有 `VkBackend/` 允许出现 `Vk*` 类型与 Vulkan API 调用。
   `Renderer/` 里的 `Mesh` / `SceneRenderer` 只允许操作 `VkBackend` 提供的封装对象。
   这条是为了让"Vulkan 显式性"被关在一个目录里，其余代码保持 ZHC 的可读性。

### 1.1 为什么有 `Application` 而 ZHC 没有

ZHC 的 `main.cpp` 是「装配 + 主循环」全写在里面，686 行。`Application` 把这两件事收进一个类，`main.cpp` 只回答"程序从哪进"。好处：

- 主循环的逻辑有了归属，不会随功能增加无限膨胀
- 将来写离屏测试可以建一个不弹窗口的 `Application`
- **生命周期规则从"注释提醒"变成"编译器保证"**，见 §4.6

---

## 2. 旧 → 新 迁移映射表

### 2.1 直接复制（零 GL 耦合）

| ZHC 文件 | 处理方式 |
|---|---|
| `Core/Vertex.h` | 原样复制 |
| `Core/PrimitiveDesc.h` | 原样复制 |
| `Core/Sampling.h` | 原样复制 |
| `Core/ObjLoader.h/.cpp` | 原样复制 |
| `Scene/Scene.h/.cpp` | 原样复制 |
| `Renderer/Transform.h` | 原样复制 |
| `Renderer/Camera.h/.cpp` | 原样复制 |
| `Renderer/CameraController.h/.cpp` | 原样复制 |
| `Renderer/Light.h` | 原样复制 |
| `Renderer/RenderSettings.h` | 复制后微调 `DebugView`（见 §4.5） |

### 2.2 接口保留、实现重写

| ZHC 类 | Vulkan 对应 | 主要差异 |
|---|---|---|
| `Platform/Window` | `Window`（同名） | 宏改 `GLFW_INCLUDE_VULKAN`；`SwapBuffers()` 由 `VulkanContext::Present()` 承担 |
| `Mesh` | `Mesh`（同名） | 去掉 `m_VertexArray`；持有 `VulkanBuffer` 顶点/索引各一 |
| `VertexBuffer` / `IndexBuffer` | **合并为 `VulkanBuffer`** | Vulkan 里两者同类型，靠 usage 位区分 |
| `VertexArray` | **删除** | Vulkan 无 VAO，见 §4.2 |
| `Texture` / `TextureLibrary` | `VulkanImage` + `Texture` | 句柄类型改；layout 转换是新负担 |
| `FrameBuffer` | `VulkanRenderTarget` | 无 FBO 对象，只需 Image + ImageView |
| `Shader` | `VulkanShader` + `VulkanPipeline` | 见 §4.3，语义变化最大 |
| `RenderCommand` | **拆分** | 静态清屏/开关 → 进 `VulkanCommand`；Draw → 进各自 Pass |
| `SceneRenderer` | `SceneRenderer`（同名） | 两个 Pass 改为两次 `vkCmdBeginRendering` |
| `PostProcess` | `PostProcess`（同名） | ping-pong 结构不变，每级一次 BeginRendering |

### 2.3 已废弃的 ZHC 概念

| 概念 | 去向 |
|---|---|
| `glad` | 删除，Vulkan 函数从 `vulkan-1.lib` 静态导入 |
| `uint32_t GetRendererID()` | 改为返回具体的 `Vk*` 句柄类型（不再是整数 ID） |
| `basicShader.SetInt("u_Texture", 0)` 纹理单元绑定 | 完全消失，改为 DescriptorSet 绑定 |

---

## 3. 迁移里程碑

每阶段产出可运行程序，编译 0 错误 0 警告后再进入下一阶段。

| # | 阶段 | 产出 | 验收标准 |
|---|---|---|---|
| 0 | 窗口骨架 | ✅ 已完成 | 窗口能开，`vkEnumerateInstanceExtensionProperties` 打印扩展数 |
| 1 | `VulkanContext` | ✅ 已完成 | 打印设备名（RTX 4070 SUPER / API 1.4.341），校验层无 ERROR |
| 1.5 | `Window` + `Application` 拆分 | ✅ 已完成 | 析构顺序实验通过：Context 先于 Window 销毁 |
| 2 | 分配器 + Buffer | 进行中 | 分配/释放若干次不泄漏，能 memcpy 上传读到 |
| 3 | 交换链 + 命令 + 同步 | Swapchain + CommandPool + Fence/Semaphore | **画面清屏为固定色，稳定 Present** |
| 4 | Pipeline + Dynamic Rendering | `VulkanPipeline` + 第一个 SPIR-V | **画出三角形** |
| 5 | Mesh 迁移 | `Mesh` 持有 Vulkan 顶点/索引缓冲 | 画出 obj 模型（无纹理） |
| 6 | Descriptor + UBO | 相机 / 模型矩阵 | 多物体 + WASD 视角能动（接 `CameraController`） |
| 7 | Image + Sampler | 纹理 + mipmap + EnvMap | 贴图与天空盒显示正确，注意 **sRGB 与 mip** |
| 8 | Depth + Shadow Pass | 第二次 BeginRendering | **PCSS 软阴影可用**（对应 `RenderShadowPass`） |
| 9 | RenderTarget + PostProcess | Bloom + FXAA | 中间图切换正确（对应 `RenderSettings::DebugView`） |
| 10 | ImGui Vulkan 后端 | `EditorUI` 迁移 | 面板恢复，能显示在纹理预览 |

阶段 4 之前都属于"样板期"，不要急着迁移业务逻辑；
**从阶段 5 开始，才逐步把 ZHC 的类搬过来**。

---

## 4. 关键设计决策与陷阱

### 4.1 Window 宏冲突

ZHC 的 `Window.h` 强制定义 `GLFW_INCLUDE_NONE`，为的是避开 glad 的 GL 头。
**Vulkan 版必须反过来**——创建表面需要 GLFW 声明 Vulkan 相关类型：

```cpp
#define GLFW_INCLUDE_VULKAN      // 不要用 GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
```

同时 `SwapBuffers()` 在 Vulkan 下无法对应任何单一调用，改为由 `VulkanContext` 提供：

```cpp
// 旧：window.SwapBuffers();
// 新：context.Present();   // 内部 vkQueuePresentKHR
```

### 4.2 VAO 消失

ZHC 的 `SceneRenderer` 里有一个空 `VertexArray m_FullScreenVAO`，注释写着"core profile 下必须绑一个 VAO 才能 draw"。
**Vulkan 里这件事彻底不存在**，全屏三角形直接：

```cpp
vkCmdDraw(cmd, 3, 1, 0, 0);   // 顶点由 gl_VertexID 生成
```

反过来，ZHC 的顶点属性布局在 `SetupBuffers()` 里配置一次、之后永远绑定；
Vulkan 里它**属于 Pipeline 状态**（`VkPipelineVertexInputStateCreateInfo`），必须在 `VulkanPipeline` 创建时声明，
且顶点格式声明与实际 Mesh 缓冲布局要手动保持一致——**没有 VAO 帮你记住这件事**。

### 4.3 Shader uniform 机制差异（最深的一道坎）

ZHC 的用法是随时绑定后按名字写：

```cpp
basicShader.Bind();
basicShader.SetMat4("u_ViewProjection", vp);
basicShader.SetInt("u_ShadowMap", 1);
```

Vulkan 里 **没有"当前 shader 对象"这种状态**，`SetXxx(name, value)` 无从实现。改为三种机制分工：

| ZHC 写法 | Vulkan 方案 | 适用 |
|---|---|---|
| 每帧变一次的矩阵 | **Push Constant** | ≤256 字节；`u_Model` 首选项 |
| 整场共享的数据 | **UBO** | `u_ViewProjection`、光源数组、PCSS 的 32 个 Poisson 点 |
| 纹理 | **Combined Image Sampler** | albedo / normal / glow / shadowMap / envMap |

注意 ZHC 的 `SceneRenderer` 有 `PackLights()` 产出**平行**的若干 `std::vector<int/vec3/vec2>`，
这套 CPU 打包逻辑可直接复制，只需把最后的 `UploadLights()` 从逐个 `SetXXX` 改成
一次 `vkUpdateDescriptorSets` 或一次 UBO 写入。

### 4.4 后处理的 ping-pong

ZHC 结构（`PostProcess::Render` → `Blit(downsample/up/composite/fxaa)` + `m_Levels[6]` + `m_Output`）
**整体可以直接平移**，每个 `Blit` 换成一次 `vkCmdBeginRendering` / `vkCmdEndRendering`。
真正的新增负担是**每次目标从"被渲染"变成"被采样"时都要做 image layout 转换**：

```
COLOR_ATTACHMENT_OPTIMAL  ⇄  SHADER_READ_ONLY_OPTIMAL
```

建议在 `VulkanRenderTarget` 内提供 `TransitionToColor(vkCmd)` / `TransitionToSampled(vkCmd)` 两个方法，
不要在业务代码里手写 `VkImageMemoryBarrier`。

### 4.5 RenderSettings::DebugView

ZHC 里 `GetColorAttachmentID()` 返回 GL 纹理 ID，ImGui 直接当 `ImTextureID` 用（阶段 10 才会碰到）。
Vulkan 下需要配对的 `(VkImageView, VkSampler)`，ImGui Vulkan 后端使用 `VkDescriptorSet`。
建议现在就在 `RenderSettings.h` 预留一个句柄结构，避免阶段 10 返工。

### 4.6 Application 的生命周期规则

销毁顺序有【两道】保证，主路径是显式的：

1. **主路径**：`Application::Shutdown()` 里显式 `m_Context.reset()`，
   在 `Run()` 末尾执行，此时 `m_Window` 仍活着。顺序意图写在代码里，不依赖约定。
2. **兜底**：`Application.h` 里 `m_Window` 声明在 `m_Context` 之前，
   即使 reset 被删掉，成员逆序析构依然保证 Context 先死。

【为什么不能只靠兜底】顺序错了不报错、不崩、画面照常，
只会在阶段 3 加入 Surface 之后变成随机崩溃。所以两道都留着。

验收方法：先注释掉 `m_Context.reset()` 复现"只有兜底"的状态，
再对调成员声明顺序，此时输出才会反转。reset 在时对调声明顺序看不到任何变化。

---

## 5. 实现要点

### 5.1 Dynamic Rendering 用法

SDK 头文件已达 Vulkan 1.3，`vkCmdBeginRenderingKHR` 已在核心中，无需挂扩展。一次 Pass 的形态：

```cpp
VkRenderingAttachmentInfo colorAttachment{};
colorAttachment.imageView   = target.GetColorView();
colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
colorAttachment.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
colorAttachment.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
colorAttachment.clearValue  = clearColor;

VkRenderingInfo renderingInfo{};
renderingInfo.renderArea          = { {0,0}, {w, h} };
renderingInfo.colorAttachmentCount = 1;
renderingInfo.pColorAttachments   = &colorAttachment;
renderingInfo.pDepthAttachment    = &depthAttachment;   // 阴影 Pass 只挂深度

vkCmdBeginRendering(cmd, &renderingInfo);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdDraw(cmd, ...);
vkCmdEndRendering(cmd);
```

这比 ZHC 的 `FrameBuffer` 更贴近原语义——场景 Pass 与阴影 Pass 只是
`pColorAttachments` 为空 vs 非空的差别，正好对应 ZHC 里 `FrameBufferSpec::DepthOnly` 开关。

### 5.2 手写显存分配器

目标 ~300–500 行，建议按这个顺序实现，每步都可单独验证：

1. **类型筛选**：查 `VkPhysicalDeviceMemoryProperties`，选出 `DEVICE_LOCAL` / `HOST_VISIBLE|HOST_COHERENT` 等组合的 `memoryTypeIndex`。
2. **按用途分池**：顶点/索引（device local）、UBO（host visible，常驻映射）、Image（device local）。每种池固定最小块大小（如 256 MB / 64 KB 两档）。
3. **块内线性分配 + free list**：小于阈值走 TLSF 或简单的 best-fit；大内存直接整块 `vkAllocateMemory` 独占。
4. **持久映射**：host visible 内存一次性 `vkMapMemory` 后永不 unmap，返回指针，简化 UBO 每帧更新。
5. **对齐**：务必遵守 `VkMemoryRequirements::alignment`，忘记对齐是最容易出的错。

分配器只管内存，不管 `VkBuffer`/`VkImage` 对象本身——那两个由 `VulkanBuffer` / `VulkanImage` 各自创建，再向分配器申请 backing memory。

### 5.3 premake5.lua 需要补的两件事

**第一件（已修）**：必须有 Lib 目录，否则 `LINK : fatal error LNK1104: 无法打开文件"vulkan-1.lib"`。

```lua
libdirs { os.getenv("VULKAN_SDK") .. "/Lib" }
```

**第二件（阶段 4 前要加）**：着色器转 SPIR-V 的预编译步骤。在 `Gemerate.bat` 里加一段，遍历 `assets/shaders/*.glsl` 生成 `.spv`：

```
"%VULKAN_SDK%\Bin\glslangValidator.exe" -V assets/shaders/PCSS.glsl -o bin/.../PCSS.spv
```

注意 ZHC 的 `.glsl` 是**单文件双阶段**（`Shader::PreProcess` 靠 `#type vertex` / `#type fragment` 拆分成多个 source）。
已确认 `assets/shaders/` 下 **7 个着色器全部是这种格式**。

glslangValidator 一次只能编译一个阶段，所以迁移时**必须先把 combined 文件拆成 `.vert` / `.frag`**。
建议写一个小的切分脚本（读 `#type` 按段写出临时文件再调用 glslangValidator），
而不是手工维护 14 个碎文件——否则以后改 shader 会忘记同步另一半。
这一步建议单独做，别混在编译流程里调试。

---

## 6. 与 AI 协作的约定

以下约定用于保证跨会话的连续性。

### 6.1 给 AI 的指令格式

定位到文件时**给绝对路径**，不要转述内容：

```
参考 e:\GameCreate\workspace\ZHC\src\Renderer\SceneRenderer.cpp
把 RenderSceneByPCSS 的绘制逻辑迁移到 Vulkan（当前阶段 8）
```

ZHC 是只读参考，AI 可直接读取，不需要粘贴代码。

### 6.2 每次只推进一个阶段

按 §3 里程碑推进。指令形如"继续阶段 5"，不要说"帮我重构"。
Vulkan 的一千行样板决定了无法一次性生成可工作的代码。

### 6.3 报错时给原始输出

请贴编译器/校验层的**原样文本**（含错误编号），不要用自然语言概括。
校验层（Validation Layer）的输出尤其重要：它的报错常常不出现在 Visual Studio 的错误列表里，
只出现在控制台输出窗口。

### 6.4 代码风格继承 ZHC

ZHC 的每个类头部都有「职责边界 / 依赖规则 / 为什么不这样写」注释块，这是它最容易维护的地方，
迁移时必须保留并在新代码里延续。新写的 Vulkan 封装同样要求：
**每个类开头写明它管什么、不管什么、依赖谁。**

### 6.5 进度清单

> 每完成一个阶段更新此处。

- [x] **阶段 0**：窗口 + Vulkan 头文件可用
- [x] **阶段 1**：`VulkanContext`（Instance / PhysicalDevice / LogicalDevice / Queue / 校验层）
      RTX 4070 SUPER，driver API 1.4.341，graphics queue family 0，无 ERROR
- [x] **阶段 1.5**：`Platform/Window` + `Application` 拆分，`main.cpp` 缩到 3 行
      析构顺序已验证：Context 先于 Window 销毁
- [ ] **阶段 2**：`VulkanAllocator` + `VulkanBuffer`
- [ ] 阶段 3–10：见 §3

---

## 7. 已踩过的坑

按踩到的时间顺序记录。每一条都是"下次直接查这里"。

| # | 现象 | 根因 | 修正 |
|---|---|---|---|
| 1 | `LINK : fatal error LNK1104: 无法打开文件"vulkan-1.lib"` | `includedirs` 只解决头文件，链接器找 `.lib` 走独立的 `libdirs` | `premake5.lua` 加 `libdirs { os.getenv("VULKAN_SDK") .. "/Lib" }`，改完必须重新生成工程 |
| 2 | `warning C4551: 缺少参数列表的函数调用`（`glfwDestroyWindow;`） | 少了参数列表，那是个函数指针值而不是调用 | `glfwDestroyWindow(window);` |
| 3 | `GLFW_INCLUDE_VULKAN` 不生效 | 宏写在 `#include <GLFW/glfw3.h>` **之后** | 宏必须写在被控制的头文件之前；现在统一在 `Window.h` 顶部定义 |
| 4 | `Application::m_Window 使用未定义的 class Window` | **值成员不能只用前向声明**，编译器需要完整大小才能布局 | `Application.h` 直接 `#include "Platform/Window.h"`；`unique_ptr` 成员才可以前向声明 |
| 5 | 控制台刷出 60+ 行 `Located json file` / `layer callstack` | 挂到 `instanceCreateInfo.pNext` 的 debugCreateInfo 订阅了 VERBOSE/INFO，Loader 启动时会向它倾倒全部扫描日志 | 那份的 `messageSeverity` 收窄为 WARNING/ERROR，与 `SetupDebugMessenger` 一致 |
| 6 | 窗口尺寸取错导致画面拉伸（未实际触发，预先规避） | `glfwCreateWindow` 的逻辑尺寸 ≠ framebuffer 像素尺寸，高 DPI 屏上差一个缩放比 | `Window` 构造末尾用 `glfwGetFramebufferSize` 覆盖，`GetWidth/GetHeight` 返回像素 |

### 7.1 关于校验层日志的补充

本机装了大量 overlay 层（Overwolf / OBS / Steam / GamePP / mediasdkhook / Social Club），
它们通过注册表以 Implicit Layer 形式插入到每个 Vulkan 程序。表现：

- 启动时刷出大量 `Located json file`（Loader 扫描注册表，**禁用这些层也消不掉**，只能靠收窄 severity）
- 一条 WARNING：`Layer VK_LAYER_OW_OVERLAY uses API version 1.2 which is older than the application specified API version of 1.3`（overlay 层自身发的，无害）
- 抓帧/调试时可能捣乱

**不要设系统级环境变量禁用它们**——会影响你玩 Vulkan 游戏时的 overlay。
要禁就在 VS 项目属性 → 调试 → 环境里设（只影响从 VS 启动的本项目）：

```
DISABLE_VULKAN_OW_OVERLAY_LAYER=1;DISABLE_VULKAN_OW_OBS_CAPTURE=1
```

---

## 8. 下一步

**阶段 2：`VulkanAllocator` + `VulkanBuffer`**（见 §3 与 §5.2）。

计划内容：

1. `VulkanAllocator`：查 `VkPhysicalDeviceMemoryProperties` 得内存类型 → 按用途分池（device-local / host-visible）→ 块内分配 → host-visible 持久映射。
2. `VulkanBuffer`：创建 `VkBuffer` → 向分配器要 backing memory → `vkBindBufferMemory` → 上传/销毁接口。
3. 验收：在 `Application::Init()` 里写一段临时测试，分配若干个 buffer、写入数据、读回来比对，确认偏移与对齐没算错。
