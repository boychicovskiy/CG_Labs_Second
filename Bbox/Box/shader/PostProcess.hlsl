//=============================================================================
// PostProcess.hlsl
//
// Post-processing chain of lab 7 (lectures 08.1 and 08.2).
//
//   PS_BrightPass - keeps only the pixels brighter than a threshold
//   PS_Blur       - one axis of a separable gaussian blur
//   PS_Composite  - exposure, tone mapping, bloom, vignette, chromatic
//                   aberration and dithering; writes to the back buffer
//
// Everything up to the composite lives in LINEAR space. The back buffer RTV
// uses an _SRGB format, so the shader outputs linear values and the output
// merger performs the gamma encoding in hardware (lecture 08.1, slide 43).
//=============================================================================

Texture2D    gSource : register(t0);   // HDR, or the bloom buffer being blurred
Texture2D    gBloom  : register(t1);   // only used by the composite pass
SamplerState gLinearClamp : register(s0);

cbuffer PostCB : register(b0)
{
    float2 gTexelSize;       // 1/width, 1/height of the SOURCE texture
    float2 gBlurDirection;   // (1,0) horizontal, (0,1) vertical

    float gExposure;
    float gBloomThreshold;
    float gBloomIntensity;
    float gVignetteStrength;

    float gChromaticAberration;
    uint  gToneMapMode;      // 0 off, 1 Reinhard, 2 exposure based
    uint  gDitherEnabled;
    uint  gPassthrough;

    float2 gScreenSize;
    float2 _postPad;
};

// Rec. 709 luminance weights (lecture 08.2, slide 9)
static const float3 kLuminance = float3(0.2126f, 0.7152f, 0.0722f);

//-----------------------------------------------------------------------------
// Fullscreen triangle, same trick as the light pass.
//-----------------------------------------------------------------------------
struct VSOut
{
    float4 PosH : SV_POSITION;
    float2 Uv   : TEXCOORD0;
};

VSOut VS(uint id : SV_VertexID)
{
    VSOut o;
    o.Uv   = float2((id << 1) & 2, id & 2);
    o.PosH = float4(o.Uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

//=============================================================================
// Bright pass (lecture 08.2, slide 15)
//=============================================================================
float4 PS_BrightPass(VSOut pin) : SV_Target
{
    float3 hdr = gSource.SampleLevel(gLinearClamp, pin.Uv, 0).rgb;

    float luminance = dot(hdr, kLuminance);

    // Soft knee instead of a hard cut: a hard threshold makes the bloom
    // flicker as pixels cross it from frame to frame.
    float contribution = saturate((luminance - gBloomThreshold) /
                                  max(gBloomThreshold, 1e-4f));

    return float4(hdr * contribution, 1.0f);
}

//=============================================================================
// Separable gaussian blur (lecture 08.2, slides 18-20)
//
// A 2D gaussian kernel is separable, so an NxN convolution becomes two 1D
// passes: N*N texture reads collapse into 2*N.
//=============================================================================
static const float kWeights[4] = { 0.2270270270f, 0.1945945946f, 0.1216216216f, 0.0540540541f };

float4 PS_Blur(VSOut pin) : SV_Target
{
    float2 step = gTexelSize * gBlurDirection;

    float3 color = gSource.SampleLevel(gLinearClamp, pin.Uv, 0).rgb * kWeights[0];

    [unroll]
    for (int i = 1; i < 4; ++i)
    {
        float2 offset = step * float(i);
        color += gSource.SampleLevel(gLinearClamp, pin.Uv + offset, 0).rgb * kWeights[i];
        color += gSource.SampleLevel(gLinearClamp, pin.Uv - offset, 0).rgb * kWeights[i];
    }

    return float4(color, 1.0f);
}

//=============================================================================
// Tone mapping (lecture 08.2, slides 6-7)
//=============================================================================
float3 ToneMapReinhard(float3 hdr)
{
    // Maps [0, inf) into [0, 1) - never clips, but washes out the highlights
    return hdr / (hdr + 1.0f);
}

float3 ToneMapExposure(float3 hdr, float exposure)
{
    return 1.0f - exp(-hdr * exposure);
}

//-----------------------------------------------------------------------------
// sRGB transfer functions (lecture 08.1, slide 30). Needed only for dithering:
// the quantisation step we are trying to hide lives in the encoded space, not
// in the linear one.
//-----------------------------------------------------------------------------
float3 LinearToSrgb(float3 c)
{
    c = saturate(c);
    return (c <= 0.0031308f) ? (c * 12.92f)
                             : (1.055f * pow(c, 1.0f / 2.4f) - 0.055f);
}

float3 SrgbToLinear(float3 c)
{
    c = saturate(c);
    return (c <= 0.04045f) ? (c / 12.92f)
                           : pow((c + 0.055f) / 1.055f, 2.4f);
}

//-----------------------------------------------------------------------------
// Ordered dithering, 4x4 Bayer matrix (lecture 08.1, slides 56-59).
//
// Applied in sRGB space and converted back, because the hardware sRGB encode
// on the render target will redo that conversion. Without the round trip the
// noise amplitude would not match one least significant bit of the output.
//-----------------------------------------------------------------------------
static const float kBayer4x4[16] = {
     0.0f / 16.0f,  8.0f / 16.0f,  2.0f / 16.0f, 10.0f / 16.0f,
    12.0f / 16.0f,  4.0f / 16.0f, 14.0f / 16.0f,  6.0f / 16.0f,
     3.0f / 16.0f, 11.0f / 16.0f,  1.0f / 16.0f,  9.0f / 16.0f,
    15.0f / 16.0f,  7.0f / 16.0f, 13.0f / 16.0f,  5.0f / 16.0f
};

float3 ApplyDither(float3 linearColor, float2 pixelPos)
{
    uint x = uint(pixelPos.x) & 3u;
    uint y = uint(pixelPos.y) & 3u;

    float threshold = kBayer4x4[y * 4u + x] - 0.5f;

    float3 encoded = LinearToSrgb(linearColor);
    encoded += threshold / 255.0f;       // half a least significant bit

    return SrgbToLinear(encoded);
}

//=============================================================================
// Composite
//=============================================================================
float4 PS_Composite(VSOut pin) : SV_Target
{
    // Debug views of the G-Buffer must be shown exactly as they are
    if (gPassthrough != 0)
        return float4(gSource.SampleLevel(gLinearClamp, pin.Uv, 0).rgb, 1.0f);

    float2 uv = pin.Uv;

    // Vector from the centre - drives both vignette and aberration
    float2 fromCenter = uv - 0.5f;
    float  dist       = length(fromCenter);

    //-------------------------------------------------------------------------
    // Chromatic aberration (slide 31): each channel is read at a slightly
    // different distance from the centre, so the fringing grows towards the
    // edges and is invisible in the middle.
    //-------------------------------------------------------------------------
    float3 hdr;

    if (gChromaticAberration > 0.0f)
    {
        float2 offset = fromCenter * gChromaticAberration * dist;

        hdr.r = gSource.SampleLevel(gLinearClamp, uv + offset, 0).r;
        hdr.g = gSource.SampleLevel(gLinearClamp, uv,          0).g;
        hdr.b = gSource.SampleLevel(gLinearClamp, uv - offset, 0).b;
    }
    else
    {
        hdr = gSource.SampleLevel(gLinearClamp, uv, 0).rgb;
    }

    //-------------------------------------------------------------------------
    // Bloom is added BEFORE tone mapping: it is light leaking inside the
    // camera, so it has to go through the same response curve as the rest of
    // the image (slide 15).
    //-------------------------------------------------------------------------
    float3 bloom = gBloom.SampleLevel(gLinearClamp, uv, 0).rgb;
    hdr += bloom * gBloomIntensity;

    //-------------------------------------------------------------------------
    // Exposure and tone mapping (slides 6-7)
    //-------------------------------------------------------------------------
    float3 ldr;

    if (gToneMapMode == 1)
    {
        ldr = ToneMapReinhard(hdr * gExposure);
    }
    else if (gToneMapMode == 2)
    {
        ldr = ToneMapExposure(hdr, gExposure);
    }
    else
    {
        // No tone mapping at all - everything above 1.0 is clipped, which is
        // exactly the saturation problem the lecture warns about.
        ldr = saturate(hdr * gExposure);
    }

    //-------------------------------------------------------------------------
    // Vignette (slide 29)
    //-------------------------------------------------------------------------
    if (gVignetteStrength > 0.0f)
    {
        float vignette = smoothstep(0.85f, 0.25f, dist);
        ldr *= lerp(1.0f, vignette, gVignetteStrength);
    }

    //-------------------------------------------------------------------------
    // Dithering against colour banding (lecture 08.1, slides 52-59)
    //-------------------------------------------------------------------------
    if (gDitherEnabled != 0)
        ldr = ApplyDither(ldr, pin.PosH.xy);

    // Linear out: the _SRGB render target format encodes it in hardware
    return float4(ldr, 1.0f);
}
