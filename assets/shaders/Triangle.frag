#version 450

// location 必须和 vertex shader 的 out 对上（都是 0）
layout(location = 0) in  vec3 fragColor;
layout(location = 0) out vec4 outColor;

void main()
{
    // 【关于颜色偏亮】交换链是 SRGB 格式，这里输出的值被当作线性光照强度，
    //   硬件自动做 gamma 编码。所以纯红 (1,0,0) 上屏仍是纯红，
    //   但中间过渡色会比你在取色器里看到的更亮 —— 这是正确行为。
    outColor = vec4(fragColor, 1.0);
}