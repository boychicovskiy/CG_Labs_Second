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
//
// Lab 8 (lecture 09) replaced the Phong shading with the Cook-Torrance BRDF:
// a microfacet model built from three terms - normal distribution D, geometry
// G and Fresnel F (slide 22) - plus the energy conservation rule kD = 1 - kS
// (slide 10) and the metallic workflow (slide 33).
//=============================================================================

Texture2D<float4>      gAlbedo    : register(t0);
Texture2D<float4>      gNormal    : register(t1);
Texture2D<float4>      gMaterial  : register(t2);   // r metallic, g roughness, b AO
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
    uint   gIblEnabled;    // lab 8: analytic environment instead of flat ambient

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

#define PI 3.14159265359f

//=============================================================================
// Cook-Torrance BRDF (lecture 09)
//=============================================================================

//-----------------------------------------------------------------------------
// Trowbridge-Reitz GGX normal distribution (slides 23, 24).
//
// D approximates the fraction of microfacets whose own normal points along the
// halfway vector H - those are exactly the mirrors that send light from L into
// V. Disney's reparametrisation alpha = roughness^2 is used, which makes the
// artist-facing roughness slider perceptually linear.
//-----------------------------------------------------------------------------
float DistributionGGX(float NdotH, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;

    float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;

    // The denominator goes to zero for a mirror (a2 -> 0) viewed exactly along
    // H, so it is clamped - otherwise single bright pixels ("fireflies") appear.
    return a2 / max(PI * d * d, 1e-7f);
}

//-----------------------------------------------------------------------------
// Schlick-GGX geometry term with Smith's method (slides 25, 26, 27).
//
// G accounts for microfacets shadowing and masking each other. Smith splits it
// into two independent factors: one for the view direction (obstruction) and
// one for the light direction (shadowing).
//
// k for direct lighting is (roughness + 1)^2 / 8 (slide 25). The IBL variant
// uses roughness^2 / 2 instead - that is the second formula on the same slide.
//-----------------------------------------------------------------------------
float GeometrySchlickGGX(float NdotX, float k)
{
    return NdotX / max(NdotX * (1.0f - k) + k, 1e-7f);
}

float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;

    return GeometrySchlickGGX(NdotV, k) * GeometrySchlickGGX(NdotL, k);
}

//-----------------------------------------------------------------------------
// Fresnel-Schlick approximation (slides 28, 29).
//
// F is the share of light reflected rather than refracted, and it grows towards
// grazing angles - at 90 degrees every surface is a mirror. F0 is the base
// reflectivity measured head-on.
//-----------------------------------------------------------------------------
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// Ambient light has no halfway vector, so N is substituted for H. That makes
// the Fresnel effect too strong on rough surfaces, hence the roughness-aware
// variant from slide 55.
float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    float3 fr = max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), F0);
    return F0 + (fr - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

//=============================================================================
// Analytic environment (a stand-in for IBL, slides 53-56 and 80)
//
// A full IBL pipeline needs three baked textures: an irradiance cube map, a
// prefiltered environment cube map and a BRDF integration LUT. None of them
// exist in this project yet, so the environment is evaluated analytically:
// a two-lobe sky gradient plus Karis' analytic fit of the BRDF LUT. The
// structure of the shading code is identical to slide 80 - only the three
// texture fetches are replaced by closed-form functions.
//=============================================================================

// Radiance arriving from direction dir. gAmbientColor is the zenith color and
// sets the overall level; everything else is derived from it so that one slider
// controls the whole environment.
//
// The horizon step is deliberately sharp: with a smooth gradient every metal
// surface reflects almost the same value and the whole object field collapses
// into flat pastel rectangles. A visible horizon gives metals something to show.
float3 SkyRadiance(float3 dir)
{
    float3 zenith  = gAmbientColor.rgb;
    float3 horizon = gAmbientColor.rgb * 1.35f + 0.02f;
    float3 ground  = gAmbientColor.rgb * 0.22f;

    float3 above = lerp(horizon, zenith, saturate(dir.y));
    return lerp(ground, above, saturate(dir.y * 6.0f + 0.5f));
}

// Cosine-weighted convolution of that sky over the hemisphere around N.
// For a gradient this smooth the integral collapses into another gradient,
// which is what an irradiance map would have stored (slide 52).
//
// The up/down contrast is what makes ambient look like light and not like fog:
// a surface facing the sky gets roughly five times what a downward-facing one
// gets. A flat value here washes out every shape in the scene.
float3 SkyIrradiance(float3 N)
{
    float3 sky    = gAmbientColor.rgb * 1.15f;
    float3 ground = gAmbientColor.rgb * 0.20f;

    return lerp(ground, sky, N.y * 0.5f + 0.5f);
}

// Prefiltered environment map stand-in: a rough surface reflects a wide lobe,
// so it converges to the irradiance gradient; a smooth one reflects the sky
// itself. Blurring by roughness is exactly what the mip chain of slide 59 does.
float3 PrefilteredSky(float3 R, float roughness)
{
    return lerp(SkyRadiance(R), SkyIrradiance(R), roughness);
}

// Analytic replacement for the BRDF integration map of slides 69-79.
// Karis' mobile approximation, accurate to a fraction of a percent.
float2 EnvBRDFApprox(float NdotV, float roughness)
{
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f,  0.022f);
    const float4 c1 = float4( 1.0f,  0.0425f,  1.040f, -0.040f);

    float4 r = roughness * c0 + c1;
    float  a = min(r.x * r.x, exp2(-9.28f * NdotV)) * r.x + r.y;

    return float2(-1.04f, 1.04f) * a + r.zw;
}

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
    float3 N      = normalize(gNormal.Load(pixel).xyz);

    float4 matData   = gMaterial.Load(pixel);
    float  metallic  = saturate(matData.r);
    float  roughness = clamp(matData.g, 0.05f, 1.0f);
    float  ao        = saturate(matData.b);

    float3 posW = ReconstructWorldPos(posH.xy, depth);
    float3 V    = normalize(gEyePosW - posW);
    float  NdotV = saturate(dot(N, V));

    // Base reflectivity (slide 33). Dielectrics reflect about 4% head-on and
    // keep their diffuse colour; metals have no diffuse at all (slide 9) and
    // their F0 is the albedo itself, tinted (slide 34).
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    //-------------------------------------------------------------------------
    // Ambient / IBL pass. Added once per frame, not once per light.
    //-------------------------------------------------------------------------
    if (gLightType == LIGHT_AMBIENT)
    {
        float3 ambient;

        if (gIblEnabled != 0)
        {
            // Slide 80, with the three texture fetches replaced by the
            // analytic sky above.
            float3 F  = FresnelSchlickRoughness(NdotV, F0, roughness);
            float3 kS = F;
            float3 kD = (1.0f - kS) * (1.0f - metallic);

            float3 irradiance = SkyIrradiance(N);
            float3 diffuse    = irradiance * albedo;

            float3 R          = reflect(-V, N);
            float3 prefiltered = PrefilteredSky(R, roughness);

            float2 envBrdf = EnvBRDFApprox(NdotV, roughness);
            float3 specular = prefiltered * (F * envBrdf.x + envBrdf.y);

            ambient = (kD * diffuse + specular) * ao;
        }
        else
        {
            // The flat constant this replaces (slide 53), kept for comparison.
            ambient = 0.03f * albedo * ao;
        }

        return float4(ambient * gIntensity, 1.0f);
    }

    //-------------------------------------------------------------------------
    // Direct light: one term of the sum on slide 17.
    //-------------------------------------------------------------------------
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

    float3 H     = normalize(V + L);
    float  NdotL = saturate(dot(N, L));
    float  NdotH = saturate(dot(N, H));
    float  HdotV = saturate(dot(H, V));

    // Li in the reflectance equation.
    float3 radiance = gLightColor * gIntensity * attenuation;

    // Only the directional light casts shadows here: cascades are built for an
    // orthographic projection. A point light would need a cube map and a spot
    // light its own perspective map (lecture 06, slide 13).
    if (gShadowsEnabled != 0 && gLightType == LIGHT_DIRECTIONAL)
    {
        uint cascade = SelectCascade(posW);
        radiance *= SampleShadow(posW, cascade);

        if (gShowCascades != 0)
            radiance *= CascadeColor(cascade);
    }

    //-------------------------------------------------------------------------
    // Cook-Torrance specular: D * G * F / (4 * (N.V) * (N.L))   (slides 21, 35)
    //-------------------------------------------------------------------------
    float  D = DistributionGGX(NdotH, roughness);
    float  G = GeometrySmith(NdotV, NdotL, roughness);
    float3 F = FresnelSchlick(HdotV, F0);

    float3 specular = (D * G * F) / max(4.0f * NdotV * NdotL, 1e-4f);

    // Energy conservation (slide 10): F already IS the reflected fraction kS,
    // so the refracted fraction is what is left of it. Metals absorb all of
    // the refracted light, so their diffuse term is zeroed out (slide 9).
    float3 kD = (1.0f - F) * (1.0f - metallic);

    // Lambert diffuse is divided by PI (slide 20) - without it the surface
    // would emit more energy than it receives.
    float3 Lo = (kD * albedo / PI + specular) * radiance * NdotL;

    return float4(Lo, 1.0f);
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

    // Material target as it is stored: red = metallic, green = roughness.
    if (gDebugMode == 3)
        return float4(gMaterial.Load(pixel).rgb, 1.0f);

    if (gDebugMode == 4)
    {
        float r = gMaterial.Load(pixel).g;
        return float4(r, r, r, 1.0f);
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
