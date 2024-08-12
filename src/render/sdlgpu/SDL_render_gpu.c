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
#include "../../video/SDL_pixels_c.h"
#include "../SDL_d3dmath.h"
#include "../SDL_sysrender.h"
#include "SDL_shaders_gpu.h"

// TODO YUV
#undef SDL_HAVE_YUV
#define SDL_HAVE_YUV 0

// FIXME how much is enough? Should we add dynamic resizing?
#define VERTEX_BUFFER_SIZE (1 << 20)

typedef struct GPU_ShaderUniformData
{
    Float4X4 mvp;
    SDL_FColor color;
    float texture_size[2];
} GPU_ShaderUniformData;

typedef struct GPU_RenderData
{
    SDL_GpuDevice *device;
    GPU_Shaders shaders;
    SDL_GpuFence *present_fence;

    struct
    {
        SDL_GpuTexture *texture;
        SDL_GpuTextureFormat format;
        Uint32 width;
        Uint32 height;
        SDL_GpuSwapchainComposition composition;
        SDL_GpuPresentMode present_mode;
    } swapchain;

    struct
    {
        SDL_GpuTransferBuffer *transfer_buf;
        void *mapped_transfer_buf;
        SDL_GpuBuffer *buffer;
    } vertices;

    struct
    {
        SDL_GpuRenderPass *render_pass;
        SDL_Texture *render_target;
        SDL_GpuCommandBuffer *command_buffer;
        SDL_GpuColorAttachmentInfo color_attachment;
        SDL_GpuViewport viewport;
        SDL_Rect scissor;
        SDL_FColor draw_color;
        SDL_bool scissor_enabled;
        GPU_ShaderUniformData shader_data;
    } state;

    SDL_GpuSampler *samplers[3][2];
} GPU_RenderData;

typedef struct GPU_TextureData
{
    SDL_GpuTexture *texture;
    Uint32 width;
    Uint32 height;
    SDL_GpuTextureFormat format;
    GPU_FragmentShaderID shader;
    const float *shader_params;
    void *pixels;
    int pitch;
    SDL_Rect locked_rect;
    SDL_bool sampler_outdated;

#if SDL_HAVE_YUV
    /* YUV texture support */
    SDL_bool yuv;
    SDL_bool nv12;
    GLuint utexture;
    SDL_bool utexture_external;
    GLuint vtexture;
    SDL_bool vtexture_external;
#endif
} GPU_TextureData;

enum
{
    BLEND_INVALID = 0x7fffffff,
};

static SDL_GpuBlendFactor GetBlendFunc(SDL_BlendFactor factor)
{
    switch (factor) {
    case SDL_BLENDFACTOR_ZERO:
        return SDL_GPU_BLENDFACTOR_ZERO;
    case SDL_BLENDFACTOR_ONE:
        return SDL_GPU_BLENDFACTOR_ONE;
    case SDL_BLENDFACTOR_SRC_COLOR:
        return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case SDL_BLENDFACTOR_SRC_ALPHA:
        return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case SDL_BLENDFACTOR_DST_COLOR:
        return SDL_GPU_BLENDFACTOR_DST_COLOR;
    case SDL_BLENDFACTOR_ONE_MINUS_DST_COLOR:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case SDL_BLENDFACTOR_DST_ALPHA:
        return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case SDL_BLENDFACTOR_ONE_MINUS_DST_ALPHA:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    default:
        return BLEND_INVALID;
    }
}

static SDL_GpuBlendOp GetBlendEquation(SDL_BlendOperation operation)
{
    switch (operation) {
    case SDL_BLENDOPERATION_ADD:
        return SDL_GPU_BLENDOP_ADD;
    case SDL_BLENDOPERATION_SUBTRACT:
        return SDL_GPU_BLENDOP_SUBTRACT;
    case SDL_BLENDOPERATION_REV_SUBTRACT:
        return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
    case SDL_BLENDOPERATION_MINIMUM:
        return SDL_GPU_BLENDOP_MIN;
    case SDL_BLENDOPERATION_MAXIMUM:
        return SDL_GPU_BLENDOP_MAX;
    default:
        return BLEND_INVALID;
    }
}

static SDL_bool GPU_SupportsBlendMode(SDL_Renderer *renderer, SDL_BlendMode blendMode)
{
    SDL_BlendFactor srcColorFactor = SDL_GetBlendModeSrcColorFactor(blendMode);
    SDL_BlendFactor srcAlphaFactor = SDL_GetBlendModeSrcAlphaFactor(blendMode);
    SDL_BlendOperation colorOperation = SDL_GetBlendModeColorOperation(blendMode);
    SDL_BlendFactor dstColorFactor = SDL_GetBlendModeDstColorFactor(blendMode);
    SDL_BlendFactor dstAlphaFactor = SDL_GetBlendModeDstAlphaFactor(blendMode);
    SDL_BlendOperation alphaOperation = SDL_GetBlendModeAlphaOperation(blendMode);

    if (GetBlendFunc(srcColorFactor) == BLEND_INVALID ||
        GetBlendFunc(srcAlphaFactor) == BLEND_INVALID ||
        GetBlendEquation(colorOperation) == BLEND_INVALID ||
        GetBlendFunc(dstColorFactor) == BLEND_INVALID ||
        GetBlendFunc(dstAlphaFactor) == BLEND_INVALID ||
        GetBlendEquation(alphaOperation) == BLEND_INVALID) {
        return SDL_FALSE;
    }

    return SDL_TRUE;
}

static SDL_GpuTextureFormat PixFormatToTexFormat(Uint32 pixel_format)
{
    switch (pixel_format) {
    case SDL_PIXELFORMAT_ARGB8888:
    case SDL_PIXELFORMAT_XRGB8888:
        return SDL_GPU_TEXTUREFORMAT_B8G8R8A8;
    case SDL_PIXELFORMAT_ABGR8888:
    case SDL_PIXELFORMAT_XBGR8888:
        return SDL_GPU_TEXTUREFORMAT_R8G8B8A8;
    case SDL_PIXELFORMAT_YV12:
    case SDL_PIXELFORMAT_IYUV:
    case SDL_PIXELFORMAT_NV12:
    case SDL_PIXELFORMAT_NV21:
        return SDL_GPU_TEXTUREFORMAT_A8; // YUV TODO
    case SDL_PIXELFORMAT_UYVY:           // YUV FIXME
    default:
        return SDL_GPU_TEXTUREFORMAT_INVALID;
    }
}

static int GPU_CreateTexture(SDL_Renderer *renderer, SDL_Texture *texture, SDL_PropertiesID create_props)
{
    GPU_RenderData *renderdata = (GPU_RenderData *)renderer->internal;
    GPU_TextureData *data;
    SDL_GpuTextureFormat format;
    SDL_GpuTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER_BIT;

    format = PixFormatToTexFormat(texture->format);

    if (format == SDL_GPU_TEXTUREFORMAT_INVALID) {
        return SDL_SetError("Texture format %s not supported by SDL_Gpu",
                            SDL_GetPixelFormatName(texture->format));
    }

    data = (GPU_TextureData *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        return -1;
    }

    if (texture->access == SDL_TEXTUREACCESS_STREAMING) {
        size_t size;
        data->pitch = texture->w * SDL_BYTESPERPIXEL(texture->format);
        size = (size_t)texture->h * data->pitch;
        if (texture->format == SDL_PIXELFORMAT_YV12 ||
            texture->format == SDL_PIXELFORMAT_IYUV) {
            /* Need to add size for the U and V planes */
            size += 2 * ((texture->h + 1) / 2) * ((data->pitch + 1) / 2);
        }
        if (texture->format == SDL_PIXELFORMAT_NV12 ||
            texture->format == SDL_PIXELFORMAT_NV21) {
            /* Need to add size for the U/V plane */
            size += 2 * ((texture->h + 1) / 2) * ((data->pitch + 1) / 2);
        }
        data->pixels = SDL_calloc(1, size);
        if (!data->pixels) {
            SDL_free(data);
            return -1;
        }

        // TODO allocate and map persistent transfer buffer
    }

    if (texture->access == SDL_TEXTUREACCESS_TARGET) {
        usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET_BIT;
    }

    texture->internal = data;
    SDL_GpuTextureCreateInfo tci = { 0 };
    tci.format = format;
    tci.layerCount = 1;
    tci.levelCount = 1;
    tci.usageFlags = usage;
    tci.width = texture->w;
    tci.height = texture->h;
    tci.depth = 1;
    tci.sampleCount = SDL_GPU_SAMPLECOUNT_1;

    data->format = format;
    data->texture = SDL_GpuCreateTexture(renderdata->device, &tci);

    if (!data->texture) {
        return -1;
    }

#if SDL_HAVE_YUV
    if (texture->format == SDL_PIXELFORMAT_YV12 ||
        texture->format == SDL_PIXELFORMAT_IYUV) {
        data->yuv = SDL_TRUE;

        data->utexture = (GLuint)SDL_GetNumberProperty(create_props, SDL_PROP_TEXTURE_CREATE_OPENGL_TEXTURE_U_NUMBER, 0);
        if (data->utexture) {
            data->utexture_external = SDL_TRUE;
        } else {
            renderdata->glGenTextures(1, &data->utexture);
        }
        data->vtexture = (GLuint)SDL_GetNumberProperty(create_props, SDL_PROP_TEXTURE_CREATE_OPENGL_TEXTURE_V_NUMBER, 0);
        if (data->vtexture) {
            data->vtexture_external = SDL_TRUE;
        } else {
            renderdata->glGenTextures(1, &data->vtexture);
        }

        renderdata->glBindTexture(textype, data->utexture);
        renderdata->glTexParameteri(textype, GL_TEXTURE_MIN_FILTER,
                                    scaleMode);
        renderdata->glTexParameteri(textype, GL_TEXTURE_MAG_FILTER,
                                    scaleMode);
        renderdata->glTexImage2D(textype, 0, internalFormat, (texture_w + 1) / 2,
                                 (texture_h + 1) / 2, 0, format, type, NULL);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_OPENGL_TEXTURE_U_NUMBER, data->utexture);

        renderdata->glBindTexture(textype, data->vtexture);
        renderdata->glTexParameteri(textype, GL_TEXTURE_MIN_FILTER,
                                    scaleMode);
        renderdata->glTexParameteri(textype, GL_TEXTURE_MAG_FILTER,
                                    scaleMode);
        renderdata->glTexImage2D(textype, 0, internalFormat, (texture_w + 1) / 2,
                                 (texture_h + 1) / 2, 0, format, type, NULL);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_OPENGL_TEXTURE_V_NUMBER, data->vtexture);
    }

    if (texture->format == SDL_PIXELFORMAT_NV12 ||
        texture->format == SDL_PIXELFORMAT_NV21) {
        data->nv12 = SDL_TRUE;

        data->utexture = (GLuint)SDL_GetNumberProperty(create_props, SDL_PROP_TEXTURE_CREATE_OPENGL_TEXTURE_UV_NUMBER, 0);
        if (data->utexture) {
            data->utexture_external = SDL_TRUE;
        } else {
            renderdata->glGenTextures(1, &data->utexture);
        }
        renderdata->glBindTexture(textype, data->utexture);
        renderdata->glTexParameteri(textype, GL_TEXTURE_MIN_FILTER,
                                    scaleMode);
        renderdata->glTexParameteri(textype, GL_TEXTURE_MAG_FILTER,
                                    scaleMode);
        renderdata->glTexImage2D(textype, 0, GL_LUMINANCE_ALPHA, (texture_w + 1) / 2,
                                 (texture_h + 1) / 2, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, NULL);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_OPENGL_TEXTURE_UV_NUMBER, data->utexture);
    }
#endif

    // FIXME do we need an RGB shader?
    // if (texture->format == SDL_PIXELFORMAT_ABGR8888 || texture->format == SDL_PIXELFORMAT_ARGB8888) {
    data->shader = FRAG_SHADER_TEXTURE_RGBA;
    // } else {
    // data->shader = FRAG_SHADER_TEXTURE_RGB;
    // }

#if SDL_HAVE_YUV
    if (data->yuv || data->nv12) {
        if (data->yuv) {
            data->shader = SHADER_YUV;
        } else if (texture->format == SDL_PIXELFORMAT_NV12) {
            if (SDL_GetHintBoolean("SDL_RENDER_OPENGL_NV12_RG_SHADER", SDL_FALSE)) {
                data->shader = SHADER_NV12_RG;
            } else {
                data->shader = SHADER_NV12_RA;
            }
        } else {
            if (SDL_GetHintBoolean("SDL_RENDER_OPENGL_NV12_RG_SHADER", SDL_FALSE)) {
                data->shader = SHADER_NV21_RG;
            } else {
                data->shader = SHADER_NV21_RA;
            }
        }
        data->shader_params = SDL_GetYCbCRtoRGBConversionMatrix(texture->colorspace, texture->w, texture->h, 8);
        if (!data->shader_params) {
            return SDL_SetError("Unsupported YUV colorspace");
        }
    }
#endif /* SDL_HAVE_YUV */

    return 0;
}

static int GPU_UpdateTexture(SDL_Renderer *renderer, SDL_Texture *texture,
                             const SDL_Rect *rect, const void *pixels, int pitch)
{
    GPU_RenderData *renderdata = (GPU_RenderData *)renderer->internal;
    GPU_TextureData *data = (GPU_TextureData *)texture->internal;
    const Uint32 texturebpp = SDL_BYTESPERPIXEL(texture->format);

    Uint32 row_size = texturebpp * rect->w;
    Uint32 data_size = row_size * rect->h;

    SDL_GpuTransferBuffer *tbuf = SDL_GpuCreateTransferBuffer(
        renderdata->device, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, data_size);

    if (tbuf == NULL) {
        return -1;
    }

    Uint8 *output;
    SDL_GpuMapTransferBuffer(renderdata->device, tbuf, SDL_FALSE, (void **)&output);

    if (pitch == row_size) {
        memcpy(output, pixels, data_size);
    } else {
        // FIXME is negative pitch supposed to work?
        // If not, maybe use SDL_GpuTextureTransferInfo::imagePitch instead of this
        const Uint8 *input = pixels;

        for (int i = 0; i < rect->h; ++i) {
            memcpy(output, input, row_size);
            output += row_size;
            input += pitch;
        }
    }

    SDL_GpuUnmapTransferBuffer(renderdata->device, tbuf);

    SDL_GpuCommandBuffer *cbuf = renderdata->state.command_buffer;
    SDL_GpuCopyPass *cpass = SDL_GpuBeginCopyPass(cbuf);

    SDL_GpuTextureTransferInfo tex_src = { 0 };
    tex_src.transferBuffer = tbuf;
    tex_src.imageHeight = rect->h;
    tex_src.imagePitch = rect->w;

    SDL_GpuTextureRegion tex_dst = { 0 };
    tex_dst.textureSlice.texture = data->texture;
    tex_dst.x = rect->x;
    tex_dst.y = rect->y;
    tex_dst.w = rect->w;
    tex_dst.h = rect->h;
    tex_dst.d = 1;

    SDL_GpuUploadToTexture(cpass, &tex_src, &tex_dst, SDL_TRUE);
    SDL_GpuEndCopyPass(cpass);

#if SDL_HAVE_YUV
    if (data->yuv) {
        renderdata->glPixelStorei(GL_UNPACK_ROW_LENGTH, ((pitch + 1) / 2));

        /* Skip to the correct offset into the next texture */
        pixels = (const void *)((const Uint8 *)pixels + rect->h * pitch);
        if (texture->format == SDL_PIXELFORMAT_YV12) {
            renderdata->glBindTexture(textype, data->vtexture);
        } else {
            renderdata->glBindTexture(textype, data->utexture);
        }
        renderdata->glTexSubImage2D(textype, 0, rect->x / 2, rect->y / 2,
                                    (rect->w + 1) / 2, (rect->h + 1) / 2,
                                    data->format, data->formattype, pixels);

        /* Skip to the correct offset into the next texture */
        pixels = (const void *)((const Uint8 *)pixels + ((rect->h + 1) / 2) * ((pitch + 1) / 2));
        if (texture->format == SDL_PIXELFORMAT_YV12) {
            renderdata->glBindTexture(textype, data->utexture);
        } else {
            renderdata->glBindTexture(textype, data->vtexture);
        }
        renderdata->glTexSubImage2D(textype, 0, rect->x / 2, rect->y / 2,
                                    (rect->w + 1) / 2, (rect->h + 1) / 2,
                                    data->format, data->formattype, pixels);
    }

    if (data->nv12) {
        renderdata->glPixelStorei(GL_UNPACK_ROW_LENGTH, ((pitch + 1) / 2));

        /* Skip to the correct offset into the next texture */
        pixels = (const void *)((const Uint8 *)pixels + rect->h * pitch);
        renderdata->glBindTexture(textype, data->utexture);
        renderdata->glTexSubImage2D(textype, 0, rect->x / 2, rect->y / 2,
                                    (rect->w + 1) / 2, (rect->h + 1) / 2,
                                    GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, pixels);
    }
#endif

    return 0;
}

#if SDL_HAVE_YUV
static int GL_UpdateTextureYUV(SDL_Renderer *renderer, SDL_Texture *texture,
                               const SDL_Rect *rect,
                               const Uint8 *Yplane, int Ypitch,
                               const Uint8 *Uplane, int Upitch,
                               const Uint8 *Vplane, int Vpitch)
{
    GL_RenderData *renderdata = (GL_RenderData *)renderer->internal;
    const GLenum textype = renderdata->textype;
    GL_TextureData *data = (GL_TextureData *)texture->internal;

    GL_ActivateRenderer(renderer);

    renderdata->drawstate.texture = NULL; /* we trash this state. */

    renderdata->glBindTexture(textype, data->texture);
    renderdata->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    renderdata->glPixelStorei(GL_UNPACK_ROW_LENGTH, Ypitch);
    renderdata->glTexSubImage2D(textype, 0, rect->x, rect->y, rect->w,
                                rect->h, data->format, data->formattype,
                                Yplane);

    renderdata->glPixelStorei(GL_UNPACK_ROW_LENGTH, Upitch);
    renderdata->glBindTexture(textype, data->utexture);
    renderdata->glTexSubImage2D(textype, 0, rect->x / 2, rect->y / 2,
                                (rect->w + 1) / 2, (rect->h + 1) / 2,
                                data->format, data->formattype, Uplane);

    renderdata->glPixelStorei(GL_UNPACK_ROW_LENGTH, Vpitch);
    renderdata->glBindTexture(textype, data->vtexture);
    renderdata->glTexSubImage2D(textype, 0, rect->x / 2, rect->y / 2,
                                (rect->w + 1) / 2, (rect->h + 1) / 2,
                                data->format, data->formattype, Vplane);

    return GL_CheckError("glTexSubImage2D()", renderer);
}

static int GL_UpdateTextureNV(SDL_Renderer *renderer, SDL_Texture *texture,
                              const SDL_Rect *rect,
                              const Uint8 *Yplane, int Ypitch,
                              const Uint8 *UVplane, int UVpitch)
{
    GL_RenderData *renderdata = (GL_RenderData *)renderer->internal;
    const GLenum textype = renderdata->textype;
    GL_TextureData *data = (GL_TextureData *)texture->internal;

    GL_ActivateRenderer(renderer);

    renderdata->drawstate.texture = NULL; /* we trash this state. */

    renderdata->glBindTexture(textype, data->texture);
    renderdata->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    renderdata->glPixelStorei(GL_UNPACK_ROW_LENGTH, Ypitch);
    renderdata->glTexSubImage2D(textype, 0, rect->x, rect->y, rect->w,
                                rect->h, data->format, data->formattype,
                                Yplane);

    renderdata->glPixelStorei(GL_UNPACK_ROW_LENGTH, UVpitch / 2);
    renderdata->glBindTexture(textype, data->utexture);
    renderdata->glTexSubImage2D(textype, 0, rect->x / 2, rect->y / 2,
                                (rect->w + 1) / 2, (rect->h + 1) / 2,
                                GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, UVplane);

    return GL_CheckError("glTexSubImage2D()", renderer);
}
#endif

static int GPU_LockTexture(SDL_Renderer *renderer, SDL_Texture *texture,
                           const SDL_Rect *rect, void **pixels, int *pitch)
{
    GPU_TextureData *data = (GPU_TextureData *)texture->internal;

    data->locked_rect = *rect;
    *pixels =
        (void *)((Uint8 *)data->pixels + rect->y * data->pitch +
                 rect->x * SDL_BYTESPERPIXEL(texture->format));
    *pitch = data->pitch;
    return 0;
}

static void GPU_UnlockTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    GPU_TextureData *data = (GPU_TextureData *)texture->internal;
    const SDL_Rect *rect;
    void *pixels;

    rect = &data->locked_rect;
    pixels =
        (void *)((Uint8 *)data->pixels + rect->y * data->pitch +
                 rect->x * SDL_BYTESPERPIXEL(texture->format));
    GPU_UpdateTexture(renderer, texture, rect, pixels, data->pitch);
}

static void GPU_SetTextureScaleMode(SDL_Renderer *renderer, SDL_Texture *texture, SDL_ScaleMode scale_mode)
{
    /* nothing to do in this backend. */
}

static int GPU_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture)
{
    GPU_RenderData *data = (GPU_RenderData *)renderer->internal;

    data->state.render_target = texture;

    return 0;
}

static int GPU_QueueNoOp(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    return 0; /* nothing to do in this backend. */
}

static SDL_FColor GetDrawCmdColor(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    SDL_FColor color = cmd->data.color.color;

    if (SDL_RenderingLinearSpace(renderer)) {
        SDL_ConvertToLinear(&color);
    }

    color.r *= cmd->data.color.color_scale;
    color.g *= cmd->data.color.color_scale;
    color.b *= cmd->data.color.color_scale;

    return color;
}

static int GPU_QueueDrawPoints(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count)
{
    float *verts = (float *)SDL_AllocateRenderVertices(renderer, count * 2 * sizeof(float), 0, &cmd->data.draw.first);

    if (!verts) {
        return -1;
    }

    cmd->data.draw.count = count;
    for (int i = 0; i < count; i++) {
        *(verts++) = 0.5f + points[i].x;
        *(verts++) = 0.5f + points[i].y;
    }

    return 0;
}

static int GPU_QueueGeometry(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
                             const float *xy, int xy_stride, const SDL_FColor *color, int color_stride, const float *uv, int uv_stride,
                             int num_vertices, const void *indices, int num_indices, int size_indices,
                             float scale_x, float scale_y)
{
    int i;
    int count = indices ? num_indices : num_vertices;
    float *verts;
    size_t sz = 2 * sizeof(float) + 4 * sizeof(float) + (texture ? 2 : 0) * sizeof(float);
    const float color_scale = cmd->data.draw.color_scale;
    SDL_bool convert_color = SDL_RenderingLinearSpace(renderer);

    verts = (float *)SDL_AllocateRenderVertices(renderer, count * sz, 0, &cmd->data.draw.first);
    if (!verts) {
        return -1;
    }

    cmd->data.draw.count = count;
    size_indices = indices ? size_indices : 0;

    for (i = 0; i < count; i++) {
        int j;
        float *xy_;
        SDL_FColor col_;
        if (size_indices == 4) {
            j = ((const Uint32 *)indices)[i];
        } else if (size_indices == 2) {
            j = ((const Uint16 *)indices)[i];
        } else if (size_indices == 1) {
            j = ((const Uint8 *)indices)[i];
        } else {
            j = i;
        }

        xy_ = (float *)((char *)xy + j * xy_stride);

        *(verts++) = xy_[0] * scale_x;
        *(verts++) = xy_[1] * scale_y;

        col_ = *(SDL_FColor *)((char *)color + j * color_stride);
        if (convert_color) {
            SDL_ConvertToLinear(&col_);
        }

        // FIXME: The Vulkan backend doesn't multiply by color_scale. GL does. I'm not sure which one is wrong.
        *(verts++) = col_.r * color_scale;
        *(verts++) = col_.g * color_scale;
        *(verts++) = col_.b * color_scale;
        *(verts++) = col_.a;

        if (texture) {
            float *uv_ = (float *)((char *)uv + j * uv_stride);
            *(verts++) = uv_[0] * texture->w;
            *(verts++) = uv_[1] * texture->h;
        }
    }
    return 0;
}

static SDL_GpuGraphicsPipeline *SetupPipeline(
    GPU_RenderData *data,
    const SDL_RenderCommand *cmd,
    GPU_VertexShaderID vert_shader,
    GPU_FragmentShaderID frag_shader,
    SDL_GpuPrimitiveType prim_type)
{
    // TODO pipeline cache

    SDL_GpuColorAttachmentDescription ad = { 0 };

    if (data->state.render_target) {
        ad.format = ((GPU_TextureData *)data->state.render_target->internal)->format;
    } else {
        ad.format = data->swapchain.format;
    }

    SDL_BlendMode blend = cmd->data.draw.blend;
    ad.blendState.blendEnable = cmd->data.draw.blend != 0;
    ad.blendState.colorWriteMask = 0xF;
    ad.blendState.alphaBlendOp = GetBlendEquation(SDL_GetBlendModeAlphaOperation(blend));
    ad.blendState.dstAlphaBlendFactor = GetBlendFunc(SDL_GetBlendModeDstAlphaFactor(blend));
    ad.blendState.srcAlphaBlendFactor = GetBlendFunc(SDL_GetBlendModeSrcAlphaFactor(blend));
    ad.blendState.colorBlendOp = GetBlendEquation(SDL_GetBlendModeColorOperation(blend));
    ad.blendState.dstColorBlendFactor = GetBlendFunc(SDL_GetBlendModeDstColorFactor(blend));
    ad.blendState.srcColorBlendFactor = GetBlendFunc(SDL_GetBlendModeSrcColorFactor(blend));

    SDL_GpuGraphicsPipelineCreateInfo pci = { 0 };
    pci.attachmentInfo.hasDepthStencilAttachment = SDL_FALSE;
    pci.attachmentInfo.colorAttachmentCount = 1;
    pci.attachmentInfo.colorAttachmentDescriptions = &ad;
    pci.vertexShader = GPU_GetVertexShader(&data->shaders, vert_shader);
    pci.fragmentShader = GPU_GetFragmentShader(&data->shaders, frag_shader);
    pci.multisampleState.sampleCount = SDL_GPU_SAMPLECOUNT_1;
    pci.multisampleState.sampleMask = 0xFFFF;
    pci.primitiveType = prim_type;

    pci.rasterizerState.cullMode = SDL_GPU_CULLMODE_NONE;
    pci.rasterizerState.fillMode = SDL_GPU_FILLMODE_FILL;
    pci.rasterizerState.frontFace = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    SDL_GpuVertexBinding bind = { 0 };

    Uint32 num_attribs = 0;
    SDL_GpuVertexAttribute attribs[4] = { 0 };

    /* Position */
    attribs[num_attribs].location = num_attribs;
    attribs[num_attribs].format = SDL_GPU_VERTEXELEMENTFORMAT_VECTOR2;
    attribs[num_attribs].offset = bind.stride;
    bind.stride += 2 * sizeof(float);
    num_attribs++;

    if (prim_type == SDL_GPU_PRIMITIVETYPE_TRIANGLELIST) {
        /* Color */
        attribs[num_attribs].location = num_attribs;
        attribs[num_attribs].format = SDL_GPU_VERTEXELEMENTFORMAT_VECTOR4;
        attribs[num_attribs].offset = bind.stride;
        bind.stride += 4 * sizeof(float);
        num_attribs++;

        /* UVs */
        if (cmd->data.draw.texture) {
            attribs[num_attribs].location = num_attribs;
            attribs[num_attribs].format = SDL_GPU_VERTEXELEMENTFORMAT_VECTOR2;
            attribs[num_attribs].offset = bind.stride;
            bind.stride += 2 * sizeof(float);
            num_attribs++;
        }
    }

    pci.vertexInputState.vertexAttributeCount = num_attribs;
    pci.vertexInputState.vertexAttributes = attribs;
    pci.vertexInputState.vertexBindingCount = 1;
    pci.vertexInputState.vertexBindings = &bind;

    return SDL_GpuCreateGraphicsPipeline(data->device, &pci);
}

static void GPU_InvalidateCachedState(SDL_Renderer *renderer)
{
    GPU_RenderData *data = (GPU_RenderData *)renderer->internal;

    data->state.render_target = NULL;
    data->state.scissor_enabled = SDL_FALSE;
}

static SDL_GpuRenderPass *RestartRenderPass(GPU_RenderData *data)
{
    if (data->state.render_pass) {
        SDL_GpuEndRenderPass(data->state.render_pass);
    }

    data->state.render_pass = SDL_GpuBeginRenderPass(
        data->state.command_buffer, &data->state.color_attachment, 1, NULL);

    if (data->state.viewport.w > 0 && data->state.viewport.h > 0) {
        SDL_GpuSetViewport(data->state.render_pass, &data->state.viewport);
    }

    if (data->state.scissor_enabled) {
        SDL_GpuSetScissor(data->state.render_pass, &data->state.scissor);
    }

    data->state.color_attachment.loadOp = SDL_GPU_LOADOP_LOAD;

    return data->state.render_pass;
}

static void PushUniforms(GPU_RenderData *data, SDL_RenderCommand *cmd)
{
    GPU_ShaderUniformData uniforms = { 0 };
    uniforms.mvp.m[0][0] = 2.0f / data->state.viewport.w;
    uniforms.mvp.m[1][1] = -2.0f / data->state.viewport.h;
    uniforms.mvp.m[2][2] = 1.0f;
    uniforms.mvp.m[3][0] = -1.0f;
    uniforms.mvp.m[3][1] = 1.0f;
    uniforms.mvp.m[3][3] = 1.0f;

    uniforms.color = data->state.draw_color;

    if (cmd->data.draw.texture) {
        uniforms.texture_size[0] = cmd->data.draw.texture->w;
        uniforms.texture_size[1] = cmd->data.draw.texture->h;
    }

    SDL_GpuPushVertexUniformData(data->state.command_buffer, 0, &uniforms, sizeof(uniforms));
}

static SDL_GpuSampler **SamplerPointer(
    GPU_RenderData *data, SDL_TextureAddressMode address_mode, SDL_ScaleMode scale_mode)
{
    return &data->samplers[scale_mode][address_mode - 1];
}

static void Draw(
    GPU_RenderData *data, SDL_RenderCommand *cmd,
    Uint32 num_verts,
    Uint32 offset,
    SDL_GpuPrimitiveType prim)
{
    if (!data->state.render_pass) {
        RestartRenderPass(data);
    }

    GPU_VertexShaderID v_shader;
    GPU_FragmentShaderID f_shader;
    SDL_GpuRenderPass *pass = data->state.render_pass;
    GPU_TextureData *tdata = NULL;

    if (cmd->data.draw.texture) {
        tdata = (GPU_TextureData *)cmd->data.draw.texture->internal;
    }

    if (prim == SDL_GPU_PRIMITIVETYPE_TRIANGLELIST) {
        if (cmd->data.draw.texture) {
            v_shader = VERT_SHADER_TRI_TEXTURE;
            f_shader = tdata->shader;
        } else {
            v_shader = VERT_SHADER_TRI_COLOR;
            f_shader = FRAG_SHADER_COLOR;
        }
    } else {
        v_shader = VERT_SHADER_LINEPOINT;
        f_shader = FRAG_SHADER_COLOR;
    }

    SDL_GpuGraphicsPipeline *pipe = SetupPipeline(data, cmd, v_shader, f_shader, prim);
    SDL_GpuBindGraphicsPipeline(data->state.render_pass, pipe);

    if (tdata) {
        SDL_GpuTextureSamplerBinding sampler_bind = { 0 };
        sampler_bind.sampler = *SamplerPointer(data, cmd->data.draw.texture_address_mode, cmd->data.draw.texture->scaleMode);
        sampler_bind.texture = tdata->texture;
        SDL_GpuBindFragmentSamplers(pass, 0, &sampler_bind, 1);
    }

    SDL_GpuBufferBinding buffer_bind = { 0 };
    buffer_bind.buffer = data->vertices.buffer;
    buffer_bind.offset = offset;

    SDL_GpuBindVertexBuffers(pass, 0, &buffer_bind, 1);
    PushUniforms(data, cmd);
    SDL_GpuDrawPrimitives(data->state.render_pass, 0, num_verts);

    SDL_GpuReleaseGraphicsPipeline(data->device, pipe); // TODO remove when we have cache
}

static int UploadVertices(GPU_RenderData *data, void *vertices, size_t vertsize)
{
    if (vertsize == 0) {
        return 0;
    }

    SDL_assert(vertsize <= VERTEX_BUFFER_SIZE);
    memcpy(data->vertices.mapped_transfer_buf, vertices, vertsize);

    SDL_GpuCopyPass *pass = SDL_GpuBeginCopyPass(data->state.command_buffer);

    if (!pass) {
        return -1;
    }

    SDL_GpuTransferBufferLocation src = { 0 };
    src.transferBuffer = data->vertices.transfer_buf;

    SDL_GpuBufferRegion dst = { 0 };
    dst.buffer = data->vertices.buffer;
    dst.size = (Uint32)vertsize;

    SDL_GpuUploadToBuffer(pass, &src, &dst, SDL_TRUE);
    SDL_GpuEndCopyPass(pass);

    return 0;
}

static int GPU_RunCommandQueue(SDL_Renderer *renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize)
{
    GPU_RenderData *data = (GPU_RenderData *)renderer->internal;

    if (UploadVertices(data, vertices, vertsize) != 0) {
        return -1;
    }

    data->state.color_attachment.loadOp = SDL_GPU_LOADOP_LOAD;
    data->state.color_attachment.textureSlice.layer = 0;
    data->state.color_attachment.textureSlice.mipLevel = 0;

    if (renderer->target) {
        GPU_TextureData *tdata = renderer->target->internal;
        data->state.color_attachment.textureSlice.texture = tdata->texture;
    } else {
        data->state.color_attachment.textureSlice.texture = data->swapchain.texture;
    }

    if (!data->state.color_attachment.textureSlice.texture) {
        // FIXME is this an error? Happens if the swapchain texture couldn't be acquired
        return SDL_SetError("Render target texture is NULL");
    }

    while (cmd) {
        switch (cmd->command) {
        case SDL_RENDERCMD_SETDRAWCOLOR:
        {
            data->state.draw_color = GetDrawCmdColor(renderer, cmd);
            break;
        }

        case SDL_RENDERCMD_SETVIEWPORT:
        {
            SDL_Rect *viewport = &cmd->data.viewport.rect;
            data->state.viewport.x = viewport->x;
            data->state.viewport.y = viewport->y;
            data->state.viewport.w = viewport->w;
            data->state.viewport.h = viewport->h;
            data->state.viewport.minDepth = 0;
            data->state.viewport.maxDepth = 1;

            if (data->state.render_pass && viewport->w > 0 && viewport->h > 0) {
                SDL_GpuSetViewport(data->state.render_pass, &data->state.viewport);
            }

            break;
        }

        case SDL_RENDERCMD_SETCLIPRECT:
        {
            const SDL_Rect *rect = &cmd->data.cliprect.rect;
            data->state.scissor.x = rect->x;
            data->state.scissor.y = rect->y;
            data->state.scissor.w = rect->w;
            data->state.scissor.h = rect->h;
            data->state.scissor_enabled = cmd->data.cliprect.enabled;

            if (data->state.render_pass && cmd->data.cliprect.enabled) {
                // TODO clear scissor if disabled?
                SDL_GpuSetScissor(data->state.render_pass, &data->state.scissor);
            }

            break;
        }

        case SDL_RENDERCMD_CLEAR:
        {
            data->state.color_attachment.clearColor = GetDrawCmdColor(renderer, cmd);
            data->state.color_attachment.loadOp = SDL_GPU_LOADOP_CLEAR;

            if (data->state.render_pass) {
                RestartRenderPass(data);
            }

            break;
        }

        case SDL_RENDERCMD_FILL_RECTS: /* unused */
            break;

        case SDL_RENDERCMD_COPY: /* unused */
            break;

        case SDL_RENDERCMD_COPY_EX: /* unused */
            break;

        case SDL_RENDERCMD_DRAW_LINES:
        {
            Uint32 count = (Uint32)cmd->data.draw.count;
            Uint32 offset = (Uint32)cmd->data.draw.first;

            if (count > 2) {
                /* joined lines cannot be grouped */
                Draw(data, cmd, count, offset, SDL_GPU_PRIMITIVETYPE_LINESTRIP);
            } else {
                /* let's group non joined lines */
                SDL_RenderCommand *finalcmd = cmd;
                SDL_RenderCommand *nextcmd = cmd->next;
                SDL_BlendMode thisblend = cmd->data.draw.blend;

                while (nextcmd) {
                    const SDL_RenderCommandType nextcmdtype = nextcmd->command;
                    if (nextcmdtype != SDL_RENDERCMD_DRAW_LINES) {
                        break; /* can't go any further on this draw call, different render command up next. */
                    } else if (nextcmd->data.draw.count != 2) {
                        break; /* can't go any further on this draw call, those are joined lines */
                    } else if (nextcmd->data.draw.blend != thisblend) {
                        break; /* can't go any further on this draw call, different blendmode copy up next. */
                    } else {
                        finalcmd = nextcmd; /* we can combine copy operations here. Mark this one as the furthest okay command. */
                        count += (Uint32)nextcmd->data.draw.count;
                    }
                    nextcmd = nextcmd->next;
                }

                Draw(data, cmd, count, offset, SDL_GPU_PRIMITIVETYPE_LINELIST);
                cmd = finalcmd; /* skip any copy commands we just combined in here. */
            }
            break;
        }

        case SDL_RENDERCMD_DRAW_POINTS:
        case SDL_RENDERCMD_GEOMETRY:
        {
            /* as long as we have the same copy command in a row, with the
               same texture, we can combine them all into a single draw call. */
            SDL_Texture *thistexture = cmd->data.draw.texture;
            SDL_BlendMode thisblend = cmd->data.draw.blend;
            const SDL_RenderCommandType thiscmdtype = cmd->command;
            SDL_RenderCommand *finalcmd = cmd;
            SDL_RenderCommand *nextcmd = cmd->next;
            Uint32 count = (Uint32)cmd->data.draw.count;

            while (nextcmd) {
                const SDL_RenderCommandType nextcmdtype = nextcmd->command;
                if (nextcmdtype != thiscmdtype) {
                    break; /* can't go any further on this draw call, different render command up next. */
                } else if (nextcmd->data.draw.texture != thistexture || nextcmd->data.draw.blend != thisblend) {
                    // FIXME should we check address mode too?
                    break; /* can't go any further on this draw call, different texture/blendmode copy up next. */
                } else {
                    finalcmd = nextcmd; /* we can combine copy operations here. Mark this one as the furthest okay command. */
                    count += (Uint32)nextcmd->data.draw.count;
                }
                nextcmd = nextcmd->next;
            }

            Uint32 offset = (Uint32)cmd->data.draw.first;

            SDL_GpuPrimitiveType prim = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST; /* SDL_RENDERCMD_GEOMETRY */
            if (thiscmdtype == SDL_RENDERCMD_DRAW_POINTS) {
                prim = SDL_GPU_PRIMITIVETYPE_POINTLIST;
            }

            Draw(data, cmd, count, offset, prim);

            cmd = finalcmd; /* skip any copy commands we just combined in here. */
            break;
        }

        case SDL_RENDERCMD_NO_OP:
            break;
        }

        cmd = cmd->next;
    }

    if (data->state.color_attachment.loadOp && !data->state.render_pass) {
        RestartRenderPass(data);
    }

    if (data->state.render_pass) {
        SDL_GpuEndRenderPass(data->state.render_pass);
        data->state.render_pass = NULL;
    }

    return 0;
}

static SDL_Surface *GPU_RenderReadPixels(SDL_Renderer *renderer, const SDL_Rect *rect)
{
    SDL_Unsupported(); // TODO
    return NULL;
}

static void RenewSwapchain(SDL_Renderer *renderer)
{
    GPU_RenderData *data = (GPU_RenderData *)renderer->internal;

    data->swapchain.texture = SDL_GpuAcquireSwapchainTexture(
        data->state.command_buffer, renderer->window, &data->swapchain.width, &data->swapchain.height);

    if (data->swapchain.texture) {
        data->swapchain.format = SDL_GpuGetSwapchainTextureFormat(data->device, renderer->window);
    }
}

static int GPU_RenderPresent(SDL_Renderer *renderer)
{
    GPU_RenderData *data = (GPU_RenderData *)renderer->internal;

    SDL_GpuFence *next_fence = SDL_GpuSubmitAndAcquireFence(data->state.command_buffer);

    if (data->present_fence) {
        SDL_GpuWaitForFences(data->device, SDL_TRUE, &data->present_fence, 1);
        SDL_GpuReleaseFence(data->device, data->present_fence);
    }

    SDL_assert(next_fence != NULL);
    data->present_fence = next_fence;

    data->state.command_buffer = SDL_GpuAcquireCommandBuffer(data->device);
    RenewSwapchain(renderer);

    return 0;
}

static void GPU_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    GPU_RenderData *renderdata = (GPU_RenderData *)renderer->internal;
    GPU_TextureData *data = (GPU_TextureData *)texture->internal;

    if (renderdata->state.render_target == texture) {
        renderdata->state.render_target = NULL;
    }

    if (!data) {
        return;
    }

    SDL_GpuReleaseTexture(renderdata->device, data->texture);

#if SDL_HAVE_YUV
    if (data->yuv) {
        if (!data->utexture_external) {
            renderdata->glDeleteTextures(1, &data->utexture);
        }
        if (!data->vtexture_external) {
            renderdata->glDeleteTextures(1, &data->vtexture);
        }
    }
    if (data->nv12) {
        if (!data->utexture_external) {
            renderdata->glDeleteTextures(1, &data->utexture);
        }
    }
#endif
    SDL_free(data->pixels);
    SDL_free(data);
    texture->internal = NULL;
}

static void GPU_DestroyRenderer(SDL_Renderer *renderer)
{
    GPU_RenderData *data = (GPU_RenderData *)renderer->internal;

    if (!data) {
        return;
    }

    if (data->present_fence) {
        SDL_GpuWaitForFences(data->device, SDL_TRUE, &data->present_fence, 1);
        SDL_GpuReleaseFence(data->device, data->present_fence);
    }

    if (data->state.command_buffer) {
        SDL_GpuSubmit(data->state.command_buffer);
        data->state.command_buffer = NULL;
    }

    for (Uint32 i = 0; i < sizeof(data->samplers) / sizeof(SDL_GpuSampler *); ++i) {
        SDL_GpuReleaseSampler(data->device, ((SDL_GpuSampler **)data->samplers)[i]);
    }

    if (renderer->window) {
        SDL_GpuUnclaimWindow(data->device, renderer->window);
    }

    SDL_GpuUnmapTransferBuffer(data->device, data->vertices.transfer_buf);
    SDL_GpuReleaseTransferBuffer(data->device, data->vertices.transfer_buf);
    SDL_GpuReleaseBuffer(data->device, data->vertices.buffer);

    GPU_ReleaseShaders(&data->shaders, data->device);
    SDL_GpuDestroyDevice(data->device);

    SDL_free(data);
}

static int GPU_SetVSync(SDL_Renderer *renderer, const int vsync)
{
    return SDL_Unsupported(); // TODO
}

static int InitVertexBuffer(GPU_RenderData *data, Uint32 size)
{
    data->vertices.buffer = SDL_GpuCreateBuffer(data->device, SDL_GPU_BUFFERUSAGE_VERTEX_BIT, size);

    if (!data->vertices.buffer) {
        return -1;
    }

    data->vertices.transfer_buf = SDL_GpuCreateTransferBuffer(data->device, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, size);

    if (!data->vertices.transfer_buf) {
        return -1;
    }

    SDL_GpuMapTransferBuffer(data->device, data->vertices.transfer_buf, SDL_FALSE, &data->vertices.mapped_transfer_buf);

    return 0;
}

static int InitSamplers(GPU_RenderData *data)
{
    struct
    {
        struct
        {
            SDL_TextureAddressMode address_mode;
            SDL_ScaleMode scale_mode;
        } sdl;
        struct
        {
            SDL_GpuSamplerAddressMode address_mode;
            SDL_GpuFilter filter;
            Uint32 anisotropy;
        } gpu;
    } configs[] = {
        {
            { SDL_TEXTURE_ADDRESS_CLAMP, SDL_SCALEMODE_NEAREST },
            { SDL_GPU_SAMPLERADDRESSMODE_REPEAT, SDL_GPU_FILTER_NEAREST, 0 },
        },
        {
            { SDL_TEXTURE_ADDRESS_CLAMP, SDL_SCALEMODE_LINEAR },
            { SDL_GPU_SAMPLERADDRESSMODE_REPEAT, SDL_GPU_FILTER_LINEAR, 0 },
        },
        {
            { SDL_TEXTURE_ADDRESS_CLAMP, SDL_SCALEMODE_BEST },
            { SDL_GPU_SAMPLERADDRESSMODE_REPEAT, SDL_GPU_FILTER_LINEAR, 16 },
        },
        {
            { SDL_TEXTURE_ADDRESS_WRAP, SDL_SCALEMODE_NEAREST },
            { SDL_GPU_SAMPLERADDRESSMODE_REPEAT, SDL_GPU_FILTER_NEAREST, 0 },
        },
        {
            { SDL_TEXTURE_ADDRESS_WRAP, SDL_SCALEMODE_LINEAR },
            { SDL_GPU_SAMPLERADDRESSMODE_REPEAT, SDL_GPU_FILTER_LINEAR, 0 },
        },
        {
            { SDL_TEXTURE_ADDRESS_WRAP, SDL_SCALEMODE_BEST },
            { SDL_GPU_SAMPLERADDRESSMODE_REPEAT, SDL_GPU_FILTER_LINEAR, 16 },
        },
    };

    for (Uint32 i = 0; i < SDL_arraysize(configs); ++i) {
        SDL_GpuSamplerCreateInfo sci = { 0 };
        sci.maxAnisotropy = configs[i].gpu.anisotropy;
        sci.anisotropyEnable = configs[i].gpu.anisotropy > 0;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = configs[i].gpu.address_mode;
        sci.minFilter = sci.magFilter = configs[i].gpu.filter;

        SDL_GpuSampler *sampler = SDL_GpuCreateSampler(data->device, &sci);

        if (sampler == NULL) {
            return -1;
        }

        *SamplerPointer(data, configs[i].sdl.address_mode, configs[i].sdl.scale_mode) = sampler;
    }

    return 0;
}

static int GPU_CreateRenderer(SDL_Renderer *renderer, SDL_Window *window, SDL_PropertiesID create_props)
{
    GPU_RenderData *data = NULL;

    data = (GPU_RenderData *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        return -1;
    }

    renderer->internal = data;

    SDL_SetupRendererColorspace(renderer, create_props);

    if (renderer->output_colorspace != SDL_COLORSPACE_SRGB) {
        // TODO
        SDL_SetError("Unsupported output colorspace");
        goto error;
    }

    data->device = SDL_GpuCreateDevice(SDL_TRUE, SDL_FALSE, create_props);

    if (!data->device) {
        goto error;
    }

    if (GPU_InitShaders(&data->shaders, data->device) != 0) {
        goto error;
    }

    if (InitVertexBuffer(data, VERTEX_BUFFER_SIZE) != 0) {
        goto error;
    }

    if (InitSamplers(data) != 0) {
        goto error;
    }

    data->swapchain.composition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    data->swapchain.present_mode = SDL_GPU_PRESENTMODE_VSYNC;

    if (!SDL_GpuClaimWindow(data->device, window, data->swapchain.composition, data->swapchain.present_mode)) {
        goto error;
    }

    data->state.command_buffer = SDL_GpuAcquireCommandBuffer(data->device);
    RenewSwapchain(renderer);

    renderer->SupportsBlendMode = GPU_SupportsBlendMode;
    renderer->CreateTexture = GPU_CreateTexture;
    renderer->UpdateTexture = GPU_UpdateTexture;
#if SDL_HAVE_YUV
    renderer->UpdateTextureYUV = GL_UpdateTextureYUV;
    renderer->UpdateTextureNV = GL_UpdateTextureNV;
#endif
    renderer->LockTexture = GPU_LockTexture;
    renderer->UnlockTexture = GPU_UnlockTexture;
    renderer->SetTextureScaleMode = GPU_SetTextureScaleMode;
    renderer->SetRenderTarget = GPU_SetRenderTarget;
    renderer->QueueSetViewport = GPU_QueueNoOp;
    renderer->QueueSetDrawColor = GPU_QueueNoOp;
    renderer->QueueDrawPoints = GPU_QueueDrawPoints;
    renderer->QueueDrawLines = GPU_QueueDrawPoints; /* lines and points queue vertices the same way. */
    renderer->QueueGeometry = GPU_QueueGeometry;
    renderer->InvalidateCachedState = GPU_InvalidateCachedState;
    renderer->RunCommandQueue = GPU_RunCommandQueue;
    renderer->RenderReadPixels = GPU_RenderReadPixels;
    renderer->RenderPresent = GPU_RenderPresent;
    renderer->DestroyTexture = GPU_DestroyTexture;
    renderer->DestroyRenderer = GPU_DestroyRenderer;
    renderer->SetVSync = GPU_SetVSync;
    GPU_InvalidateCachedState(renderer);
    renderer->window = window;

    renderer->name = GL_RenderDriver.name;
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_ARGB8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_ABGR8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_XRGB8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_XBGR8888);

    // TODO YUV
#if SDL_HAVE_YUV
    /* We support YV12 textures using 3 textures and a shader */
    if (data->shaders && data->num_texture_units >= 3) {
        SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_YV12);
        SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_IYUV);
    }

    /* We support NV12 textures using 2 textures and a shader */
    if (data->shaders && data->num_texture_units >= 2) {
        SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_NV12);
        SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_NV21);
    }
#endif
#ifdef SDL_PLATFORM_MACOS
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_UYVY);
#endif

    renderer->rect_index_order[0] = 0;
    renderer->rect_index_order[1] = 1;
    renderer->rect_index_order[2] = 3;
    renderer->rect_index_order[3] = 1;
    renderer->rect_index_order[4] = 3;
    renderer->rect_index_order[5] = 2;

    data->state.draw_color.r = 1.0f;
    data->state.draw_color.g = 1.0f;
    data->state.draw_color.b = 1.0f;
    data->state.draw_color.a = 1.0f;

    return 0;

error:
    GPU_DestroyRenderer(renderer);
    return -1;
}

SDL_RenderDriver GPU_RenderDriver = {
    GPU_CreateRenderer, "gpu"
};

#endif /* SDL_VIDEO_RENDER_GPU */
