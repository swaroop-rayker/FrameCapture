// BGRA8 / scRGB-FP16 -> NV12 colour conversion (SPEC.md §4.2, §6).
//
// Every stage in §6 is locked, and each lock below corresponds to a bug class that
// ships silently if you get it wrong:
//
//   * The SDR source SRV is DXGI_FORMAT_B8G8R8A8_UNORM, never _UNORM_SRGB. An
//     _SRGB view applies a second gamma decode and washes the output out.
//   * .rgb is read WITHOUT swizzling. "B8G8R8A8" describes memory layout; the
//     hardware presents .r as red. Hand-swizzling to .bgr produces the red/blue
//     swapped recording.
//   * The BT.709 matrix is applied to non-linear R'G'B', as video requires.
//   * Chroma is a box average over the 2x2 luma quad, never a point sample.
//     Point sampling produces visible chroma fringing on text.
//   * An FP16 source is scRGB (system HDR on) and takes the tone-map branch.
//     Reinterpreting those bits as BGRA8 is the black/neon-green bug.
//
// The HDR path is a *uniform* branch -- every thread in the dispatch takes the same
// side -- so it costs nothing in divergence, and both paths share the same matrix
// and chroma code. Duplicating that tail into two shaders is how the two paths
// would quietly drift apart.

// The tone-map, the sRGB encode and the BT.709 luma weights live in a shared header
// because SPEC.md §15.2's preview needs the identical curve -- see `tone_map.hlsli`.
#include "tone_map.hlsli"

cbuffer ConversionParams : register(b0) {
    uint2 g_luma_size;      // luma plane dimensions in pixels
    uint g_full_range;      // 0 = limited (16-235 / 16-240), 1 = full
    uint g_source_is_scrgb; // 0 = BGRA8 sRGB, 1 = FP16 scRGB (HDR)
    float g_sdr_white_scale; // scRGB value that should map to SDR diffuse white
    float3 g_padding;
};

Texture2D<float4> g_source : register(t0);
RWTexture2D<float> g_luma : register(u0);
RWTexture2D<float2> g_chroma : register(u1);

static const float kCbDenominator = 1.8556f;
static const float kCrDenominator = 1.5748f;

float3 fetch(uint2 position) {
    const float3 raw = g_source[position].rgb;
    return (g_source_is_scrgb != 0u) ? tone_map_scrgb(raw, g_sdr_white_scale) : raw;
}

[numthreads(8, 8, 1)] void main(uint3 thread_id : SV_DispatchThreadID) {
    const uint2 chroma_pos = thread_id.xy;
    const uint2 luma_pos = chroma_pos * 2u;

    if (luma_pos.x >= g_luma_size.x || luma_pos.y >= g_luma_size.y) {
        return;
    }

    // Clamp so an odd-sized source replicates its last row/column rather than
    // reading out of bounds.
    const uint right = min(luma_pos.x + 1u, g_luma_size.x - 1u);
    const uint bottom = min(luma_pos.y + 1u, g_luma_size.y - 1u);

    const float3 c00 = fetch(uint2(luma_pos.x, luma_pos.y));
    const float3 c10 = fetch(uint2(right, luma_pos.y));
    const float3 c01 = fetch(uint2(luma_pos.x, bottom));
    const float3 c11 = fetch(uint2(right, bottom));

    // Scales expressed as UNORM fractions, because the UAVs are R8/R8G8 UNORM.
    float luma_scale;
    float luma_offset;
    float chroma_scale;
    if (g_full_range != 0u) {
        luma_scale = 1.0f;
        luma_offset = 0.0f;
        chroma_scale = 1.0f;
    } else {
        luma_scale = 219.0f / 255.0f;
        luma_offset = 16.0f / 255.0f;
        chroma_scale = 224.0f / 255.0f;
    }

    g_luma[uint2(luma_pos.x, luma_pos.y)] = (luma_of(c00) * luma_scale) + luma_offset;
    if (luma_pos.x + 1u < g_luma_size.x) {
        g_luma[uint2(luma_pos.x + 1u, luma_pos.y)] = (luma_of(c10) * luma_scale) + luma_offset;
    }
    if (luma_pos.y + 1u < g_luma_size.y) {
        g_luma[uint2(luma_pos.x, luma_pos.y + 1u)] = (luma_of(c01) * luma_scale) + luma_offset;
    }
    if (luma_pos.x + 1u < g_luma_size.x && luma_pos.y + 1u < g_luma_size.y) {
        g_luma[uint2(luma_pos.x + 1u, luma_pos.y + 1u)] = (luma_of(c11) * luma_scale) + luma_offset;
    }

    // Box average over the quad. Averaging in R'G'B' before the matrix is
    // equivalent to averaging the resulting Cb/Cr, because the transform is linear
    // in R'G'B' -- and it is one matrix evaluation instead of four.
    const float3 average = (c00 + c10 + c01 + c11) * 0.25f;
    const float average_luma = luma_of(average);

    const float cb = (average.b - average_luma) / kCbDenominator;
    const float cr = (average.r - average_luma) / kCrDenominator;

    g_chroma[chroma_pos] = float2((cb * chroma_scale) + 0.5f, (cr * chroma_scale) + 0.5f);
}
