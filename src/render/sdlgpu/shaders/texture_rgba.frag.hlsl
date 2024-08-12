Texture2D<float4> u_texture : register(t0);
SamplerState _u_texture_sampler : register(s0);

static float4 o_color;
static float2 v_uv;
static float4 v_color;

struct SPIRV_Cross_Input
{
    float4 v_color : TEXCOORD0;
    float2 v_uv : TEXCOORD1;
};

struct SPIRV_Cross_Output
{
    float4 o_color : SV_Target0;
};

void frag_main()
{
    o_color = u_texture.Sample(_u_texture_sampler, v_uv) * v_color;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    v_uv = stage_input.v_uv;
    v_color = stage_input.v_color;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.o_color = o_color;
    return stage_output;
}
