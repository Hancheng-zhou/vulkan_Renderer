#version 450

// location 必须与管线里 VkVertexInputAttributeDescription 的 location 一一对应
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexcoord;
layout(location = 3) in vec4 inTangent;   // 5b 暂不使用，但布局必须声明

layout(location = 0) out vec3 fragColor;

// ---------------------------------------------------------------------------
//  推送常量：窗口宽高比
//
//  【为什么需要】gl_Position 输出的是 NDC，x 和 y 都在 [-1,1]。
//  驱动会把它线性映射到视口的宽和高上。窗口是 800x600 时，
//  x 的 1 个单位对应 400 像素，y 的 1 个单位对应 300 像素 ——
//  任何几何都会被横向拉伸 1.33 倍。
//
//  除以宽高比即可抵消。这个值每帧从 CPU 推过来（窗口 resize 后会变）。
// ---------------------------------------------------------------------------
layout(push_constant) uniform PushConstants
{
    float aspect;   // width / height
} pc;

// ---------------------------------------------------------------------------
//  临时模型变换：绕 Y 轴转 35 度、再绕 X 轴转 20 度
//
//  【为什么写死在这里】5b 还没有相机矩阵（阶段 6 才有）。
//  不转的话，正交投影下立方体会退化成一个正方形，看不出是立方体。
//  阶段 6 会换成真正的 model / view / projection。
// ---------------------------------------------------------------------------
const mat4 kModel =
    mat4( 0.8192,  0.0000, -0.5736, 0.0000,
         -0.2082,  0.9397, -0.2974, 0.0000,
          0.5390,  0.3420,  0.7698, 0.0000,
          0.0000,  0.0000,  0.0000, 1.0000);

void main()
{
    vec4 worldPos = kModel * vec4(inPosition, 1.0);

    vec3 ndc = worldPos.xyz;

    // 抵消视口的非等比缩放
    ndc.x /= pc.aspect;

    // 【关键：Vulkan 的 NDC z 范围是 [0, 1]，不是 OpenGL 的 [-1, 1]】
    //
    //   直接把模型空间的 z 输出出去，旋转后会出现负值。
    //   负 z 的顶点落在近平面之外，会被裁剪 —— 表现是"物体缺一个角"，
    //   而且校验层完全不报错。
    //
    //   5b 还没有深度测试，全部压到中间深度即可。
    //   阶段 8 接入深度缓冲后，这里换成真正的投影矩阵，z 会自然落在 [0,1]。
    const float kFixedDepth = 0.5;

    gl_Position = vec4(ndc.xy, kFixedDepth, 1.0);

    // 法线也转到世界空间（纯旋转不需要逆转置矩阵）
    vec3 worldNormal = mat3(kModel) * inNormal;

    // 法线可视化：[-1,1] 映射到 [0,1]。
    // 三个可见面朝向不同 -> 颜色不同，一眼能看出这是立方体。
    fragColor = worldNormal * 0.5 + 0.5;
}
