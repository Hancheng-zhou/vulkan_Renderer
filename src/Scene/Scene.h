#pragma once

#include "Renderer/Camera.h"
#include "Renderer/RenderObject.h"

#include <cstddef>
#include <vector>

// ===========================================================================
//  Scene/Scene.h
//
//  场景 = "这个世界里有什么"的完整描述。纯数据，不含任何 Vulkan 调用。
//
//  依赖方向刻意是 Renderer -> Scene，而不是 Scene -> Renderer：
//  渲染器来读场景，场景不知道渲染器的存在。
//  这样以后换后端（延迟渲染、后处理、软件光栅）时场景数据一行都不用改。
//
//  职责边界：
//    管：相机、可见对象列表
//    不管：怎么画（Renderer 的事）、GPU 资源生命周期
//
//  【当前版本去掉了 Light】
//    还没有光照系统（阶段 7 之后），加一个空的 Light 列表只会
//    让"什么时候该用它"变得模糊。等真正需要时再加。
// ===========================================================================
class Scene
{
public:
    // Camera 没有默认构造（位置/朝向/视场角都是必需的），
    // 所以 Scene 也不能默认构造。参数原样转发：
    // "场景建出来的那一刻，观察条件就是确定的"
    Scene(const glm::vec3& cameraPosition,
        const glm::vec3& cameraTarget,
        float fovDegrees,
        float aspectRatio,
        float nearZ = 0.1f,
        float farZ = 100.0f);

    // ---- 相机 ------------------------------------------------------------
    // 刻意给两个版本：
    //   const    —— 渲染器只该"读"场景
    //   非 const —— WASD / 鼠标拖动必须能改相机
    const Camera& GetCamera() const { return m_Camera; }
    Camera& GetCamera() { return m_Camera; }

    // ---- 可见对象 --------------------------------------------------------
    const std::vector<RenderObject>& GetObjects() const { return m_RenderObjects; }
    std::size_t GetObjectCount() const { return m_RenderObjects.size(); }

    RenderObject& GetObject(std::size_t index) { return m_RenderObjects[index]; }
    const RenderObject& GetObject(std::size_t index) const { return m_RenderObjects[index]; }

    // 加一个物体，返回它的引用（方便紧接着改字段）
    RenderObject& AddObject(RenderObject object);

private:
    Camera                    m_Camera;
    std::vector<RenderObject> m_RenderObjects;
};
