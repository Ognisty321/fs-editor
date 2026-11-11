#version 440
// A video frame from its decoder's planes, second of two steps: RGB from the luma (tex) and the
// chroma rows chroma.frag made as wide as it (texC: U in r, V in g), drawn into a texture of the
// frame's size, which the view then shows as it shows a frame converted on the CPU.
// The two steps are what swscale made of the frame before (SWS_BICUBIC | SWS_FULL_CHR_H_INT): the
// luma as it is, the chroma taken at each luma sample's place with swscale's bicubic where its plane
// is smaller, along the rows first and then down the columns, and swscale's coefficients for the
// matrix and range. fs-media's tests have this arithmetic on the CPU beside swscale's frame (shade
// in crates/fs-media/tests/media.rs); keep the three alike.
// p0: the luma plane's size, then a chroma plane's, in samples.
// p1.xy: where the first chroma sample lies, in luma samples from the first luma sample;
// p1.zw: chroma samples a luma sample, each way (0.5 for a plane half as large, 1 for one as large).
// c0, c1, c2: R, G and B as factors of (Y, U, V, 1). c3.x: the most a row of chroma can come to.
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
layout(binding = 2) uniform sampler2D texC;

// How much each of the four samples around a place counts, the place `t` of the way from the second
// to the third: swscale's bicubic (B = 0, C = 0.6) for a plane smaller than the luma, and the line
// between the two for one as large, where swscale takes the sample itself.
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
    // the place among the chroma rows, counted from the first one
    float place = (uv.y * p0.y - 0.5 - p1.y) * p1.w;
    float first = floor(place);
    vec4 w = weights(place - first, p1.w < 1.0);
    vec2 chroma = vec2(0.0);
    for (int j = 0; j < 4; ++j) {
        // past an edge the row at the edge again, as swscale has it
        float y = (clamp(first + float(j) - 1.0, 0.0, p0.w - 1.0) + 0.5) / p0.w;
        chroma += w[j] * texture(texC, vec2(uv.x, y)).rg;
    }
    vec4 yuv = vec4(texture(tex, uv).r, chroma, 1.0);
    fragColor = vec4(clamp(vec3(dot(c0, yuv), dot(c1, yuv), dot(c2, yuv)), 0.0, 1.0), 1.0);
}
