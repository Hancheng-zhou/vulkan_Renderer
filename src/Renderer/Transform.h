// ===========================================================================
//  Renderer/Transform.h
//
//  【从 ZHC 原样迁移，零改动】
//
//  职责边界：
//    管：位置 / 旋转 / 缩放的组合，产出模型矩阵与法线矩阵
//    不管：谁在用这个变换（那是 RenderObject 的事）
//
//  为什么模型矩阵是 T * R * S 而不是别的顺序：
//    矩阵右乘意味着"先应用右边的变换"。所以 T*R*S 的效果是
//    先把顶点缩放 -> 再旋转 -> 最后平移。
//
//    这个顺序符合直觉：一个 1x1x1 的立方体，缩放成 2x2x2，
//    旋转 45 度，再挪到 (5,0,0)。换任何别的顺序都会出错 ——
//    比如 S*R*T 会让平移量也被缩放。
// ===========================================================================

#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
class Transform
{
public:
    glm::mat4 GetModelMatrix() const
    {
        const glm::mat4 T = glm::translate(glm::mat4(1.0f), Position);
        const glm::mat4 R = glm::eulerAngleXYZ(Rotation.x, Rotation.y, Rotation.z);
        const glm::mat4 S = glm::scale(glm::mat4(1.0f), Scale);

        return T * R * S;
    }

    // 法线矩阵 = 模型矩阵左上 3x3 的逆转置。
    //
    // 【为什么不能直接用 Model 的 3x3】非均匀缩放会让法线不再垂直于表面：
    //   一个球被压扁后，法线也会被"压歪"。
    //   逆转置能抵消这个歪斜，代价是每次要算一次求逆。
    //   （当前 shader 用 mat3(Model) 是因为我们的变换只有旋转+均匀缩放，
    //    等阶段 7 支持非均匀缩放后再切到这个。）
    glm::mat3 GetNormalMatrix() const
    {
        return glm::transpose(glm::inverse(glm::mat3(GetModelMatrix())));
    }

public:
    glm::vec3 Position{ 0.0f };
    glm::vec3 Rotation{ 0.0f };   // 弧度（eulerAngleXYZ 的入参单位）
    glm::vec3 Scale{ 1.0f, 1.0f, 1.0f };
};