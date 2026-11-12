#version 440
// A video frame from its decoder's planes, first of two steps (yuv.frag is the second and tells the
// whole): the rows of the two chroma planes (tex: U, texV: V) made as wide as the luma, U into r
// and V into g of a texture as wide as the luma and as high as the chroma.
// p0, p1, c3.x: as in yuv.frag.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    vec4 rect;   // target rectangle in NDC: x0, y0, x1, y1
    vec4 uvRect; // texture sub-rectangle: u0, v0, u1, v1
    vec4 p0;
    vec4 p1;
    vec4 c0;
    vec4 c1;
    vec4 c2;
    vec4 c3;
};
layout(binding = 1) uniform sampler2D tex;
layout(binding = 2) uniform sampler2D texV;

// as in yuv.frag
vec4 weights(float t, bool cubic)
{
    if (!cubic) return vec4(0.0, 1.0 - t, t, 0.0);
    const float C = 0.6;
    return vec4(((-C * t + 2.0 * C) * t - C) * t,
                ((2.0 - C) * t - (3.0 - C)) * t * t + 1.0,
                ((-(2.0 - C) * t + (3.0 - 2.0 * C)) * t + C) * t,
                (C * t - C) * t * t);
}

void main()
{
    // the place among a row's chroma samples, counted from the first one
    float place = (uv.x * p0.x - 0.5 - p1.x) * p1.z;
    float first = floor(place);
    vec4 w = weights(place - first, p1.z < 1.0);
    vec2 row = vec2(0.0);
    for (int i = 0; i < 4; ++i) {
        // past an edge the sample at the edge again, as swscale has it
        vec2 at = vec2((clamp(first + float(i) - 1.0, 0.0, p0.z - 1.0) + 0.5) / p0.z, uv.y);
        row += w[i] * vec2(texture(tex, at).r, texture(texV, at).r);
    }
    // swscale keeps a row in 15 bits: what the cubic overshoots past the largest value is cut off
    fragColor = vec4(min(row, vec2(c3.x)), 0.0, 1.0);
}
