#include "VulkanDescriptor.h"
#include <stdexcept>
#include <string>

namespace
{
    void ThrowOnFailed(VkResult result, const char* whatFailed)
    {
        if (result != VK_SUCCESS)
        {
            throw std::runtime_error(std::string("[VulkanDescriptor] ") + whatFailed +
                " failed, VkResult = " +
                std::to_string(static_cast<int>(result)));
        }
    }
}

VulkanDescriptor::VulkanDescriptor(VkDevice device, uint32_t bindingCount, uint32_t frameCount)
    :m_Device(device),m_BindingCount(bindingCount)
{
    if (bindingCount == 0 || frameCount == 0)
    {
        throw std::runtime_error("[VulkanDescriptor] bindingCount/frameCount must be > 0");
    }
    // ---- 1. 每个 binding 的声明 ------------------------------------------
    //
    // 全部按 UNIFORM_BUFFER 处理：阶段 6a 只有相机 UBO。
    // 将来加纹理时，那个 binding 要改成 COMBINED_IMAGE_SAMPLER，
    // 并把 descriptorCount 保持 1。
    std::vector<VkDescriptorSetLayoutBinding> bindings(bindingCount);

    for (uint32_t i = 0; i < bindingCount; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        bindings[i].pImmutableSamplers = nullptr;
    }
    // ---- 2. 布局 ---------------------------------------------------------
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = bindingCount;
    layoutInfo.pBindings = bindings.data();

    ThrowOnFailed(vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &m_Layout),
        "vkCreateDescriptorSetLayout");
    // ---- 3. 池子 ---------------------------------------------------------
    //
    // 池子是"分配 set 的预算"。分配总数必须提前声明，不能动态增长。
    // 这里一次给足 frameCount 个 set 的量。
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = bindingCount * frameCount;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = frameCount;

    ThrowOnFailed(vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_Pool),
        "vkCreateDescriptorPool");
    // ---- 4. 为每帧各分配一个 set ----------------------------------------
    std::vector<VkDescriptorSetLayout> layouts(frameCount, m_Layout);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_Pool;
    allocInfo.descriptorSetCount = frameCount;
    allocInfo.pSetLayouts = layouts.data();

    m_Sets.resize(frameCount);
    ThrowOnFailed(vkAllocateDescriptorSets(m_Device, &allocInfo, m_Sets.data()),
        "vkAllocateDescriptorSets");

    std::printf("[VulkanDescriptor] %u set(s) x %u binding(s)\n", frameCount, bindingCount);
}

VulkanDescriptor::~VulkanDescriptor()
{
    // 【set 不能单独销毁】它们归 pool 所有，销毁 pool 时一并回收。
    m_Sets.clear();

    if (m_Pool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_Device, m_Pool, nullptr);
        m_Pool = VK_NULL_HANDLE;
    }
    if (m_Layout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_Device, m_Layout, nullptr);
        m_Layout = VK_NULL_HANDLE;
    }
}

void VulkanDescriptor::BindUniformBuffer(uint32_t frame, uint32_t binding, VkBuffer buffer, VkDeviceSize size)
{
    if (frame >= m_Sets.size())
    {
        throw std::runtime_error("[VulkanDescriptor] frame index out of range");
    }
    if (binding >= m_BindingCount)
    {
        throw std::runtime_error("[VulkanDescriptor] binding index out of range");
    }

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = buffer;
    bufferInfo.offset = 0;
    bufferInfo.range = size;   // 【必须给真实大小】VK_WHOLE_SIZE 也能用，
    // 但显式写出来能提前暴露"buffer 太小"的问题

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_Sets[frame];
    write.dstBinding = binding;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &bufferInfo;

    // 最后一个参数是"要拷贝的 write 操作的副本数"，
    // 传 1 表示立即执行（不拷贝）。只有写大量重复 set 时才需要 >1
    vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
}
