cbuffer Context : register(b0)
{
    row_major float4x4 u_context_mvp : packoffset(c0);
    float4 u_context_color : packoffset(c4);
    float2 u_context_texture_size : packoffset(c5);
};


static float4 gl_Position;
static float2 a_position;
static float4 v_color;
static float4 a_color;

struct SPIRV_Cross_Input
{
    float2 a_position : TEXCOORD0;
    float4 a_color : TEXCOORD1;
};

struct SPIRV_Cross_Output
{
    float4 v_color : TEXCOORD0;
    float4 gl_Position : SV_Position;
};

void vert_main()
{
    gl_Position = mul(float4(a_position, 0.0f, 1.0f), u_context_mvp);
    v_color = a_color;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    a_position = stage_input.a_position;
    a_color = stage_input.a_color;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.v_color = v_color;
    return stage_output;
}
