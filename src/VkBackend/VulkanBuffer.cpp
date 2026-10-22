#include "VkBackend/VulkanBuffer.h"
#include "VkBackend/VulkanCommandPool.h"
#include <cstring>
#include <stdexcept>

VulkanBuffer::VulkanBuffer(VkDevice device, VulkanAllocator& allocator, const CreateInfo& info)
	:m_Device(device),m_Allocator(allocator),m_Size(info.size)
{
    if (info.size == 0)
    {
        throw std::runtime_error("[VulkanBuffer] size must not be 0");
    }

    // ---- 1. 创建 VkBuffer 对象 -------------------------------------------
    //
    //  此刻它还只是"一个带用途标签的空壳" —— 没有显存，不能用。
    //  显存要等下一步 bind。
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = info.size;
    bufferInfo.usage = info.usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;   // 只给一个队列族用

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &m_Buffer) != VK_SUCCESS)
    {
        throw std::runtime_error("[VulkanBuffer] vkCreateBuffer failed");
    }

    // ---- 2. 问驱动"这块 buffer 要什么样的显存" ---------------------------
    //
    //  这一步【不能跳过】。memoryTypeBits 是 buffer 特有的：
    //  某些设备上特定 usage 只能用特定内存类型。
    //  跳过它直接猜一个 typeIndex，在别的机器上就会失败。
    vkGetBufferMemoryRequirements(device, m_Buffer, &m_Requirements);

    // ---- 3. 申请 backing memory 并绑定 -----------------------------------
    //
    //  alignment 取 buffer 要求和调用方要求的较大值。
    //  调用方传 0 时用 buffer 自己的要求 —— 这是唯一安全默认值。
    const VkDeviceSize alignment =
        info.alignment != 0 ? max(info.alignment, m_Requirements.alignment)
        : m_Requirements.alignment;

    m_Allocation = allocator.Allocate(info.size, alignment,
        m_Requirements.memoryTypeBits, info.memoryUsage);

    // bind 之后 buffer 才真正可用。这一步只是"登记对应关系"，
    // 不拷贝数据，也不是上传入口。
    if (vkBindBufferMemory(device, m_Buffer, m_Allocation.memory, m_Allocation.offset) != VK_SUCCESS)
    {
        throw std::runtime_error("[VulkanBuffer] vkBindBufferMemory failed");
    }

    m_IsHostVisible = (m_Allocation.mapped != nullptr);
}

VulkanBuffer::~VulkanBuffer()
{
    // 销毁顺序：先 VkBuffer，再把显存还给 allocator。
    //
    // 【为什么先 buffer】显存里可能还有 buffer 在用的数据。
    //   反过来先还显存，buffer 就指向一块已释放的内存。
    //   Vulkan 不检查这个顺序，错了只会在随机时刻崩。
    if (m_Buffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(m_Device, m_Buffer, nullptr);
        m_Buffer = VK_NULL_HANDLE;
    }

    m_Allocator.Free(m_Allocation);
    m_Allocation = {};
}

void VulkanBuffer::Upload(const void* data, VkDeviceSize size, VkDeviceSize offset)
{
    if (!m_IsHostVisible)
    {
        throw std::runtime_error(
            "[VulkanBuffer] Upload called on GpuOnly buffer; use a staging buffer instead");
    }

    if (data == nullptr)
    {
        throw std::runtime_error("[VulkanBuffer] Upload source is null");
    }

    if (offset + size > m_Size)
    {
        throw std::runtime_error("[VulkanBuffer] Upload range out of bounds");
    }

    // HOST_COHERENT 内存的写入对 GPU 立即可见，所以 memcpy 完就结束了，
    // 不需要 vkFlushMappedMemoryRanges。
    //
    // 【为什么这里不做内存屏障】CPU/GPU 之间的可见性由 COHERENT 保证；
    //   GPU 多个队列之间的同步是另一回事（阶段 3 的信号量与屏障）。
    //   两件事别混。
    std::memcpy(static_cast<char*>(m_Allocation.mapped) + offset, data, size);
}

void VulkanBuffer::UploadStaged(VulkanCommandPool& pool, VkQueue queue, const void* data, VkDeviceSize size)
{
    if (m_IsHostVisible)
    {
        throw std::runtime_error("[VulkanBuffer] UploadStaged is for device-local buffers; use Upload instead");
    }
    if (size > m_Size)
    {
        throw std::runtime_error("[VulkanBuffer] UploadStaged range out of bounds");
    }
    // ---- 1. 建 staging buffer --------------------------------------------
    //
    //  usage 必须是 TRANSFER_SRC：它的唯一用途就是被拷贝走。
    //  用 VERTEX_BUFFER_BIT 也能跑，但驱动会为它准备不必要的读路径。
    VulkanBuffer::CreateInfo stagingInfo{};
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.size = size;
    stagingInfo.memoryUsage = VulkanAllocator::Usage::CPUToGPU;
    VulkanBuffer stagingBuffer(m_Device, m_Allocator, stagingInfo);
    // ---- 2. CPU 写数据 ---------------------------------------------------
    stagingBuffer.Upload(data, size, 0);
    // ---- 3. GPU 拷贝 -----------------------------------------------------
    //
    //  SubmitOneShot 内部会等待完成，所以返回时数据已经在目标缓冲里了，
    //  staging 可以安全销毁。
    pool.SubmitOneShot(
        queue, [&](VkCommandBuffer cmd)
        {
            VkBufferCopy region{};
            region.size = size;
            region.srcOffset = 0;
            region.dstOffset = 0;
            vkCmdCopyBuffer(cmd, stagingBuffer.GetHandle(), m_Buffer,1,&region);
        }
    );

}
