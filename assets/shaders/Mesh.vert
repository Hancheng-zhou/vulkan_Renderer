#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexcoord;
layout(location = 3) in vec4 inTangent;

// 输出到 fragment：法线（世界空间）+ 世界坐标（将来算光照衰减要用）
layout(location = 0) out vec3 vWorldNormal;
layout(location = 1) out vec3 vWorldPos;

layout(set = 0, binding = 0) uniform CameraUniform
{
    mat4 ViewProjection;
    vec4 CameraPosition;
} camera;

layout(push_constant) uniform ModelPush
{
    mat4 Model;
} push;

void main()
{
    vec4 worldPos = push.Model * vec4(inPosition, 1.0);
    vWorldPos     = worldPos.xyz;

    // 【这里用 mat3(Model) 而不是逆转置矩阵】
    //   当前的变换只包含旋转和均匀缩放，两种情况都能用 mat3 直接转法线。
    //   一旦支持非均匀缩放（比如把立方体压成扁盒），必须改用
    //   transpose(inverse(mat3(Model)))，否则法线会被"压歪"。
    //   Transform::GetNormalMatrix() 已经准备好了这个版本。
    vWorldNormal = mat3(push.Model) * inNormal;

    gl_Position = camera.ViewProjection * worldPos;
}
