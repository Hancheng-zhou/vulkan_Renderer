// ===========================================================================
//  VkBackend/VulkanAllocator.h
//
//  职责边界：
//    管：向驱动申请 VkDeviceMemory 大块、块内子分配与释放、内存类型筛选
//    不管：VkBuffer / VkImage 对象的创建（那是 VulkanBuffer / VulkanImage 的事）
//
//  为什么需要它（OpenGL 里没有这个概念）：
//    glBufferData 一行同时做了"申请显存 + 拷数据"。Vulkan 把这两件事拆开，
//    且要求你【自己决定显存怎么分】。直接每个 buffer vkAllocateMemory 一次
//    是最直觉的做法，但驱动对分配次数有上限
//    （VkPhysicalDeviceLimits::maxMemoryAllocationCount，常见 4096），
//    场景资源一多就撞墙，且每次分配都有驱动开销。
//
//    所以本类的形态是：向驱动要【大块】，在大块内部做【子分配】。
//
//  设计取舍（为什么是 free list 而不是更复杂的 TLSF/伙伴算法）：
//    子分配要支持"任意顺序释放"，线性 bump 分配器做不到
//    （ZHC 的 Mesh 会被 RebuildGeometry 销毁重建，必须能单独释放某一段）。
//    first-fit + 相邻合并的 free list 是最简单的可释放方案，
//    一百多行、行为可预测。等项目真的出现分配热点再换。
// ===========================================================================
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

class VulkanAllocator
{
public:
    // 用途决定内存属性，而不是让调用方直接传 VkMemoryPropertyFlags：
    // 把"我要什么"和"驱动怎么满足"隔开，调用侧不需要记住
    // HOST_VISIBLE | HOST_COHERENT 这个组合。
    enum class Usage
    {
        GPUOnly,// DEVICE_LOCAL：GPU 读写最快，CPU 不可见。顶点/索引/纹理最终位置
        CPUToGPU,// HOST_VISIBLE | HOST_COHERENT：CPU 可写，GPU 可读。UBO / 暂存
    };
    struct Allocation
    {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;
        void* mapped = nullptr;
        bool IsValid() const { return memory != VK_NULL_HANDLE; }
    };
    explicit VulkanAllocator(VkDevice device,VkPhysicalDevice physicalDevice);
    ~VulkanAllocator();
    VulkanAllocator(const VulkanAllocator&) = delete;
    VulkanAllocator& operator=(const VulkanAllocator&) = delete;

    // ---- 分配 / 释放 ------------------------------------------------------
    //
    //  typeBits 来自 VkMemoryRequirements::memoryTypeBits，
    //  它是位掩码：第 n 位为 1 表示"这块 buffer/image 可以放在第 n 类内存里"。
    //  必须把它和你要的属性求交集，不能只看属性。
    Allocation Allocate(VkDeviceSize size, VkDeviceSize alignment, uint32_t typeBits, Usage usage);
    void Free(const Allocation& allocation);
    // ---- 统计（接到 ImGui 面板上很直观）----------------------------------
    VkDeviceSize GetTotalAllocateBytes() const { return m_TotalAllocateBytes; }
    VkDeviceSize GetBlockCount() const 
    {
        VkDeviceSize total = 0;
        for (const auto& blocksOfType : m_Blocks)
        {
            total += static_cast<VkDeviceSize>(blocksOfType.size());
        }
        return total;
    }


private:
    // 块内的一段连续空闲空间
    struct FreeRange
    {
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;
    };
    // 一次 vkAllocateMemory 得到的大块。同一 memoryTypeIndex 的块归到一组。
    struct MemoryBlock
    {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        // HOST_VISIBLE 内存的常驻映射。构造时 vkMapMemory 一次，
        // 之后所有子分配都靠 mapped + offset 直接寻址，永不 unmap。
        void* mapped = nullptr;

        std::vector<FreeRange> freeList;
    };
    uint32_t FindMemoryType(uint32_t typeBits, Usage usage) const;
    MemoryBlock& CreateBlock(uint32_t typeIndex, VkDeviceSize size);
    // 在指定块里找第一个放得下的空闲段（first-fit）。
    // 成功返回 true 并把结果写进 outAllocation，同时收缩该空闲段。
    bool TryAllocateFromBlock(
        MemoryBlock& block,
        VkDeviceSize size,
        VkDeviceSize alignment,
        Allocation& outAllocation) const;
    // 把 offset 向上取整到 alignment 的倍数。alignment 必须是 2 的幂
    // （Vulkan 规范保证），所以位运算即可。
    static VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }
    static void Coalesce(std::vector<FreeRange>& ranges);
private:
    VkPhysicalDeviceMemoryProperties m_MemoryProperties{};
    // 按下标索引：m_Blocks[typeIndex] 是该类内存的所有块。
    // 用 vector<vector<>> 而不是 map —— memoryTypeIndex 数量很少（≤32），
    // 直接按下标存，查找是 O(1)。
    VkDevice m_Device = VK_NULL_HANDLE;
    std::vector<std::vector<MemoryBlock>> m_Blocks;

    VkDeviceSize m_TotalAllocateBytes = 0;

    // 单块最小尺寸。太小会导致块数膨胀、碎片增多；太大小项目会浪费。
    // 256 MB 对开发期足够，且大部分驱动默认堆远大于此。
    static constexpr VkDeviceSize k_MinBlockSize = VkDeviceSize(256) * 1024 * 1024;
};

