// ===========================================================================
//  Renderer/CameraController.h
//
//  ！！从 ZHC 原样迁移，零改动 ！！
//
//  "输入 → 相机动作"的全部逻辑，连它自己的状态一起收在这里。
//
//  依赖边界（本文件最重要的约定）：
//    不包含任何窗口 / GLFW 头。只消费 InputState —— 一个纯数据的每帧输入快照。
//
//    按键映射（"W 是 +前后、D 是 +左右"）属于【绑定】逻辑，归输入层
//    （Application 里的 PollInput）；相机控制器只该知道"有个轴让我往上走"。
//    这样以后加手柄 / 脚本回放 / 无头测试，只需要换一个填 InputState 的地方。
//
//  生命周期：
//    m_Camera 是【引用】，不拥有相机 —— 相机归 Application/Scene 所有。
// ===========================================================================
#pragma once
#include "Renderer/Camera.h"
#include <glm/glm.hpp>

struct InputState
{
    glm::vec3 MoveAxis{ 0.0f };        // 已归一化的三轴：(前后, 上下, 左右)
    bool Fast = false;     // Shift 加速
    bool RotateHold = false;     // 右键按住
    bool ViewportHovered = false;     // 鼠标是否悬停在场景视口上
    bool AcceptCameraKeys = false;     // 键盘是否该归相机

};
class CameraController
{
public:
    explicit CameraController(Camera& camera);

    // ---- 由窗口回调转发进来：只【记录】原始输入，不做任何决策 ----
    //
    // 决策必须留到 OnUpdate：回调发生在 PollEvents() 期间，
    // 那一刻当帧的 ViewportHovered 还不存在。所以这里只攒，不判。
    void OnCursorMove(double x, double y);
    void OnScroll(double yOffset);

    // 每帧调用一次
    void OnUpdate(const InputState& input, float deltaTime);

    // ---- 视角预设 --------------------------------------------------------
    void CaptureStartupView();
    void SaveViewSlot(int slot);
    void RecallViewSlot(int slot);
    void ResetToStartupView();

private:
    bool m_Rotating = false;

    // 上一次光标位置。必须一直维护（不管转不转），
    // 否则"按下右键"到"第一次移动"之间的位移会被算成一次巨大的 delta
    double m_LastCursorX = 0.0;
    double m_LastCursorY = 0.0;

    // 累积量：回调只管往里加，OnUpdate 里统一消费并清空
    float m_PendingDx = 0.0f;
    float m_PendingDy = 0.0f;
    float m_ScrollDelta = 0.0f;

    // 手感参数
    float m_MoveSpeed = 2.5f;    // 单位 / 秒
    float m_FastFactor = 5.0f;    // Shift 倍率
    float m_RotateSensitivity = 0.15f;   // 度 / 像素
    float m_ZoomSensitivity = 2.0f;    // 度 / 滚轮格

    CameraState m_ViewSlots[4];
    CameraState m_StartupView;

    Camera& m_Camera;
};