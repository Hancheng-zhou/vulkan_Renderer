#version 450
// ---------------------------------------------------------------------------
//  顶点位置直接写在着色器里，靠 gl_VertexID 索引。
//
//  【为什么不用顶点缓冲】阶段 4 的目标是验证管线能通，不是验证数据通路。
//    vkCmdDraw(cmd, 3, 1, 0, 0) 里第一个参数就是顶点数，着色器按
//    gl_VertexID = 0/1/2 自己生成位置 —— 完全不需要 VkBuffer。
//    阶段 5 会换成真正的顶点缓冲。
//
//  【NDC 坐标】Vulkan 的可见范围是 x,y ∈ [-1,1]，z ∈ [0,1]。
//    这里 z 填 0（靠近近平面一侧），后面接深度测试时要留意这个约定。
// ---------------------------------------------------------------------------

layout(location = 0) out vec3 fragColor;

const vec2 kPosition[3] = vec2[](vec2(0.0,-0.5),vec2(0.5,0.5),vec2(-0.5,0.5));
const vec3 kColors[3] = vec3[](vec3(1.0,0.0,0.0),vec3(0.0,1.0,0.0),vec3(0.0,0.0,1.0));

void main()
{
	gl_Position  = vec4(kPosition[gl_VertexIndex],0.0,1.0);
	fragColor = kColors[gl_VertexIndex];
}
