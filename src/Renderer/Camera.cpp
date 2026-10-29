#include "Renderer/Camera.h"

#include <algorithm>   // std::clamp
#include <cmath>       // std::cos, std::sin, std::atan2, std::asin, std::fmod

// glm/glm.hpp 只含核心类型，lookAt / perspective 属于 gtc 扩展，必须显式包含
#include <glm/gtc/matrix_transform.hpp>
namespace
{
    // 世界上方向。刻意不使用相机的 up：pitch 变化时二者不再一致，
    // 用相机的 up 去推 forward 会自相矛盾
    const glm::vec3 k_WorldUp{ 0.0f, 1.0f, 0.0f };

    constexpr float k_MinDirectionLengthSq = 1e-12f;

    // 方向向量 -> 欧拉角，与 RecalculateOrientation 中 forward 的公式互为逆运算。
    // 顺手把 pitch 夹紧：正上/正下方的方向会让 forward 与世界上方向平行，
    // 之后算 right 时叉积退化成零向量，normalize 会产出 NaN
    void DirectionToYawPitch(const glm::vec3& direction, float& outYawDegrees, float& outPitchDegrees)
    {
        outYawDegrees = glm::degrees(std::atan2(direction.z, direction.x));
        // asin 定义域是 [-1,1]，归一化引入的浮点误差可能让它越界并返回 NaN
        outPitchDegrees = std::clamp(
            glm::degrees(std::asin(std::clamp(direction.y, -1.0f, 1.0f))),
            -Camera::k_MaxPitch, Camera::k_MaxPitch);
    }
}
// ---------------------------------------------------------------------------
//  CameraState
// ---------------------------------------------------------------------------

CameraState CameraState::FromLookAt(const glm::vec3& position,
    const glm::vec3& target,
    float fovDegrees,
    float nearZ,
    float farZ)
{
    CameraState state;
    state.Position = position;
    state.FovDegrees = fovDegrees;
    state.NearZ = nearZ;
    state.FarZ = farZ;

    glm::vec3 direction = target - position;
    if (glm::dot(direction, direction) < k_MinDirectionLengthSq)
    {
        direction = glm::vec3(0.0f, 0.0f, -1.0f);   // 与目标重合，退回默认朝向
    }
    DirectionToYawPitch(glm::normalize(direction), state.Yaw, state.Pitch);

    return state;
}
// ---------------------------------------------------------------------------
//  Camera
// ---------------------------------------------------------------------------

Camera::Camera(const glm::vec3& position,
    const glm::vec3& target,
    float fovDegrees,
    float aspectRatio,
    float nearZ,
    float farZ)
    : m_Position(position)
    , m_FovDegrees(std::clamp(fovDegrees, k_MinFov, k_MaxFov))
    , m_AspectRatio(aspectRatio > 0.0f ? aspectRatio : 1.0f)
    , m_NearZ(nearZ)
    , m_FarZ(farZ)
{
    glm::vec3 direction = target - m_Position;
    if (glm::dot(direction, direction) < k_MinDirectionLengthSq)
    {
        direction = glm::vec3(0.0f, 0.0f, -1.0f);
    }
    DirectionToYawPitch(glm::normalize(direction), m_Yaw, m_Pitch);

    RecalculateOrientation();   // 内部会调用 RecalculateView()
    RecalculateProjection();
}

void Camera::SetPosition(const glm::vec3& position)
{
    m_Position = position;
    RecalculateView();
}

void Camera::MoveForward(float distance)
{
    if (distance == 0.0f) { return; }
    m_Position += m_Forward * distance;
    RecalculateView();
}

void Camera::MoveRight(float distance)
{
    if (distance == 0.0f) { return; }
    m_Position += m_Right * distance;
    RecalculateView();
}

void Camera::MoveUp(float distance)
{
    if (distance == 0.0f) { return; }
    m_Position += k_WorldUp * distance;
    RecalculateView();
}

void Camera::Move(const glm::vec3& worldOffset)
{
    if (worldOffset == glm::vec3(0.0f)) { return; }
    m_Position += worldOffset;
    RecalculateView();
}

void Camera::Rotate(float deltaYawDegrees, float deltaPitchDegrees)
{
    if (deltaYawDegrees == 0.0f && deltaPitchDegrees == 0.0f) { return; }

    m_Yaw += deltaYawDegrees;
    m_Pitch = std::clamp(m_Pitch + deltaPitchDegrees, -k_MaxPitch, k_MaxPitch);

    // 连续拖动会让 yaw 无限增长，长时间运行后 float 精度会下降
    m_Yaw = std::fmod(m_Yaw, 360.0f);

    RecalculateOrientation();
}

void Camera::LookAt(const glm::vec3& target)
{
    const glm::vec3 direction = target - m_Position;
    if (glm::dot(direction, direction) < k_MinDirectionLengthSq)
    {
        return;
    }
    DirectionToYawPitch(glm::normalize(direction), m_Yaw, m_Pitch);
    RecalculateOrientation();
}

void Camera::SetOrientation(float yawDegrees, float pitchDegrees)
{
    m_Yaw = std::fmod(yawDegrees, 360.0f);
    m_Pitch = std::clamp(pitchDegrees, -k_MaxPitch, k_MaxPitch);
    RecalculateOrientation();
}

void Camera::Zoom(float deltaFovDegrees)
{
    SetFovDegrees(m_FovDegrees + deltaFovDegrees);
}

void Camera::SetFovDegrees(float fovDegrees)
{
    const float newFov = std::clamp(fovDegrees, k_MinFov, k_MaxFov);
    if (newFov == m_FovDegrees) { return; }
    m_FovDegrees = newFov;
    RecalculateProjection();
}

void Camera::SetClipPlanes(float nearZ, float farZ)
{
    // near 为 0 或负数会让投影矩阵除零，far <= near 会把视锥翻过来。
    // 这类非法参数直接忽略，而不是悄悄产出一个坏矩阵
    if (nearZ <= 0.0f || farZ <= nearZ) { return; }
    if (nearZ == m_NearZ && farZ == m_FarZ) { return; }

    m_NearZ = nearZ;
    m_FarZ = farZ;
    RecalculateProjection();
}

void Camera::SetViewportSize(uint32_t width, uint32_t height)
{
    // 调用方必须传 framebuffer 尺寸（像素）而不是窗口尺寸：
    // 高 DPI 屏上两者不相等。窗口最小化时 height 为 0，保留旧宽高比避免除零
    SetAspectRatio(height > 0
        ? static_cast<float>(width) / static_cast<float>(height)
        : m_AspectRatio);
}

void Camera::SetAspectRatio(float aspectRatio)
{
    if (aspectRatio <= 0.0f || aspectRatio == m_AspectRatio) { return; }
    m_AspectRatio = aspectRatio;
    RecalculateProjection();
}

CameraState Camera::GetState() const
{
    CameraState state;
    state.Position = m_Position;
    state.Yaw = m_Yaw;
    state.Pitch = m_Pitch;
    state.FovDegrees = m_FovDegrees;
    state.NearZ = m_NearZ;
    state.FarZ = m_FarZ;
    return state;
}

void Camera::SetState(const CameraState& state)
{
    // 外部存下来的状态不保证合法，每个量都过一遍和构造函数相同的约束
    m_Position = state.Position;
    m_Yaw = std::fmod(state.Yaw, 360.0f);
    m_Pitch = std::clamp(state.Pitch, -k_MaxPitch, k_MaxPitch);
    m_FovDegrees = std::clamp(state.FovDegrees, k_MinFov, k_MaxFov);

    if (state.NearZ > 0.0f && state.FarZ > state.NearZ)
    {
        m_NearZ = state.NearZ;
        m_FarZ = state.FarZ;
    }

    RecalculateOrientation();
    RecalculateProjection();
}

void Camera::RecalculateOrientation()
{
    const float yawRad = glm::radians(m_Yaw);
    const float pitchRad = glm::radians(m_Pitch);
    const float cosPitch = std::cos(pitchRad);

    // 球坐标 -> 笛卡尔。结果已经是单位向量，不必再 normalize
    m_Forward = glm::vec3(std::cos(yawRad) * cosPitch,
        std::sin(pitchRad),
        std::sin(yawRad) * cosPitch);

    // pitch 被夹在 ±89°，forward 与 k_WorldUp 的夹角不会为 0，叉积不退化
    m_Right = glm::normalize(glm::cross(m_Forward, k_WorldUp));

    // forward 与 right 垂直且都是单位向量，叉积结果已是单位向量
    m_Up = glm::cross(m_Right, m_Forward);

    RecalculateView();
}

void Camera::RecalculateView()
{
    m_View = glm::lookAt(m_Position, m_Position + m_Forward, m_Up);
}

void Camera::RecalculateProjection()
{
    // =======================================================================
    //  【这是从 ZHC 迁移时唯一必须改的函数】
    //
    //  ZHC 原版只写了一句 glm::perspective(...)，因为它面向 OpenGL。
    //  Vulkan 有两处不同，都在这里处理：
    //
    //  1) 深度范围：[0, 1] 而不是 [-1, 1]
    //     由全局宏 GLM_FORCE_DEPTH_ZERO_TO_ONE 保证（见 premake5.lua）。
    //     漏掉这个宏的话，投影出的 z 有一半落在负数区间，
    //     靠近相机那部分几何会被近平面裁掉 —— 和之前立方体缺角同一类问题。
    //
    //  2) y 轴方向：Vulkan 的 NDC y 朝下，OpenGL 朝上
    //     做法是把投影矩阵第二行取反（正负互换），相当于投影时上下翻一次。
    //     这样世界空间的 +Y 才会显示在屏幕上方。
    //
    //  【为什么不在 shader 里翻转 ndc.y】
    //    那只是把最终画面翻回来，但深度、裁剪、以及后续的视锥计算
    //    仍然带着错误朝向。在投影矩阵里修正才是彻底的。
    //
    //  【连带影响：面剔除方向】
    //    y 翻转后，三角形的屏幕绕序会回到 OpenGL 的 CCW 语义，
    //    所以管线的 frontFace 必须是 COUNTER_CLOCKWISE（已经在阶段 6a 改好）。
    // =======================================================================
    glm::mat4 projection = glm::perspective(
        glm::radians(m_FovDegrees), m_AspectRatio, m_NearZ, m_FarZ);

    projection[1][1] *= -1.0f;

    m_Projection = projection;
}