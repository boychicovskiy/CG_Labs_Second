//=============================================================================
// GeometryPass.hlsl
//
// Opaque geometry stage of the deferred renderer (lecture 03, slides 8-9, 18).
// Writes surface properties into the G-Buffer. No lighting is computed here.
//
// Render targets:
//   SV_Target0  Albedo    R8G8B8A8_UNORM      rgb = diffuse color
//   SV_Target1  Normal    R16G16B16A16_FLOAT  xyz = world-space normal
//   SV_Target2  Specular  R8G8B8A8_UNORM      rgb = Ks, a = Ns / 255
//=============================================================================

Texture2D    gDiffuseMap : register(t0);
Texture2D    gAlphaMap   : register(t1);   // map_d from the .mtl file
SamplerState gSampler    : register(s0);

cbuffer ObjectCB : register(b0)
{
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
};

cbuffer GeoPassCB : register(b1)
{
    float4x4 gViewProj;
};

cbuffer MaterialCB : register(b2)
{
    float4 gDiffuseAlbedo;    // Kd
    float3 gSpecularColor;    // Ks
    float  gSpecPower;        // Ns
    float2 gUvScale;          // tiling
    float2 gUvOffset;         // uv animation
    uint   gAlphaTest;        // 1 = sample gAlphaMap and clip
    float3 _matPad;
};

struct VertexIn
{
    float3 PosL     : POSITION;
    float3 NormalL  : NORMAL;
    float4 Color    : COLOR;
    float2 TexCoord : TEXCOORD;
};

struct VertexOut
{
    float4 PosH     : SV_POSITION;
    float3 NormalW  : NORMAL;
    float2 TexCoord : TEXCOORD0;
};

struct GBufferOut
{
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 Specular : SV_Target2;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;

    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosH   = mul(posW, gViewProj);

    vout.NormalW = mul(vin.NormalL, (float3x3) gWorldInvTranspose);

    // Tiling and uv animation carried over from homework #1
    vout.TexCoord = vin.TexCoord * gUvScale + gUvOffset;

    return vout;
}

// isFront: the rasterizer runs with CULL_NONE because Sponza's foliage and
// fabrics are single-sided. Back faces arrive with an inverted normal, so we
// flip it here - otherwise those pixels come out unlit.
GBufferOut PS(VertexOut pin, bool isFront : SV_IsFrontFace)
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

    GBufferOut o;

    o.Albedo   = float4(texColor * gDiffuseAlbedo.rgb, 1.0f);
    o.Normal   = float4(N, 0.0f);

    // Ns in .mtl is in [0..1000]; the lab models stay under 255,
    // so a simple /255 fits the UNORM alpha channel.
    o.Specular = float4(gSpecularColor, saturate(gSpecPower / 255.0f));

    return o;
}
