#pragma once
#include <cstdint>
#include <string>

// ===========================================================================
//  "程序化几何的配方"：生成什么形状 + 按什么参数生成。
//
//  【和 Transform::Scale 的区别】
//    Transform::Scale 是【变换】：几何不变，在顶点着色器里缩放
//    PrimitiveDesc    是【几何本身】：顶点数据真的被重新生成
//
//  依赖方向：不认识 Vulkan、不认识 Mesh、不认识 Scene —— 只是"一份配方"
// ===========================================================================

enum class PrimitiveType
{
	Cube = 0,
	Sphere = 1,
	Obj = 2,
};

// 下标必须等于枚举值。加形状时枚举和这个数组一起改。
inline constexpr const char* k_PrimitiveTypeNames[] = { "Cube", "Sphere", "Obj" };
inline constexpr int         k_PrimitiveTypeCount = 3;

inline const char* PrimitiveTypeName(PrimitiveType type)
{
	const int index = static_cast<int>(type);
	return (index >= 0 && index < k_PrimitiveTypeCount) ? k_PrimitiveTypeNames[index] : "Unknown Mesh Type";

}
struct PrimitiveDesc
{
    PrimitiveType Type = PrimitiveType::Cube;

    float Size = 1.0f;      // Cube: 边长   Sphere: 半径

    // 只对 Sphere 有意义，但刻意留在同一个结构里：
    // 从球切回立方体再切回来时，之前调好的细分级别还在
    uint32_t Segments = 32; // 经线数
    uint32_t Rings = 16; // 纬线数

    std::string Path;

    // 【这里用浮点 == 是安全的】这些值只由同一套控件写入，
    // 不参与任何累加/插值，不存在"数学上应相等但浮点上不等"的情况
    bool operator==(const PrimitiveDesc& other) const
    {
        return Type == other.Type
            && Size == other.Size
            && Segments == other.Segments
            && Rings == other.Rings
            && Path == other.Path;
    }

    bool operator!=(const PrimitiveDesc& other) const { return !(*this == other); }
};