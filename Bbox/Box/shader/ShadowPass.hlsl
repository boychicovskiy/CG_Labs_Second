//=============================================================================
// ShadowPass.hlsl
//
// Depth-only pass that fills one cascade of the shadow map array (lecture 06,
// slides 13-14). There is no pixel shader at all: the PSO leaves PS empty and
// declares zero render targets, so the rasterizer only writes depth.
//
// Two entry points share the same constant buffer:
//   VS           - static geometry (Sponza), world matrix comes from the CB
//   VS_Instanced - the scattered box field, world transform comes from the
//                  per-instance vertex stream
//=============================================================================

cbuffer ShadowPassCB : register(b0)
{
    float4x4 gLightViewProj;   // changes per cascade
    float4x4 gWorld;
};

//-----------------------------------------------------------------------------
// Static geometry
//-----------------------------------------------------------------------------
float4 VS(float3 posL : POSITION) : SV_POSITION
{
    float4 posW = mul(float4(posL, 1.0f), gWorld);
    return mul(posW, gLightViewProj);
}

//-----------------------------------------------------------------------------
// Instanced boxes. The whole field is drawn here, not the culled subset:
// an object outside the camera frustum can still cast a shadow into it.
//-----------------------------------------------------------------------------
struct InstancedIn
{
    float3 PosL   : POSITION;
    float4 Center : INSTCENTER;
    float4 Extent : INSTEXTENT;
};

float4 VS_Instanced(InstancedIn vin) : SV_POSITION
{
    float3 posW = vin.PosL * vin.Extent.xyz + vin.Center.xyz;
    return mul(float4(posW, 1.0f), gLightViewProj);
}
