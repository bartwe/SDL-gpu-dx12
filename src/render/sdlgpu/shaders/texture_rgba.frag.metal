#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct main0_out
{
    float4 o_color [[color(0)]];
};

struct main0_in
{
    float4 v_color [[user(locn0)]];
    float2 v_uv [[user(locn1)]];
};

fragment main0_out main0(main0_in in [[stage_in]], texture2d<float> u_texture [[texture(0)]], sampler u_textureSmplr [[sampler(0)]])
{
    main0_out out = {};
    out.o_color = u_texture.sample(u_textureSmplr, in.v_uv) * in.v_color;
    return out;
}

