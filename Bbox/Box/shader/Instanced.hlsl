//=============================================================================
// Instanced.hlsl
//
// The scattered object field of homework #4 (lecture 05).
//
// One cube mesh is drawn with DrawIndexedInstanced; the per-instance stream
// carries the box centre, its half extent and a colour. Only instances that
// survived frustum culling on the CPU are present in that buffer, so the
// instance count is the visible count.
//
// Writes into the same G-Buffer as the rest of the geometry pass, which means
// the boxes are lit by the deferred light stage like everything else.
//
// Lab 8: the field doubles as a PBR material chart. Center.w and Extent.w were
// free padding in the 48-byte instance record, so they now carry metallic and
// roughness. The CPU fills them from the normalised box position, which turns
// the field into the grid from slide 44 of lecture 09: metallic along one axis,
// roughness along the other.
//=============================================================================

cbuffer InstancePassCB : register(b0)
{
    float4x4 gViewProj;
};

struct VertexIn
{
    // slot 0 - the cube mesh, local coordinates are +-1
    float3 PosL    : POSITION;
    float3 NormalL : NORMAL;

    // slot 1 - per instance data
    float4 Center  : INSTCENTER;   // xyz = centre, w = metallic
    float4 Extent  : INSTEXTENT;   // xyz = half extent, w = roughness
    float4 Color   : INSTCOLOR;    // rgb = base color
};

struct VertexOut
{
    float4 PosH      : SV_POSITION;
    float3 NormalW   : NORMAL;
    float4 Color     : COLOR;
    float2 MatParams : MATPARAMS;   // x = metallic, y = roughness
};

struct GBufferOut
{
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 Material : SV_Target2;   // r = metallic, g = roughness, b = AO
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;

    // The instance transform is a pure scale plus translation, so the world
    // position is one multiply-add and the AABB used for culling on the CPU
    // is exactly the box drawn here - no conservative padding needed.
    float3 posW = vin.PosL * vin.Extent.xyz + vin.Center.xyz;

    vout.PosH = mul(float4(posW, 1.0f), gViewProj);

    // Non-uniform scale: the correct normal transform is division by the
    // scale, not multiplication by it.
    vout.NormalW = normalize(vin.NormalL / max(vin.Extent.xyz, 1e-6f));

    vout.Color = vin.Color;

    // Roughness is clamped away from zero: a perfectly smooth GGX lobe is a
    // delta function and produces fireflies on a single point light.
    vout.MatParams = float2(saturate(vin.Center.w),
                            clamp(vin.Extent.w, 0.05f, 1.0f));

    return vout;
}

GBufferOut PS(VertexOut pin, bool isFront : SV_IsFrontFace)
{
    float3 N = normalize(pin.NormalW);
    if (!isFront)
        N = -N;

    GBufferOut o;

    o.Albedo   = float4(pin.Color.rgb, 1.0f);
    o.Normal   = float4(N, 0.0f);
    o.Material = float4(pin.MatParams.x, pin.MatParams.y, 1.0f, 1.0f);

    return o;
}
