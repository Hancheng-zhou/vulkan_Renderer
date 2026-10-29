#version 450

layout(location = 0) in  vec3 vWorldNormal;
layout(location = 1) in  vec3 vWorldPos;

layout(location = 0) out vec4 outColor;

void main()
{
    // ---- 一个最小化的 Lambert 光照 ---------------------------------------
    //
    // 【为什么现在就加光照】多物体场景里如果只有法线着色，
    //   所有立方体看起来一模一样（法线相同），只能靠位置区分。
    //   加一个固定方向光之后，每个面有明暗层次，立体感和空间关系一目了然。
    //
    // 【为什么写死在 shader 里】光照参数应该走 UBO（阶段 7 的 Light），
    //   现在只有一个方向光，写死能少一个 descriptor binding。
    const vec3 kLightDir = normalize(vec3(0.5, 1.0, 0.35));   // 指向光源
    const vec3 kBaseColor = vec3(0.85, 0.75, 0.65);
    const vec3 kAmbient   = vec3(0.15);

    vec3 N = normalize(vWorldNormal);
    float ndotl = max(dot(N, kLightDir), 0.0);

    vec3 lit = kBaseColor * (kAmbient + ndotl * 0.85);

    outColor = vec4(lit, 1.0);
}
