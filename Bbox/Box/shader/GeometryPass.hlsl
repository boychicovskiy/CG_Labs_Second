//=============================================================================
// GeometryPass.hlsl
//
// Opaque geometry stage of the deferred renderer (lecture 03) extended with
// normal mapping and displacement tessellation (lecture 04).
//
// Two pipelines share the same pixel shader and the same G-Buffer layout:
//
//   plain  : VS -> PS                       (materials without a height map)
//   tessel.: VS_Tess -> HS -> DS -> PS      (stone materials with map_bump)
//
// G-Buffer render targets:
//   SV_Target0  Albedo    R8G8B8A8_UNORM      rgb = diffuse color
//   SV_Target1  Normal    R16G16B16A16_FLOAT  xyz = world-space normal
//   SV_Target2  Specular  R8G8B8A8_UNORM      rgb = Ks, a = Ns / 255
//=============================================================================

Texture2D    gDiffuseMap      : register(t0);
Texture2D    gAlphaMap        : register(t1);   // map_d
Texture2D    gNormalMap       : register(t2);   // generated from the height map
Texture2D    gDisplacementMap : register(t3);   // map_bump (grayscale heightfield)

SamplerState gSampler : register(s0);

cbuffer ObjectCB : register(b0)
{
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
};

cbuffer GeoPassCB : register(b1)
{
    float4x4 gViewProj;

    float3 gEyePosW;
    float  gTessFactorMax;

    float  gTessFactorMin;
    float  gTessDistNear;
    float  gTessDistFar;
    float  gDisplacementScale;

    uint   gNormalMapEnabled;
    uint   gFlipGreenChannel;
    uint   gBackfaceCullHS;
    uint   _passPad0;
};

cbuffer MaterialCB : register(b2)
{
    float4 gDiffuseAlbedo;    // Kd
    float3 gSpecularColor;    // Ks
    float  gSpecPower;        // Ns
    float2 gUvScale;
    float2 gUvOffset;
    uint   gAlphaTest;
    uint   gHasNormalMap;
    uint   gHasDisplacement;
    float  gMatDispScale;
};

#define MAX_TESS_FACTOR 8.0

//-----------------------------------------------------------------------------
// Shared structures
//-----------------------------------------------------------------------------
struct VertexIn
{
    float3 PosL     : POSITION;
    float3 NormalL  : NORMAL;
    float4 TangentL : TANGENT;    // xyz = tangent, w = handedness
    float2 TexCoord : TEXCOORD;
};

// Input of the pixel shader. Produced either by VS or by DS - the layouts
// must stay identical, otherwise one of the two pipelines fails to link.
struct PSInput
{
    float4 PosH     : SV_POSITION;
    float3 NormalW  : NORMAL;
    float4 TangentW : TANGENT;
    float2 TexCoord : TEXCOORD0;
};

//=============================================================================
// PLAIN PIPELINE
//=============================================================================
PSInput VS(VertexIn vin)
{
    PSInput vout;

    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);

    vout.PosH     = mul(posW, gViewProj);
    vout.NormalW  = mul(vin.NormalL, (float3x3) gWorldInvTranspose);
    vout.TangentW = float4(mul(vin.TangentL.xyz, (float3x3) gWorld), vin.TangentL.w);
    vout.TexCoord = vin.TexCoord * gUvScale + gUvOffset;

    return vout;
}

//=============================================================================
// TESSELLATION PIPELINE
//=============================================================================

// Control point: everything stays in world space, projection happens in the DS
// after the vertex has been displaced.
struct TessVertexOut
{
    float3 PosW     : POSITION;
    float3 NormalW  : NORMAL;
    float4 TangentW : TANGENT;
    float2 TexCoord : TEXCOORD0;
};

TessVertexOut VS_Tess(VertexIn vin)
{
    TessVertexOut vout;

    vout.PosW     = mul(float4(vin.PosL, 1.0f), gWorld).xyz;
    vout.NormalW  = mul(vin.NormalL, (float3x3) gWorldInvTranspose);
    vout.TangentW = float4(mul(vin.TangentL.xyz, (float3x3) gWorld), vin.TangentL.w);
    vout.TexCoord = vin.TexCoord * gUvScale + gUvOffset;

    return vout;
}

struct PatchConstants
{
    float Edges[3]  : SV_TessFactor;
    float Inside    : SV_InsideTessFactor;
};

//-----------------------------------------------------------------------------
// Distance adaptive tessellation (lecture 04, slide 54).
//
// The factor of an edge is computed from the midpoint of that edge only.
// Two neighbouring patches share the same two endpoints, so they compute the
// exact same factor and no cracks appear along the seam.
//-----------------------------------------------------------------------------
float EdgeTessFactor(float3 a, float3 b)
{
    float d = distance(0.5f * (a + b), gEyePosW);
    float t = saturate((d - gTessDistNear) / max(gTessDistFar - gTessDistNear, 1e-4f));

    return lerp(gTessFactorMax, gTessFactorMin, t);
}

PatchConstants ConstantsHS(InputPatch<TessVertexOut, 3> patch, uint patchID : SV_PrimitiveID)
{
    PatchConstants o;

    // Back-face culling in the hull shader (slide 70): a patch whose every
    // tessellation factor is zero is discarded before the tessellator runs.
    if (gBackfaceCullHS != 0)
    {
        float3 e0 = patch[1].PosW - patch[0].PosW;
        float3 e2 = patch[2].PosW - patch[0].PosW;

        float3 faceNormal = normalize(cross(e2, e0));
        float3 view       = normalize(patch[0].PosW - gEyePosW);

        // Small epsilon: displaced vertices may still be visible at dot == 0.
        if (dot(view, faceNormal) < -0.25f)
        {
            o.Edges[0] = 0.0f;
            o.Edges[1] = 0.0f;
            o.Edges[2] = 0.0f;
            o.Inside   = 0.0f;
            return o;
        }
    }

    // Edge i sits opposite to control point i.
    o.Edges[0] = EdgeTessFactor(patch[1].PosW, patch[2].PosW);
    o.Edges[1] = EdgeTessFactor(patch[2].PosW, patch[0].PosW);
    o.Edges[2] = EdgeTessFactor(patch[0].PosW, patch[1].PosW);

    o.Inside = (o.Edges[0] + o.Edges[1] + o.Edges[2]) / 3.0f;

    return o;
}

// Pass-through control point phase - the driver recognises this pattern and
// optimises it away (slide 38).
[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("ConstantsHS")]
[maxtessfactor(MAX_TESS_FACTOR)]
TessVertexOut HS(InputPatch<TessVertexOut, 3> patch, uint id : SV_OutputControlPointID)
{
    return patch[id];
}

[domain("tri")]
PSInput DS(PatchConstants input,
           float3 bary : SV_DomainLocation,
           const OutputPatch<TessVertexOut, 3> patch)
{
    PSInput dout;

    // Barycentric interpolation of every attribute
    float3 posW = bary.x * patch[0].PosW     + bary.y * patch[1].PosW     + bary.z * patch[2].PosW;
    float3 nrmW = bary.x * patch[0].NormalW  + bary.y * patch[1].NormalW  + bary.z * patch[2].NormalW;
    float4 tanW = bary.x * patch[0].TangentW + bary.y * patch[1].TangentW + bary.z * patch[2].TangentW;
    float2 uv   = bary.x * patch[0].TexCoord + bary.y * patch[1].TexCoord + bary.z * patch[2].TexCoord;

    nrmW = normalize(nrmW);

    // Displacement along the interpolated normal.
    // SampleLevel, not Sample: there are no screen-space derivatives outside
    // the pixel shader, so the mip level has to be given explicitly.
    if (gHasDisplacement != 0)
    {
        float height = gDisplacementMap.SampleLevel(gSampler, uv, 0).r;

        // Centre around 0.5 so the average surface stays where it was and the
        // silhouette does not visibly inflate.
        posW += nrmW * ((height - 0.5f) * gDisplacementScale * gMatDispScale);
    }

    dout.PosH     = mul(float4(posW, 1.0f), gViewProj);
    dout.NormalW  = nrmW;
    dout.TangentW = tanW;
    dout.TexCoord = uv;

    return dout;
}

//=============================================================================
// SHARED PIXEL SHADER
//=============================================================================

// isFront: the rasterizer runs with CULL_NONE because Sponza's foliage and
// fabrics are single-sided. Back faces arrive with an inverted normal, so we
// flip it here - otherwise those pixels come out unlit.
struct GBufferOut
{
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 Specular : SV_Target2;
};

GBufferOut PS(PSInput pin, bool isFront : SV_IsFrontFace)
{
    // Alpha masking for foliage / chains / plants in Sponza.
    // clip() must run before anything is written, so no [earlydepthstencil].
    if (gAlphaTest != 0)
    {
        float mask = gAlphaMap.Sample(gSampler, pin.TexCoord).r;
        clip(mask - 0.15f);
    }

    float3 texColor = gDiffuseMap.Sample(gSampler, pin.TexCoord).rgb;

    float3 N = normalize(pin.NormalW);
    if (!isFront)
        N = -N;

    //-------------------------------------------------------------------------
    // Normal mapping (lecture 04, slides 4, 10, 11)
    //-------------------------------------------------------------------------
    if (gNormalMapEnabled != 0 && gHasNormalMap != 0)
    {
        // Gram-Schmidt: re-orthogonalise T against the interpolated N (slide 11).
        // Interpolation across the triangle breaks orthogonality of the basis.
        float3 T = normalize(pin.TangentW.xyz - dot(pin.TangentW.xyz, N) * N);
        float3 B = cross(N, T) * pin.TangentW.w;

        // Unpack from [0,1] back to [-1,1] (slide 4)
        float3 nTS = gNormalMap.Sample(gSampler, pin.TexCoord).xyz * 2.0f - 1.0f;

        if (gFlipGreenChannel != 0)
            nTS.y = -nTS.y;

        // Tangent space -> world space (slide 10)
        float3x3 tbn = float3x3(T, B, N);
        N = normalize(mul(nTS, tbn));
    }

    GBufferOut o;

    o.Albedo   = float4(texColor * gDiffuseAlbedo.rgb, 1.0f);
    o.Normal   = float4(N, 0.0f);

    // Ns in .mtl is in [0..1000]; the lab models stay under 255,
    // so a simple /255 fits the UNORM alpha channel.
    o.Specular = float4(gSpecularColor, saturate(gSpecPower / 255.0f));

    return o;
}
