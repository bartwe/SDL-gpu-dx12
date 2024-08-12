#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct Context
{
    float4x4 mvp;
    float4 color;
    float2 texture_size;
};

struct main0_out
{
    float4 v_color [[user(locn0)]];
    float2 v_uv [[user(locn1)]];
    float4 gl_Position [[position]];
};

struct main0_in
{
    float2 a_position [[attribute(0)]];
    float4 a_color [[attribute(1)]];
    float2 a_uv [[attribute(2)]];
};

vertex main0_out main0(main0_in in [[stage_in]], constant Context& u_context [[buffer(0)]])
{
    main0_out out = {};
    out.gl_Position = u_context.mvp * float4(in.a_position, 0.0, 1.0);
    out.v_color = in.a_color;
    out.v_uv = in.a_uv / u_context.texture_size;
    return out;
}

