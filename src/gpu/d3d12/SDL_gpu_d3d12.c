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

#if SDL_GPU_D3D12

#define D3D12_NO_HELPERS
#define CINTERFACE
#define COBJMACROS

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>

#include "../SDL_gpu_driver.h"

/* Macros */

#define D3DCOMPILER_API STDMETHODCALLTYPE

#define ERROR_CHECK(msg)                                     \
    if (FAILED(res)) {                                       \
        D3D12_INTERNAL_LogError(renderer->device, msg, res); \
    }

#define ERROR_CHECK_RETURN(msg, ret)                         \
    if (FAILED(res)) {                                       \
        D3D12_INTERNAL_LogError(renderer->device, msg, res); \
        return ret;                                          \
    }

/* Defines */
#if defined(_WIN32)
#define D3D12_DLL     "d3d12.dll"
#define DXGI_DLL      "dxgi.dll"
#define DXGIDEBUG_DLL "dxgidebug.dll"
#elif defined(__APPLE__)
#define D3D12_DLL       "libdxvk_d3d12.dylib"
#define DXGI_DLL        "libdxvk_dxgi.dylib"
#define DXGIDEBUG_DLL   "libdxvk_dxgidebug.dylib"
#define D3DCOMPILER_DLL "libvkd3d-utils.1.dylib"
#else
#define D3D12_DLL       "libdxvk_d3d12.so"
#define DXGI_DLL        "libdxvk_dxgi.so"
#define DXGIDEBUG_DLL   "libdxvk_dxgidebug.so"
#define D3DCOMPILER_DLL "libvkd3d-utils.so.1"
#endif

#define D3D12_CREATE_DEVICE_FUNC      "D3D12CreateDevice"
#define CREATE_DXGI_FACTORY1_FUNC     "CreateDXGIFactory1"
#define D3DCOMPILE_FUNC               "D3DCompile"
#define DXGI_GET_DEBUG_INTERFACE_FUNC "DXGIGetDebugInterface"
#define WINDOW_PROPERTY_DATA          "SDL_GpuD3D12WindowPropertyData"
#define D3D_FEATURE_LEVEL_CHOICE      D3D_FEATURE_LEVEL_11_1
#define D3D_FEATURE_LEVEL_CHOICE_STR  "11_1"
#define SWAPCHAIN_BUFFER_COUNT        2

// #define SDL_GPU_SHADERSTAGE_COMPUTE 2

#ifdef _WIN32
#define HRESULT_FMT "(0x%08lX)"
#else
#define HRESULT_FMT "(0x%08X)"
#endif

/* Function Pointer Signatures */
typedef HRESULT(WINAPI *PFN_CREATE_DXGI_FACTORY1)(const GUID *riid, void **ppFactory);
typedef HRESULT(WINAPI *PFN_DXGI_GET_DEBUG_INTERFACE)(const GUID *riid, void **ppDebug);
typedef HRESULT(D3DCOMPILER_API *PFN_D3DCOMPILE)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName, const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude, LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2, ID3DBlob **ppCode, ID3DBlob **ppErrorMsgs);

/* IIDs (from https://www.magnumdb.com/) */
static const IID D3D_IID_IDXGIFactory1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
static const IID D3D_IID_IDXGIFactory4 = { 0x1bc6ea02, 0xef36, 0x464f, { 0xbf, 0x0c, 0x21, 0xca, 0x39, 0xe5, 0x16, 0x8a } };
static const IID D3D_IID_IDXGIFactory5 = { 0x7632e1f5, 0xee65, 0x4dca, { 0x87, 0xfd, 0x84, 0xcd, 0x75, 0xf8, 0x83, 0x8d } };
static const IID D3D_IID_IDXGIFactory6 = { 0xc1b6694f, 0xff09, 0x44a9, { 0xb0, 0x3c, 0x77, 0x90, 0x0a, 0x0a, 0x1d, 0x17 } };
static const IID D3D_IID_IDXGIAdapter1 = { 0x29038f61, 0x3839, 0x4626, { 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05 } };
static const IID D3D_IID_IDXGISwapChain3 = { 0x94d99bdb, 0xf1f8, 0x4ab0, { 0xb2, 0x36, 0x7d, 0xa0, 0x17, 0x0e, 0xda, 0xb1 } };
// static const IID D3D_IID_ID3DUserDefinedAnnotation = { 0xb2daad8b, 0x03d4, 0x4dbf, { 0x95, 0xeb, 0x32, 0xab, 0x4b, 0x63, 0xd0, 0xab } };
static const IID D3D_IID_IDXGIDebug = { 0x119e7452, 0xde9e, 0x40fe, { 0x88, 0x06, 0x88, 0xf9, 0x0c, 0x12, 0xb4, 0x41 } };

// static const IID D3D_IID_IDXGIInfoQueue = { 0xd67441c7, 0x672a, 0x476f, { 0x9e, 0x82, 0xcd, 0x55, 0xb4, 0x49, 0x49, 0xce } };

// static const GUID D3D_IID_D3DDebugObjectName = { 0x429b8c22, 0x9188, 0x4b0c, { 0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00 } };
// static const GUID D3D_IID_DXGI_DEBUG_ALL = { 0xe48ae283, 0xda80, 0x490b, { 0x87, 0xe6, 0x43, 0xe9, 0xa9, 0xcf, 0xda, 0x08 } };

static const IID D3D_IID_ID3D12Device = { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };
static const IID D3D_IID_ID3D12CommandQueue = { 0x0ec870a6, 0x5d7e, 0x4c22, { 0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed } };
static const IID D3D_IID_ID3D12DescriptorHeap = { 0x8efb471d, 0x616c, 0x4f49, { 0x90, 0xf7, 0x12, 0x7b, 0xb7, 0x63, 0xfa, 0x51 } };
static const IID D3D_IID_ID3D12Resource = { 0x696442be, 0xa72e, 0x4059, { 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad } };
static const IID D3D_IID_ID3D11Texture2D = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };

/* Conversions */

static DXGI_FORMAT SwapchainCompositionToTextureFormat[] = {
    DXGI_FORMAT_B8G8R8A8_UNORM,                /* SDR */
    DXGI_FORMAT_B8G8R8A8_UNORM, /* SDR_SRGB */ /* NOTE: The RTV uses the sRGB format */
    DXGI_FORMAT_R16G16B16A16_FLOAT,            /* HDR */
    DXGI_FORMAT_R10G10B10A2_UNORM,             /* HDR_ADVANCED*/
};

static DXGI_COLOR_SPACE_TYPE SwapchainCompositionToColorSpace[] = {
    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,   /* SDR */
    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,   /* SDR_SRGB */
    DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,   /* HDR */
    DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 /* HDR_ADVANCED */
};

/* Structures */
typedef struct D3D12Renderer D3D12Renderer;

typedef struct D3D12WindowData
{
    SDL_Window *window;
    IDXGISwapChain3 *swapchain;
    // D3D12Texture texture;
    // D3D12TextureContainer textureContainer;
    SDL_GpuPresentMode presentMode;
    SDL_GpuSwapchainComposition swapchainComposition;
    DXGI_FORMAT swapchainFormat;
    DXGI_COLOR_SPACE_TYPE swapchainColorSpace;
    // D3D12Fence *inFlightFences[MAX_FRAMES_IN_FLIGHT];
    Uint32 frameCounter;
} D3D12WindowData;

struct D3D12Renderer
{
    void *dxgidebug_dll;
    IDXGIDebug *dxgiDebug;
    void *d3dcompiler_dll;
    PFN_D3DCOMPILE D3DCompile_func;
    void *dxgi_dll;
    IDXGIFactory4 *factory;
    BOOL supportsTearing;
    IDXGIAdapter1 *adapter;
    void *d3d12_dll;
    ID3D12Device *device;
    SDL_Mutex *windowLock;
    size_t claimedWindowCount;
    size_t claimedWindowCapacity;
    D3D12WindowData **claimedWindows;
    ID3D12CommandQueue *commandQueue;
};

/* Logging */

static void D3D12_INTERNAL_LogError(
    ID3D12Device *device,
    const char *msg,
    HRESULT res)
{
#define MAX_ERROR_LEN 1024 /* FIXME: Arbitrary! */

    /* Buffer for text, ensure space for \0 terminator after buffer */
    char wszMsgBuff[MAX_ERROR_LEN + 1];
    DWORD dwChars; /* Number of chars returned. */

    if (res == DXGI_ERROR_DEVICE_REMOVED) {
        if (device) {
            res = ID3D12Device_GetDeviceRemovedReason(device);
        }
    }

    /* Try to get the message from the system errors. */
    dwChars = FormatMessage(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        res,
        0,
        wszMsgBuff,
        MAX_ERROR_LEN,
        NULL);

    /* No message? Screw it, just post the code. */
    if (dwChars == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s! Error Code: " HRESULT_FMT, msg, res);
        return;
    }

    /* Ensure valid range */
    dwChars = SDL_min(dwChars, MAX_ERROR_LEN);

    /* Trim whitespace from tail of message */
    while (dwChars > 0) {
        if (wszMsgBuff[dwChars - 1] <= ' ') {
            dwChars--;
        } else {
            break;
        }
    }

    /* Ensure null-terminated string */
    wszMsgBuff[dwChars] = '\0';

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s! Error Code: %s " HRESULT_FMT, msg, wszMsgBuff, res);
}

void D3D12_DestroyDevice(SDL_GpuDevice *device) { SDL_assert(SDL_FALSE); }

/* State Creation */

SDL_GpuComputePipeline *D3D12_CreateComputePipeline(
    SDL_GpuRenderer *driverData,
    SDL_GpuComputePipelineCreateInfo *pipelineCreateInfo) { SDL_assert(SDL_FALSE); }

SDL_GpuGraphicsPipeline *D3D12_CreateGraphicsPipeline(
    SDL_GpuRenderer *driverData,
    SDL_GpuGraphicsPipelineCreateInfo *pipelineCreateInfo) { SDL_assert(SDL_FALSE); }

SDL_GpuSampler *D3D12_CreateSampler(
    SDL_GpuRenderer *driverData,
    SDL_GpuSamplerCreateInfo *samplerCreateInfo) { SDL_assert(SDL_FALSE); }

SDL_GpuShader *D3D12_CreateShader(
    SDL_GpuRenderer *driverData,
    SDL_GpuShaderCreateInfo *shaderCreateInfo) { SDL_assert(SDL_FALSE); }

SDL_GpuTexture *D3D12_CreateTexture(
    SDL_GpuRenderer *driverData,
    SDL_GpuTextureCreateInfo *textureCreateInfo) { SDL_assert(SDL_FALSE); }

SDL_GpuBuffer *D3D12_CreateBuffer(
    SDL_GpuRenderer *driverData,
    SDL_GpuBufferUsageFlags usageFlags,
    Uint32 sizeInBytes) { SDL_assert(SDL_FALSE); }

SDL_GpuTransferBuffer *D3D12_CreateTransferBuffer(
    SDL_GpuRenderer *driverData,
    SDL_GpuTransferBufferUsage usage,
    Uint32 sizeInBytes) { SDL_assert(SDL_FALSE); }

/* Debug Naming */

void D3D12_SetBufferName(
    SDL_GpuRenderer *driverData,
    SDL_GpuBuffer *buffer,
    const char *text) { SDL_assert(SDL_FALSE); }

void D3D12_SetTextureName(
    SDL_GpuRenderer *driverData,
    SDL_GpuTexture *texture,
    const char *text) { SDL_assert(SDL_FALSE); }

void D3D12_InsertDebugLabel(
    SDL_GpuCommandBuffer *commandBuffer,
    const char *text) { SDL_assert(SDL_FALSE); }

void D3D12_PushDebugGroup(
    SDL_GpuCommandBuffer *commandBuffer,
    const char *name) { SDL_assert(SDL_FALSE); }

void D3D12_PopDebugGroup(
    SDL_GpuCommandBuffer *commandBuffer) { SDL_assert(SDL_FALSE); }

/* Disposal */

void D3D12_ReleaseTexture(
    SDL_GpuRenderer *driverData,
    SDL_GpuTexture *texture) { SDL_assert(SDL_FALSE); }

void D3D12_ReleaseSampler(
    SDL_GpuRenderer *driverData,
    SDL_GpuSampler *sampler) { SDL_assert(SDL_FALSE); }

void D3D12_ReleaseBuffer(
    SDL_GpuRenderer *driverData,
    SDL_GpuBuffer *buffer) { SDL_assert(SDL_FALSE); }

void D3D12_ReleaseTransferBuffer(
    SDL_GpuRenderer *driverData,
    SDL_GpuTransferBuffer *transferBuffer) { SDL_assert(SDL_FALSE); }

void D3D12_ReleaseShader(
    SDL_GpuRenderer *driverData,
    SDL_GpuShader *shader) { SDL_assert(SDL_FALSE); }

void D3D12_ReleaseComputePipeline(
    SDL_GpuRenderer *driverData,
    SDL_GpuComputePipeline *computePipeline) { SDL_assert(SDL_FALSE); }

void D3D12_ReleaseGraphicsPipeline(
    SDL_GpuRenderer *driverData,
    SDL_GpuGraphicsPipeline *graphicsPipeline) { SDL_assert(SDL_FALSE); }

/* Render Pass */

void D3D12_BeginRenderPass(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuColorAttachmentInfo *colorAttachmentInfos,
    Uint32 colorAttachmentCount,
    SDL_GpuDepthStencilAttachmentInfo *depthStencilAttachmentInfo) { SDL_assert(SDL_FALSE); }

void D3D12_BindGraphicsPipeline(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuGraphicsPipeline *graphicsPipeline) { SDL_assert(SDL_FALSE); }

void D3D12_SetViewport(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuViewport *viewport) { SDL_assert(SDL_FALSE); }

void D3D12_SetScissor(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuRect *scissor) { SDL_assert(SDL_FALSE); }

void D3D12_BindVertexBuffers(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstBinding,
    SDL_GpuBufferBinding *pBindings,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

void D3D12_BindIndexBuffer(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuBufferBinding *pBinding,
    SDL_GpuIndexElementSize indexElementSize) { SDL_assert(SDL_FALSE); }

void D3D12_BindVertexSamplers(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuTextureSamplerBinding *textureSamplerBindings,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

void D3D12_BindVertexStorageTextures(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuTextureSlice *storageTextureSlices,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

void D3D12_BindVertexStorageBuffers(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuBuffer **storageBuffers,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

void D3D12_BindFragmentSamplers(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuTextureSamplerBinding *textureSamplerBindings,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

void D3D12_BindFragmentStorageTextures(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuTextureSlice *storageTextureSlices,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

void D3D12_BindFragmentStorageBuffers(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuBuffer **storageBuffers,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

void D3D12_PushVertexUniformData(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 slotIndex,
    const void *data,
    Uint32 dataLengthInBytes) { SDL_assert(SDL_FALSE); }

void D3D12_PushFragmentUniformData(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 slotIndex,
    const void *data,
    Uint32 dataLengthInBytes) { SDL_assert(SDL_FALSE); }

void D3D12_DrawIndexedPrimitives(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 baseVertex,
    Uint32 startIndex,
    Uint32 primitiveCount,
    Uint32 instanceCount) { SDL_assert(SDL_FALSE); }

void D3D12_DrawPrimitives(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 vertexStart,
    Uint32 primitiveCount) { SDL_assert(SDL_FALSE); }

void D3D12_DrawPrimitivesIndirect(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuBuffer *buffer,
    Uint32 offsetInBytes,
    Uint32 drawCount,
    Uint32 stride) { SDL_assert(SDL_FALSE); }

void D3D12_DrawIndexedPrimitivesIndirect(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuBuffer *buffer,
    Uint32 offsetInBytes,
    Uint32 drawCount,
    Uint32 stride) { SDL_assert(SDL_FALSE); }

void D3D12_EndRenderPass(
    SDL_GpuCommandBuffer *commandBuffer) { SDL_assert(SDL_FALSE); }

/* Compute Pass */

void D3D12_BeginComputePass(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuStorageTextureReadWriteBinding *storageTextureBindings,
    Uint32 storageTextureBindingCount,
    SDL_GpuStorageBufferReadWriteBinding *storageBufferBindings,
    Uint32 storageBufferBindingCount) { SDL_assert(SDL_FALSE); }

void D3D12_BindComputePipeline(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuComputePipeline *computePipeline) { SDL_assert(SDL_FALSE); }

void D3D12_BindComputeStorageTextures(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuTextureSlice *storageTextureSlices,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

void D3D12_BindComputeStorageBuffers(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    SDL_GpuBuffer **storageBuffers,
    Uint32 bindingCount) { SDL_assert(SDL_FALSE); }

void D3D12_PushComputeUniformData(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 slotIndex,
    const void *data,
    Uint32 dataLengthInBytes) { SDL_assert(SDL_FALSE); }

void D3D12_DispatchCompute(
    SDL_GpuCommandBuffer *commandBuffer,
    Uint32 groupCountX,
    Uint32 groupCountY,
    Uint32 groupCountZ) { SDL_assert(SDL_FALSE); }

void D3D12_EndComputePass(
    SDL_GpuCommandBuffer *commandBuffer) { SDL_assert(SDL_FALSE); }

/* TransferBuffer Data */

void D3D12_MapTransferBuffer(
    SDL_GpuRenderer *device,
    SDL_GpuTransferBuffer *transferBuffer,
    SDL_bool cycle,
    void **ppData) { SDL_assert(SDL_FALSE); }

void D3D12_UnmapTransferBuffer(
    SDL_GpuRenderer *device,
    SDL_GpuTransferBuffer *transferBuffer) { SDL_assert(SDL_FALSE); }

void D3D12_SetTransferData(
    SDL_GpuRenderer *driverData,
    const void *source,
    SDL_GpuTransferBufferRegion *destination,
    SDL_bool cycle) { SDL_assert(SDL_FALSE); }

void D3D12_GetTransferData(
    SDL_GpuRenderer *driverData,
    SDL_GpuTransferBufferRegion *source,
    void *destination) { SDL_assert(SDL_FALSE); }

/* Copy Pass */

void D3D12_BeginCopyPass(
    SDL_GpuCommandBuffer *commandBuffer) { SDL_assert(SDL_FALSE); }

void D3D12_UploadToTexture(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuTextureTransferInfo *source,
    SDL_GpuTextureRegion *destination,
    SDL_bool cycle) { SDL_assert(SDL_FALSE); }

void D3D12_UploadToBuffer(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuTransferBufferLocation *source,
    SDL_GpuBufferRegion *destination,
    SDL_bool cycle) { SDL_assert(SDL_FALSE); }

void D3D12_CopyTextureToTexture(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuTextureLocation *source,
    SDL_GpuTextureLocation *destination,
    Uint32 w,
    Uint32 h,
    Uint32 d,
    SDL_bool cycle) { SDL_assert(SDL_FALSE); }

void D3D12_CopyBufferToBuffer(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuBufferLocation *source,
    SDL_GpuBufferLocation *destination,
    Uint32 size,
    SDL_bool cycle) { SDL_assert(SDL_FALSE); }

void D3D12_GenerateMipmaps(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuTexture *texture) { SDL_assert(SDL_FALSE); }

void D3D12_DownloadFromTexture(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuTextureRegion *source,
    SDL_GpuTextureTransferInfo *destination) { SDL_assert(SDL_FALSE); }

void D3D12_DownloadFromBuffer(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuBufferRegion *source,
    SDL_GpuTransferBufferLocation *destination) { SDL_assert(SDL_FALSE); }

void D3D12_EndCopyPass(
    SDL_GpuCommandBuffer *commandBuffer) { SDL_assert(SDL_FALSE); }

void D3D12_Blit(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_GpuTextureRegion *source,
    SDL_GpuTextureRegion *destination,
    SDL_GpuFilter filterMode,
    SDL_bool cycle) { SDL_assert(SDL_FALSE); }

/* Submission/Presentation */

SDL_bool D3D12_SupportsSwapchainComposition(
    SDL_GpuRenderer *driverData,
    SDL_Window *window,
    SDL_GpuSwapchainComposition swapchainComposition) { SDL_assert(SDL_FALSE); }

SDL_bool D3D12_SupportsPresentMode(
    SDL_GpuRenderer *driverData,
    SDL_Window *window,
    SDL_GpuPresentMode presentMode) { SDL_assert(SDL_FALSE); }

static D3D12WindowData *D3D12_INTERNAL_FetchWindowData(
    SDL_Window *window)
{
    SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    return (D3D12WindowData *)SDL_GetProperty(properties, WINDOW_PROPERTY_DATA, NULL);
}

static SDL_bool D3D12_INTERNAL_InitializeSwapchainTexture(
    D3D12Renderer *renderer,
    IDXGISwapChain3 *swapchain,
    DXGI_FORMAT swapchainFormat,
    DXGI_FORMAT rtvFormat)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc;
    HRESULT res;

    ID3D12DescriptorHeap *rtvHeap;
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = SWAPCHAIN_BUFFER_COUNT;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    res = ID3D12Device_CreateDescriptorHeap(renderer->device, &rtvHeapDesc,
                                            &D3D_IID_ID3D12DescriptorHeap,
                                            (void **)&rtvHeap);
    ERROR_CHECK_RETURN("Could not create descriptor heap!", 0);

    UINT rtvDescriptorSize = ID3D12Device_GetDescriptorHandleIncrementSize(renderer->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvDescriptor;
    SDL_zero(rtvDescriptor);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvHeap, &rtvDescriptor);

    ID3D12Resource *renderTargets[SWAPCHAIN_BUFFER_COUNT];
    SDL_zero(renderTargets);

    for (UINT i = 0; i < SWAPCHAIN_BUFFER_COUNT; ++i) {
        // Get a pointer to the back buffer
        res = IDXGISwapChain3_GetBuffer(swapchain, i,
                                        &D3D_IID_ID3D12Resource,
                                        (void **)&renderTargets[i]);
        ERROR_CHECK_RETURN("Could not get swapchain buffer descriptor heap!", 0);

        // Create an RTV for each buffer

        SDL_zero(rtvDesc);
        rtvDesc.Format = rtvFormat;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

        ID3D12Device_CreateRenderTargetView(renderer->device, renderTargets[i], &rtvDesc, rtvDescriptor);

        rtvDescriptor.ptr += 1 * rtvDescriptorSize;
    }
    return SDL_TRUE;
}

static SDL_bool D3D12_INTERNAL_CreateSwapchain(
    D3D12Renderer *renderer,
    D3D12WindowData *windowData,
    SDL_GpuSwapchainComposition swapchainComposition,
    SDL_GpuPresentMode presentMode)
{
    HWND dxgiHandle;
    int width, height;
    Uint32 i;
    DXGI_SWAP_CHAIN_DESC1 swapchainDesc;
    DXGI_FORMAT swapchainFormat;
    IDXGIFactory1 *pParent;
    IDXGISwapChain1 *swapchain;
    IDXGISwapChain3 *swapchain3;
    Uint32 colorSpaceSupport;
    HRESULT res;

    /* Get the DXGI handle */
#ifdef _WIN32
    dxgiHandle = (HWND)SDL_GetProperty(SDL_GetWindowProperties(windowData->window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
#else
    dxgiHandle = (HWND)windowData->window;
#endif

    /* Get the window size */
    SDL_GetWindowSize(windowData->window, &width, &height);

    swapchainFormat = SwapchainCompositionToTextureFormat[swapchainComposition];

    // Initialize the swapchain buffer descriptor
    swapchainDesc.Width = 0;
    swapchainDesc.Height = 0;
    swapchainDesc.Format = swapchainFormat;
    swapchainDesc.Stereo = SDL_FALSE;
    swapchainDesc.SampleDesc.Count = 1;
    swapchainDesc.SampleDesc.Quality = 0;
    swapchainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_UNORDERED_ACCESS | DXGI_USAGE_SHADER_INPUT;
    swapchainDesc.BufferCount = SWAPCHAIN_BUFFER_COUNT;
    swapchainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapchainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapchainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapchainDesc.Flags = 0;

    // Initialize the fullscreen descriptor (if needed)
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc = {};
    fullscreenDesc.RefreshRate.Numerator = 0;
    fullscreenDesc.RefreshRate.Denominator = 0;
    fullscreenDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    fullscreenDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;

    if (renderer->supportsTearing) {
        swapchainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    } else {
        swapchainDesc.Flags = 0;
    }

    /* Create the swapchain! */
    res = IDXGIFactory4_CreateSwapChainForHwnd(
        renderer->factory,
        (IUnknown *)renderer->commandQueue,
        dxgiHandle,
        &swapchainDesc,
        NULL,
        NULL,
        &swapchain);
    ERROR_CHECK_RETURN("Could not create swapchain", 0);

    res = IDXGISwapChain3_QueryInterface(
        swapchain,
        &D3D_IID_IDXGISwapChain3,
        (void **)&swapchain3);
    IDXGISwapChain1_Release(swapchain);
    ERROR_CHECK_RETURN("Could not create swapchain", 0);

    IDXGISwapChain3_CheckColorSpaceSupport(
        swapchain3,
        windowData->swapchainColorSpace,
        &colorSpaceSupport);

    if (!(colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Requested colorspace is unsupported!");
        return SDL_FALSE;
    }

    IDXGISwapChain3_SetColorSpace1(
        swapchain3,
        windowData->swapchainColorSpace);

    /*
     * The swapchain's parent is a separate factory from the factory that
     * we used to create the swapchain, and only that parent can be used to
     * set the window association. Trying to set an association on our factory
     * will silently fail and doesn't even verify arguments or return errors.
     * See https://gamedev.net/forums/topic/634235-dxgidisabling-altenter/4999955/
     */
    res = IDXGISwapChain_GetParent(
        swapchain3,
        &D3D_IID_IDXGIFactory1,
        (void **)&pParent);
    if (FAILED(res)) {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION,
            "Could not get swapchain parent! Error Code: " HRESULT_FMT,
            res);
    } else {
        /* Disable DXGI window crap */
        res = IDXGIFactory1_MakeWindowAssociation(
            pParent,
            dxgiHandle,
            DXGI_MWA_NO_WINDOW_CHANGES);
        if (FAILED(res)) {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "MakeWindowAssociation failed! Error Code: " HRESULT_FMT,
                res);
        }

        /* We're done with the parent now */
        IDXGIFactory1_Release(pParent);
    }
    /* Initialize the swapchain data */
    windowData->swapchain = swapchain3;
    windowData->presentMode = presentMode;
    windowData->swapchainComposition = swapchainComposition;
    windowData->swapchainFormat = swapchainFormat;
    windowData->swapchainColorSpace = SwapchainCompositionToColorSpace[swapchainComposition];
    windowData->frameCounter = 0;

    //    for (i = 0; i < MAX_FRAMES_IN_FLIGHT; i += 1) {
    //        windowData->inFlightFences[i] = NULL;
    //    }

    /* If a you are using a FLIP model format you can't create the swapchain as DXGI_FORMAT_B8G8R8A8_UNORM_SRGB.
     * You have to create the swapchain as DXGI_FORMAT_B8G8R8A8_UNORM and then set the render target view's format to DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
     */
    if (!D3D12_INTERNAL_InitializeSwapchainTexture(
            renderer,
            swapchain3,
            swapchainFormat,
            (swapchainComposition == SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR) ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : windowData->swapchainFormat)) {
        IDXGISwapChain_Release(swapchain3);
        return SDL_FALSE;
    }

    /* Initialize dummy container */
    // SDL_zerop(&windowData->textureContainer);
    // windowData->textureContainer.textures = SDL_calloc(1, sizeof(D3D12Texture *));

    return SDL_TRUE;
}

SDL_bool D3D12_ClaimWindow(
    SDL_GpuRenderer *driverData,
    SDL_Window *window,
    SDL_GpuSwapchainComposition swapchainComposition,
    SDL_GpuPresentMode presentMode)
{
    D3D12Renderer *renderer = (D3D12Renderer *)driverData;
    D3D12WindowData *windowData = D3D12_INTERNAL_FetchWindowData(window);

    if (windowData == NULL) {
        windowData = (D3D12WindowData *)SDL_malloc(sizeof(D3D12WindowData));
        SDL_zerop(windowData);
        windowData->window = window;

        if (D3D12_INTERNAL_CreateSwapchain(renderer, windowData, swapchainComposition, presentMode)) {
            SDL_SetProperty(SDL_GetWindowProperties(window), WINDOW_PROPERTY_DATA, windowData);

            SDL_LockMutex(renderer->windowLock);

            if (renderer->claimedWindowCount >= renderer->claimedWindowCapacity) {
                renderer->claimedWindowCapacity *= 2;
                renderer->claimedWindows = SDL_realloc(
                    renderer->claimedWindows,
                    renderer->claimedWindowCapacity * sizeof(D3D12WindowData *));
            }
            renderer->claimedWindows[renderer->claimedWindowCount] = windowData;
            renderer->claimedWindowCount += 1;

            SDL_UnlockMutex(renderer->windowLock);

            return SDL_TRUE;
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not create swapchain, failed to claim window!");
            SDL_free(windowData);
            return SDL_FALSE;
        }
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Window already claimed!");
        return SDL_FALSE;
    }
}

void D3D12_UnclaimWindow(
    SDL_GpuRenderer *driverData,
    SDL_Window *window) { SDL_assert(SDL_FALSE); }

SDL_bool D3D12_SetSwapchainParameters(
    SDL_GpuRenderer *driverData,
    SDL_Window *window,
    SDL_GpuSwapchainComposition swapchainComposition,
    SDL_GpuPresentMode presentMode) { SDL_assert(SDL_FALSE); }

SDL_GpuTextureFormat D3D12_GetSwapchainTextureFormat(
    SDL_GpuRenderer *driverData,
    SDL_Window *window) { SDL_assert(SDL_FALSE); }

SDL_GpuCommandBuffer *D3D12_AcquireCommandBuffer(
    SDL_GpuRenderer *driverData) { SDL_assert(SDL_FALSE); }

SDL_GpuTexture *D3D12_AcquireSwapchainTexture(
    SDL_GpuCommandBuffer *commandBuffer,
    SDL_Window *window,
    Uint32 *pWidth,
    Uint32 *pHeight) { SDL_assert(SDL_FALSE); }

void D3D12_Submit(
    SDL_GpuCommandBuffer *commandBuffer) { SDL_assert(SDL_FALSE); }

SDL_GpuFence *D3D12_SubmitAndAcquireFence(
    SDL_GpuCommandBuffer *commandBuffer) { SDL_assert(SDL_FALSE); }

void D3D12_Wait(
    SDL_GpuRenderer *driverData) { SDL_assert(SDL_FALSE); }

void D3D12_WaitForFences(
    SDL_GpuRenderer *driverData,
    SDL_bool waitAll,
    SDL_GpuFence **pFences,
    Uint32 fenceCount) { SDL_assert(SDL_FALSE); }

SDL_bool D3D12_QueryFence(
    SDL_GpuRenderer *driverData,
    SDL_GpuFence *fence) { SDL_assert(SDL_FALSE); }

void D3D12_ReleaseFence(
    SDL_GpuRenderer *driverData,
    SDL_GpuFence *fence) { SDL_assert(SDL_FALSE); }

/* Feature Queries */

SDL_bool D3D12_IsTextureFormatSupported(
    SDL_GpuRenderer *driverData,
    SDL_GpuTextureFormat format,
    SDL_GpuTextureType type,
    SDL_GpuTextureUsageFlags usage) { SDL_assert(SDL_FALSE); }

SDL_GpuSampleCount D3D12_GetBestSampleCount(
    SDL_GpuRenderer *driverData,
    SDL_GpuTextureFormat format,
    SDL_GpuSampleCount desiredSampleCount) { SDL_assert(SDL_FALSE); }

static SDL_bool D3D12_PrepareDriver(SDL_VideoDevice *_this)
{
    void *d3d12_dll, *dxgi_dll, *d3dcompiler_dll;
    PFN_D3D12_CREATE_DEVICE D3D12CreateDeviceFunc;
    PFN_CREATE_DXGI_FACTORY1 CreateDXGIFactoryFunc;
    PFN_D3DCOMPILE D3DCompileFunc;
    HRESULT res;
    ID3D12Device *device;

    IDXGIFactory1 *factory;
    IDXGIFactory4 *factory4;
    IDXGIFactory6 *factory6;
    IDXGIAdapter1 *adapter;

    /* Can we load D3D12? */

    d3d12_dll = SDL_LoadObject(D3D12_DLL);
    if (d3d12_dll == NULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "D3D12: Could not find " D3D12_DLL);
        return SDL_FALSE;
    }

    D3D12CreateDeviceFunc = (PFN_D3D12_CREATE_DEVICE)SDL_LoadFunction(
        d3d12_dll,
        D3D12_CREATE_DEVICE_FUNC);
    if (D3D12CreateDeviceFunc == NULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "D3D12: Could not find function " D3D12_CREATE_DEVICE_FUNC " in " D3D12_DLL);
        SDL_UnloadObject(d3d12_dll);
        return SDL_FALSE;
    }

    /* Can we load DXGI? */

    dxgi_dll = SDL_LoadObject(DXGI_DLL);
    if (dxgi_dll == NULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "D3D12: Could not find " DXGI_DLL);
        return SDL_FALSE;
    }

    CreateDXGIFactoryFunc = (PFN_CREATE_DXGI_FACTORY1)SDL_LoadFunction(
        dxgi_dll,
        CREATE_DXGI_FACTORY1_FUNC);
    if (CreateDXGIFactoryFunc == NULL) {
        SDL_UnloadObject(dxgi_dll);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "D3D12: Could not find function " CREATE_DXGI_FACTORY1_FUNC " in " DXGI_DLL);
        return SDL_FALSE;
    }

    /* Can we create a device? */

    /* Create the DXGI factory */
    res = CreateDXGIFactoryFunc(
        &D3D_IID_IDXGIFactory1,
        (void **)&factory);
    if (FAILED(res)) {
        SDL_UnloadObject(d3d12_dll);
        SDL_UnloadObject(dxgi_dll);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "D3D12: Could not create DXGIFactory");
        return SDL_FALSE;
    }

    /* Check for DXGI 1.4 support */
    res = IDXGIFactory1_QueryInterface(
        factory,
        &D3D_IID_IDXGIFactory4,
        (void **)&factory4);
    if (FAILED(res)) {
        IDXGIFactory1_Release(factory);
        SDL_UnloadObject(d3d12_dll);
        SDL_UnloadObject(dxgi_dll);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "D3D12: Failed to find DXGI1.4 support, required for DX12");
        return SDL_FALSE;
    }
    IDXGIFactory4_Release(factory4);

    res = IDXGIAdapter1_QueryInterface(
        factory,
        &D3D_IID_IDXGIFactory6,
        (void **)&factory6);
    if (SUCCEEDED(res)) {
        res = IDXGIFactory6_EnumAdapterByGpuPreference(
            factory6,
            0,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            &D3D_IID_IDXGIAdapter1,
            (void **)&adapter);
        IDXGIFactory6_Release(factory6);
    } else {
        res = IDXGIFactory1_EnumAdapters1(
            factory,
            0,
            &adapter);
    }
    if (FAILED(res)) {
        IDXGIFactory1_Release(factory);
        SDL_UnloadObject(d3d12_dll);
        SDL_UnloadObject(dxgi_dll);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "D3D12: Failed to find adapter for D3D12Device");
        return SDL_FALSE;
    }

    res = D3D12CreateDeviceFunc(
        (IUnknown *)adapter,
        D3D_FEATURE_LEVEL_CHOICE,
        &D3D_IID_ID3D12Device,
        (void **)&device);

    if (SUCCEEDED(res)) {
        ID3D12Device_Release(device);
    }
    IDXGIAdapter1_Release(adapter);
    IDXGIFactory1_Release(factory);

    SDL_UnloadObject(d3d12_dll);
    SDL_UnloadObject(dxgi_dll);

    if (FAILED(res)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "D3D12: Could not create D3D12Device with feature level " D3D_FEATURE_LEVEL_CHOICE_STR);
        return SDL_FALSE;
    }

    /* Can we load D3DCompiler? */

    d3dcompiler_dll = SDL_LoadObject(D3DCOMPILER_DLL);
    if (d3dcompiler_dll == NULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "D3D12: Could not find " D3DCOMPILER_DLL);
        return SDL_FALSE;
    }

    D3DCompileFunc = (PFN_D3DCOMPILE)SDL_LoadFunction(
        d3dcompiler_dll,
        D3DCOMPILE_FUNC);
    SDL_UnloadObject(d3dcompiler_dll); /* We're not going to call this function, so we can just unload now. */
    if (D3DCompileFunc == NULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "D3D12: Could not find function D3DCompile in " D3DCOMPILER_DLL);
        return SDL_FALSE;
    }

    return SDL_TRUE;
}

static void D3D12_INTERNAL_TryInitializeDXGIDebug(D3D12Renderer *renderer)
{
    PFN_DXGI_GET_DEBUG_INTERFACE DXGIGetDebugInterfaceFunc;
    HRESULT res;

    renderer->dxgidebug_dll = SDL_LoadObject(DXGIDEBUG_DLL);
    if (renderer->dxgidebug_dll == NULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Could not find " DXGIDEBUG_DLL);
        return;
    }

    DXGIGetDebugInterfaceFunc = (PFN_DXGI_GET_DEBUG_INTERFACE)SDL_LoadFunction(
        renderer->dxgidebug_dll,
        DXGI_GET_DEBUG_INTERFACE_FUNC);
    if (DXGIGetDebugInterfaceFunc == NULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Could not load function: " DXGI_GET_DEBUG_INTERFACE_FUNC);
        return;
    }

    res = DXGIGetDebugInterfaceFunc(&D3D_IID_IDXGIDebug, (void **)&renderer->dxgiDebug);
    if (FAILED(res)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Could not get IDXGIDebug interface");
    }

    /*
    res = DXGIGetDebugInterfaceFunc(&D3D_IID_IDXGIInfoQueue, (void **)&renderer->dxgiInfoQueue);
    if (FAILED(res)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Could not get IDXGIInfoQueue interface");
    }
    */
}

static void D3D12_INTERNAL_DestroyRenderer(D3D12Renderer **rendererRef)
{
    D3D12Renderer *renderer;
    renderer = *rendererRef;
    if (!renderer)
        return;
    *rendererRef = NULL;
    if (renderer->windowLock) {
        SDL_DestroyMutex(renderer->windowLock);
        renderer->windowLock = NULL;
    }

    if (renderer->commandQueue) {
        ID3D12CommandQueue_Release(renderer->commandQueue);
        renderer->commandQueue = NULL;
    }
    if (renderer->device) {
        ID3D12Device_Release(renderer->device);
        renderer->device = NULL;
    }
    if (renderer->adapter) {
        IDXGIAdapter1_Release(renderer->adapter);
        renderer->adapter = NULL;
    }
    if (renderer->factory) {
        IDXGIFactory4_Release(renderer->factory);
        renderer->factory = NULL;
    }
    if (renderer->dxgiDebug) {
        IDXGIDebug_Release(renderer->dxgiDebug);
        renderer->dxgiDebug = NULL;
    }
    if (renderer->d3d12_dll) {
        SDL_UnloadObject(renderer->d3d12_dll);
        renderer->d3d12_dll = NULL;
    }
    if (renderer->dxgi_dll) {
        SDL_UnloadObject(renderer->dxgi_dll);
        renderer->dxgi_dll = NULL;
    }
    if (renderer->d3dcompiler_dll) {
        SDL_UnloadObject(renderer->d3dcompiler_dll);
        renderer->d3dcompiler_dll = NULL;
    }
    if (renderer->dxgidebug_dll) {
        SDL_UnloadObject(renderer->dxgidebug_dll);
        renderer->dxgidebug_dll = NULL;
    }
    renderer->D3DCompile_func = NULL;
    SDL_free(renderer);
}

static SDL_GpuDevice *D3D12_CreateDevice(SDL_bool debugMode, SDL_bool preferLowPower)
{
    SDL_GpuDevice *result;
    D3D12Renderer *renderer;
    PFN_CREATE_DXGI_FACTORY1 CreateDXGIFactoryFunc;
    HRESULT res;
    IDXGIFactory1 *factory1;
    IDXGIFactory5 *factory5;
    IDXGIFactory6 *factory6;
    DXGI_ADAPTER_DESC1 adapterDesc;
    PFN_D3D12_CREATE_DEVICE D3D12CreateDeviceFunc;

    renderer = (D3D12Renderer *)SDL_malloc(sizeof(D3D12Renderer));
    SDL_zerop(renderer);

    /* Load the D3DCompiler library */
    renderer->d3dcompiler_dll = SDL_LoadObject(D3DCOMPILER_DLL);
    if (renderer->d3dcompiler_dll == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not find " D3DCOMPILER_DLL);
        D3D12_INTERNAL_DestroyRenderer(&renderer);
        return NULL;
    }

    renderer->D3DCompile_func = (PFN_D3DCOMPILE)SDL_LoadFunction(renderer->d3dcompiler_dll, D3DCOMPILE_FUNC);
    if (renderer->D3DCompile_func == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not load function: " D3DCOMPILE_FUNC);
        D3D12_INTERNAL_DestroyRenderer(&renderer);
        return NULL;
    }

    /* Load the DXGI library */
    renderer->dxgi_dll = SDL_LoadObject(DXGI_DLL);
    if (renderer->dxgi_dll == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not find " DXGI_DLL);
        D3D12_INTERNAL_DestroyRenderer(&renderer);
        return NULL;
    }

    /* Initialize the DXGI debug layer, if applicable */
    if (debugMode) {
        D3D12_INTERNAL_TryInitializeDXGIDebug(renderer);
    }

    /* Load the CreateDXGIFactory1 function */
    CreateDXGIFactoryFunc = (PFN_CREATE_DXGI_FACTORY1)SDL_LoadFunction(
        renderer->dxgi_dll,
        CREATE_DXGI_FACTORY1_FUNC);
    if (CreateDXGIFactoryFunc == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not load function: " CREATE_DXGI_FACTORY1_FUNC);
        D3D12_INTERNAL_DestroyRenderer(&renderer);
        return NULL;
    }

    /* Create the DXGI factory */
    res = CreateDXGIFactoryFunc(
        &D3D_IID_IDXGIFactory1,
        (void **)&factory1);
    if (FAILED(res)) {
        D3D12_INTERNAL_DestroyRenderer(&renderer);
        ERROR_CHECK_RETURN("Could not create DXGIFactory", NULL);
    }

    /* Check for DXGI 1.4 support */
    res = IDXGIFactory1_QueryInterface(
        factory1,
        &D3D_IID_IDXGIFactory4,
        (void **)&renderer->factory);
    if (FAILED(res)) {
        D3D12_INTERNAL_DestroyRenderer(&renderer);
        ERROR_CHECK_RETURN("DXGI1.4 support not found, required for DX12", NULL);
    }
    IDXGIFactory1_Release(factory1);

    /* Check for explicit tearing support */
    res = IDXGIFactory4_QueryInterface(
        renderer->factory,
        &D3D_IID_IDXGIFactory5,
        (void **)&factory5);
    if (SUCCEEDED(res)) {
        res = IDXGIFactory5_CheckFeatureSupport(
            factory5,
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &renderer->supportsTearing,
            sizeof(renderer->supportsTearing));
        if (FAILED(res)) {
            renderer->supportsTearing = FALSE;
        }
        IDXGIFactory5_Release(factory5);
    }

    /* Select the appropriate device for rendering */
    res = IDXGIAdapter1_QueryInterface(
        renderer->factory,
        &D3D_IID_IDXGIFactory6,
        (void **)&factory6);
    if (SUCCEEDED(res)) {
        res = IDXGIFactory6_EnumAdapterByGpuPreference(
            factory6,
            0,
            preferLowPower ? DXGI_GPU_PREFERENCE_MINIMUM_POWER : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            &D3D_IID_IDXGIAdapter1,
            (void **)&renderer->adapter);
        IDXGIFactory6_Release(factory6);
    } else {
        res = IDXGIFactory4_EnumAdapters1(
            renderer->factory,
            0,
            &renderer->adapter);
    }

    if (FAILED(res)) {
        D3D12_INTERNAL_DestroyRenderer(&renderer);
        ERROR_CHECK_RETURN("Could not find adapter for D3D12Device", NULL);
    }

    /* Get information about the selected adapter. Used for logging info. */
    res = IDXGIAdapter1_GetDesc1(renderer->adapter, &adapterDesc);
    if (FAILED(res)) {
        D3D12_INTERNAL_DestroyRenderer(&renderer);
        ERROR_CHECK_RETURN("Could not get adapter description", NULL);
    }

    /* Load the D3D library */
    renderer->d3d12_dll = SDL_LoadObject(D3D12_DLL);
    if (renderer->d3d12_dll == NULL) {
        D3D12_INTERNAL_DestroyRenderer(&renderer);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not find " D3D12_DLL);
        return NULL;
    }

    /* Load the CreateDevice function */
    D3D12CreateDeviceFunc = (PFN_D3D12_CREATE_DEVICE)SDL_LoadFunction(
        renderer->d3d12_dll,
        D3D12_CREATE_DEVICE_FUNC);
    if (D3D12CreateDeviceFunc == NULL) {
        D3D12_INTERNAL_DestroyRenderer(&renderer);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not load function: " D3D12_CREATE_DEVICE_FUNC);
        return NULL;
    }

    /* Create the D3D12Device */
    res = D3D12CreateDeviceFunc(
        (IUnknown *)renderer->adapter,
        D3D_FEATURE_LEVEL_CHOICE,
        &D3D_IID_ID3D12Device,
        (void **)&renderer->device);

    if (FAILED(res)) {
        D3D12_INTERNAL_DestroyRenderer(&renderer);
        ERROR_CHECK_RETURN("Could not create D3D12Device", NULL);
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    res = ID3D12Device_CreateCommandQueue(
        renderer->device,
        &queueDesc,
        &D3D_IID_ID3D12CommandQueue,
        (void **)&renderer->commandQueue);

    if (FAILED(res)) {
        D3D12_INTERNAL_DestroyRenderer(&renderer);
        ERROR_CHECK_RETURN("Could not create D3D12CommandQueue", NULL);
    }

    renderer->windowLock = SDL_CreateMutex();

    /* Create the SDL_Gpu Device */
    result = (SDL_GpuDevice *)SDL_malloc(sizeof(SDL_GpuDevice));
    SDL_zerop(result);
    ASSIGN_DRIVER(D3D12)
    result->driverData = (SDL_GpuRenderer *)renderer;

    return result;
}

SDL_GpuDriver D3D12Driver = {
    "D3D12",
    SDL_GPU_BACKEND_D3D12,
    D3D12_PrepareDriver,
    D3D12_CreateDevice
};

#endif /* SDL_GPU_D12 */
