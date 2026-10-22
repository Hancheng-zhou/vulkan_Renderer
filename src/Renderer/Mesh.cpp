#include "Renderer/Mesh.h"

#include "VkBackend/VulkanBuffer.h"
#include "VkBackend/VulkanCommandPool.h"
#include "VkBackend/VulkanAllocator.h"

#include <algorithm>   // std::max
#include <cmath>       // std::sin / std::cos
#include <cstdio>
#include <stdexcept>
#include <utility>

#include <glm/gtc/constants.hpp>   // glm::pi 
// ---------------------------------------------------------------------------
//  内部工具：把 CPU 数据放进 device-local 缓冲
// ---------------------------------------------------------------------------
namespace
{
    std::unique_ptr<VulkanBuffer> CreateDeviceBuffer(const MeshContext& context,
        const void* data,
        VkDeviceSize size,
        VkBufferUsageFlags usage)
    {
        VulkanBuffer::CreateInfo info{};
        info.size = size;
        // usage 由调用方给（VERTEX_BUFFER / INDEX_BUFFER），
        // 但【两者都必须加上 TRANSFER_DST】，否则 staging 拷贝会被拒绝
        info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.memoryUsage = VulkanAllocator::Usage::GPUOnly;

        auto buffer = std::make_unique<VulkanBuffer>(context.Device,
            *context.Allocator,
            info);

        buffer->UploadStaged(*context.CommandPool, context.GraphicsQueue, data, size);

        return buffer;
    }
}

Mesh::~Mesh() = default;

Mesh::Mesh(const MeshContext& context, std::vector<Vertex> vertices, std::vector<uint32_t> indices)
    :m_Device(context.Device),m_VertexCount(vertices.size()),m_IndexCount(indices.size())
{
    if (!context.IsValid())
    {
        throw std::runtime_error("[Mesh] invalid MeshContext");
    }
    if (vertices.empty() || indices.empty())
    {
        throw std::runtime_error("[Mesh] empty geometry");
    }
    m_VertexBuffer = CreateDeviceBuffer(context, 
                                        vertices.data(), 
                                        sizeof(Vertex) * m_VertexCount, 
                                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    m_IndexBuffer = CreateDeviceBuffer(context,
        indices.data(),
        sizeof(uint32_t) * m_IndexCount,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

std::unique_ptr<Mesh> Mesh::Create(const MeshContext& context, const PrimitiveDesc& desc)
{
    switch (desc.Type)
    {
    case PrimitiveType::Cube:
    {
        return CreateCube(context, desc.Size);

    }
    case PrimitiveType::Sphere:
    {
        return CreateSphere(context, desc.Size, desc.Segments, desc.Rings);
    }
    case PrimitiveType::Obj:
        // 阶段 5c 实现 ObjLoader，先退回立方体而不是崩掉
        std::fprintf(stderr, "[Mesh] Obj loading not implemented yet, falling back to cube\n");
        return CreateCube(context, desc.Size);
    default:
        break;
    }

    // 兜底：枚举加了新值但这里忘补 case。走到函数末尾没 return 是未定义行为
    return CreateCube(context, desc.Size);
}

std::unique_ptr<Mesh> Mesh::CreateCube(const MeshContext& context, float size)
{
    // 立方体以【原点为中心】，size 是边长。
    // 居中很关键：绕任意轴旋转、围绕它缩放时，原点在中心才不会"甩出去"
    const float h = size * 0.5f;

    // 24 个顶点（不是 8 个）：法线和 UV 是"按面"的。
    // 共享 8 个角点的话，一个角点只能有一个法线，
    // 三个相邻面就会互相污染，光照出现错误渐变。
    //
    // 每个面内部按【左下 -> 右下 -> 右上 -> 左上】排列，
    // 从面外部看是逆时针（配合 CCW 正面 + 背面剔除）
    std::vector<Vertex> vertices =
    {
        // +Z 面（前）
        { { -h, -h,  h }, {  0,  0,  1 }, { 0, 0 } },
        { {  h, -h,  h }, {  0,  0,  1 }, { 1, 0 } },
        { {  h,  h,  h }, {  0,  0,  1 }, { 1, 1 } },
        { { -h,  h,  h }, {  0,  0,  1 }, { 0, 1 } },

        // -Z 面（后）—— 起点换到 +X 侧，从背后看仍是逆时针
        { {  h, -h, -h }, {  0,  0, -1 }, { 0, 0 } },
        { { -h, -h, -h }, {  0,  0, -1 }, { 1, 0 } },
        { { -h,  h, -h }, {  0,  0, -1 }, { 1, 1 } },
        { {  h,  h, -h }, {  0,  0, -1 }, { 0, 1 } },

        // +X 面（右）
        { {  h, -h,  h }, {  1,  0,  0 }, { 0, 0 } },
        { {  h, -h, -h }, {  1,  0,  0 }, { 1, 0 } },
        { {  h,  h, -h }, {  1,  0,  0 }, { 1, 1 } },
        { {  h,  h,  h }, {  1,  0,  0 }, { 0, 1 } },

        // -X 面（左）
        { { -h, -h, -h }, { -1,  0,  0 }, { 0, 0 } },
        { { -h, -h,  h }, { -1,  0,  0 }, { 1, 0 } },
        { { -h,  h,  h }, { -1,  0,  0 }, { 1, 1 } },
        { { -h,  h, -h }, { -1,  0,  0 }, { 0, 1 } },

        // +Y 面（上）
        { { -h,  h,  h }, {  0,  1,  0 }, { 0, 0 } },
        { {  h,  h,  h }, {  0,  1,  0 }, { 1, 0 } },
        { {  h,  h, -h }, {  0,  1,  0 }, { 1, 1 } },
        { { -h,  h, -h }, {  0,  1,  0 }, { 0, 1 } },

        // -Y 面（下）
        { { -h, -h, -h }, {  0, -1,  0 }, { 0, 0 } },
        { {  h, -h, -h }, {  0, -1,  0 }, { 1, 0 } },
        { {  h, -h,  h }, {  0, -1,  0 }, { 1, 1 } },
        { { -h, -h,  h }, {  0, -1,  0 }, { 0, 1 } },
    };

    // 每面拆两个三角形：0-1-2 与 0-2-3。
    //
    // 【千万别写 0-1-2 / 1-2-3】那样第二个三角形绕序翻转，
    // 会被背面剔除吃掉，表现是"每个面都缺一半"
    std::vector<uint32_t> indices;
    indices.reserve(36);

    for (uint32_t face = 0; face < 6; ++face)
    {
        const uint32_t base = face * 4;

        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);

        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }

    return std::make_unique<Mesh>(context, std::move(vertices), std::move(indices));
}

std::unique_ptr<Mesh> Mesh::CreateSphere(const MeshContext& context, float radius, uint32_t segments, uint32_t rings)
{
    // 下限保护：0 会除零，1 会退化成一条线段
    segments = max(segments, 3u);
    rings = max(rings, 2u);

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    vertices.reserve(static_cast<std::size_t>(segments + 1) * (rings + 1));
    indices.reserve(static_cast<std::size_t>(segments) * rings * 6);

    constexpr float pi = glm::pi<float>();

    // ---- 顶点：纬线 × 经线 ----
    //
    // 为什么是 segments+1 / rings+1：每个方向多出的那一圈与起始位置重合，
    // 但 u 分别是 0 和 1，让 UV 接缝恰好落在重复的那一圈上。
    // 只生成 segments 个的话，最后一段的 u 会从 (n-1)/n 直接插值回 0，
    // 贴图被整条反向压缩，看起来像一道撕裂
    for (uint32_t y = 0; y <= rings; ++y)
    {
        const float v = static_cast<float>(y) / static_cast<float>(rings);
        const float phi = pi * v;            // 极角：0 = 北极(+Y)
        const float sinPhi = std::sin(phi);
        const float cosPhi = std::cos(phi);

        for (uint32_t x = 0; x <= segments; ++x)
        {
            const float u = static_cast<float>(x) / static_cast<float>(segments);
            const float theta = 2.0f * pi * u;  // 方位角：绕 Y 轴一圈

            // 单位球上，位置方向就是法线 —— 省一次 normalize
            const glm::vec3 n(std::sin(theta) * sinPhi,
                cosPhi,
                std::cos(theta) * sinPhi);

            Vertex vertex;
            vertex.Position = n * radius;             // 球心在原点
            vertex.Normal= n;
            vertex.Texcoord = glm::vec2(u, 1.0f - v); // V 翻转，与 Cube 约定一致
            vertices.push_back(vertex);
        }
    }

    // ---- 索引：相邻两圈拼四边形，各拆两个三角形 ----
    //
    // 绕序必须与 CreateCube 一致（CCW）：
    //   ?P/?φ × ?P/?θ = sinφ · N，sinφ > 0，
    //   所以 a→c→b（先走 φ 再走 θ）得到的法线朝球外
    for (uint32_t y = 0; y < rings; ++y)
    {
        for (uint32_t x = 0; x < segments; ++x)
        {
            const uint32_t a = y * (segments + 1) + x;
            const uint32_t b = a + 1;
            const uint32_t c = a + (segments + 1);
            const uint32_t d = c + 1;

            indices.push_back(a); indices.push_back(c); indices.push_back(b);
            indices.push_back(b); indices.push_back(c); indices.push_back(d);
        }
    }

    return std::make_unique<Mesh>(context, std::move(vertices), std::move(indices));
}
void Mesh::Bind(VkCommandBuffer cmd) const
{
    // 顶点缓冲：第一个参数 0 必须与管线里的 binding 编号一致
    VkBuffer     vertexBuffer = m_VertexBuffer->GetHandle();
    VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &vertexOffset);

    // 索引缓冲：VK_INDEX_TYPE_UINT32 对应我们的 uint32_t 索引。
    // 用错类型（比如 UINT16）不会报错，只会读到错误的索引值
    vkCmdBindIndexBuffer(cmd, m_IndexBuffer->GetHandle(), 0, VK_INDEX_TYPE_UINT32);
}
void Mesh::Draw(VkCommandBuffer cmd) const
{
    // 参数：(命令缓冲, 索引数, 实例数, 首个索引, 顶点偏移, 首个实例)
    //
    // 【vertexOffset 与 index 里的值是相加关系】
    //   我们的索引从 0 开始，所以这里填 0。
    //   如果将来把多个 Mesh 打进一个缓冲，就要用它做偏移。
    vkCmdDrawIndexed(cmd, m_IndexCount, 1, 0, 0, 0);
}