// ===========================================================================
//  Renderer/Camera.h
//
//  ！！从 ZHC 迁移，除投影矩阵外逻辑完全一致 ！！
//
//  职责边界：
//    管：相机姿态（position + yaw/pitch）、view 矩阵、projection 矩阵
//    不管：窗口与输入。按键/鼠标由 Application 读取后调用 Move/Rotate/Zoom，
//          这样 Camera 不依赖 GLFW，换输入源（手柄/脚本/回放）时不用动它。
//
//  为什么内部存 yaw/pitch 而不是 target 点：
//    视角控制天然是增量式的（鼠标拖动 = 角度增量），yaw/pitch 直接加减；
//    若内部存 target 点，每次旋转都要先算出方向再反推 target，来回转换
//    既别扭又容易积累误差。构造时收 target 只是为了调用方便，进来就转换。
//
//  坐标系约定（GLM 右手系）：
//    +X 向右，+Y 向上，初始视线朝 -Z（yaw = -90°）
//    yaw   绕世界 Y 轴，向右转为正
//    pitch 俯仰，抬头为正，夹紧在 ±k_MaxPitch 内
//
//  【Vulkan 适配】RecalculateProjection 里有两处改动，见 .cpp 注释
// ===========================================================================
#pragma once

#include <glm/glm.hpp>
#include <cstdint>

struct CameraState
{
	glm::vec3 Position{ 0.0f };
	float Yaw = -90.0f;
	float Pitch = 0.0f;
	float FovDegrees = 45.0f;
	float NearZ = 0.1f;
	float FarZ = 100.0f;

	static CameraState FromLookAt(const glm::vec3& position,
		const glm::vec3& target,
		float Degrees = 45.0f,
		float nearZ = 0.1f,
		float farZ = 100.0f);
};

class Camera
{
public:
	// pitch 必须夹紧：越接近 ±90°，视线越平行于世界上方向，
	// glm::lookAt 的 up 参考向量就越退化，画面会翻转或抖动
	static constexpr float k_MaxPitch = 89.0f;
	// FOV 上下限，防止滚轮把视锥推到退化（0° 与 180° 都不合法）
	static constexpr float k_MinFov = 1.0f;
	static constexpr float k_MaxFov = 120.0f;
    Camera(const glm::vec3& position,
        const glm::vec3& target,
        float fovDegrees,
        float aspectRatio,
        float nearZ = 0.1f,
        float farZ = 100.0f);
    // ---- 状态修改 --------------------------------------------------------
    void SetPosition(const glm::vec3& position);

    void MoveForward(float distance);   // 沿相机前方
    void MoveRight(float distance);     // 沿相机右方
    void MoveUp(float distance);        // 沿【世界】Y 轴：低头时按上升是垂直往上
    void Move(const glm::vec3& worldOffset);

    void Rotate(float deltaYawDegrees, float deltaPitchDegrees);
    void Zoom(float deltaFovDegrees);

    void LookAt(const glm::vec3& target);
    void SetOrientation(float yawDegrees, float pitchDegrees);

    void SetFovDegrees(float fovDegrees);
    void SetClipPlanes(float nearZ, float farZ);

    // 窗口尺寸变化时调用。内部只在宽高比真的变了时才重算投影矩阵
    void SetViewportSize(uint32_t width, uint32_t height);
    void SetAspectRatio(float aspectRatio);

    // ---- 状态快照 --------------------------------------------------------
    CameraState GetState() const;
    void SetState(const CameraState& state);

    // ---- 只读查询 --------------------------------------------------------
    const glm::mat4& GetViewMatrix()       const { return m_View; }
    const glm::mat4& GetProjectionMatrix() const { return m_Projection; }
    glm::mat4        GetViewProjection()   const { return m_Projection * m_View; }

    const glm::vec3& GetPosition() const { return m_Position; }
    const glm::vec3& GetForward()  const { return m_Forward; }
    const glm::vec3& GetRight()    const { return m_Right; }
    const glm::vec3& GetUp()       const { return m_Up; }
    float GetYaw()         const { return m_Yaw; }
    float GetPitch()       const { return m_Pitch; }
    float GetFovDegrees()  const { return m_FovDegrees; }
    float GetAspectRatio() const { return m_AspectRatio; }
    float GetNearZ()       const { return m_NearZ; }
    float GetFarZ()        const { return m_FarZ; }

private:
    void RecalculateOrientation();
    void RecalculateView();
    void RecalculateProjection();

private:
    glm::vec3 m_Position{ 0.0f };
    float m_Yaw = -90.0f;
    float m_Pitch = 0.0f;

    glm::vec3 m_Forward{ 0.0f, 0.0f, -1.0f };
    glm::vec3 m_Right{ 1.0f, 0.0f, 0.0f };
    glm::vec3 m_Up{ 0.0f, 1.0f, 0.0f };

    float m_FovDegrees = 45.0f;
    float m_AspectRatio = 16.0f / 9.0f;
    float m_NearZ = 0.1f;
    float m_FarZ = 100.0f;

    glm::mat4 m_View{ 1.0f };
    glm::mat4 m_Projection{ 1.0f };
};