#include "Scene/Scene.h"

#include <utility>

Scene::Scene(const glm::vec3& cameraPosition,
    const glm::vec3& cameraTarget,
    float fovDegrees,
    float aspectRatio,
    float nearZ,
    float farZ)
    : m_Camera(cameraPosition, cameraTarget, fovDegrees, aspectRatio, nearZ, farZ)
{
}

RenderObject& Scene::AddObject(RenderObject object)
{
    // 【为什么返回引用】调用方常常"加完之后立刻改几个字段"：
    //     scene.AddObject(obj).Xform.Position = ...;
    //   返回引用能把这两步合成一句。
    //
    // 【为什么用 emplace_back 之后取 back()】
    //   push_back 需要一次移动，emplace_back 直接原地构造。
    //   注意返回的引用在下一次 AddObject 后就可能失效（vector 扩容会搬移），
    //   所以只该在加完立刻用，不要存下来。
    m_RenderObjects.push_back(std::move(object));
    return m_RenderObjects.back();
}
