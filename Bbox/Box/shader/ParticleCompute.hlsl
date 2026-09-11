//=============================================================================
// ParticleCompute.hlsl
//
// Simulation half of the particle system (lecture 07, homework #6).
// Position updates happen here and nowhere else - that is the hard requirement
// of the assignment.
//
// Two structured buffers swap roles every frame:
//   u0  gDstParticles  AppendStructuredBuffer   - this frame's survivors
//   u1  gSrcParticles  ConsumeStructuredBuffer  - last frame's particles
//
// Append and Consume both drive a hidden counter attached to the UAV, which is
// why these must be descriptor-table UAVs and not root descriptors.
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

AppendStructuredBuffer<Particle>  gDstParticles : register(u0);
ConsumeStructuredBuffer<Particle> gSrcParticles : register(u1);

// Snapshot of last frame's alive count, taken with CopyBufferRegion after the
// simulation finished. Reading the live Append/Consume counter here would race
// with the Consume() calls of the other threads in this very dispatch.
RWByteAddressBuffer gCountSnapshot : register(u2);

// Live counter of the destination buffer - used only for the capacity guard.
RWByteAddressBuffer gDstCounter : register(u3);

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

#define THREAD_GROUP_SIZE 64

//-----------------------------------------------------------------------------
// Cheap integer hash (Chris Wellons' triple32-style mix). Particle systems are
// a Monte-Carlo technique (slide 2), so every initial attribute is drawn from
// a range with a random offset (slide 6).
//-----------------------------------------------------------------------------
uint Hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float Rand01(inout uint state)
{
    state = Hash(state);
    return float(state) * (1.0f / 4294967296.0f);
}

float RandRange(inout uint state, float lo, float hi)
{
    return lo + (hi - lo) * Rand01(state);
}

// Uniform direction inside a cone around +Y, half angle given by cosMin
float3 RandomConeDirection(inout uint state, float cosMin)
{
    float z   = RandRange(state, cosMin, 1.0f);
    float phi = RandRange(state, 0.0f, 6.28318530718f);
    float r   = sqrt(saturate(1.0f - z * z));

    // +Y is "up" for the fountain, so the cone axis is Y
    return float3(r * cos(phi), z, r * sin(phi));
}

//=============================================================================
// EmitCS - generation stage (slide 19)
//=============================================================================
[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void EmitCS(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gEmitCount)
        return;

    // Capacity guard: Append past the end of the buffer would write out of
    // bounds. The read is racy by nature, hence the conservative margin.
    uint alive = gDstCounter.Load(0);
    if (alive + gEmitCount >= gMaxParticles)
        return;

    uint seed = Hash(dtid.x * 747796405u + gFrameIndex * 2891336453u);

    Particle p;

    // Small spawn volume so the fountain has a base, not a single point
    float3 jitter = float3(RandRange(seed, -1.0f, 1.0f),
                           RandRange(seed, -1.0f, 1.0f),
                           RandRange(seed, -1.0f, 1.0f));

    p.Position = gEmitterPos + jitter * (gSizeMax * 2.0f);

    // cos(30 degrees) ~ 0.866 -> a fairly narrow upward cone
    float3 dir = RandomConeDirection(seed, 0.80f);
    p.Velocity = dir * RandRange(seed, gSpeedMin, gSpeedMax);

    p.Age      = 0.0f;
    p.Lifetime = RandRange(seed, gLifeMin, gLifeMax);
    p.Size     = RandRange(seed, gSizeMin, gSizeMax);

    // Ember palette: hot yellow-white core fading to deep orange
    float t = Rand01(seed);
    p.Color = float4(1.0f,
                     lerp(0.35f, 0.95f, t),
                     lerp(0.05f, 0.35f, t * t),
                     1.0f);

    p._pad = float3(0.0f, 0.0f, 0.0f);

    gDstParticles.Append(p);
}

//=============================================================================
// UpdateCS - dynamics and death stages (slides 8, 19)
//
// Dispatched over the whole pool; threads past the alive count exit early.
// That avoids calling Consume() on an empty buffer, which would underflow the
// counter and hand back garbage slots.
//=============================================================================
[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void UpdateCS(uint3 dtid : SV_DispatchThreadID)
{
    uint aliveCount = gCountSnapshot.Load(0);

    if (dtid.x >= aliveCount)
        return;

    Particle p = gSrcParticles.Consume();

    p.Age += gDeltaTime;

    // Death: lifetime expired (slide 19)
    if (p.Age >= p.Lifetime)
        return;   // simply not appended to the destination buffer

    // Explicit Euler integration (slide 8): forces change acceleration,
    // acceleration changes velocity, velocity changes position.
    float3 acceleration = gGravity - p.Velocity * gDrag;

    p.Velocity += acceleration * gDeltaTime;
    p.Position += p.Velocity   * gDeltaTime;

    // Colour is animated over the life of the particle (slide 12):
    // embers cool down and shrink as they rise.
    float lifeT = saturate(p.Age / max(p.Lifetime, 1e-4f));

    p.Color.rgb *= (1.0f - 0.55f * lifeT);
    p.Size      *= (1.0f - 0.35f * gDeltaTime);

    gDstParticles.Append(p);
}
