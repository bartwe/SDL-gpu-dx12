/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2024 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#if SDL_VIDEO_RENDER_GPU

#include "SDL_shaders_gpu.h"

#include "shaders/spir-v.h"

/* SDL_Gpu shader implementation */

typedef struct GPU_ShaderModuleSource
{
    const unsigned char *code;
    unsigned int code_len;
    SDL_GpuShaderFormat format;
} GPU_ShaderModuleSource;

#define IF_VULKAN(...) __VA_ARGS__

typedef struct GPU_ShaderSources
{
    IF_VULKAN(GPU_ShaderModuleSource spirv;)
    unsigned int num_samplers;
    unsigned int num_uniform_buffers;
} GPU_ShaderSources;

#define SHADER_SPIRV(code) \
    IF_VULKAN(.spirv = { code, code##_##len, SDL_GPU_SHADERFORMAT_SPIRV }, )

/* clang-format off */
static const GPU_ShaderSources vert_shader_sources[NUM_VERT_SHADERS] = {
    [VERT_SHADER_LINEPOINT] = {
        .num_samplers = 0,
        .num_uniform_buffers = 1,
        SHADER_SPIRV(linepoint_vert_spv)
    },
    [VERT_SHADER_TRI_COLOR] = {
        .num_samplers = 0,
        .num_uniform_buffers = 1,
        SHADER_SPIRV(tri_color_vert_spv)
    },
    [VERT_SHADER_TRI_TEXTURE] = {
        .num_samplers = 0,
        .num_uniform_buffers = 1,
        SHADER_SPIRV(tri_texture_vert_spv)
    },
};

static const GPU_ShaderSources frag_shader_sources[NUM_FRAG_SHADERS] = {
    [FRAG_SHADER_COLOR] = {
        .num_samplers = 0,
        .num_uniform_buffers = 0,
        SHADER_SPIRV(color_frag_spv)
    },
    [FRAG_SHADER_TEXTURE_RGBA] = {
        .num_samplers = 1,
        .num_uniform_buffers = 0,
        SHADER_SPIRV(texture_rgba_frag_spv)
    },
};
/* clang-format on */

static SDL_GpuShader *CompileShader(const GPU_ShaderSources *sources, SDL_GpuDevice *device, SDL_GpuShaderStage stage)
{
    const GPU_ShaderModuleSource *sms = NULL;

    switch (SDL_GpuGetDriver(device)) {
    case SDL_GPU_DRIVER_VULKAN:
        sms = &sources->spirv;
        break;

    default:
        SDL_SetError("Unsupported GPU backend");
        return NULL;
    }

    SDL_GpuShaderCreateInfo sci = { 0 };
    sci.code = sms->code;
    sci.codeSize = sms->code_len;
    sci.format = sms->format;
    sci.entryPointName = "main"; // XXX is this guaranteed on all backends?
    sci.samplerCount = sources->num_samplers;
    sci.uniformBufferCount = sources->num_uniform_buffers;
    sci.stage = stage;

    return SDL_GpuCreateShader(device, &sci);
}

int GPU_InitShaders(GPU_Shaders *shaders, SDL_GpuDevice *device)
{
    for (int i = 0; i < SDL_arraysize(vert_shader_sources); ++i) {
        shaders->vert_shaders[i] = CompileShader(
            &vert_shader_sources[i], device, SDL_GPU_SHADERSTAGE_VERTEX);
        if (shaders->vert_shaders[i] == NULL) {
            GPU_ReleaseShaders(shaders, device);
            return -1;
        }
    }

    for (int i = 0; i < SDL_arraysize(frag_shader_sources); ++i) {
        shaders->frag_shaders[i] = CompileShader(
            &frag_shader_sources[i], device, SDL_GPU_SHADERSTAGE_FRAGMENT);
        if (shaders->frag_shaders[i] == NULL) {
            GPU_ReleaseShaders(shaders, device);
            return -1;
        }
    }

    return 0;
}

void GPU_ReleaseShaders(GPU_Shaders *shaders, SDL_GpuDevice *device)
{
    for (int i = 0; i < SDL_arraysize(shaders->vert_shaders); ++i) {
        SDL_GpuReleaseShader(device, shaders->vert_shaders[i]);
        shaders->vert_shaders[i] = NULL;
    }

    for (int i = 0; i < SDL_arraysize(shaders->frag_shaders); ++i) {
        SDL_GpuReleaseShader(device, shaders->frag_shaders[i]);
        shaders->frag_shaders[i] = NULL;
    }
}

SDL_GpuShader *GPU_GetVertexShader(GPU_Shaders *shaders, GPU_VertexShaderID id)
{
    SDL_assert((unsigned int)id < SDL_arraysize(shaders->vert_shaders));
    SDL_GpuShader *shader = shaders->vert_shaders[id];
    SDL_assert(shader != NULL);
    return shader;
}

SDL_GpuShader *GPU_GetFragmentShader(GPU_Shaders *shaders, GPU_FragmentShaderID id)
{
    SDL_assert((unsigned int)id < SDL_arraysize(shaders->frag_shaders));
    SDL_GpuShader *shader = shaders->frag_shaders[id];
    SDL_assert(shader != NULL);
    return shader;
}

#endif /* SDL_VIDEO_RENDER_OGL */
