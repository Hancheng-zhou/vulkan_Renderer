// ===========================================================================
//  VkBackend/VulkanCommandPool.h
//
//  职责边界：
//    管：VkCommandPool 的创建与销毁、一次性分配 N 条 primary 命令缓冲
//    不管："这一帧录什么"（那是 RenderFrame 的事）
//
//  与上一版 VulkanCommand 的差别：
//    1. pool 与 buffer 解耦 —— pool 是全局的，不属于某一帧
//    2. 一次分配 N 条，对应 MAX_FRAMES_IN_FLIGHT
//    3. 改名：它不是"命令"，是"命令缓冲池"
//
//  为什么命令缓冲要按帧分配：
//    双帧并行时，GPU 正在执行第 N-1 帧的命令，CPU 同时在录第 N 帧。
//    如果共用一个缓冲，CPU 的录制会覆盖 GPU 正在读的内容。
//    所以【每帧一条缓冲】是多帧并行的前提。
//
//  为什么 pool 要带 RESET_COMMAND_BUFFER_BIT：
//    每帧录制前要单独重置这一帧的缓冲。没有这个 flag 就只能整个 pool
//    一起 reset，那会把其它帧正在用的缓冲一起清掉。全局一个
// ===========================================================================
#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>
#include <functional>
class VulkanCommandPool
{
public:
	//bufferCount  MAX_FRAMES_IN_FLIGHT
	VulkanCommandPool(VkDevice device, uint32_t queueFamily, uint32_t bufferCount);
	~VulkanCommandPool();
	VulkanCommandPool(const VulkanCommandPool&) = delete;
	VulkanCommandPool& operator=(const VulkanCommandPool&) = delete;
	VkCommandPool Get() const { return m_Pool; }
	// 一次性分配好的缓冲，下标即帧号
	const std::vector<VkCommandBuffer>& GetBuffers() const { return m_Buffers; }
	// 重置并把所有缓冲标记为"初始状态"。
	 // 【要求 GPU 空闲】只在初始化或重建这类低频路径上调用。
	void ResetAll();

	// 一次性提交：分配临时命令缓冲 -> 录制 -> 提交 -> 等完成 -> 回收。
	//
	// 【用途】数据上传、图像 layout 转换这类"启动时做一次"的操作。
	//   它们不需要和主循环的多帧流水线耦合，也不该占用帧槽位的缓冲。
	//
	// 【为什么同步等待】调用方（UploadStaged）需要保证拷贝完成后再
	//   销毁 staging buffer。不等待的话，GPU 可能还在读那块已被释放的内存。
	//   这种"等待"在启动期可以接受；每帧都等就废了。
	// 典型场景 ： uploadstaged 图像初始化 任何启动时做一次就不复用的GPU操作
	void SubmitOneShot(VkQueue queue, const std::function<void(VkCommandBuffer)>& recorder);

private:
	VkDevice      m_Device = VK_NULL_HANDLE;
	VkCommandPool m_Pool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> m_Buffers;
};

// ===========================================================================
//  CommandRecording —— RAII 录制作用域
//
//  为什么需要它：
//    vkBeginCommandBuffer / vkEndCommandBuffer 必须成对。手写裸接口时，
//    任何一个提前 return、一个抛异常、一个 if 分支漏掉，都会让缓冲
//    停在 recording 状态 —— 之后 vkQueueSubmit 报 "command buffer is
//    in recording state"，而报错点离真正的 bug 很远。
//
//  用法：
//      {
//          CommandRecording rec(frame.CommandBuffer);
//          VkCommandBuffer cmd = rec.Get();
//          vkCmdBeginRendering(cmd, &info);
//          vkCmdEndRendering(cmd);
//      }   // 离开作用域自动 vkEndCommandBuffer
//
//  注意：构造时【不】自动 reset。reset 需要等 fence，属于调用方的责任。
// ===========================================================================

class CommandRecording
{
public:
	explicit CommandRecording(VkCommandBuffer commandBuffer);
	~CommandRecording();

	CommandRecording(const CommandRecording&) = delete;
	CommandRecording& operator=(const CommandRecording&) = delete;

	VkCommandBuffer Get() const { return m_CommandBuffer; }

	// 提前结束（一般不需要，析构会自动做）。
	// 结束后再调用 Get() 仍然返回句柄，但不能再录命令。
	void End();

private:
	VkCommandBuffer m_CommandBuffer = VK_NULL_HANDLE;
	bool            m_Ended = false;
};