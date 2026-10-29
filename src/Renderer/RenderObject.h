// ===========================================================================
//  Renderer/RenderObject.h
//
//  场景里的一个可见对象 = 几何 + 位姿。
//
//  【与 ZHC 版的差别】
//    ZHC 多一个 Material 字段（纹理 + 光照参数）。
//    我们还没有 Texture（阶段 7），所以先去掉，
//    等材质系统建起来再加回来 —— 那时这个文件几乎不用改结构。
//
//  【为什么不持有"怎么画"】
//    没有 shader、没有 pipeline、没有 draw 调用。
//    渲染器来读它，它不知道渲染器的存在。
//    这条规则让 Scene 可以完全独立于 VkBackend 存在。
// ===========================================================================
#pragma once

#include "Renderer/Mesh.h"
#include "Renderer/Transform.h"
#include "Core/PrimitiveDesc.h"

#include <memory>
#include <string>
struct RenderObject
{
    std::string Name;

    // 共享几何：多个物体可以指向同一个 Mesh（比如一排同尺寸的立方体）。
    // 【为什么用 shared_ptr】几何数据在 GPU 上，重复上传既浪费显存
    //   也浪费时间。共享后只有最后一个引用消失时才真正释放。
    std::shared_ptr<Mesh> Geometry;

    Transform Xform;

    bool IsVisible = true;

    // 程序化几何的两份配方（与 ZHC 一致，为将来的"运行时改参数"预留）：
    //   Primitive      = 想要的
    //   BuiltPrimitive = 已经生成的
    // 两者不等说明"参数改了但还没重建"。
    PrimitiveDesc Primitive;
    PrimitiveDesc BuiltPrimitive;

    bool IsGeometryPending() const
    {
        return Geometry == nullptr || Primitive != BuiltPrimitive;
    }

    // 挂上外面建好的共享几何，同时把配方标成"已同步"。
    //
    // 【为什么不直接写 Geometry = mesh】那样配方还是默认值，
    //   面板会显示"up to date"却和真实几何不符 —— 一个不报错的谎。
    void SetSharedGeometry(const std::shared_ptr<Mesh>& geometry, const PrimitiveDesc& desc)
    {
        Geometry = geometry;
        Primitive = desc;
        BuiltPrimitive = desc;
    }
};
