#pragma once
#include <glm/glm.hpp>

// ===========================================================================
//  Core/Vertex.h
//
//  顶点格式，与 ZHC 完全一致。
//
//  为什么是 48 字节：
//    pos      vec3  -> 12
//    normal   vec3  -> 12
//    texcoord vec2  ->  8
//    tangent  vec4  -> 16
//    合计 48
//
//  tangent 用 vec4 而不是 vec3：w 分量是副切线的朝向符号（±1），
//  法线贴图重建 TBN 矩阵时需要它。阶段 7 会用到。
//
//  【为什么这里仍然不写 VkVertexInputAttributeDescription】
//    Core/ 层必须与图形 API 无关。布局描述由 Renderer 侧构造。
// ===========================================================================
struct Vertex
{
	glm::vec3 Position;
	glm::vec3 Normal;
	glm::vec2 Texcoord;
	glm::vec4 Tangent{ 0.0f,0.0f,0.0f,1.0f };
};