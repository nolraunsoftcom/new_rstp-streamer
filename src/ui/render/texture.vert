#version 440

layout(location = 0) in vec2 position;   // [-1,1] 정규화 좌표
layout(location = 1) in vec2 texcoord;

layout(location = 0) out vec2 v_texcoord;

layout(std140, binding = 0) uniform buf {
    vec2 scale;   // KeepAspectRatio 레터박스용 (x,y) 배율
} ubuf;

void main() {
    v_texcoord = texcoord;
    gl_Position = vec4(position.x * ubuf.scale.x, position.y * ubuf.scale.y, 0.0, 1.0);
}
