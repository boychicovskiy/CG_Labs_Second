Texture2D    gDiffuseMap : register(t0);
SamplerState gSampler    : register(s0);

cbuffer ObjectCB : register(b0)
{
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
    float2   gUvScale;
    float2   gUvOffset;
};

cbuffer PassCB : register(b1)
{
    float4x4 gViewProj;

    float3 gEyePosW;
    float _pad0;

    float3 gLightDirW;
    float _pad1;

    float4 gAmbient;
    float4 gDiffuse;
    float4 gSpecular;

    float gSpecPower;
    float3 _pad2;
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
    float3 PosW     : TEXCOORD0;
    float3 NormalW  : NORMAL;
    float4 Color    : COLOR;
    float2 TexCoord : TEXCOORD1;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;

    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;

    vout.NormalW = mul(vin.NormalL, (float3x3) gWorldInvTranspose);

    vout.PosH = mul(posW, gViewProj);

    vout.Color = vin.Color;

    vout.TexCoord = vin.TexCoord * gUvScale + gUvOffset;

    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    float3 N = normalize(pin.NormalW);
    float3 L = normalize(-gLightDirW);
    float3 V = normalize(gEyePosW - pin.PosW);

    float3 base = gDiffuseMap.Sample(gSampler, pin.TexCoord).rgb;

    float ndotl = saturate(dot(N, L));

    float3 ambient  = gAmbient.rgb  * base;
    float3 diffuse  = gDiffuse.rgb  * base * ndotl;

    float3 R    = reflect(-L, N);
    float  spec = pow(saturate(dot(R, V)), gSpecPower);
    float3 specular = gSpecular.rgb * spec;

    return float4(ambient + diffuse + specular, 1.0f);
}
