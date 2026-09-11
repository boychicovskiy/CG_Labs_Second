//=============================================================================
// ParticleRender.hlsl
//
// Rendering half of the particle system (lecture 07, homework #6).
//
//   VS : reads one particle by SV_VertexID from a structured buffer.
//        No vertex buffer and no input layout at all - the topology is a plain
//        point list and the draw is DrawInstancedIndirect (slide 18).
//   GS : expands that single point into a camera facing billboard, two
//        triangles as a strip. This is the "store only one vertex, expand in
//        the geometry shader" option from slide 11.
//   PS : writes into the same G-Buffer as the rest of the scene.
//
// The particles are opaque, so no sorting is needed - the depth buffer does
// the work, and the deferred light stage lights them like any other surface.
//=============================================================================

struct Particle
{
    float3 Position;
    float  Age;

    float3 Velocity;
    float  Lifetime;

    float4 Color;

    float  Size;
    float3 _pad;
};

StructuredBuffer<Particle> gParticles : register(t0);

cbuffer ParticleCB : register(b0)
{
    float4x4 gViewProj;

    float3 gCameraRight;
    float  gDeltaTime;

    float3 gCameraUp;
    float  gTotalTime;

    float3 gEmitterPos;
    uint   gEmitCount;

    float3 gGravity;
    uint   gFrameIndex;

    float  gLifeMin;
    float  gLifeMax;
    float  gSpeedMin;
    float  gSpeedMax;

    float  gSizeMin;
    float  gSizeMax;
    float  gDrag;
    uint   gMaxParticles;
};

struct VSOut
{
    float3 PosW  : POSITION;
    float4 Color : COLOR;
    float  Size  : TEXCOORD0;
};

struct GSOut
{
    float4 PosH    : SV_POSITION;
    float4 Color   : COLOR;
    float2 Corner  : TEXCOORD0;   // [-1..1] across the billboard
    float3 NormalW : NORMAL;
};

struct GBufferOut
{
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 Specular : SV_Target2;
};

//-----------------------------------------------------------------------------
// One vertex = one particle. SV_VertexID indexes the structured buffer
// directly, exactly as slide 18 describes for a point list without IA.
//-----------------------------------------------------------------------------
VSOut VS(uint vertexID : SV_VertexID)
{
    Particle p = gParticles[vertexID];

    VSOut vout;
    vout.PosW  = p.Position;
    vout.Color = p.Color;
    vout.Size  = p.Size;

    return vout;
}

//-----------------------------------------------------------------------------
// Point -> billboard. The quad is built from the camera right/up vectors, so
// it always faces the viewer (slide 11).
//-----------------------------------------------------------------------------
[maxvertexcount(4)]
void GS(point VSOut input[1], inout TriangleStream<GSOut> stream)
{
    VSOut p = input[0];

    float3 right = gCameraRight * p.Size;
    float3 up    = gCameraUp    * p.Size;

    // Billboard faces the camera, so its normal is the view direction reversed
    float3 faceNormal = normalize(cross(gCameraRight, gCameraUp));

    // Strip order: bottom-left, top-left, bottom-right, top-right
    const float2 corners[4] = {
        float2(-1.0f, -1.0f),
        float2(-1.0f,  1.0f),
        float2( 1.0f, -1.0f),
        float2( 1.0f,  1.0f)
    };

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float2 c = corners[i];
        float3 posW = p.PosW + right * c.x + up * c.y;

        GSOut o;
        o.PosH    = mul(float4(posW, 1.0f), gViewProj);
        o.Color   = p.Color;
        o.Corner  = c;
        o.NormalW = faceNormal;

        stream.Append(o);
    }

    stream.RestartStrip();
}

GBufferOut PS(GSOut pin)
{
    // Round the square billboard into a disc. clip() keeps the particle opaque
    // - it either covers the pixel or it does not, so no blending and no
    // sorting are involved.
    float r2 = dot(pin.Corner, pin.Corner);
    clip(1.0f - r2);

    // Fake a sphere normal across the disc so the deferred lights give the
    // particle some volume instead of a flat card.
    float3 sphereN = normalize(
        gCameraRight * pin.Corner.x +
        gCameraUp    * pin.Corner.y +
        pin.NormalW  * sqrt(saturate(1.0f - r2)));

    GBufferOut o;

    o.Albedo   = float4(pin.Color.rgb, 1.0f);
    o.Normal   = float4(sphereN, 0.0f);
    o.Specular = float4(0.15f, 0.12f, 0.08f, 16.0f / 255.0f);

    return o;
}
