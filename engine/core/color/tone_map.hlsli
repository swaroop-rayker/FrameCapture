// scRGB -> SDR BT.709 tone mapping (SPEC.md §4.2, §6).
//
// Shared by `bgra_to_nv12.hlsl` (the recording path) and `downscale_preview.hlsl`
// (SPEC.md §15.2's preview). One copy rather than two, because two would be two answers
// to "what does an HDR desktop look like" and the preview's job is to show the user what
// is being recorded. A preview that tone-mapped differently from the encoder would be
// wrong in exactly the way that is hardest to notice: plausible, and not the file.
//
// Parameterised on the white scale rather than reading a constant buffer, so the two
// shaders keep their own `cbuffer` layouts and this file imposes nothing on either.

#ifndef FC_TONE_MAP_HLSLI
#define FC_TONE_MAP_HLSLI

static const float3 kBt709Luma = float3(0.2126f, 0.7152f, 0.0722f);

float luma_of(float3 rgb) {
    return dot(rgb, kBt709Luma);
}

// Linear -> sRGB encoding. The SDR path receives values that are already
// sRGB-encoded, so the tone-mapped HDR path must encode too or the two produce
// different output for the same picture.
// `select` is an HLSL 2021 intrinsic and fxc targets cs_5_0, so the branchless
// choice is spelled with step/lerp.
float3 encode_srgb(float3 linear_rgb) {
    const float3 low = linear_rgb * 12.92f;
    const float3 high = (1.055f * pow(max(linear_rgb, 1e-8f), 1.0f / 2.4f)) - 0.055f;
    const float3 use_high = step(0.0031308f, linear_rgb);
    return lerp(low, high, use_high);
}

// scRGB -> SDR BT.709, per SPEC.md §4.2's tone-map requirement.
//
// scRGB is linear with BT.709 primaries where 1.0 is the 80-nit SDR reference.
// Windows composites the HDR desktop with SDR content at a higher level, so the
// value that should read as diffuse white is `sdr_white_scale`, defaulting to
// 203 nits / 80 nits per ITU-R BT.2408.
//
// Highlights above that are rolled off with extended Reinhard rather than clipped:
// clipping turns every specular highlight into a flat white blob, which is the
// most visible artifact of a naive HDR downconvert.
float3 tone_map_scrgb(float3 scrgb, float sdr_white_scale) {
    // Negative components are outside BT.709 and cannot be shown on an SDR
    // display. Clamping is the honest choice; letting them through produces
    // out-of-range luma that wraps.
    float3 linear_rgb = max(scrgb, 0.0f);

    // After this, 1.0 is diffuse white.
    linear_rgb /= max(sdr_white_scale, 1e-4f);

    // Soft shoulder applied to luminance, with the triple scaled uniformly so hue
    // is preserved -- a per-channel curve desaturates bright colours badly.
    //
    // Below the knee the curve is the identity, which matters more than it looks:
    // almost all of an HDR desktop is ordinary SDR content composited at diffuse
    // white, and it must come through *unchanged*. Only genuine highlights are
    // compressed, and tanh rolls them into the top of the range asymptotically
    // rather than clipping them into flat white blobs.
    //
    // tanh'(0) == 1, so the curve is C1-continuous at the knee and there is no
    // visible banding where it engages.
    const float kKnee = 0.8f;
    const float luminance = max(luma_of(linear_rgb), 1e-6f);
    if (luminance > kKnee) {
        const float shoulder = kKnee + ((1.0f - kKnee) * tanh((luminance - kKnee) / (1.0f - kKnee)));
        linear_rgb *= shoulder / luminance;
    }

    return encode_srgb(saturate(linear_rgb));
}

#endif // FC_TONE_MAP_HLSLI
