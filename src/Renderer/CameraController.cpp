#include "Renderer/CameraController.h"

CameraController::CameraController(Camera& camera)
    : m_Camera(camera)
{
    // 记录"出厂设置"
    m_StartupView = camera.GetState();

    // 四个常用观察方向（对应数字键 1~4）。
    // 用 FromLookAt 而不是手写 yaw/pitch：反解欧拉角容易出错，
    // 而且极端俯仰角必须夹紧，FromLookAt 内部已经处理了
    m_ViewSlots[0] = CameraState::FromLookAt(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), 45.0f);
    m_ViewSlots[1] = CameraState::FromLookAt(glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.0f), 45.0f);
    m_ViewSlots[2] = CameraState::FromLookAt(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f), 45.0f);
    m_ViewSlots[3] = CameraState::FromLookAt(glm::vec3(3.0f, 3.0f, 3.0f), glm::vec3(0.0f), 45.0f);
}

void CameraController::OnCursorMove(double x, double y)
{
    // 不管转不转，都要更新上一次位置。
    // 否则"按下右键的那一刻"到"第一次移动"之间的位移会算成巨大的 delta，
    // 视角会猛地甩一下
    const double dx = x - m_LastCursorX;
    const double dy = y - m_LastCursorY;
    m_LastCursorX = x;
    m_LastCursorY = y;

    // 只累积，不直接改相机：此刻还不知道鼠标在不在视口上
    m_PendingDx += static_cast<float>(dx);
    m_PendingDy += static_cast<float>(dy);
}

void CameraController::OnScroll(double yOffset)
{
    m_ScrollDelta += static_cast<float>(yOffset);
}

void CameraController::OnUpdate(const InputState& input, float deltaTime)
{
    // ---- 滚轮缩放 ----
    if (m_ScrollDelta != 0.0f)
    {
        if (input.ViewportHovered)
        {
            // 滚轮向上表示"拉近"，对应视场角变小，所以取负号
            m_Camera.Zoom(-m_ScrollDelta * m_ZoomSensitivity);
        }
        // 不管有没有用掉都清零：否则余量会一直攒着，等鼠标移回视口时跳一下
        m_ScrollDelta = 0.0f;
    }

    // ---- 键盘移动 ----
    if (input.AcceptCameraKeys && glm::dot(input.MoveAxis, input.MoveAxis) > 0.0f)
    {
        // 三个轴放在一起归一化：否则同时按两个方向时合成速度是 √2 倍
        const glm::vec3 axis = glm::normalize(input.MoveAxis);

        // 乘 deltaTime 让速度与帧率解耦
        const float speed = m_MoveSpeed * (input.Fast ? m_FastFactor : 1.0f) * deltaTime;

        m_Camera.MoveForward(axis.z * speed);
        m_Camera.MoveUp(axis.y * speed);
        m_Camera.MoveRight(axis.x * speed);
    }

    // ---- 右键旋转的起手判定 ----
    // 松开就结束，不管光标这时在哪 —— 否则拖出视口再松手，
    // 下次在面板上按住右键也会莫名其妙地转视角
    if (!input.RotateHold)
    {
        m_Rotating = false;
    }
    else if (!m_Rotating && input.ViewportHovered)
    {
        m_Rotating = true;   // 只能在视口内"起手"
    }

    // ---- 应用累积的鼠标位移 ----
    if (m_Rotating && (m_PendingDx != 0.0f || m_PendingDy != 0.0f))
    {
        // 鼠标向下移动（dy > 0）时应该低头，pitch 减小，所以 dy 取负号
        m_Camera.Rotate(m_PendingDx * m_RotateSensitivity,
            -m_PendingDy * m_RotateSensitivity);
    }

    // 累积量每帧都要清空：不清的话，松手期间累积的位移会在下次起手时
    // 一次性甩出去，视角猛地跳一下
    m_PendingDx = 0.0f;
    m_PendingDy = 0.0f;
}

void CameraController::CaptureStartupView() { m_StartupView = m_Camera.GetState(); }
void CameraController::SaveViewSlot(int slot) { m_ViewSlots[slot] = m_Camera.GetState(); }
void CameraController::RecallViewSlot(int slot) { m_Camera.SetState(m_ViewSlots[slot]); }
void CameraController::ResetToStartupView() { m_Camera.SetState(m_StartupView); }