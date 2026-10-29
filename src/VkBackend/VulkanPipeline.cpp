#include "VkBackend/VulkanPipeline.h"

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

namespace
{
    void ThrowOnFailed(VkResult result, const char* whatFailed)
    {
        if (result != VK_SUCCESS)
        {
            throw std::runtime_error(std::string("[VulkanPipeline] ") + whatFailed +
                " failed, VkResult = " +
                std::to_string(static_cast<int>(result)));
        }
    }
}

VulkanPipeline::VulkanPipeline(const CreateInfo& info):m_Device(info.device)
{
    // ---- 1. 加载并创建 shader module -------------------------------------
    const std::vector<char> vertCode = ReadFile(info.vertexSpvPath);
    const std::vector<char> fragCode = ReadFile(info.fragSpvPath);

    VkShaderModule vertModule = CreateShaderModule(vertCode);
    VkShaderModule fragModule = CreateShaderModule(fragCode);
    // ---- 2. 两个可编程阶段 -----------------------------------------------
    //
    //  pName = "main"：GLSL 的入口函数名。Vulkan 允许任意入口名，
    //  这里保持传统写法。
    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName = "main";
    const VkPipelineShaderStageCreateInfo shaderStages[] = { vertStage, fragStage };
    // ---- 3. 顶点输入：空 -------------------------------------------------
    //
    //  【为什么可以全空】三角形的位置由着色器里的 gl_VertexID 生成，
    //    不从任何 VkBuffer 读数据。阶段 5 接顶点缓冲时，
    //    这里要填 bindingDescription 和 attributeDescriptions。
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(info.vertexBindings.size());
    vertexInput.pVertexBindingDescriptions = info.vertexBindings.empty() ? nullptr : info.vertexBindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(info.vertexAttributes.size());
    vertexInput.pVertexAttributeDescriptions = info.vertexAttributes.empty() ? nullptr : info.vertexAttributes.data();
    // ---- 4. 图元装配 -----------------------------------------------------
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // primitiveRestartEnable 保持 false：只有 strip 拓扑才需要它
    // ---- 5. 视口状态：数量必须填，具体值靠动态设置（application上设置） -----------------------
    //
    //  【坑点】即使 viewport/scissor 是动态的，这里的 count 也必须填 1。
    //    count 为 0 会被校验层判为非法（它检查的是"声明了要几个"）。
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    // pViewports / pScissors 留空 —— 由 vkCmdSetViewport / vkCmdSetScissor 提供
    // ---- 6. 光栅化 -------------------------------------------------------
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;

    // ---- 面剔除 ----------------------------------------------------------
    //
    // 【Vulkan 的绕序和 OpenGL 相反 —— 这是最容易配错的一对参数】
    //   Vulkan 的 NDC 里 y 轴【朝下】，OpenGL 朝上。
    //   同一组"从外部看逆时针"的顶点，在 Vulkan 屏幕空间里看起来是顺时针。
    //
    //   我们的顶点数据照搬 ZHC（按 OpenGL 的 CCW 约定排列），
    //   所以必须声明 FRONT_FACE_CLOCKWISE，驱动才会把它认作正面。
    //
    //   配反的后果是静默的：物体整个消失，或者只剩内部的面，
    //   校验层不会报任何错。
    rasterizer.cullMode  = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.depthBiasEnable = VK_FALSE;
    // ---- 7. 多重采样 -----------------------------------------------------
    //
    // 【这里就是 MSAA 的开关】
    //   rasterizationSamples = 1 时，每个像素只采样中心点，边缘是硬切换（锯齿）。
    //   = 4 时，驱动把每个像素拆成 4 个子样本位置分别判定覆盖，
    //   边缘像素会得到 1/4、2/4、3/4 的部分覆盖率，
    //   最终由 resolve 平均成一个中间色。
    //
    // 【为什么它必须和附件一致】
    //   管线说"我会用 4x 光栅化"，而颜色附件只有 1 个样本时，
    //   驱动没有地方存放另外 3 个子样本的结果 —— 直接创建失败。
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = info.samples;

    // 【sampleShadingEnable 保持 FALSE】
    //   TRUE = 每个子样本都单独跑一遍片元着色器（per-sample shading），
    //          质量略高但着色成本 ×4。
    //   FALSE = 每个像素只着色一次，覆盖率只影响 resolve 时的权重。
    //   绝大多数场景用 FALSE —— MSAA 的价值在几何边缘，不在着色精度。
    multisampling.sampleShadingEnable = VK_FALSE;
    // ---- 8. 颜色混合：直接覆盖 -------------------------------------------
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.blendEnable = VK_FALSE;   // 不做混合，fragment 输出直接写
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;
    // ---- 8.5 深度/模板状态 -----------------------------------------------
    //
    //  【这里是 OpenGL 的 glEnable(GL_DEPTH_TEST) + glDepthFunc(GL_LESS)
    //    搬进不可变管线后的形态】
    //
    //  depthTestEnable   —— 是否比较深度。关掉就是 6c 里"后面盖住前面"的行为
    //  depthWriteEnable  —— 通过比较后是否写回深度图。
    //                       透明物体阶段会把它关掉（半透明不该遮挡后面的东西）
    //  depthCompareOp    —— 判定规则。LESS = "新深度更小才通过"。
    //                       因为我们的投影矩阵是 [0,1]（GLM_FORCE_DEPTH_ZERO_TO_ONE），
    //                       近处 = 0，远处 = 1，所以"更小"就是"更近"。
    //                       【填错成 GREATER 的表现是物体像从里面看，前后完全颠倒】
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;
    // 这两项只在 depthBoundsTestEnable 为 TRUE 时生效，
    // 但校验层要求它们落在 [0,1] 内，所以顺手填对。
    depthStencil.minDepthBounds = 0.0f;
    depthStencil.maxDepthBounds = 1.0f;
    // ---- 9. 动态状态 -----------------------------------------------------
    //
    //  声明"这两项在录制时设置"。管线里不固化它们，resize 时就不必重建。
    const VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // ---- 10. 管线布局 ----------------------------------------------------
    //
    //  布局描述"shader 能访问哪些外部资源"（descriptor set / push constant）。
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

    // 推送常量区间：告诉驱动"顶点阶段会从 offset 0 读这么多字节"。
    //
    // 【必须与 shader 的 layout(push_constant) 块完全一致】
    //   size 小于实际使用 -> 校验层在 vkCmdPushConstants 时报越界
    //   stageFlags 少了 VERTEX -> 顶点着色器里读到的值是未定义的
    VkPushConstantRange pushRange{};
    if (info.pushConstantSize > 0)
    {
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset     = 0;
        pushRange.size       = info.pushConstantSize;

        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges    = &pushRange;
    }
    // descriptor set 布局：告诉管线"shader 会从 set 0 读资源"
    if (info.descriptorSetLayout != VK_NULL_HANDLE)
    {
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &info.descriptorSetLayout;
    }
    ThrowOnFailed(vkCreatePipelineLayout(m_Device, &layoutInfo, nullptr, &m_Layout),
        "vkCreatePipelineLayout");
    // ---- 11. Dynamic Rendering 的附件格式声明 ----------------------------
    //
    //  【这是与 RenderPass 版本最大的差别】
    //    传统管线靠 VkGraphicsPipelineCreateInfo::renderPass 知道附件格式；
    //    动态渲染没有 RenderPass，只能通过 pNext 挂这个结构显式告知。
    //
    //    漏掉它的典型表现：vkCreateGraphicsPipelines 返回
    //    VK_ERROR_INVALID_SHADER_NV 或校验层报
    //    "pColorAttachmentFormats is required"。
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &info.colorFormat;
    // depthAttachmentFormat / stencilAttachmentFormat 留空（阶段 8 才有深度）
    renderingInfo.depthAttachmentFormat = info.depthFormat;
    renderingInfo.stencilAttachmentFormat = 
        info.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT || 
        info.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ?
        info.depthFormat : VK_FORMAT_UNDEFINED;
    // ---- 12. 组装并创建 --------------------------------------------------
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;      // ← 动态渲染的关键
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_Layout;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    if (info.depthFormat != VK_FORMAT_UNDEFINED)
    {
        pipelineInfo.pDepthStencilState = &depthStencil;
    }
    // 【必须为 VK_NULL_HANDLE】动态渲染不用 RenderPass。
    //   填了有效句柄反而会和 pNext 里的 RenderingCreateInfo 冲突。
    pipelineInfo.renderPass = VK_NULL_HANDLE;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

    const VkResult createResult = vkCreateGraphicsPipelines(
        m_Device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_Pipeline);
    // ---- 13. module 用完即销毁 -------------------------------------------
    //
    //  管线创建时已经把字节码"吸收"进去了，module 本身不再需要。
    //  注意【无论成功失败都要销毁】，否则失败路径会泄漏。
    vkDestroyShaderModule(m_Device, fragModule, nullptr);
    vkDestroyShaderModule(m_Device, vertModule, nullptr);
    if (createResult != VK_SUCCESS)
    {
        vkDestroyPipelineLayout(m_Device, m_Layout, nullptr);
        m_Layout = VK_NULL_HANDLE;

        throw std::runtime_error(
            "[VulkanPipeline] vkCreateGraphicsPipelines failed, VkResult = " +
            std::to_string(static_cast<int>(createResult)));
    }

    std::printf("[Pipeline] created (dynamic rendering, %s, %ux MSAA)\n",
        info.vertexSpvPath.c_str(),
        static_cast<uint32_t>(info.samples));
}

VulkanPipeline::~VulkanPipeline()
{
    // 销毁顺序：pipeline 先，layout 后。
    // layout 是 pipeline 的依赖，反过来会让 pipeline 引用一个已销毁的布局。
    if (m_Pipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
        m_Pipeline = VK_NULL_HANDLE;
    }
    if (m_Layout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_Device, m_Layout, nullptr);
        m_Layout = VK_NULL_HANDLE;
    }
}

std::vector<char> VulkanPipeline::ReadFile(const std::string path)
{
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open())
    {
        throw std::runtime_error("[VulkanPipeline] cannot open shader: " + path);

    }
    const std::streamsize size = static_cast<std::streamsize>(file.tellg());
    if (size <= 0)
    {
        throw std::runtime_error("[VulkanPipeline] empty shader: " + path);
    }
    std::vector<char> buffer(static_cast<size_t>(size));
    file.seekg(0);
    file.read(buffer.data(), size);
    file.close();

    // SPIR-V 必须以魔数 0x07230203 开头。检查一次，能在"忘了编译着色器"
    // 或"编译成了 OpenGL 方言"时立刻给出人话，而不是让驱动报个天书错误。
    if (buffer.size() < 4)
    {
        throw std::runtime_error("[VulkanPipeline] shader too small: " + path);
    }
    const uint32_t magic =
        (static_cast<uint32_t>(static_cast<unsigned char>(buffer[0]))) |
        (static_cast<uint32_t>(static_cast<unsigned char>(buffer[1])) << 8) |
        (static_cast<uint32_t>(static_cast<unsigned char>(buffer[2])) << 16) |
        (static_cast<uint32_t>(static_cast<unsigned char>(buffer[3])) << 24);

    if (magic != 0x07230203u)
    {
        throw std::runtime_error(
            "[VulkanPipeline] not a SPIR-V file (bad magic): " + path +
            "  -- did you run CompileShaders.bat?");
    }

    return buffer;
}

VkShaderModule VulkanPipeline::CreateShaderModule(const std::vector<char>& code)
{
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();

    // 【为什么要 reinterpret_cast】codeSize 的单位是【字节】，
    //   而 pCode 要求 uint32_t*。SPIR-V 规范保证长度是 4 的倍数，
    //   所以这个转换是安全的；但绝不能假设 buffer 的对齐比 uint32_t 好。
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    ThrowOnFailed(vkCreateShaderModule(m_Device, &createInfo, nullptr, &module),
        "vkCreateShaderModule");

    return module;
}
