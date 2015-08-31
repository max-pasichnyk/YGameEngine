#include "D3D12Renderer/PrecompiledHeader.h"
#include "D3D12Renderer/D3D12GPUOutputBuffer.h"
#include "D3D12Renderer/D3D12GPUDevice.h"
#include "D3D12Renderer/D3D12GPUContext.h"
#include "D3D12Renderer/D3D12RenderBackend.h"
#include "D3D12Renderer/D3D12Helpers.h"
#include "Engine/SDLHeaders.h"
Log_SetChannel(D3D12GPUOutputBuffer);

// fix up a warning
#ifdef SDL_VIDEO_DRIVER_WINDOWS
    #undef WIN32_LEAN_AND_MEAN
#endif
#include <SDL/SDL_syswm.h>

static const char *SDL_D3D11_RENDERER_OUTPUT_WINDOW_POINTER_STRING = "D3D12RendererOutputBufferPtr";

static uint32 CalculateDXGISwapChainBufferCount(bool exclusiveFullscreen, RENDERER_VSYNC_TYPE vsyncType)
{
    //DebugAssert(D3D12RenderBackend::GetInstance()->GetFrameLatency() > 0);
    //return 2 + (D3D12RenderBackend::GetInstance()->GetFrameLatency() - 1) + ((vsyncType == RENDERER_VSYNC_TYPE_TRIPLE_BUFFERING) ? 1 : 0);
    return 2 + ((vsyncType == RENDERER_VSYNC_TYPE_TRIPLE_BUFFERING) ? 1 : 0);
}

D3D12GPUOutputBuffer::D3D12GPUOutputBuffer(D3D12RenderBackend *pBackend, ID3D12Device *pD3DDevice, IDXGISwapChain3 *pDXGISwapChain, HWND hWnd, uint32 width, uint32 height, PIXEL_FORMAT backBufferFormat, PIXEL_FORMAT depthStencilFormat, DXGI_FORMAT backBufferDXGIFormat, DXGI_FORMAT depthStencilDXGIFormat, RENDERER_VSYNC_TYPE vsyncType)
    : GPUOutputBuffer(vsyncType)
    , m_pBackend(pBackend)
    , m_pD3DDevice(pD3DDevice)
    , m_pDXGISwapChain(pDXGISwapChain)
    , m_hWnd(hWnd)
    , m_width(width)
    , m_height(height)
    , m_backBufferFormat(backBufferFormat)
    , m_depthStencilFormat(depthStencilFormat)
    , m_backBufferDXGIFormat(backBufferDXGIFormat)
    , m_depthStencilDXGIFormat(depthStencilDXGIFormat)
    , m_currentBackBufferIndex(pDXGISwapChain->GetCurrentBackBufferIndex())
    , m_pDepthStencilBuffer(nullptr)
{

}

D3D12GPUOutputBuffer::~D3D12GPUOutputBuffer()
{
    InternalReleaseBuffers();

    SAFE_RELEASE(m_pDXGISwapChain);
}

D3D12GPUOutputBuffer *D3D12GPUOutputBuffer::Create(D3D12RenderBackend *pBackend, IDXGIFactory4 *pDXGIFactory, ID3D12Device *pD3DDevice, ID3D12CommandQueue *pCommandQueue, HWND hWnd, PIXEL_FORMAT backBufferFormat, PIXEL_FORMAT depthStencilFormat, RENDERER_VSYNC_TYPE vsyncType)
{
    HRESULT hResult;

    // select formats
    DXGI_FORMAT backBufferDXGIFormat = D3D12Helpers::PixelFormatToDXGIFormat(backBufferFormat);
    DXGI_FORMAT depthStencilDXGIFormat = (depthStencilFormat != PIXEL_FORMAT_UNKNOWN) ? D3D12Helpers::PixelFormatToDXGIFormat(depthStencilFormat) : DXGI_FORMAT_UNKNOWN;
    if (backBufferDXGIFormat == DXGI_FORMAT_UNKNOWN || (depthStencilFormat != PIXEL_FORMAT_UNKNOWN && depthStencilDXGIFormat == DXGI_FORMAT_UNKNOWN))
    {
        Log_ErrorPrintf("Invalid swap chain format (%s / %s)", NameTable_GetNameString(NameTables::PixelFormat, backBufferFormat), NameTable_GetNameString(NameTables::PixelFormat, depthStencilFormat));
        return false;
    }

    // get client rect of the window
    RECT clientRect;
    GetClientRect(hWnd, &clientRect);
    uint32 width = Max(clientRect.right - clientRect.left, (LONG)1);
    uint32 height = Max(clientRect.bottom - clientRect.top, (LONG)1);

    // setup swap chain desc
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc;
    Y_memzero(&swapChainDesc, sizeof(swapChainDesc));
    swapChainDesc.Width = width;
    swapChainDesc.Height = height;
    swapChainDesc.Format = backBufferDXGIFormat;
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_BACK_BUFFER;
    swapChainDesc.BufferCount = CalculateDXGISwapChainBufferCount(false, vsyncType);
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapChainDesc.Flags = 0;

    // create the swapchain
    IDXGISwapChain1 *pDXGISwapChain1;
    hResult = pDXGIFactory->CreateSwapChainForHwnd(pCommandQueue, hWnd, &swapChainDesc, nullptr, nullptr, &pDXGISwapChain1);
    if (FAILED(hResult))
    {
        Log_ErrorPrintf("CreateSwapChainForHwnd failed with hResult %08X.", hResult);
        return nullptr;
    }

    // disable alt+enter, we handle it elsewhere
    hResult = pDXGIFactory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(hResult))
    {
        Log_ErrorPrintf("MakeWindowAssociation failed with hResult %08X.", hResult);
        pDXGISwapChain1->Release();
        return nullptr;
    }

    // query IDXGISwapChain3
    IDXGISwapChain3 *pDXGISwapChain;
    hResult = pDXGISwapChain1->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&pDXGISwapChain);
    if (FAILED(hResult))
    {
        Log_ErrorPrintf("IDXGISwapChain1::QueryInterface failed with hResult %08X.", hResult);
        pDXGISwapChain1->Release();
        return nullptr;
    }

    // reference to version 1 not needed
    pDXGISwapChain1->Release();

    // create object
    D3D12GPUOutputBuffer *pOutputBuffer = new D3D12GPUOutputBuffer(pBackend, pD3DDevice, pDXGISwapChain, hWnd, width, height, backBufferFormat, depthStencilFormat, backBufferDXGIFormat, depthStencilDXGIFormat, vsyncType);

    // create buffers
    if (!pOutputBuffer->InternalCreateBuffers())
    {
        pOutputBuffer->Release();
        return nullptr;
    }

    // done
    return pOutputBuffer;
}

ID3D12Resource *D3D12GPUOutputBuffer::GetCurrentBackBufferResource() const
{
    DebugAssert(m_currentBackBufferIndex < m_backBuffers.GetSize());
    return m_backBuffers[m_currentBackBufferIndex];
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12GPUOutputBuffer::GetCurrentBackBufferViewDescriptorCPUHandle() const
{
    return m_renderTargetViewsDescriptorStart.GetOffsetCPUHandle(m_currentBackBufferIndex);
}

void D3D12GPUOutputBuffer::UpdateCurrentBackBuffer()
{
    uint32 newBackBufferIndex = m_pDXGISwapChain->GetCurrentBackBufferIndex();
    DebugAssert(newBackBufferIndex < m_backBuffers.GetSize());
    if (newBackBufferIndex != m_currentBackBufferIndex)
        Log_DevPrintf("Update backbuffer index = %u", newBackBufferIndex);

    m_currentBackBufferIndex = newBackBufferIndex;
}

void D3D12GPUOutputBuffer::InternalResizeBuffers(uint32 width, uint32 height, RENDERER_VSYNC_TYPE vsyncType)
{
    HRESULT hResult;

    // check if fullscreen state was lost
    BOOL newFullscreenState;
    hResult = m_pDXGISwapChain->GetFullscreenState(&newFullscreenState, nullptr);
    if (FAILED(hResult))
        newFullscreenState = FALSE;

    // release resources
    InternalReleaseBuffers();

    // calculate buffer count
    uint32 bufferCount = CalculateDXGISwapChainBufferCount((newFullscreenState == TRUE), m_vsyncType);
    Log_DevPrintf("New swap chain buffer count = %u", bufferCount);

    // invoke resize
    hResult = m_pDXGISwapChain->ResizeBuffers(bufferCount, width, height, m_backBufferDXGIFormat, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
    if (FAILED(hResult))
        Panic("IDXGISwapChain::ResizeBuffers failed.");

    // update attributes
    m_width = width;
    m_height = height;
    m_vsyncType = vsyncType;

    // recreate textures
    if (!InternalCreateBuffers())
        Panic("Failed to recreate texture objects on resized swap chain.");
}

bool D3D12GPUOutputBuffer::InternalCreateBuffers()
{
    HRESULT hResult;

    // get descriptor
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc;
    m_pDXGISwapChain->GetDesc1(&swapChainDesc);

    // find the current backbuffer index
    m_currentBackBufferIndex = m_pDXGISwapChain->GetCurrentBackBufferIndex();
    m_backBuffers.Reserve(swapChainDesc.BufferCount);
    Log_DevPrintf("Current backbuffer index = %u", m_currentBackBufferIndex);

    // allocate RTV descriptors
    if (!m_pBackend->GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)->AllocateRange(swapChainDesc.BufferCount, &m_renderTargetViewsDescriptorStart))
    {
        Log_ErrorPrintf("Failed to allocate RTV descriptors.");
        return false;
    }

    // create render target views
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc;
    rtvDesc.Format = m_backBufferDXGIFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice = 0;
    rtvDesc.Texture2D.PlaneSlice = 0;
    for (uint32 i = 0; i < swapChainDesc.BufferCount; i++)
    {
        // get a pointer to this backbuffer
        ID3D12Resource *pBackBuffer;
        hResult = m_pDXGISwapChain->GetBuffer(i, __uuidof(ID3D12Resource), (void **)&pBackBuffer);
        if (FAILED(hResult))
        {
            Log_ErrorPrintf("IDXGISwapChain::GetBuffer failed with hResult %08X", hResult);
            m_pBackend->GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)->Free(m_renderTargetViewsDescriptorStart);
            return false;
        }

        // pass through
        m_pD3DDevice->CreateRenderTargetView(pBackBuffer, &rtvDesc, m_renderTargetViewsDescriptorStart.GetOffsetCPUHandle(i));
        m_backBuffers.Add(pBackBuffer);
    }

    // allocate depth stencil buffer
    if (m_depthStencilDXGIFormat != DXGI_FORMAT_UNKNOWN)
    {
        // create resource
        D3D12_HEAP_PROPERTIES heapProperties = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
        D3D12_RESOURCE_DESC resourceDesc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, m_width, m_height, 1, 1, m_depthStencilDXGIFormat,{ 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL };
        hResult = m_pD3DDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, __uuidof(ID3D12Resource), (void **)&m_pDepthStencilBuffer);
        if (FAILED(hResult))
        {
            Log_ErrorPrintf("CreateCommittedResource for DepthStencil failed with hResult %08X", hResult);
            m_pBackend->GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)->Free(m_renderTargetViewsDescriptorStart);
            return false;
        }

        // allocate DSV descriptor
        if (!m_pBackend->GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV)->Allocate(&m_depthStencilViewDescriptor))
        {
            Log_ErrorPrintf("Failed to allocate DSV descriptors.");
            m_pBackend->GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)->Free(m_renderTargetViewsDescriptorStart);
            SAFE_RELEASE(m_pDepthStencilBuffer);
            return false;
        }

        // create depth stencil buffer views
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
        dsvDesc.Format = m_depthStencilDXGIFormat;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
        dsvDesc.Texture2D.MipSlice = 0;
        m_pD3DDevice->CreateDepthStencilView(m_pDepthStencilBuffer, &dsvDesc, m_depthStencilViewDescriptor);
    }

    return true;
}

void D3D12GPUOutputBuffer::InternalReleaseBuffers()
{
    D3D12RenderBackend::GetInstance()->GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)->Free(m_renderTargetViewsDescriptorStart);
    D3D12RenderBackend::GetInstance()->GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV)->Free(m_depthStencilViewDescriptor);

    for (uint32 i = 0; i < m_backBuffers.GetSize(); i++)
        m_backBuffers[i]->Release();
    m_backBuffers.Clear();

    SAFE_RELEASE(m_pDepthStencilBuffer);
}

void D3D12GPUOutputBuffer::SetVSyncType(RENDERER_VSYNC_TYPE vsyncType)
{
    if (m_vsyncType == vsyncType)
        return;

    if (vsyncType == RENDERER_VSYNC_TYPE_TRIPLE_BUFFERING || m_vsyncType == RENDERER_VSYNC_TYPE_TRIPLE_BUFFERING)
        InternalResizeBuffers(m_width, m_height, vsyncType);
    else
        m_vsyncType = vsyncType;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

GPUOutputBuffer *D3D12GPUDevice::CreateOutputBuffer(RenderSystemWindowHandle hWnd, RENDERER_VSYNC_TYPE vsyncType)
{
    // can only be done on the render thread
    DebugAssert(m_pGPUContext != nullptr);
    return D3D12GPUOutputBuffer::Create(m_pBackend, m_pDXGIFactory, m_pD3DDevice, m_pGPUContext->GetD3DCommandQueue(), (HWND)hWnd, m_outputBackBufferFormat, m_outputDepthStencilFormat, vsyncType);
}

GPUOutputBuffer *D3D12GPUDevice::CreateOutputBuffer(SDL_Window *pSDLWindow, RENDERER_VSYNC_TYPE vsyncType)
{
    // retreive the hwnd from the sdl window
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(pSDLWindow, &info))
    {
        Log_ErrorPrintf("SDL_GetWindowWMInfo failed: %s", SDL_GetError());
        return false;
    }

    return CreateOutputBuffer((RenderSystemWindowHandle)info.info.win.window, vsyncType);
}
