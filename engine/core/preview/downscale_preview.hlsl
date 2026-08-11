// Source -> 960x540 BGRA preview (SPEC.md §15.2).
//
//   > Preview generation is a separate GPU shader dispatch off the same source texture,
//   > adding < 0.2 ms/frame.
//
// Three things about this shader are decisions rather than defaults:
//
//   * **It box-filters, it does not point-sample.** A point-sampled half-scale of a
//     desktop is unreadable: text stems are one pixel wide at 1080p, so dropping every
//     other column deletes half of every glyph. The filter is what makes the preview a
//     preview rather than a thumbnail with aliasing.
//   * **It writes BGRA byte order through an RGBA8 UAV.** §15.2 specifies BGRA and
//     `DXGI_FORMAT_B8G8R8A8_UNORM` is not in D3D11's guaranteed typed-UAV-store set, so
//     the store is swizzled instead: `float4(b, g, r, 1)` into an R8G8B8A8_UNORM surface
//     lays down B, G, R, A in memory, which is what the GUI's `QImage::Format_RGB32`
//     expects. The alternative -- swizzling on the CPU during the readback -- would put a
//     per-pixel loop on the one path that has to stay a `memcpy`.
//   * **The HDR branch is the encoder's, verbatim.** `tone_map.hlsli` is shared with
//     `bgra_to_nv12.hlsl` precisely so a user with system HDR on sees the picture that is
//     going into the file rather than a second interpretation of it.

#include "../color/tone_map.hlsli"

cbuffer PreviewParams : register(b0) {
    uint2 g_destination_size;   // 960 x 540
    float2 g_source_per_dest;   // source pixels covered by one destination pixel
    float2 g_inverse_source;    // 1 / source size, for normalised sampling
    uint2 g_taps;               // bilinear taps per axis; each covers 2 source pixels
    uint g_source_is_scrgb;     // 0 = BGRA8 sRGB, 1 = FP16 scRGB (HDR)
    float g_sdr_white_scale;    // scRGB value that should map to SDR diffuse white
};

Texture2D<float4> g_source : register(t0);
SamplerState g_bilinear : register(s0);
RWTexture2D<float4> g_destination : register(u0);

float3 tap_at(float2 box_origin, float2 step_size, float2 offset) {
    const float2 position = box_origin + (step_size * (offset + 0.5f));
    return g_source.SampleLevel(g_bilinear, position * g_inverse_source, 0.0f).rgb;
}

[numthreads(8, 8, 1)] void main(uint3 thread_id : SV_DispatchThreadID) {
    if (thread_id.x >= g_destination_size.x || thread_id.y >= g_destination_size.y) {
        return;
    }

    // The source rectangle this destination pixel covers, in source pixels.
    const float2 box_origin = float2(thread_id.xy) * g_source_per_dest;

    // Each bilinear tap averages a 2x2 source neighbourhood for free, so `g_taps` taps
    // per axis cover a 2*taps-wide box. The taps are spread evenly across the box and
    // sampled at its sub-cell centres, which is a box filter evaluated at half the cost.
    const float2 taps = float2(g_taps);
    const float2 step_size = g_source_per_dest / taps;

    float3 accumulated = float3(0.0f, 0.0f, 0.0f);

    // ---------------------------------------------------------------------------
    // Why the HDR test is a `[branch]` and not the ternary it used to be -- and how
    // little that turned out to be worth
    // ---------------------------------------------------------------------------
    // fxc *flattens* `cond ? tone_map(x) : x`: it evaluates both sides and selects with a
    // `movc`. Confirmed on the disassembly -- the SDR path was paying, per tap, two `exp`
    // for the tanh shoulder, three `log` and three `exp` for the sRGB encode, and four
    // `div`, for a value it then discarded. `g_source_is_scrgb` is uniform across the
    // dispatch (it comes from the source texture's format), so a real branch costs nothing
    // in divergence and skips the block entirely. Same for the tap count, which is 1 for
    // the 1080p-to-960x540 case that is the whole default configuration.
    //
    // **This was found while chasing an overage it did not cause, and the measurement is
    // recorded so nobody re-derives the wrong conclusion.** The dispatch first measured
    // median 0.237 ms against §15.2's 0.2 ms; the flattened tone-map was the obvious
    // suspect and was wrong. Measured back to back on the Radeon 780M, the two forms are
    // **0.171 ms flattened against 0.165 ms branched** -- about 3%. The real cause was the
    // *test*, which synchronised the GPU after every dispatch and so measured a part
    // repeatedly waking up (see `TheDispatchStaysInsideTheBudgetFifteenTwoStates`).
    //
    // The branch stays because it is a real if small win, it is what the code should have
    // said in the first place, and the HDR path -- where the tone-map runs inside a 4-tap
    // loop -- is where it would actually matter. It is not a fix for anything.
    [branch] if (g_source_is_scrgb != 0u) {
        // Tone-mapped per tap rather than after the average: the curve is non-linear above
        // the knee, so averaging first would let one specular highlight drag a whole
        // destination pixel through the shoulder.
        [loop] for (uint y = 0u; y < g_taps.y; ++y) {
            [loop] for (uint x = 0u; x < g_taps.x; ++x) {
                accumulated += tone_map_scrgb(tap_at(box_origin, step_size, float2(x, y)), g_sdr_white_scale);
            }
        }
        accumulated /= max(taps.x * taps.y, 1.0f);
    } else [branch] if (g_taps.x == 1u && g_taps.y == 1u) {
        accumulated = tap_at(box_origin, step_size, float2(0.0f, 0.0f));
    } else {
        [loop] for (uint sy = 0u; sy < g_taps.y; ++sy) {
            [loop] for (uint sx = 0u; sx < g_taps.x; ++sx) {
                accumulated += tap_at(box_origin, step_size, float2(sx, sy));
            }
        }
        accumulated /= max(taps.x * taps.y, 1.0f);
    }

    // B, G, R, A in memory -- see the header note.
    g_destination[thread_id.xy] = float4(accumulated.b, accumulated.g, accumulated.r, 1.0f);
}
