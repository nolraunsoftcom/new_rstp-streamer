#version 440

// NV12(2-plane) → RGB. luma=Y(R8), chroma=CbCr(RG8 interleaved). 하드웨어 샘플러가
// chroma half-res를 bilinear 업샘플(linear filter)한다. YUV→RGB는 BT.601 기준.
//
// video-range(기본): Y∈[16,235], C∈[16,240] → 정규화 후 행렬.
//   Y' = (Y - 16/255), U = (Cb - 128/255), V = (Cr - 128/255)
//   R = 1.164*Y' + 1.596*V
//   G = 1.164*Y' - 0.391*U - 0.813*V
//   B = 1.164*Y' + 2.018*U
// full-range(fullRange=1): Y∈[0,255], C∈[0,255]
//   R = Y + 1.402*V ; G = Y - 0.344*U - 0.714*V ; B = Y + 1.772*U
layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D lumaTex;    // R8: Y
layout(binding = 2) uniform sampler2D chromaTex;  // RG8: Cb=r, Cr=g

layout(std140, binding = 3) uniform fbuf {
    int fullRange;   // 0 = video-range(BT.601), 1 = full-range
} ubuf;

void main() {
    float y = texture(lumaTex, v_texcoord).r;
    vec2  c = texture(chromaTex, v_texcoord).rg;   // r=Cb, g=Cr
    float u = c.r;
    float v = c.g;

    vec3 rgb;
    if (ubuf.fullRange != 0) {
        float yy = y;
        float uu = u - 0.5;
        float vv = v - 0.5;
        rgb = vec3(
            yy + 1.402 * vv,
            yy - 0.344136 * uu - 0.714136 * vv,
            yy + 1.772 * uu);
    } else {
        float yy = y - 0.0627451;        // 16/255
        float uu = u - 0.5;
        float vv = v - 0.5;
        rgb = vec3(
            1.164384 * yy + 1.596027 * vv,
            1.164384 * yy - 0.391762 * uu - 0.812968 * vv,
            1.164384 * yy + 2.017232 * uu);
    }

    fragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
