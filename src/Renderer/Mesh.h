// ===========================================================================
//  Renderer/Mesh.h
//
//  职责边界：
//    管：一份几何的 CPU 数据、对应的两个 VulkanBuffer、绑定与绘制
//    不管：物体在哪、长什么样（那是 RenderObject / Material）
//
//  与 ZHC 的 Mesh 的差别：
//    ZHC 持有 VertexArray + VertexBuffer + IndexBuffer 三件套（GL 概念）。
//    Vulkan 没有 VAO，顶点布局属于 Pipeline，所以这里只剩两个缓冲。
//
//  为什么需要 MeshContext：
//    创建缓冲需要 device、allocator、command pool、queue 四样东西。
//    每个生成函数都传四个参数会让签名爆炸，也会让调用方反复犯错。
//    打包成一个结构体，"环境"和"数据"就分开了。
// ===========================================================================
#pragma once
#include "Core/Vertex.h"
#include "Core/PrimitiveDesc.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>
class VulkanAllocator;
class VulkanCommandPool;
class VulkanBuffer;

struct MeshContext
{
	VkDevice Device = VK_NULL_HANDLE;
	VulkanAllocator* Allocator = nullptr;
	VulkanCommandPool* CommandPool = nullptr;
	VkQueue GraphicsQueue = VK_NULL_HANDLE;
	bool IsValid() const
	{
		return Device != VK_NULL_HANDLE && Allocator != nullptr &&
		       CommandPool != nullptr && GraphicsQueue != VK_NULL_HANDLE;
	}
};

class Mesh
{
public:
	Mesh(const MeshContext& context, std::vector<Vertex> vertices, std::vector<uint32_t> indices);

	// 【析构必须在 .cpp 里定义，不能写 = default】
	//   它要销毁 unique_ptr<VulkanBuffer>，而 VulkanBuffer 在本头文件里
	//   只有前向声明。写 = default 会在所有 include 本头的翻译单元里
	//   实例化析构函数，那里看不到完整类型 -> "use of undefined type"。
	~Mesh();
	Mesh(const Mesh&) = delete;
	Mesh& operator=(const Mesh&) = delete;
	Mesh(Mesh&&) = default;
	Mesh& operator=(Mesh&&) = default;

	// 配方 -> 几何的唯一分派点。
	// 【为什么要有这一层】没有它，每个调用点都得自己 switch，
	//   "支持哪些形状"会有 N 份副本，加形状时漏一个就出问题
	static std::unique_ptr<Mesh> Create(const MeshContext& context, const PrimitiveDesc& desc);

	static std::unique_ptr<Mesh> CreateCube(const MeshContext& context, float size = 1.0f);
	static std::unique_ptr<Mesh> CreateSphere(const MeshContext& context,
		float radius = 1.0f,
		uint32_t segments = 32,
		uint32_t rings = 16);
	// 录制用：绑定顶点/索引缓冲，然后发起索引绘制
	void Bind(VkCommandBuffer cmd) const;
	void Draw(VkCommandBuffer cmd) const;
	uint32_t GetIndexCount()  const { return m_IndexCount; }
	uint32_t GetVertexCount() const { return m_VertexCount; }
private:
	VkDevice m_Device = VK_NULL_HANDLE;

	std::unique_ptr<VulkanBuffer> m_VertexBuffer;
	std::unique_ptr<VulkanBuffer> m_IndexBuffer;

	uint32_t m_VertexCount = 0;
	uint32_t m_IndexCount = 0;

};

