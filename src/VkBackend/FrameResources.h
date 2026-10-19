// ===========================================================================
//  VkBackend/FrameResources.h
//
//  职责边界：
//    持有"一帧"需要的 GPU 侧资源：1 条命令缓冲 + 1 个信号量 + 1 个围栏
//
//  为什么把 Command 和 Sync 放在一起：
//    它们是同一个东西的两面。每加一帧，必须同时加"一条缓冲 + 一套同步"，
//    而且必须配对正确 —— 第 N 帧的 submit 要用第 N 帧的 fence 和信号量。
//    分成两个类时，这个配对关系只存在于调用方的记忆里。
//    放在一个结构体里，配对由数据结构本身表达。
//
//  两个同步对象的分工（混用必崩）：
//    ImageAvailable  : acquire 完成 -> 渲染可以开始      （GPU 间，semaphore）
//    InFlight        : 这一帧 submit 完成 -> CPU 可以录下一帧（CPU 等，fence）
//
//  【这里为什么没有 renderFinished 信号量】
//    它不能按帧索引，必须按【交换链图像】索引 —— 见 Application.h 里
//    m_RenderFinishedSemaphores 的注释。原因简述：
//      vkQueuePresentKHR 返回后，present 引擎何时真正消耗掉信号量
//      是不确定的；而 fence 只跟踪 submit，不跟踪 present。
//      帧数(2) < 图像数(3) 时，绕一圈回来可能撞上还没释放的那个。
//
//  【InFlight 必须初始为 signaled】
//    否则第一帧的 vkWaitForFences 永久阻塞，程序启动即卡死。
// ===========================================================================
#pragma once

#include <vulkan/vulkan.h>

struct FrameResources
{
	VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
	VkSemaphore     ImageAvailable = VK_NULL_HANDLE;
	VkFence			InFlight = VK_NULL_HANDLE;

	void CreateSyncObjects(VkDevice device);
	void DestroySyncObjects(VkDevice device);

};