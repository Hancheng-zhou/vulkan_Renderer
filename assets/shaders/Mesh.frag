#version 450

layout(location = 0) in vec3 fragColor;
// 【location 必须是 0】
//   fragment shader 的 out location == 颜色附件索引。
//   我们只有一个颜色附件（VkRenderingInfo::pColorAttachments[0]），
//   所以这里只能是 0。
//
//   写成 1 不会导致失败，只会被校验层警告"写入的附件不存在"，
//   且这次写入被丢弃 —— 画面可能正常（因为 location 0 没写，附件内容
//   是未定义的），也可能全黑，取决于驱动。
//
//   注意：fragment 的 in 和 out 是两个独立的 location 空间。
//   in location 0 来自 vertex 的 out location 0；
//   out location 0 去颜色附件 0。两者同号不冲突。
layout(location = 0) out vec4 outColor;

void main()
{
	outColor = vec4(fragColor, 1.0);

}
