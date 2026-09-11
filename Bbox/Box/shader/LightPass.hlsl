//=============================================================================
// LightPass.hlsl
//
// Light stage of the deferred renderer (lecture 03, slides 11, 13, 20-23).
//
// One fullscreen triangle is drawn per light with additive blending, so the
// back buffer accumulates the contribution of every light:
//     Final = Src * ONE + Dest * ONE
//
// G-Buffer is read with Texture2D.Load(int3), which needs no sampler and no
// interpolated texture coordinates - SV_POSITION already gives pixel coords.
//
// World position is NOT stored in the G-Buffer. It is reconstructed from the
// depth buffer and the inverse view-projection matrix (slide 17).
//=============================================================================

Texture2D<float4>      gAlbedo    : register(t0);
Texture2D<float4>      gNormal    : register(t1);
Texture2D<float4>      gSpecular  : register(t2);
Texture2D<float>       gDepth     : register(t3);
Texture2DArray<float>  gShadowMap : register(t4);   // one slice per cascade

// Comparison sampler (lecture 06, slide 40): SampleCmp compares the value
// passed from the shader against every fetched texel and blends the 0/1
// results, which gives hardware 2x2 PCF for free.
SamplerComparisonState gShadowSampler : register(s0);

cbuffer LightPassCB : register(b0)
{
    float4x4 gInvViewProj;

    float3 gEyePosW;
    float  _lpPad0;

    float2 gInvScreen;     // 1/width, 1/height
    uint   gDebugMode;     // 0 = off, 1..5 = show one G-Buffer channel
    float  _lpPad1;

    float4 gAmbientColor;

    // cascaded shadow maps
    float4x4 gView;
    float4x4 gCascadeViewProj[4];
    float4   gCascadeSplits;      // far plane of each cascade, in view space

    uint  gShadowsEnabled;
    uint  gShowCascades;
    float gShadowBias;
    float gShadowTexelSize;
};

cbuffer LightCB : register(b1)
{
    float3 gLightColor;
    float  gIntensity;

    float3 gLightPosW;     // point / spot
    float  gRange;         // point / spot

    float3 gLightDirW;     // directional / spot
    float  gCosOuter;

    uint   gLightType;     // 0 = directional, 1 = point, 2 = spot, 3 = ambient
    float  gCosInner;
    float2 _lightPad;
};

#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT       1
#define LIGHT_SPOT        2
#define LIGHT_AMBIENT     3

//-----------------------------------------------------------------------------
// Fullscreen triangle generated from SV_VertexID (lecture 03, slide 23).
// Three vertices, no vertex buffer, no input layout:
//   id = 0 -> uv (0,0) -> ndc (-1, +1)
//   id = 1 -> uv (2,0) -> ndc (+3, +1)
//   id = 2 -> uv (0,2) -> ndc (-1, -3)
// The triangle covers the whole NDC square with a single primitive, which is
// cheaper than two triangles because there is no diagonal seam.
//-----------------------------------------------------------------------------
float4 VS(uint id : SV_VertexID) : SV_POSITION
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

//-----------------------------------------------------------------------------
// Rebuild world position from the depth buffer.
//-----------------------------------------------------------------------------
float3 ReconstructWorldPos(float2 pixelXY, float depth)
{
    // pixel coords -> normalized device coords
    float2 ndc = pixelXY * gInvScreen * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);

    float4 clipPos  = float4(ndc, depth, 1.0f);
    float4 worldPos = mul(clipPos, gInvViewProj);

    return worldPos.xyz / worldPos.w;
}

//-----------------------------------------------------------------------------
// Cascade selection (lecture 06, slide 31).
//
// The split distances are stored in view space, so the pixel depth has to be
// taken in view space too - not the [0..1] value from the depth buffer.
//-----------------------------------------------------------------------------
uint SelectCascade(float3 posW)
{
    float viewZ = mul(float4(posW, 1.0f), gView).z;

    uint cascade = 0;
    if (viewZ > gCascadeSplits.x) cascade = 1;
    if (viewZ > gCascadeSplits.y) cascade = 2;
    if (viewZ > gCascadeSplits.z) cascade = 3;

    return cascade;
}

//-----------------------------------------------------------------------------
// Percentage Closer Filtering (slide 39): the depth COMPARISONS are averaged,
// not the depths themselves. Averaging depths first and comparing once would
// just move the hard edge, not soften it.
//
// 3x3 taps of SampleCmpLevelZero, and each tap is already bilinear 2x2 thanks
// to the comparison sampler - so this is effectively a 4x4 kernel.
//-----------------------------------------------------------------------------
// Single exit point on purpose: with an early "return" inside the bounds check
// fxc's flow analysis emits X4000 "potentially uninitialized variable", even
// though every path does return a value.
float SampleShadow(float3 posW, uint cascade)
{
    // Default is "lit": that is also the answer outside the cascade
    float shadow = 1.0f;

    float4 lightClip = mul(float4(posW, 1.0f), gCascadeViewProj[cascade]);

    // Orthographic projection, so w is always 1 - divide anyway for generality
    float3 ndc = lightClip.xyz / lightClip.w;

    // NDC -> shadow map UV
    float2 uv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;

    bool inside = (uv.x >= 0.0f) && (uv.x <= 1.0f) &&
                  (uv.y >= 0.0f) && (uv.y <= 1.0f) &&
                  (ndc.z <= 1.0f);

    if (inside)
    {
        float compareDepth = ndc.z - gShadowBias;
        float sum = 0.0f;

        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            [unroll]
            for (int x = -1; x <= 1; ++x)
            {
                float2 offset = float2(x, y) * gShadowTexelSize;
                sum += gShadowMap.SampleCmpLevelZero(
                    gShadowSampler,
                    float3(uv + offset, cascade),
                    compareDepth);
            }
        }

        shadow = sum / 9.0f;
    }

    return shadow;
}

// Tint used by the cascade debug view. Single exit point, same reason as above.
float3 CascadeColor(uint cascade)
{
    float3 color = float3(1.0f, 1.0f, 0.4f);            // cascade 3

    if      (cascade == 0) color = float3(1.0f, 0.4f, 0.4f);
    else if (cascade == 1) color = float3(0.4f, 1.0f, 0.4f);
    else if (cascade == 2) color = float3(0.4f, 0.6f, 1.0f);

    return color;
}

float4 PS(float4 posH : SV_POSITION) : SV_Target
{
    int3 pixel = int3(posH.xy, 0);

    float depth = gDepth.Load(pixel);

    // Nothing was drawn here - leave the cleared back buffer untouched.
    if (depth >= 1.0f)
        discard;

    float3 albedo = gAlbedo.Load(pixel).rgb;

    // Ambient is a separate light source (slide 13): it must be added once,
    // not once per light, otherwise the scene washes out.
    if (gLightType == LIGHT_AMBIENT)
        return float4(albedo * gAmbientColor.rgb * gIntensity, 1.0f);

    float3 N = normalize(gNormal.Load(pixel).xyz);

    float4 specData  = gSpecular.Load(pixel);
    float3 specColor = specData.rgb;
    float  specPower = max(specData.a * 255.0f, 1.0f);

    float3 posW = ReconstructWorldPos(posH.xy, depth);

    float3 L           = float3(0.0f, 1.0f, 0.0f);
    float  attenuation = 1.0f;

    if (gLightType == LIGHT_DIRECTIONAL)
    {
        L = normalize(-gLightDirW);
    }
    else
    {
        float3 toLight  = gLightPosW - posW;
        float  distance = length(toLight);

        // Outside the light volume - the rasterizer did not cull it for us
        // because we draw a fullscreen triangle, so cull it here.
        if (distance > gRange)
            discard;

        L = toLight / max(distance, 1e-4f);

        // Smooth quadratic falloff that reaches exactly zero at gRange.
        float falloff = saturate(1.0f - distance / gRange);
        attenuation   = falloff * falloff;

        if (gLightType == LIGHT_SPOT)
        {
            float cosAngle = dot(-L, normalize(gLightDirW));
            float cone     = saturate((cosAngle - gCosOuter) / max(gCosInner - gCosOuter, 1e-4f));
            attenuation   *= cone * cone;
        }
    }

    float  nDotL   = saturate(dot(N, L));
    float3 radiance = gLightColor * gIntensity * attenuation;

    // Only the directional light casts shadows here: cascades are built for an
    // orthographic projection. A point light would need a cube map and a spot
    // light its own perspective map (slide 13).
    if (gShadowsEnabled != 0 && gLightType == LIGHT_DIRECTIONAL)
    {
        uint cascade = SelectCascade(posW);
        radiance *= SampleShadow(posW, cascade);

        if (gShowCascades != 0)
            radiance *= CascadeColor(cascade);
    }

    float3 diffuse = albedo * radiance * nDotL;

    // Phong specular. Zero it out on unlit faces, otherwise highlights show up
    // on surfaces that face away from the light.
    float3 V    = normalize(gEyePosW - posW);
    float3 R    = reflect(-L, N);
    float  spec = (nDotL > 0.0f) ? pow(saturate(dot(R, V)), specPower) : 0.0f;

    float3 specular = specColor * radiance * spec;

    return float4(diffuse + specular, 1.0f);
}

//-----------------------------------------------------------------------------
// Debug view: dump a single G-Buffer channel to the screen.
// Drawn with one fullscreen triangle and blending disabled.
//-----------------------------------------------------------------------------
float4 PS_Debug(float4 posH : SV_POSITION) : SV_Target
{
    int3 pixel = int3(posH.xy, 0);

    if (gDebugMode == 1)
        return float4(gAlbedo.Load(pixel).rgb, 1.0f);

    if (gDebugMode == 2)
    {
        float3 n = normalize(gNormal.Load(pixel).xyz);
        return float4(n * 0.5f + 0.5f, 1.0f);   // [-1..1] -> [0..1]
    }

    if (gDebugMode == 3)
        return float4(gSpecular.Load(pixel).rgb, 1.0f);

    if (gDebugMode == 4)
    {
        float ns = gSpecular.Load(pixel).a;
        return float4(ns, ns, ns, 1.0f);
    }

    if (gDebugMode == 5)
    {
        // Depth is heavily non-linear; a power curve makes it readable.
        float d = gDepth.Load(pixel);
        float v = pow(saturate(d), 64.0f);
        return float4(v, v, v, 1.0f);
    }

    return float4(0.0f, 0.0f, 0.0f, 1.0f);
}
