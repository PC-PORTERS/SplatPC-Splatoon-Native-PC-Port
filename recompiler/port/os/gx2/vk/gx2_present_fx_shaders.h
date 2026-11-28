// gx2_present_fx_shaders.h - GLSL 450 compute shaders for FXAA 3.11, SMAA 1x,
// AMD FidelityFX FSR 1.0 (EASU + RCAS), and Anime4K spatial upscaling.
#pragma once

namespace gx2_present_fx_shaders {

inline constexpr const char* kFxaaShader = R"glsl(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D tex0;
layout(set = 0, binding = 1) uniform sampler2D tex1;
layout(set = 0, binding = 2, rgba8) uniform writeonly image2D outImg;
layout(push_constant) uniform Push {
    vec4 srcSize;
    vec4 dstSize;
    vec4 params;
} pc;

float luma(vec3 c) { return sqrt(dot(max(c, vec3(0.0)), vec3(0.299, 0.587, 0.114))); }
vec3 sampleRgb(vec2 uv) { return textureLod(tex0, clamp(uv, vec2(0.0), vec2(1.0)), 0.0).rgb; }

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outImg);
    if (any(greaterThanEqual(id, size))) return;
    vec2 px = pc.srcSize.zw;
    vec2 uv = (vec2(id) + 0.5) * pc.dstSize.zw;
    vec3 cM = sampleRgb(uv);
    float lM = luma(cM);
    float lN = luma(sampleRgb(uv + vec2(0.0, -px.y)));
    float lS = luma(sampleRgb(uv + vec2(0.0, px.y)));
    float lW = luma(sampleRgb(uv + vec2(-px.x, 0.0)));
    float lE = luma(sampleRgb(uv + vec2(px.x, 0.0)));
    float lo = min(lM, min(min(lN, lS), min(lW, lE)));
    float hi = max(lM, max(max(lN, lS), max(lW, lE)));
    float range = hi - lo;
    if (range < max(0.0312, hi * 0.125)) {
        imageStore(outImg, id, vec4(cM, 1.0));
        return;
    }
    float lNW = luma(sampleRgb(uv + vec2(-px.x, -px.y)));
    float lNE = luma(sampleRgb(uv + vec2(px.x, -px.y)));
    float lSW = luma(sampleRgb(uv + vec2(-px.x, px.y)));
    float lSE = luma(sampleRgb(uv + vec2(px.x, px.y)));
    float edgeH = abs(lNW + lNE - 2.0 * lN) + 2.0 * abs(lW + lE - 2.0 * lM) + abs(lSW + lSE - 2.0 * lS);
    float edgeV = abs(lNW + lSW - 2.0 * lW) + 2.0 * abs(lN + lS - 2.0 * lM) + abs(lNE + lSE - 2.0 * lE);
    bool horizontal = edgeH >= edgeV;
    float l1 = horizontal ? lN : lW;
    float l2 = horizontal ? lS : lE;
    float g1 = abs(l1 - lM);
    float g2 = abs(l2 - lM);
    bool steeper1 = g1 >= g2;
    float gradient = 0.25 * max(g1, g2);
    float stepLen = horizontal ? px.y : px.x;
    float lAvg;
    if (steeper1) {
        stepLen = -stepLen;
        lAvg = 0.5 * (l1 + lM);
    } else {
        lAvg = 0.5 * (l2 + lM);
    }
    vec2 cur = uv;
    if (horizontal) cur.y += stepLen * 0.5;
    else cur.x += stepLen * 0.5;
    vec2 off = horizontal ? vec2(px.x, 0.0) : vec2(0.0, px.y);
    vec2 a = cur - off, b = cur + off;
    float ea = luma(sampleRgb(a)) - lAvg;
    float eb = luma(sampleRgb(b)) - lAvg;
    bool da = abs(ea) >= gradient;
    bool db = abs(eb) >= gradient;
    const float quality[10] = float[10](1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);
    for (int i = 0; i < 10 && !(da && db); ++i) {
        if (!da) {
            a -= off * quality[i];
            ea = luma(sampleRgb(a)) - lAvg;
            da = abs(ea) >= gradient;
        }
        if (!db) {
            b += off * quality[i];
            eb = luma(sampleRgb(b)) - lAvg;
            db = abs(eb) >= gradient;
        }
    }
    float distA = horizontal ? uv.x - a.x : uv.y - a.y;
    float distB = horizontal ? b.x - uv.x : b.y - uv.y;
    bool closerA = distA < distB;
    float dist = min(distA, distB);
    float len = max(distA + distB, 1e-6);
    bool smaller = lM < lAvg;
    bool correct = ((closerA ? ea : eb) < 0.0) != smaller;
    float edgeOffset = correct ? (-dist / len + 0.5) : 0.0;
    float sub = clamp(abs((2.0 * (lN + lS + lW + lE) + lNW + lNE + lSW + lSE) / 12.0 - lM) / range, 0.0, 1.0);
    sub = sub * sub * (3.0 - 2.0 * sub);
    float finalOffset = max(edgeOffset, sub * sub * 0.75);
    vec2 f = uv;
    if (horizontal) f.y += finalOffset * stepLen;
    else f.x += finalOffset * stepLen;
    imageStore(outImg, id, vec4(sampleRgb(f), 1.0));
}
)glsl";

// SMAA 1x Pass 1: Color-delta edge detection with local contrast adaptation.
inline constexpr const char* kSmaaEdgeShader = R"glsl(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D tex0;
layout(set = 0, binding = 1) uniform sampler2D tex1;
layout(set = 0, binding = 2, rgba8) uniform writeonly image2D outImg;
layout(push_constant) uniform Push {
    vec4 srcSize;
    vec4 dstSize;
    vec4 params;
} pc;

float chanDelta(vec3 a, vec3 b) {
    vec3 d = abs(a - b);
    return max(d.r, max(d.g, d.b));
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outImg);
    if (any(greaterThanEqual(id, size))) return;
    vec2 uv = (vec2(id) + 0.5) * pc.dstSize.zw;
    vec2 px = pc.srcSize.zw;
    const float kThreshold = 0.08;
    const float kLocalContrast = 2.0;

    vec3 cC = textureLod(tex0, uv, 0.0).rgb;
    vec3 cL = textureLod(tex0, uv + vec2(-px.x, 0.0), 0.0).rgb;
    vec3 cT = textureLod(tex0, uv + vec2(0.0, -px.y), 0.0).rgb;
    float dL = chanDelta(cC, cL);
    float dT = chanDelta(cC, cT);

    float eV = step(kThreshold, dL);
    float eH = step(kThreshold, dT);
    if (eV + eH > 0.0) {
        float dR = chanDelta(cC, textureLod(tex0, uv + vec2(px.x, 0.0), 0.0).rgb);
        float dB = chanDelta(cC, textureLod(tex0, uv + vec2(0.0, px.y), 0.0).rgb);
        float dL2 = chanDelta(cL, textureLod(tex0, uv + vec2(-2.0 * px.x, 0.0), 0.0).rgb);
        float dT2 = chanDelta(cT, textureLod(tex0, uv + vec2(0.0, -2.0 * px.y), 0.0).rgb);
        float maxD = max(max(dL, dT), max(max(dR, dB), max(dL2, dT2)));
        if (dL * kLocalContrast < maxD) eV = 0.0;
        if (dT * kLocalContrast < maxD) eH = 0.0;
    }
    // RG = (horizontal boundary edgeV/left-edge, top-edge/vertical) -> x: vertical boundary (left), y: horizontal boundary (top)
    imageStore(outImg, id, vec4(eV, eH, 0.0, 1.0));
}
)glsl";

// SMAA 1x Pass 2: Edge span search & blending weights (top, bottom, left, right).
inline constexpr const char* kSmaaWeightShader = R"glsl(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D tex0;
layout(set = 0, binding = 1) uniform sampler2D tex1;
layout(set = 0, binding = 2, rgba8) uniform writeonly image2D outImg;
layout(push_constant) uniform Push {
    vec4 srcSize;
    vec4 dstSize;
    vec4 params;
} pc;

float searchHoriz(vec2 uv, float stepX, float yOff, out float crossEdge) {
    vec2 px = pc.srcSize.zw;
    float dist = 0.0;
    for (int i = 1; i <= 12; ++i) {
        float e = textureLod(tex0, uv + vec2(float(i) * stepX * px.x, 0.0), 0.0).g;
        if (e < 0.5) break;
        dist = float(i);
    }
    vec2 endUv = uv + vec2((dist + 0.5) * stepX * px.x, yOff * px.y);
    crossEdge = textureLod(tex0, endUv, 0.0).r;
    return dist;
}

float searchVert(vec2 uv, float stepY, float xOff, out float crossEdge) {
    vec2 px = pc.srcSize.zw;
    float dist = 0.0;
    for (int i = 1; i <= 12; ++i) {
        float e = textureLod(tex0, uv + vec2(0.0, float(i) * stepY * px.y), 0.0).r;
        if (e < 0.5) break;
        dist = float(i);
    }
    vec2 endUv = uv + vec2(xOff * px.x, (dist + 0.5) * stepY * px.y);
    crossEdge = textureLod(tex0, endUv, 0.0).g;
    return dist;
}

// Analytic subpixel trapezoid coverage along an edge of length d1 + d2 + 1.
float spanWeight(float d1, float d2, float c1, float c2, bool preferSide1) {
    float len = d1 + d2 + 1.0;
    if (len <= 1.0 && c1 < 0.5 && c2 < 0.5) return 0.0;
    float t1 = (d1 + 0.5) / len;
    float t2 = (d2 + 0.5) / len;
    if (c1 > 0.5 && c2 > 0.5) {
        return clamp(0.5 - min(t1, t2), 0.0, 0.5);
    }
    if (c1 > 0.5) {
        return preferSide1 ? clamp(0.5 * (1.0 - t1), 0.0, 0.5) : 0.0;
    }
    if (c2 > 0.5) {
        return preferSide1 ? 0.0 : clamp(0.5 * (1.0 - t2), 0.0, 0.5);
    }
    return 0.125 * clamp(1.0 - len * 0.06, 0.25, 1.0);
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outImg);
    if (any(greaterThanEqual(id, size))) return;
    vec2 uv = (vec2(id) + 0.5) * pc.dstSize.zw;
    vec2 px = pc.srcSize.zw;
    vec2 edges = texelFetch(tex0, id, 0).rg;

    float wTop = 0.0, wBot = 0.0, wLeft = 0.0, wRight = 0.0;
    // Horizontal edges (edge.g at current pixel -> blend with pixel above; below neighbor's edge.g -> blend with pixel below)
    if (edges.g > 0.5) {
        float cL = 0.0, cR = 0.0;
        float dL = searchHoriz(uv, -1.0, -0.5, cL);
        float dR = searchHoriz(uv, 1.0, -0.5, cR);
        wTop = spanWeight(dL, dR, cL, cR, dL <= dR);
    }
    float eBelow = textureLod(tex0, uv + vec2(0.0, px.y), 0.0).g;
    if (eBelow > 0.5) {
        float cL = 0.0, cR = 0.0;
        float dL = searchHoriz(uv + vec2(0.0, px.y), -1.0, 0.5, cL);
        float dR = searchHoriz(uv + vec2(0.0, px.y), 1.0, 0.5, cR);
        wBot = spanWeight(dL, dR, cL, cR, dL <= dR);
    }
    // Vertical edges (edge.r at current pixel -> blend left; right neighbor's edge.r -> blend right)
    if (edges.r > 0.5) {
        float cU = 0.0, cD = 0.0;
        float dU = searchVert(uv, -1.0, -0.5, cU);
        float dD = searchVert(uv, 1.0, -0.5, cD);
        wLeft = spanWeight(dU, dD, cU, cD, dU <= dD);
    }
    float eRight = textureLod(tex0, uv + vec2(px.x, 0.0), 0.0).r;
    if (eRight > 0.5) {
        float cU = 0.0, cD = 0.0;
        float dU = searchVert(uv + vec2(px.x, 0.0), -1.0, 0.5, cU);
        float dD = searchVert(uv + vec2(px.x, 0.0), 1.0, 0.5, cD);
        wRight = spanWeight(dU, dD, cU, cD, dU <= dD);
    }

    imageStore(outImg, id, vec4(wTop, wBot, wLeft, wRight));
}
)glsl";

// SMAA 1x Pass 3: Subpixel neighborhood blend.
inline constexpr const char* kSmaaBlendShader = R"glsl(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D tex0;
layout(set = 0, binding = 1) uniform sampler2D tex1;
layout(set = 0, binding = 2, rgba8) uniform writeonly image2D outImg;
layout(push_constant) uniform Push {
    vec4 srcSize;
    vec4 dstSize;
    vec4 params;
} pc;

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outImg);
    if (any(greaterThanEqual(id, size))) return;
    vec2 uv = (vec2(id) + 0.5) * pc.dstSize.zw;
    vec2 px = pc.srcSize.zw;
    vec4 w = texelFetch(tex1, id, 0);
    float vertSpan = w.x + w.y;  // top + bottom blend
    float horizSpan = w.z + w.w; // left + right blend
    if (vertSpan + horizSpan < 1e-4) {
        imageStore(outImg, id, vec4(textureLod(tex0, uv, 0.0).rgb, 1.0));
        return;
    }
    vec3 color;
    if (horizSpan >= vertSpan) {
        float shift = (w.w - w.z) * px.x;
        color = textureLod(tex0, clamp(uv + vec2(shift, 0.0), vec2(0.0), vec2(1.0)), 0.0).rgb;
    } else {
        float shift = (w.y - w.x) * px.y;
        color = textureLod(tex0, clamp(uv + vec2(0.0, shift), vec2(0.0), vec2(1.0)), 0.0).rgb;
    }
    imageStore(outImg, id, vec4(color, 1.0));
}
)glsl";

// AMD FidelityFX FSR 1.0 Pass 1: Edge-Adaptive Spatial Upsampling (EASU).
inline constexpr const char* kFsrEasuShader = R"glsl(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D tex0;
layout(set = 0, binding = 1) uniform sampler2D tex1;
layout(set = 0, binding = 2, rgba8) uniform writeonly image2D outImg;
layout(push_constant) uniform Push {
    vec4 srcSize;
    vec4 dstSize;
    vec4 params;
} pc;

float fsrLuma(vec3 c) { return c.b * 0.5 + c.r * 0.5 + c.g; }

vec3 fsrFetch(ivec2 coord) {
    ivec2 maxCoord = ivec2(pc.srcSize.xy) - ivec2(1);
    ivec2 c = clamp(coord, ivec2(0), maxCoord);
    return texelFetch(tex0, c, 0).rgb;
}

void easuSet(inout vec2 dir, inout float len, vec2 pp, bool biS, bool biT, bool biU, bool biV, float lA, float lB,
             float lC, float lD, float lE) {
    float w = 0.0;
    if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
    if (biT) w = pp.x * (1.0 - pp.y);
    if (biU) w = (1.0 - pp.x) * pp.y;
    if (biV) w = pp.x * pp.y;
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = lenX > 0.0 ? 1.0 / lenX : 0.0;
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = lenY > 0.0 ? 1.0 / lenY : 0.0;
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    lenY *= lenY;
    len += lenY * w;
}

void easuTap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {
    vec2 v;
    v.x = (off.x * dir.x) + (off.y * dir.y);
    v.y = (off.x * (-dir.y)) + (off.y * dir.x);
    v *= len2;
    float d2 = min(dot(v, v), clp);
    float wB = 0.4 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 1.5625 * wB - 0.5625;
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outImg);
    if (any(greaterThanEqual(id, size))) return;

    vec2 pp = (vec2(id) + 0.5) * (pc.srcSize.xy * pc.dstSize.zw) - 0.5;
    vec2 fp = floor(pp);
    ivec2 ip = ivec2(fp);
    pp -= fp;

    // 12-tap cross footprint
    vec3 b = fsrFetch(ip + ivec2(0, -1));
    vec3 c = fsrFetch(ip + ivec2(1, -1));
    vec3 e = fsrFetch(ip + ivec2(-1, 0));
    vec3 f = fsrFetch(ip + ivec2(0, 0));
    vec3 g = fsrFetch(ip + ivec2(1, 0));
    vec3 h = fsrFetch(ip + ivec2(2, 0));
    vec3 i = fsrFetch(ip + ivec2(-1, 1));
    vec3 j = fsrFetch(ip + ivec2(0, 1));
    vec3 k = fsrFetch(ip + ivec2(1, 1));
    vec3 l = fsrFetch(ip + ivec2(2, 1));
    vec3 n = fsrFetch(ip + ivec2(0, 2));
    vec3 o = fsrFetch(ip + ivec2(1, 2));

    float bL = fsrLuma(b), cL = fsrLuma(c);
    float eL = fsrLuma(e), fL = fsrLuma(f), gL = fsrLuma(g), hL = fsrLuma(h);
    float iL = fsrLuma(i), jL = fsrLuma(j), kL = fsrLuma(k), lL = fsrLuma(l);
    float nL = fsrLuma(n), oL = fsrLuma(o);

    vec2 dir = vec2(0.0);
    float len = 0.0;
    easuSet(dir, len, pp, true, false, false, false, bL, eL, fL, gL, jL);
    easuSet(dir, len, pp, false, true, false, false, cL, fL, gL, hL, kL);
    easuSet(dir, len, pp, false, false, true, false, fL, iL, jL, kL, nL);
    easuSet(dir, len, pp, false, false, false, true, gL, jL, kL, lL, oL);

    float dirLenSq = dot(dir, dir);
    if (dirLenSq < 1e-6) {
        dir = vec2(1.0, 0.0);
    } else {
        dir *= inversesqrt(dirLenSq);
    }
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + (0.21 - 0.5) * len;
    float clp = 1.0 / lob;

    vec3 min4 = min(min(f, g), min(j, k));
    vec3 max4 = max(max(f, g), max(j, k));
    vec3 aC = vec3(0.0);
    float aW = 0.0;
    easuTap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    easuTap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    easuTap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    easuTap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    easuTap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    easuTap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    easuTap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    easuTap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    easuTap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    easuTap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    easuTap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    easuTap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);

    vec3 outCol = clamp(aC / max(aW, 1e-5), min4, max4);
    imageStore(outImg, id, vec4(outCol, 1.0));
}
)glsl";

// AMD FidelityFX FSR 1.0 Pass 2: Robust Contrast-Adaptive Sharpening (RCAS).
inline constexpr const char* kFsrRcasShader = R"glsl(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D tex0;
layout(set = 0, binding = 1) uniform sampler2D tex1;
layout(set = 0, binding = 2, rgba8) uniform writeonly image2D outImg;
layout(push_constant) uniform Push {
    vec4 srcSize;
    vec4 dstSize;
    vec4 params;
} pc;

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outImg);
    if (any(greaterThanEqual(id, size))) return;
    ivec2 maxCoord = size - ivec2(1);

    vec3 b = texelFetch(tex0, clamp(id + ivec2(0, -1), ivec2(0), maxCoord), 0).rgb;
    vec3 d = texelFetch(tex0, clamp(id + ivec2(-1, 0), ivec2(0), maxCoord), 0).rgb;
    vec3 e = texelFetch(tex0, id, 0).rgb;
    vec3 f = texelFetch(tex0, clamp(id + ivec2(1, 0), ivec2(0), maxCoord), 0).rgb;
    vec3 h = texelFetch(tex0, clamp(id + ivec2(0, 1), ivec2(0), maxCoord), 0).rgb;

    float bL = b.b * 0.5 + b.r * 0.5 + b.g;
    float dL = d.b * 0.5 + d.r * 0.5 + d.g;
    float eL = e.b * 0.5 + e.r * 0.5 + e.g;
    float fL = f.b * 0.5 + f.r * 0.5 + f.g;
    float hL = h.b * 0.5 + h.r * 0.5 + h.g;

    float mn4L = min(min(bL, dL), min(fL, hL));
    float mx4L = max(max(bL, dL), max(fL, hL));
    float hitMin = min(mn4L, eL) / max(4.0 * mx4L, 1e-4);
    float hitMax = (1.0 - max(mx4L, eL)) / max(4.0 * mn4L - 4.0, -1e-4);
    float lobe = max(-hitMin, hitMax);

    float sharp = clamp(pc.params.x, 0.0, 1.0);
    float peak = -1.0 / mix(16.0, 4.0, sharp);
    float w = clamp(lobe, peak, 0.0);
    float nz = 0.25 * (bL + dL + fL + hL) - eL;
    nz = clamp(abs(nz) / max(mx4L - mn4L, 1e-4), 0.0, 1.0);
    w *= (1.0 - 0.5 * nz);
    w *= step(0.001, sharp);

    vec3 outCol = clamp(((b + d + f + h) * w + e) / (1.0 + 4.0 * w), vec3(0.0), vec3(1.0));
    imageStore(outImg, id, vec4(outCol, 1.0));
}
)glsl";

// Anime4K Pass 1: Catmull-Rom bicubic spatial upsample.
inline constexpr const char* kAnime4kUpsampleShader = R"glsl(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D tex0;
layout(set = 0, binding = 1) uniform sampler2D tex1;
layout(set = 0, binding = 2, rgba8) uniform writeonly image2D outImg;
layout(push_constant) uniform Push {
    vec4 srcSize;
    vec4 dstSize;
    vec4 params;
} pc;

float catmullRom(float x) {
    float a = abs(x);
    if (a < 1.0) return 1.5 * a * a * a - 2.5 * a * a + 1.0;
    if (a < 2.0) return -0.5 * a * a * a + 2.5 * a * a - 4.0 * a + 2.0;
    return 0.0;
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outImg);
    if (any(greaterThanEqual(id, size))) return;

    vec2 pp = (vec2(id) + 0.5) * (pc.srcSize.xy * pc.dstSize.zw) - 0.5;
    vec2 fp = floor(pp);
    ivec2 ip = ivec2(fp);
    vec2 frac = pp - fp;
    ivec2 maxCoord = ivec2(pc.srcSize.xy) - ivec2(1);

    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    vec3 mn = vec3(1e9), mx = vec3(-1e9);
    for (int j = -1; j <= 2; ++j) {
        float wy = catmullRom(float(j) - frac.y);
        for (int i = -1; i <= 2; ++i) {
            float wx = catmullRom(float(i) - frac.x);
            float w = wx * wy;
            vec3 c = texelFetch(tex0, clamp(ip + ivec2(i, j), ivec2(0), maxCoord), 0).rgb;
            if (i >= 0 && i <= 1 && j >= 0 && j <= 1) {
                mn = min(mn, c);
                mx = max(mx, c);
            }
            sum += c * w;
            wsum += w;
        }
    }
    vec3 outCol = clamp(sum / max(wsum, 1e-5), mn, mx);
    imageStore(outImg, id, vec4(outCol, 1.0));
}
)glsl";

// Anime4K Pass 2: Sobel gradient-guided line thinning / clean dark-line push.
inline constexpr const char* kAnime4kThinShader = R"glsl(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D tex0;
layout(set = 0, binding = 1) uniform sampler2D tex1;
layout(set = 0, binding = 2, rgba8) uniform writeonly image2D outImg;
layout(push_constant) uniform Push {
    vec4 srcSize;
    vec4 dstSize;
    vec4 params;
} pc;

float a4Luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outImg);
    if (any(greaterThanEqual(id, size))) return;
    ivec2 maxCoord = size - ivec2(1);

    vec3 tl = texelFetch(tex0, clamp(id + ivec2(-1, -1), ivec2(0), maxCoord), 0).rgb;
    vec3 tc = texelFetch(tex0, clamp(id + ivec2(0, -1), ivec2(0), maxCoord), 0).rgb;
    vec3 tr = texelFetch(tex0, clamp(id + ivec2(1, -1), ivec2(0), maxCoord), 0).rgb;
    vec3 ml = texelFetch(tex0, clamp(id + ivec2(-1, 0), ivec2(0), maxCoord), 0).rgb;
    vec3 mc = texelFetch(tex0, id, 0).rgb;
    vec3 mr = texelFetch(tex0, clamp(id + ivec2(1, 0), ivec2(0), maxCoord), 0).rgb;
    vec3 bl = texelFetch(tex0, clamp(id + ivec2(-1, 1), ivec2(0), maxCoord), 0).rgb;
    vec3 bc = texelFetch(tex0, clamp(id + ivec2(0, 1), ivec2(0), maxCoord), 0).rgb;
    vec3 br = texelFetch(tex0, clamp(id + ivec2(1, 1), ivec2(0), maxCoord), 0).rgb;

    float ltl = a4Luma(tl), ltc = a4Luma(tc), ltr = a4Luma(tr);
    float lml = a4Luma(ml), lmc = a4Luma(mc), lmr = a4Luma(mr);
    float lbl = a4Luma(bl), lbc = a4Luma(bc), lbr = a4Luma(br);

    float gx = (ltr + 2.0 * lmr + lbr) - (ltl + 2.0 * lml + lbl);
    float gy = (lbl + 2.0 * lbc + lbr) - (ltl + 2.0 * ltc + ltr);
    float gMag = length(vec2(gx, gy));

    vec3 outCol = mc;
    if (gMag > 0.04) {
        vec2 n = vec2(gx, gy) / gMag;
        // Sample along the gradient to push bright halos away from line cores.
        vec2 uv = (vec2(id) + 0.5) * pc.dstSize.zw;
        vec2 px = pc.dstSize.zw;
        vec3 p = textureLod(tex0, clamp(uv + n * px, vec2(0.0), vec2(1.0)), 0.0).rgb;
        vec3 q = textureLod(tex0, clamp(uv - n * px, vec2(0.0), vec2(1.0)), 0.0).rgb;
        vec3 target = a4Luma(p) > a4Luma(q) ? p : q;
        float strength = clamp(0.30 * pc.params.x + 0.15, 0.10, 0.45) * smoothstep(0.04, 0.28, gMag);
        if (a4Luma(target) > lmc) outCol = mix(mc, target, strength);
    }
    imageStore(outImg, id, vec4(clamp(outCol, vec3(0.0), vec3(1.0)), 1.0));
}
)glsl";

// Anime4K Pass 3: Edge-guided unsharp deblur with local min/max anti-ringing.
inline constexpr const char* kAnime4kRefineShader = R"glsl(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D tex0;
layout(set = 0, binding = 1) uniform sampler2D tex1;
layout(set = 0, binding = 2, rgba8) uniform writeonly image2D outImg;
layout(push_constant) uniform Push {
    vec4 srcSize;
    vec4 dstSize;
    vec4 params;
} pc;

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outImg);
    if (any(greaterThanEqual(id, size))) return;
    ivec2 maxCoord = size - ivec2(1);

    vec3 t = texelFetch(tex0, clamp(id + ivec2(0, -1), ivec2(0), maxCoord), 0).rgb;
    vec3 l = texelFetch(tex0, clamp(id + ivec2(-1, 0), ivec2(0), maxCoord), 0).rgb;
    vec3 c = texelFetch(tex0, id, 0).rgb;
    vec3 r = texelFetch(tex0, clamp(id + ivec2(1, 0), ivec2(0), maxCoord), 0).rgb;
    vec3 b = texelFetch(tex0, clamp(id + ivec2(0, 1), ivec2(0), maxCoord), 0).rgb;

    vec3 mn = min(c, min(min(t, b), min(l, r)));
    vec3 mx = max(c, max(max(t, b), max(l, r)));
    vec3 avg = 0.25 * (t + l + r + b);
    float sharp = mix(0.25, 1.35, clamp(pc.params.x, 0.0, 1.0)) * step(0.001, pc.params.x);
    vec3 outCol = clamp(c + (c - avg) * sharp, mn, mx);
    imageStore(outImg, id, vec4(clamp(outCol, vec3(0.0), vec3(1.0)), 1.0));
}
)glsl";

} // namespace gx2_present_fx_shaders
