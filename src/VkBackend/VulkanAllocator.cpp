#include "VkBackend/VulkanAllocator.h"
#include <stdexcept>
#include <algorithm>

VulkanAllocator::VulkanAllocator(VkDevice device,VkPhysicalDevice physicalDevice)
    :m_Device(device)
{
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &m_MemoryProperties);
	m_Blocks.resize(m_MemoryProperties.memoryTypeCount);
}
VulkanAllocator::~VulkanAllocator()
{
    // 只释放 VkDeviceMemory，不碰任何 VkBuffer —— 那些归 VulkanBuffer 管。
    //
    // 【生命周期约束】调用方必须保证本对象先于 VkDevice 销毁。
    //   这些内存都属于 device，device 一死它们全部失效。
    //   Application 里的保证方式是 m_Allocator 声明在 m_Context 之前
    //   （逆序析构 -> allocator 先死）。
    for (auto& blocksOfType : m_Blocks)
    {
        for (MemoryBlock& block : blocksOfType)
        {
            if (block.mapped != nullptr)
            {
                vkUnmapMemory(m_Device, block.memory);
                block.mapped = nullptr;
            }
            vkFreeMemory(m_Device, block.memory, nullptr);
            block.memory = VK_NULL_HANDLE;
        }
    }

}

VulkanAllocator::Allocation VulkanAllocator::Allocate(VkDeviceSize size, VkDeviceSize alignment, uint32_t typeBits, Usage usage)
{

    if (size == 0)
    {
        throw std::runtime_error("[VulkanAllocator] cannot allocate 0 bytes");

    }
    const uint32_t typeIndex = FindMemoryType(typeBits, usage);
    // ---- 1. 先试现有块 ---------------------------------------------------
    for (MemoryBlock& block : m_Blocks[typeIndex])
    {
        Allocation candidate;
        if (TryAllocateFromBlock(block, size, alignment, candidate))
        {
            //分配成功
            m_TotalAllocateBytes += candidate.size;
            return candidate;
        }
    }
    // ---- 2. 都放不下，开新块 ---------------------------------------------
    //
    // 新块至少 k_MinBlockSize；但请求本身就超了时，按请求来
    // （否则这次分配永远失败）。向上对齐到 alignment，
    // 保证块内第一个分配不必为了对齐而浪费头部空间。
    const VkDeviceSize requested = AlignUp(size, alignment);
    const VkDeviceSize blockSize = requested > k_MinBlockSize ? requested : k_MinBlockSize;
    MemoryBlock& block = CreateBlock(typeIndex, blockSize);
    Allocation candidate;
    if (!TryAllocateFromBlock(block, size, alignment, candidate))
    {
        throw std::runtime_error("[VulkanAllocator] fresh block cannot satisfy allocation");
    }
    m_TotalAllocateBytes += candidate.size;
    return candidate;
}

void VulkanAllocator::Free(const Allocation& allocation)
{
    if (!allocation.IsValid())
    {
        return ;
    }
    for (auto& blocksOfType : m_Blocks)
    {
        for (MemoryBlock& block : blocksOfType)
        {
            if (block.memory != allocation.memory)
            {
                continue;
            }
            block.freeList.push_back({ allocation.offset,allocation.size });
            Coalesce(block.freeList);
            m_TotalAllocateBytes -= allocation.size;
            return;
        }

    }
}

uint32_t VulkanAllocator::FindMemoryType(uint32_t typeBits, Usage usage) const
{
    // 按用途映射到驱动支持的内存属性。
    //
    // HOST_COHERENT 的含义：CPU 的写对 GPU 立即可见，不需要手动 flush。
    //   不加它就得每次写完后调 vkFlushMappedMemoryRanges，很容易忘。
    //   代价是某些架构上略慢。开发期一律加上，别为省这点开销引入 bug。
    const VkMemoryPropertyFlags required =
        (usage == Usage::GPUOnly) ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT :
        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    for (uint32_t i = 0; i < m_MemoryProperties.memoryTypeCount; ++i)
    {
        const bool supportedByResource = (typeBits & (1u << i)) != 0;
        const bool hasProperties = (m_MemoryProperties.memoryTypes[i].propertyFlags & required) == required;
        if (supportedByResource && hasProperties)
        {
            return i;
        }
    }
    throw std::runtime_error("[VulkanAllocator] failed to find suitable memory type");
}

VulkanAllocator::MemoryBlock& VulkanAllocator::CreateBlock(uint32_t typeIndex, VkDeviceSize size)
{
    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType  =    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = size;
    allocateInfo.memoryTypeIndex = typeIndex;
    MemoryBlock block;
    block.size = size;
    const VkResult result = vkAllocateMemory(m_Device, &allocateInfo, nullptr, &block.memory);
    if (result != VK_SUCCESS || block.memory == VK_NULL_HANDLE)
    {
        throw std::runtime_error("[VulkanAllocator] vkAllocateMemory failed");
    }
    // HOST_VISIBLE 的内存才需要映射。GpuOnly 映射会直接返回错误。
    //
    // 【为什么只映射一次】vkMapMemory 有真实开销，且映射次数受驱动限制。
    //   块内所有子分配共用这一份映射，靠 offset 寻址 —— 这就是"持久映射"。
    const bool isHostVisible =
        (m_MemoryProperties.memoryTypes[typeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    if (isHostVisible)
    {
        const VkResult mapResult = vkMapMemory(m_Device, block.memory, 0, size, 0, &block.mapped);
        if (mapResult != VK_SUCCESS)
        {
            vkFreeMemory(m_Device, block.memory, nullptr);
            throw std::runtime_error("[VulkanAllocator] vkMapMemory failed");
        }

    }
    block.freeList.push_back({ 0,size });
    m_Blocks[typeIndex].push_back(block);
    return m_Blocks[typeIndex].back();
    // TODO: 在此处插入 return 语句
}

bool VulkanAllocator::TryAllocateFromBlock(MemoryBlock& block, VkDeviceSize size, VkDeviceSize alignment, Allocation& outAllocation) const
{
    for (std::size_t i = 0; i < block.freeList.size(); ++i)
    {
        FreeRange& range = block.freeList[i];
        const VkDeviceSize alignedOffset = AlignUp(range.offset, alignment);
        const VkDeviceSize headWaste = alignedOffset - range.offset;
        if (range.size < headWaste + size)
        {
            continue; //这个FreeRange 放不下
        }
        outAllocation.memory = block.memory;
        outAllocation.size = size;
        outAllocation.offset = alignedOffset;
        outAllocation.mapped = block.mapped != nullptr ? static_cast<char*>(block.mapped) + alignedOffset : nullptr;
        range.offset = alignedOffset + size;
        range.size = range.size - (headWaste + size);
        // 顺手清掉耗尽段，避免 freeList 无限增长
        if (range.size == 0)
        {
            block.freeList.erase(block.freeList.begin() + static_cast<std::ptrdiff_t>(i));
        }
        return true;
    }
    return false;
}

void VulkanAllocator::Coalesce(std::vector<FreeRange>& ranges)
{
    if(ranges.size()<2)
    {
        return;
    }
    std::sort(ranges.begin(), ranges.end(), [](const FreeRange& a, const FreeRange& b) {return a.offset < b.offset; });
    std::vector<FreeRange> merged;
    merged.reserve(ranges.size());
    for (const FreeRange& range : ranges)
    {
        if (!merged.empty() && merged.back().offset + merged.back().size == range.offset)
        {
            merged.back().size += range.size;
        }
        else
        {
            merged.push_back(range);
        }

    }
    ranges = std::move(merged);

}

