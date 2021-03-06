#include "D3D12Renderer/PrecompiledHeader.h"
#include "D3D12Renderer/D3D12Common.h"
#include "D3D12Renderer/D3D12GPUDevice.h"
#include "D3D12Renderer/D3D12GPUContext.h"
#include "D3D12Renderer/D3D12GPUCommandList.h"
#include "D3D12Renderer/D3D12GPUBuffer.h"
#include "D3D12Renderer/D3D12GPUTexture.h"
#include "D3D12Renderer/D3D12GPUShaderProgram.h"
#include "D3D12Renderer/D3D12GPUOutputBuffer.h"
#include "D3D12Renderer/D3D12Helpers.h"
#include "Renderer/ShaderConstantBuffer.h"
Log_SetChannel(D3D12RenderBackend);

D3D12GPUContext::D3D12GPUContext(D3D12GPUDevice *pDevice, ID3D12Device *pD3DDevice, D3D12CommandQueue *pGraphicsCommandQueue)
    : m_pDevice(pDevice)
    , m_pD3DDevice(pD3DDevice)
    , m_pGraphicsCommandQueue(pGraphicsCommandQueue)
    , m_pConstants(nullptr)
    , m_pCommandList(nullptr)
    , m_pCurrentScratchBuffer(nullptr)
    , m_pCurrentScratchViewHeap(nullptr)
    , m_pCurrentScratchSamplerHeap(nullptr)
    , m_commandCounter(0)
{
    m_pDevice->AddRef();
    m_pDevice->SetImmediateContext(this);

    // null memory
    Y_memzero(&m_currentViewport, sizeof(m_currentViewport));
    Y_memzero(&m_scissorRect, sizeof(m_scissorRect));
    m_currentTopology = DRAW_TOPOLOGY_UNDEFINED;
    m_currentD3DTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;

    // null current states
    Y_memzero(m_pCurrentVertexBuffers, sizeof(m_pCurrentVertexBuffers));
    Y_memzero(m_currentVertexBufferOffsets, sizeof(m_currentVertexBufferOffsets));
    Y_memzero(m_currentVertexBufferStrides, sizeof(m_currentVertexBufferStrides));
    m_currentVertexBufferBindCount = 0;

    m_pCurrentIndexBuffer = nullptr;
    m_currentIndexFormat = GPU_INDEX_FORMAT_COUNT;
    m_currentIndexBufferOffset = 0;

    m_pCurrentShaderProgram = nullptr;
    m_shaderStates.Resize(SHADER_PROGRAM_STAGE_COUNT);
    m_shaderStates.ZeroContents();
    m_perDrawConstantBuffers.Resize(D3D12_LEGACY_GRAPHICS_ROOT_PER_DRAW_CONSTANT_BUFFER_SLOTS);
    m_perDrawConstantBufferCount = 0;
    m_perDrawConstantBuffersDirty = false;

    m_pCurrentRasterizerState = nullptr;
    m_pCurrentDepthStencilState = nullptr;
    m_currentDepthStencilRef = 0;
    m_pCurrentBlendState = nullptr;
    m_currentBlendStateBlendFactors.SetZero();
    m_pipelineChanged = false;

    m_pCurrentSwapChain = nullptr;

    Y_memzero(m_pCurrentRenderTargetViews, sizeof(m_pCurrentRenderTargetViews));
    m_pCurrentDepthBufferView = nullptr;
    m_nCurrentRenderTargets = 0;

    m_pCurrentPredicate = nullptr;
    //m_pCurrentPredicateD3D = nullptr;
    m_predicateBypassCount = 0;
}

D3D12GPUContext::~D3D12GPUContext()
{
    // execute all pending commands and wait until they're completed
    if (m_pCommandList != nullptr)
    {
        CloseAndExecuteCommandList(false, false);
        m_pGraphicsCommandQueue->ReleaseCommandList(m_pCommandList);
    }

    // release everything back to the queue (not done above because we don't want another one after this)
    uint64 syncFence = m_pGraphicsCommandQueue->CreateSynchronizationPoint();
    if (m_pCurrentCommandAllocator != nullptr)
    {
        m_pGraphicsCommandQueue->ReleaseCommandAllocator(m_pCurrentCommandAllocator, syncFence);
        m_pCurrentCommandAllocator = nullptr;
    }
    if (m_pCurrentScratchBuffer != nullptr)
    {
        m_pGraphicsCommandQueue->ReleaseLinearBufferHeap(m_pCurrentScratchBuffer, syncFence);
        m_pCurrentScratchBuffer = nullptr;
    }
    if (m_pCurrentScratchViewHeap != nullptr)
    {
        m_pGraphicsCommandQueue->ReleaseLinearViewHeap(m_pCurrentScratchViewHeap, syncFence);
        m_pCurrentScratchViewHeap = nullptr;
    }
    if (m_pCurrentScratchSamplerHeap != nullptr)
    {
        m_pGraphicsCommandQueue->ReleaseLinearSamplerHeap(m_pCurrentScratchSamplerHeap, syncFence);
        m_pCurrentScratchSamplerHeap = nullptr;
    }

    // wait for the gpu to catch up
    m_pGraphicsCommandQueue->WaitForFence(syncFence);

#if 0
    // clear any state
    ClearState(true, true, true, true);
    m_pD3DContext->OMSetRenderTargets(0, nullptr, nullptr);
    m_pD3DContext->ClearState();
    m_pD3DContext->Flush();

    DebugAssert(m_predicateBypassCount == 0);
    if (m_pCurrentPredicateD3D != nullptr)
    {
        m_pD3DContext->SetPredication(nullptr, FALSE);
        m_pCurrentPredicateD3D = nullptr;
    }
    SAFE_RELEASE(m_pCurrentPredicate);
#endif

    // release our references to states and targets. d3d references will die when the device goes down.
    for (uint32 i = 0; i < countof(m_pCurrentVertexBuffers); i++) {
        SAFE_RELEASE(m_pCurrentVertexBuffers[i]);
    }
    SAFE_RELEASE(m_pCurrentIndexBuffer);

    SAFE_RELEASE(m_pCurrentShaderProgram);
    m_shaderStates.Obliterate();

    for (uint32 i = 0; i < m_constantBuffers.GetSize(); i++)
    {
        ConstantBuffer *constantBuffer = &m_constantBuffers[i];
        delete[] constantBuffer->pLocalMemory;
    }

    SAFE_RELEASE(m_pCurrentRasterizerState);
    SAFE_RELEASE(m_pCurrentDepthStencilState);
    SAFE_RELEASE(m_pCurrentBlendState);

    for (uint32 i = 0; i < countof(m_pCurrentRenderTargetViews); i++) {
        SAFE_RELEASE(m_pCurrentRenderTargetViews[i]);
    }
    SAFE_RELEASE(m_pCurrentDepthBufferView);

    delete m_pConstants;

    SAFE_RELEASE(m_pCurrentSwapChain);

    m_pDevice->SetThreadCopyCommandQueue(nullptr);
    m_pDevice->SetImmediateContext(nullptr);
    m_pDevice->Release();
}

void D3D12GPUContext::ResourceBarrier(ID3D12Resource *pResource, D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState)
{
    D3D12_RESOURCE_BARRIER resourceBarrier =
    {
        D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
        { pResource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, beforeState, afterState }
    };

    m_pCommandList->ResourceBarrier(1, &resourceBarrier);
    m_commandCounter++;
}

void D3D12GPUContext::ResourceBarrier(ID3D12Resource *pResource, uint32 subResource, D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState)
{
    D3D12_RESOURCE_BARRIER resourceBarrier =
    {
        D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
        { pResource, subResource, beforeState, afterState }
    };

    m_pCommandList->ResourceBarrier(1, &resourceBarrier);
    m_commandCounter++;
}

bool D3D12GPUContext::Create()
{   
    // allocate constants
    m_pConstants = new GPUContextConstants(m_pDevice, this);
    CreateConstantBuffers();

    // create command list
    if (!CreateInternalCommandList())
        return false;

    // reset to known state
    ClearState(true, true, true, true);
    return true;
}

void D3D12GPUContext::CreateConstantBuffers()
{
    // allocate constant buffer storage
    const ShaderConstantBuffer::RegistryType *registry = ShaderConstantBuffer::GetRegistry();
    m_constantBuffers.Resize(registry->GetNumTypes());
    m_constantBuffers.ZeroContents();
    for (uint32 i = 0; i < m_constantBuffers.GetSize(); i++)
    {
        ConstantBuffer *constantBuffer = &m_constantBuffers[i];

        // applicable to us?
        const ShaderConstantBuffer *declaration = registry->GetTypeInfoByIndex(i);
        if (declaration == nullptr)
            continue;
        if (declaration->GetPlatformRequirement() != RENDERER_PLATFORM_COUNT && declaration->GetPlatformRequirement() != RENDERER_PLATFORM_D3D12)
            continue;
        if (declaration->GetMinimumFeatureLevel() != RENDERER_FEATURE_LEVEL_COUNT && declaration->GetMinimumFeatureLevel() > m_pDevice->GetFeatureLevel())
            continue;

        // set size so we know to allocate it later or on demand
        constantBuffer->Size = declaration->GetBufferSize();
        constantBuffer->DirtyLowerBounds = constantBuffer->DirtyUpperBounds = -1;
        constantBuffer->pLocalMemory = new byte[constantBuffer->Size];
        Y_memzero(constantBuffer->pLocalMemory, constantBuffer->Size);

        // create per-draw constant buffers
        constantBuffer->PerDraw = (declaration->GetUpdateFrequency() == SHADER_CONSTANT_BUFFER_UPDATE_FREQUENCY_PER_DRAW);
    }
}

bool D3D12GPUContext::CreateInternalCommandList()
{
    // allocate everything
    m_pCurrentCommandAllocator = m_pGraphicsCommandQueue->RequestCommandAllocator();
    m_pCurrentScratchBuffer = m_pGraphicsCommandQueue->RequestLinearBufferHeap();
    m_pCurrentScratchViewHeap = m_pGraphicsCommandQueue->RequestLinearViewHeap();
    m_pCurrentScratchSamplerHeap = m_pGraphicsCommandQueue->RequestLinearSamplerHeap();
    m_pCommandList = m_pGraphicsCommandQueue->RequestAndOpenCommandList(m_pCurrentCommandAllocator);

    // issue copies on the graphics queue
    m_pDevice->SetThreadCopyCommandQueue(m_pCommandList);

    // set initial state
    ResetCommandList(false, false);
    return true;
}

void D3D12GPUContext::BeginFrame()
{

}

void D3D12GPUContext::Flush()
{
    CloseAndExecuteCommandList(false, false);
    ResetCommandList(true, false);
}

void D3D12GPUContext::Finish()
{
    // allocator refresh not needed because of wait
    CloseAndExecuteCommandList(true, false);
    ResetCommandList(true, false);
}

void D3D12GPUContext::CloseAndExecuteCommandList(bool waitForCompletion, bool forceWithoutDrawCommands)
{
    // transition pending resources before execution.
    m_pDevice->TransitionPendingResources(m_pGraphicsCommandQueue);

    // close the command list
    HRESULT hResult = m_pCommandList->Close();
    if (SUCCEEDED(hResult))
    {
        // execute it
        if (m_commandCounter > 0 || forceWithoutDrawCommands)
            m_pGraphicsCommandQueue->ExecuteCommandList(m_pCommandList);

        // wait for the gpu to catch up
        if (waitForCompletion)
        {
            // re-use the same allocators since the gpu has caught up
            uint64 fenceValue = m_pGraphicsCommandQueue->CreateSynchronizationPoint();
            m_pGraphicsCommandQueue->WaitForFence(fenceValue);
            m_pGraphicsCommandQueue->ReleaseStaleResources();
            m_pCurrentScratchBuffer->Reset(true);
            m_pCurrentScratchViewHeap->Reset();
            m_pCurrentScratchSamplerHeap->Reset();
        }
    }
    else
    {
        Log_ErrorPrintf("ID3D12CommandList::Close failed with hResult %08X, some commands may not be executed.", hResult);

        // mark command list as failed and get a new list
        m_pGraphicsCommandQueue->ReleaseFailedCommandList(m_pCommandList);
        m_pCommandList = m_pGraphicsCommandQueue->RequestCommandList();

        // issue copies on the graphics queue
        m_pDevice->SetThreadCopyCommandQueue(m_pCommandList);

        // since the command list was not executed, allocators can be re-used
        m_pCurrentScratchBuffer->Reset(true);
        m_pCurrentScratchViewHeap->Reset();
        m_pCurrentScratchSamplerHeap->Reset();
    }

    // clear command counter
    m_commandCounter = 0;
}

void D3D12GPUContext::ResetCommandList(bool restoreState, bool refreshAllocators)
{
    // allocator refresh needed?
    if (refreshAllocators)
    {
        // get new allocators
        GetNewAllocators(m_pGraphicsCommandQueue->GetNextFenceValue());

        // take this opportunity to clean up stale resources (even if we're not waiting, it's a good place, since the gpu will be busy for a while)
        m_pGraphicsCommandQueue->ReleaseStaleResources();
    }

    // re-open the command list
    HRESULT hResult = m_pCommandList->Reset(m_pCurrentCommandAllocator, nullptr);
    if (FAILED(hResult))
        Log_WarningPrintf("ID3D12CommandList:Reset failed with hResult %08X", hResult);

    // restore state?
    if (restoreState)
    {
        // graphics root
        m_pCommandList->SetGraphicsRootSignature(m_pDevice->GetLegacyGraphicsRootSignature());
        m_pCommandList->SetComputeRootSignature(m_pDevice->GetLegacyComputeRootSignature());
        UpdateShaderDescriptorHeaps();

        // pipeline state
        UpdatePipelineState(true);

        // render targets
        SynchronizeRenderTargetsAndUAVs();
        UpdateScissorRect();

        // other stuff
        D3D12_VIEWPORT D3D12Viewport = { (float)m_currentViewport.TopLeftX, (float)m_currentViewport.TopLeftY, (float)m_currentViewport.Width, (float)m_currentViewport.Height, m_currentViewport.MinDepth, m_currentViewport.MaxDepth };
        m_pCommandList->RSSetViewports(1, &D3D12Viewport);
        m_pCommandList->IASetPrimitiveTopology(D3D12Helpers::GetD3D12PrimitiveTopology(m_currentTopology));
        m_pCommandList->OMSetBlendFactor(m_currentBlendStateBlendFactors);
        m_pCommandList->OMSetStencilRef(m_currentDepthStencilRef);

        // vertex buffers
        if (m_currentVertexBufferBindCount > 0)
        {
            D3D12_VERTEX_BUFFER_VIEW vertexBufferViews[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
            for (uint32 i = 0; i < m_currentVertexBufferBindCount; i++)
            {
                if (m_pCurrentVertexBuffers[i] != nullptr)
                {
                    vertexBufferViews[i].BufferLocation = m_pCurrentVertexBuffers[i]->GetD3DResource()->GetGPUVirtualAddress() + m_currentVertexBufferOffsets[i];
                    vertexBufferViews[i].SizeInBytes = m_pCurrentVertexBuffers[i]->GetDesc()->Size - m_currentVertexBufferOffsets[i];
                    vertexBufferViews[i].StrideInBytes = m_currentVertexBufferStrides[i];
                }
                else
                {
                    Y_memzero(&vertexBufferViews[i], sizeof(D3D12_VERTEX_BUFFER_VIEW));
                }
            }
            m_pCommandList->IASetVertexBuffers(0, m_currentVertexBufferBindCount, vertexBufferViews);
        }
        if (m_pCurrentIndexBuffer != nullptr)
        {
            D3D12_INDEX_BUFFER_VIEW bufferView;
            bufferView.BufferLocation = m_pCurrentIndexBuffer->GetD3DResource()->GetGPUVirtualAddress() + m_currentIndexBufferOffset;
            bufferView.SizeInBytes = m_pCurrentIndexBuffer->GetDesc()->Size - m_currentIndexBufferOffset;
            bufferView.Format = (m_currentIndexFormat == GPU_INDEX_FORMAT_UINT16) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
            m_pCommandList->IASetIndexBuffer(&bufferView);
        }
    }
    else
    {
        // graphics root
        m_pCommandList->SetGraphicsRootSignature(m_pDevice->GetLegacyGraphicsRootSignature());
        m_pCommandList->SetComputeRootSignature(m_pDevice->GetLegacyComputeRootSignature());
        UpdateShaderDescriptorHeaps();

        // pipeline state
        m_shaderStates.ZeroContents();
        SAFE_RELEASE(m_pCurrentShaderProgram);
        m_perDrawConstantBuffers.ZeroContents();
        m_perDrawConstantBufferCount = 0;
        m_perDrawConstantBuffersDirty = false;
        m_pipelineChanged = false;
        UpdatePipelineState(true);

        // render targets -- still needs to be re-synced
        for (uint32 i = 0; i < m_nCurrentRenderTargets; i++)
            SAFE_RELEASE(m_pCurrentRenderTargetViews[i]);
        SAFE_RELEASE(m_pCurrentDepthBufferView);
        m_nCurrentRenderTargets = 0;
        SynchronizeRenderTargetsAndUAVs();
        UpdateScissorRect();

        // input assembler
        for (uint32 i = 0; i < m_currentVertexBufferBindCount; i++)
        {
            SAFE_RELEASE(m_pCurrentVertexBuffers[i]);
            m_currentVertexBufferStrides[i] = 0;
            m_currentVertexBufferOffsets[i] = 0;
        }
        m_currentVertexBufferBindCount = 0;
        SAFE_RELEASE(m_pCurrentIndexBuffer);

        // predicate
        //SAFE_RELEASE(m_pCurrentPredicate);

        // other stuff
        m_currentTopology = DRAW_TOPOLOGY_UNDEFINED;
        m_currentD3DTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
        m_currentBlendStateBlendFactors.SetZero();
        m_currentDepthStencilRef = 0;
        Y_memzero(&m_currentViewport, sizeof(m_currentViewport));
        Y_memzero(&m_scissorRect, sizeof(m_scissorRect));
    }
}

void D3D12GPUContext::GetNewAllocators(uint64 fenceValue)
{
    // get a new set of allocators. if anything fails, wait for the gpu, and re-use the current one.
    ID3D12CommandAllocator *pCommandAllocator = m_pGraphicsCommandQueue->RequestCommandAllocator();
    if (pCommandAllocator == nullptr)
    {
        m_pGraphicsCommandQueue->WaitForFence(fenceValue);
        pCommandAllocator->Reset();
    }
    else
    {
        m_pGraphicsCommandQueue->ReleaseCommandAllocator(m_pCurrentCommandAllocator, fenceValue);
        m_pCurrentCommandAllocator = pCommandAllocator;
    }

    D3D12LinearBufferHeap *pBufferHeap = m_pGraphicsCommandQueue->RequestLinearBufferHeap();
    if (pBufferHeap == nullptr)
    {
        m_pGraphicsCommandQueue->WaitForFence(fenceValue);
        pBufferHeap->Reset(true);
    }
    else
    {
        m_pGraphicsCommandQueue->ReleaseLinearBufferHeap(m_pCurrentScratchBuffer, fenceValue);
        m_pCurrentScratchBuffer = pBufferHeap;
    }

    D3D12LinearDescriptorHeap *pViewHeap = m_pGraphicsCommandQueue->RequestLinearViewHeap();
    if (pViewHeap == nullptr)
    {
        m_pGraphicsCommandQueue->WaitForFence(fenceValue);
        pViewHeap->Reset();
    }
    else
    {
        m_pGraphicsCommandQueue->ReleaseLinearViewHeap(m_pCurrentScratchViewHeap, fenceValue);
        m_pCurrentScratchViewHeap = pViewHeap;
    }

    D3D12LinearDescriptorHeap *pSamplerHeap = m_pGraphicsCommandQueue->RequestLinearSamplerHeap();
    if (pSamplerHeap == nullptr)
    {
        m_pGraphicsCommandQueue->WaitForFence(fenceValue);
        pSamplerHeap->Reset();
    }
    else
    {
        m_pGraphicsCommandQueue->ReleaseLinearSamplerHeap(m_pCurrentScratchSamplerHeap, fenceValue);
        m_pCurrentScratchSamplerHeap = pSamplerHeap;
    }

    // update per-draw constant buffers, they need new pointers.
    for (uint32 i = 0; i < m_constantBuffers.GetSize(); i++)
    {
        ConstantBuffer *pConstantBuffer = &m_constantBuffers[i];
        if (pConstantBuffer->PerDraw)
        {
            void *pCPUPointer;
            D3D12_GPU_VIRTUAL_ADDRESS pGPUAddress;
            if (AllocateScratchBufferMemory(pConstantBuffer->Size, D3D12_CONSTANT_BUFFER_ALIGNMENT, nullptr, nullptr, &pCPUPointer, &pGPUAddress))
            {
                Y_memcpy(pCPUPointer, pConstantBuffer->pLocalMemory, pConstantBuffer->Size);
                pConstantBuffer->LastAddress = pGPUAddress;
            }
        }
    }
}

void D3D12GPUContext::UpdateShaderDescriptorHeaps()
{
    ID3D12DescriptorHeap *pDescriptorHeaps[] = { m_pCurrentScratchViewHeap->GetD3DHeap(), m_pCurrentScratchSamplerHeap->GetD3DHeap() };
    m_pCommandList->SetDescriptorHeaps(countof(pDescriptorHeaps), pDescriptorHeaps);
}

bool D3D12GPUContext::GetPerDrawConstantBufferGPUAddress(uint32 index, D3D12_GPU_VIRTUAL_ADDRESS *pAddress)
{
    DebugAssert(index < m_constantBuffers.GetSize());
    if (m_constantBuffers[index].PerDraw)
    {
        *pAddress = m_constantBuffers[index].LastAddress;
        return true;
    }

    return false;
}

bool D3D12GPUContext::AllocateScratchBufferMemory(uint32 size, uint32 alignment, ID3D12Resource **ppScratchBufferResource, uint32 *pScratchBufferOffset, void **ppCPUPointer, D3D12_GPU_VIRTUAL_ADDRESS *pGPUAddress)
{
    // outright refuse if it's larger than the buffer size (since the alloc will always fail)
    if (size > m_pGraphicsCommandQueue->GetLinearBufferHeapSize())
        return false;

    uint32 offset;
    if (!m_pCurrentScratchBuffer->AllocateAligned(size, alignment, &offset))
    {
        // get a new buffer. we can't release the buffer with a new fence, since the command line hasn't been executed yet.
        D3D12LinearBufferHeap *pNewBuffer = m_pGraphicsCommandQueue->RequestLinearBufferHeap();
        DebugAssert(pNewBuffer != nullptr);
        
        // release the old buffer
        m_pGraphicsCommandQueue->ReleaseLinearBufferHeap(m_pCurrentScratchBuffer);
        m_pCurrentScratchBuffer = pNewBuffer;

        // retry allocation on new buffer
        if (!m_pCurrentScratchBuffer->AllocateAligned(size, alignment, &offset))
        {
            Log_ErrorPrintf("Failed to allocate on new scratch buffer (this shouldn't happen)");
            return false;
        }
    }

    if (ppScratchBufferResource != nullptr)
        *ppScratchBufferResource = m_pCurrentScratchBuffer->GetResource();
    if (pScratchBufferOffset != nullptr)
        *pScratchBufferOffset = offset;
    if (ppCPUPointer != nullptr)
        *ppCPUPointer = m_pCurrentScratchBuffer->GetPointer(offset);
    if (pGPUAddress != nullptr)
        *pGPUAddress = m_pCurrentScratchBuffer->GetGPUAddress(offset);

    return true;
}

bool D3D12GPUContext::AllocateScratchView(uint32 count, D3D12_CPU_DESCRIPTOR_HANDLE *pOutCPUHandle, D3D12_GPU_DESCRIPTOR_HANDLE *pOutGPUHandle)
{
    // outright refuse if it's larger than the buffer size (since the alloc will always fail)
    if (count > m_pGraphicsCommandQueue->GetLinearViewHeapSize())
        return false;

    if (!m_pCurrentScratchViewHeap->Allocate(count, pOutCPUHandle, pOutGPUHandle))
    {
        // get a new buffer
        D3D12LinearDescriptorHeap *pNewHeap = m_pGraphicsCommandQueue->RequestLinearViewHeap();
        DebugAssert(pNewHeap != nullptr);

        // release the old buffer
        m_pGraphicsCommandQueue->ReleaseLinearViewHeap(m_pCurrentScratchViewHeap);
        m_pCurrentScratchViewHeap = pNewHeap;
        UpdateShaderDescriptorHeaps();

        // retry allocation on new buffer
        if (!m_pCurrentScratchViewHeap->Allocate(count, pOutCPUHandle, pOutGPUHandle))
        {
            Log_ErrorPrintf("Failed to allocate on new scratch buffer (this shouldn't happen)");
            return false;
        }
    }

    return true;
}

bool D3D12GPUContext::AllocateScratchSamplers(uint32 count, D3D12_CPU_DESCRIPTOR_HANDLE *pOutCPUHandle, D3D12_GPU_DESCRIPTOR_HANDLE *pOutGPUHandle)
{
    // outright refuse if it's larger than the buffer size (since the alloc will always fail)
    if (count > m_pGraphicsCommandQueue->GetLinearSamplerHeapSize())
        return false;

    if (!m_pCurrentScratchSamplerHeap->Allocate(count, pOutCPUHandle, pOutGPUHandle))
    {
        // get a new buffer
        D3D12LinearDescriptorHeap *pNewHeap = m_pGraphicsCommandQueue->RequestLinearSamplerHeap();
        DebugAssert(pNewHeap != nullptr);

        // release the old buffer
        m_pGraphicsCommandQueue->ReleaseLinearSamplerHeap(m_pCurrentScratchSamplerHeap);
        m_pCurrentScratchSamplerHeap = pNewHeap;
        UpdateShaderDescriptorHeaps();

        // retry allocation on new buffer
        if (!m_pCurrentScratchSamplerHeap->Allocate(count, pOutCPUHandle, pOutGPUHandle))
        {
            Log_ErrorPrintf("Failed to allocate on new scratch buffer (this shouldn't happen)");
            return false;
        }
    }

    return true;
}

void D3D12GPUContext::GetCurrentRenderTargetDimensions(uint32 *width, uint32 *height)
{
    uint3 textureDimensions;
    if (m_pCurrentDepthBufferView != nullptr)
        textureDimensions = Renderer::GetTextureDimensions(m_pCurrentDepthBufferView->GetTargetTexture());
    else if (m_nCurrentRenderTargets > 0)
        textureDimensions = Renderer::GetTextureDimensions(m_pCurrentRenderTargetViews[0]->GetTargetTexture());
    else if (m_pCurrentSwapChain != nullptr)
        textureDimensions.Set(m_pCurrentSwapChain->GetWidth(), m_pCurrentSwapChain->GetHeight(), 1);
    else
        textureDimensions.Set(1, 1, 1);

    *width = textureDimensions.x;
    *height = textureDimensions.y;
}

D3D12_RESOURCE_STATES D3D12GPUContext::GetCurrentResourceState(GPUResource *pResource)
{
    GPU_RESOURCE_TYPE resourceType = pResource->GetResourceType();
    if (resourceType == GPU_RESOURCE_TYPE_BUFFER)
    {
        for (uint32 i = 0; i < m_currentVertexBufferBindCount; i++)
        {
            if (m_pCurrentVertexBuffers[i] == pResource)
                return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        }

        if (IsBoundAsUnorderedAccess(pResource))
            return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        return static_cast<D3D12GPUBuffer *>(pResource)->GetDefaultResourceState();
    }
    else
    {
        if (resourceType >= GPU_RESOURCE_TYPE_TEXTURE1D && resourceType <= GPU_RESOURCE_TYPE_DEPTH_TEXTURE)
        {
            if (IsBoundAsRenderTarget(static_cast<GPUTexture *>(pResource)))
                return D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (IsBoundAsDepthBuffer(static_cast<GPUTexture *>(pResource)))
                return D3D12_RESOURCE_STATE_DEPTH_WRITE;

            switch (resourceType)
            {
            case GPU_RESOURCE_TYPE_TEXTURE2D:
                return static_cast<D3D12GPUTexture2D *>(pResource)->GetDefaultResourceState();

            case GPU_RESOURCE_TYPE_TEXTURE2DARRAY:
                return static_cast<D3D12GPUTexture2DArray *>(pResource)->GetDefaultResourceState();
            }
        }
    }

    return D3D12_RESOURCE_STATE_COMMON;
}

bool D3D12GPUContext::IsBoundAsRenderTarget(GPUTexture *pTexture)
{
    for (uint32 i = 0; i < m_nCurrentRenderTargets; i++)
    {
        if (m_pCurrentRenderTargetViews[i] != nullptr && m_pCurrentRenderTargetViews[i]->GetTargetTexture() == pTexture)
            return true;
    }

    return false;
}

bool D3D12GPUContext::IsBoundAsDepthBuffer(GPUTexture *pTexture)
{
    return (m_pCurrentDepthBufferView != nullptr && m_pCurrentDepthBufferView->GetTargetTexture() == pTexture);
}

bool D3D12GPUContext::IsBoundAsUnorderedAccess(GPUResource *pResource)
{
    // @TODO check UAVs
    return false;
}

void D3D12GPUContext::UpdateScissorRect()
{
    if (m_pCurrentRasterizerState != nullptr && m_pCurrentRasterizerState->GetDesc()->ScissorEnable)
    {
        D3D12_RECT scissorRect = { (LONG)m_scissorRect.Left, (LONG)m_scissorRect.Top, (LONG)m_scissorRect.Right, (LONG)m_scissorRect.Bottom };
        m_pCommandList->RSSetScissorRects(1, &scissorRect);
    }
    else
    {
        // get current render target dimensions
        uint32 width, height;
        GetCurrentRenderTargetDimensions(&width, &height);

        // set scissor
        D3D12_RECT scissorRect = { (LONG)0, (LONG)0, (LONG)width, (LONG)height };
        m_pCommandList->RSSetScissorRects(1, &scissorRect);
    }
}

void D3D12GPUContext::ClearState(bool clearShaders /* = true */, bool clearBuffers /* = true */, bool clearStates /* = true */, bool clearRenderTargets /* = true */)
{
    if (clearShaders)
    {
        SetShaderProgram(nullptr);
        m_shaderStates.ZeroContents();
    }

    if (clearBuffers)
    {
        if (m_currentVertexBufferBindCount > 0)
        {
            static GPUBuffer *nullVertexBuffers[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = { nullptr };
            static const uint32 nullSizeOrOffset[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = { 0 };
            SetVertexBuffers(0, m_currentVertexBufferBindCount, nullVertexBuffers, nullSizeOrOffset, nullSizeOrOffset);
        }

        if (m_pCurrentIndexBuffer != nullptr)
            SetIndexBuffer(nullptr, GPU_INDEX_FORMAT_UINT16, 0);
    }

    if (clearStates)
    {
        SetRasterizerState(m_pDevice->GetDefaultRasterizerState());
        SetDepthStencilState(m_pDevice->GetDefaultDepthStencilState(), 0);
        SetBlendState(m_pDevice->GetDefaultBlendState());
        SetDrawTopology(DRAW_TOPOLOGY_TRIANGLE_LIST);

        RENDERER_SCISSOR_RECT scissor(0, 0, 0, 0);
        SetFullViewport(nullptr);
        SetScissorRect(&scissor);
    }

    if (clearRenderTargets)
    {
        SetRenderTargets(0, nullptr, nullptr);
    }
}

GPURasterizerState *D3D12GPUContext::GetRasterizerState()
{
    return m_pCurrentRasterizerState;
}

void D3D12GPUContext::SetRasterizerState(GPURasterizerState *pRasterizerState)
{
    if (m_pCurrentRasterizerState != pRasterizerState)
    {
        bool oldScissorState = (m_pCurrentRasterizerState != nullptr) ? m_pCurrentRasterizerState->GetDesc()->ScissorEnable : false;
        if (m_pCurrentRasterizerState != nullptr)
            m_pCurrentRasterizerState->Release();

        bool newScissorState = (pRasterizerState != nullptr) ? pRasterizerState->GetDesc()->ScissorEnable : false;
        if ((m_pCurrentRasterizerState = static_cast<D3D12GPURasterizerState *>(pRasterizerState)) != nullptr)
            m_pCurrentRasterizerState->AddRef();

        m_pipelineChanged = true;

        // set scissor rect if scissor is not enabled
        if (oldScissorState != newScissorState)
            UpdateScissorRect();
    }
}

GPUDepthStencilState *D3D12GPUContext::GetDepthStencilState()
{
    return m_pCurrentDepthStencilState;
}

uint8 D3D12GPUContext::GetDepthStencilStateStencilRef()
{
    return m_currentDepthStencilRef;
}

void D3D12GPUContext::SetDepthStencilState(GPUDepthStencilState *pDepthStencilState, uint8 stencilRef)
{
    if (m_pCurrentDepthStencilState != pDepthStencilState)
    {
        if (m_pCurrentDepthStencilState != nullptr)
            m_pCurrentDepthStencilState->Release();

        if ((m_pCurrentDepthStencilState = static_cast<D3D12GPUDepthStencilState *>(pDepthStencilState)) != nullptr)
            m_pCurrentDepthStencilState->AddRef();

        m_pipelineChanged = true;
    }

    if (m_currentDepthStencilRef != stencilRef)
    {
        m_pCommandList->OMSetStencilRef(stencilRef);
        m_currentDepthStencilRef = stencilRef;
    }
}

GPUBlendState *D3D12GPUContext::GetBlendState()
{
    return m_pCurrentBlendState;
}

const float4 &D3D12GPUContext::GetBlendStateBlendFactor()
{
    return m_currentBlendStateBlendFactors;
};

void D3D12GPUContext::SetBlendState(GPUBlendState *pBlendState, const float4 &blendFactor /* = float4::One */)
{
    if (m_pCurrentBlendState != pBlendState)
    {
        if (m_pCurrentBlendState != nullptr)
            m_pCurrentBlendState->Release();

        if ((m_pCurrentBlendState = static_cast<D3D12GPUBlendState *>(pBlendState)) != nullptr)
            m_pCurrentBlendState->AddRef();

        m_pipelineChanged = true;
    }

    if (blendFactor != m_currentBlendStateBlendFactors)
    {
        m_pCommandList->OMSetBlendFactor(blendFactor.ele);
        m_currentBlendStateBlendFactors = blendFactor;
    }
}

const RENDERER_VIEWPORT *D3D12GPUContext::GetViewport()
{
    return &m_currentViewport;
}

void D3D12GPUContext::SetViewport(const RENDERER_VIEWPORT *pNewViewport)
{
    if (Y_memcmp(&m_currentViewport, pNewViewport, sizeof(RENDERER_VIEWPORT)) == 0)
        return;

    Y_memcpy(&m_currentViewport, pNewViewport, sizeof(m_currentViewport));

    D3D12_VIEWPORT D3D12Viewport = { (float)m_currentViewport.TopLeftX, (float)m_currentViewport.TopLeftY, (float)m_currentViewport.Width, (float)m_currentViewport.Height, m_currentViewport.MinDepth, m_currentViewport.MaxDepth };
    m_pCommandList->RSSetViewports(1, &D3D12Viewport);

    // update constants
    m_pConstants->SetViewportOffset((float)m_currentViewport.TopLeftX, (float)m_currentViewport.TopLeftY, false);
    m_pConstants->SetViewportSize((float)m_currentViewport.Width, (float)m_currentViewport.Height, false);
    m_pConstants->CommitChanges();
}

void D3D12GPUContext::SetFullViewport(GPUTexture *pForRenderTarget /* = NULL */)
{
    RENDERER_VIEWPORT viewport;
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    if (pForRenderTarget != nullptr)
    {
        uint3 textureDimensions = Renderer::GetTextureDimensions(pForRenderTarget);
        viewport.Width = textureDimensions.x;
        viewport.Height = textureDimensions.y;
    }
    else
    {
        GetCurrentRenderTargetDimensions(&viewport.Width, &viewport.Height);
    }

    SetViewport(&viewport);
}

const RENDERER_SCISSOR_RECT *D3D12GPUContext::GetScissorRect()
{
    return &m_scissorRect;
}

void D3D12GPUContext::SetScissorRect(const RENDERER_SCISSOR_RECT *pScissorRect)
{
    if (Y_memcmp(&m_scissorRect, pScissorRect, sizeof(m_scissorRect)) == 0)
        return;

    Y_memcpy(&m_scissorRect, pScissorRect, sizeof(m_scissorRect));

    // only set if rasterizer state permits
    if (m_pCurrentRasterizerState != nullptr && m_pCurrentRasterizerState->GetDesc()->ScissorEnable)
    {
        D3D12_RECT D3D12ScissorRect = { (LONG)m_scissorRect.Left, (LONG)m_scissorRect.Top, (LONG)m_scissorRect.Right, (LONG)m_scissorRect.Bottom };
        m_pCommandList->RSSetScissorRects(1, &D3D12ScissorRect);
    }
}

void D3D12GPUContext::ClearTargets(bool clearColor /* = true */, bool clearDepth /* = true */, bool clearStencil /* = true */, const float4 &clearColorValue /* = float4::Zero */, float clearDepthValue /* = 1.0f */, uint8 clearStencilValue /* = 0 */)
{
    D3D12_CLEAR_FLAGS clearFlags = (D3D12_CLEAR_FLAGS)0;
    if (clearDepth)
        clearFlags |= D3D12_CLEAR_FLAG_DEPTH;
    if (clearStencil)
        clearFlags |= D3D12_CLEAR_FLAG_STENCIL;

    if (m_nCurrentRenderTargets == 0 && m_pCurrentDepthBufferView == nullptr)
    {
        // on swapchain
        if (m_pCurrentSwapChain != nullptr)
        {
            if (clearColor)
            {
                m_pCommandList->ClearRenderTargetView(m_pCurrentSwapChain->GetCurrentBackBufferViewDescriptorCPUHandle(), clearColorValue, 0, nullptr);
                m_commandCounter++;
            }

            if (clearDepth && m_pCurrentSwapChain->GetDepthStencilBufferResource() != nullptr)
            {
                m_pCommandList->ClearDepthStencilView(m_pCurrentSwapChain->GetDepthStencilBufferViewDescriptorCPUHandle(), clearFlags, clearDepthValue, clearStencilValue, 0, nullptr);
                m_commandCounter++;
            }
        }
    }
    else
    {
        if (clearColor)
        {
            for (uint32 i = 0; i < m_nCurrentRenderTargets; i++)
            {
                if (m_pCurrentRenderTargetViews[i] != nullptr)
                {
                    m_pCommandList->ClearRenderTargetView(m_pCurrentRenderTargetViews[i]->GetDescriptorHandle(), clearColorValue, 0, nullptr);
                    m_commandCounter++;
                }
            }
        }

        if (clearFlags != 0 && m_pCurrentDepthBufferView != nullptr)
        {
            m_pCommandList->ClearDepthStencilView(m_pCurrentDepthBufferView->GetDescriptorHandle(), clearFlags, clearDepthValue, clearStencilValue, 0, nullptr);
            m_commandCounter++;
        }
    }
}

void D3D12GPUContext::DiscardTargets(bool discardColor /* = true */, bool discardDepth /* = true */, bool discardStencil /* = true */)
{
    if (m_nCurrentRenderTargets == 0 && m_pCurrentDepthBufferView == nullptr)
    {
        // on swapchain
        if (m_pCurrentSwapChain != nullptr)
        {
            if (discardColor)
            {
                m_pCommandList->DiscardResource(m_pCurrentSwapChain->GetCurrentBackBufferResource(), nullptr);
                m_commandCounter++;
            }
            if (discardDepth && discardStencil && m_pCurrentSwapChain->HasDepthStencilBuffer())
            {
                m_pCommandList->DiscardResource(m_pCurrentSwapChain->GetDepthStencilBufferResource(), nullptr);
                m_commandCounter++;
            }
        }
    }
    else
    {
        if (discardColor)
        {
            for (uint32 i = 0; i < m_nCurrentRenderTargets; i++)
            {
                if (m_pCurrentRenderTargetViews[i] != nullptr)
                {
                    m_pCommandList->DiscardResource(m_pCurrentRenderTargetViews[i]->GetD3DResource(), nullptr);
                    m_commandCounter++;
                }
            }
        }

        if (discardDepth && discardStencil && m_pCurrentDepthBufferView != nullptr)
        {
            m_pCommandList->DiscardResource(m_pCurrentDepthBufferView->GetD3DResource(), nullptr);
            m_commandCounter++;
        }
    }
}

GPUOutputBuffer *D3D12GPUContext::GetOutputBuffer()
{
    return m_pCurrentSwapChain;
}

void D3D12GPUContext::SetOutputBuffer(GPUOutputBuffer *pSwapChain)
{
    DebugAssert(pSwapChain != nullptr);
    if (m_pCurrentSwapChain == pSwapChain)
        return;

    // copy out old swap chain
    D3D12GPUOutputBuffer *pOldSwapChain = m_pCurrentSwapChain;

    // copy in new swap chain
    if ((m_pCurrentSwapChain = static_cast<D3D12GPUOutputBuffer *>(pSwapChain)) != nullptr)
        m_pCurrentSwapChain->AddRef();

    // Currently rendering to window?
    if (m_nCurrentRenderTargets == 0 && m_pCurrentDepthBufferView == nullptr)
    {
        SynchronizeRenderTargetsAndUAVs();
        UpdateScissorRect();
    }

    // update references
    if (pOldSwapChain != nullptr)
        pOldSwapChain->Release();
}

bool D3D12GPUContext::GetExclusiveFullScreen()
{
    BOOL currentState;
    HRESULT hResult = m_pCurrentSwapChain->GetDXGISwapChain()->GetFullscreenState(&currentState, nullptr);
    if (FAILED(hResult))
        return false;

    return (currentState == TRUE);
}

bool D3D12GPUContext::SetExclusiveFullScreen(bool enabled, uint32 width, uint32 height, uint32 refreshRate)
{
    // @TODO
    return false;
}

bool D3D12GPUContext::ResizeOutputBuffer(uint32 width /* = 0 */, uint32 height /* = 0 */)
{
    if (width == 0 || height == 0)
    {
        // get the new size of the window
        RECT clientRect;
        GetClientRect(m_pCurrentSwapChain->GetHWND(), &clientRect);

        // changed?
        width = Max(clientRect.right - clientRect.left, (LONG)1);
        height = Max(clientRect.bottom - clientRect.top, (LONG)1);
    }

    // changed?
    if (m_pCurrentSwapChain->GetWidth() == width && m_pCurrentSwapChain->GetHeight() == height)
        return true;

    // unbind if we're currently bound to the pipeline
    if (m_nCurrentRenderTargets == 0 && m_pCurrentDepthBufferView == nullptr)
        m_pCommandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    // invoke the resize
    m_pCurrentSwapChain->InternalResizeBuffers(width, height, m_pCurrentSwapChain->GetVSyncType());

    // synchronize render targets if we were bound
    if (m_nCurrentRenderTargets == 0 && m_pCurrentDepthBufferView == nullptr)
    {
        SynchronizeRenderTargetsAndUAVs();
        UpdateScissorRect();
    }

    // done
    return true;
}

void D3D12GPUContext::PresentOutputBuffer(GPU_PRESENT_BEHAVIOUR presentBehaviour)
{
    // the barrier *to* present state has to be queued
    ResourceBarrier(m_pCurrentSwapChain->GetCurrentBackBufferResource(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    // ensure all commands have been queued. don't refresh allocators yet, because otherwise we could block here when trying to get
    // the new allocators. Instead, push the delay to BeginFrame, which will have some time passed.
    //FlushCommandList(true, false, false);
    //FlushCommandList(true, false, true);
    CloseAndExecuteCommandList(false, false);

#if 1
    // vsync off?
    if (presentBehaviour == GPU_PRESENT_BEHAVIOUR_IMMEDIATE)
    {
        // test if we can present
        DWORD waitResult = WaitForSingleObject(m_pCurrentSwapChain->GetDXGISwapChain()->GetFrameLatencyWaitableObject(), 0);
        if (waitResult == WAIT_OBJECT_0)
            m_pCurrentSwapChain->GetDXGISwapChain()->Present(0, DXGI_PRESENT_RESTART);
        else
            g_pRenderer->GetCounters()->IncrementFramesDroppedCounter();
    }
    else
    {
        // vsync on
        m_pCurrentSwapChain->GetDXGISwapChain()->Present(1, 0);
    }
#else
    if (presentBehaviour == GPU_PRESENT_BEHAVIOUR_IMMEDIATE)
        m_pCurrentSwapChain->GetDXGISwapChain()->Present(0, DXGI_PRESENT_RESTART);
    else
        m_pCurrentSwapChain->GetDXGISwapChain()->Present(1, 0);
#endif

    // restore state (since new command list)
    ResetCommandList(true, true);

    // create a new synchronization point for the next frame.
    m_pGraphicsCommandQueue->CreateSynchronizationPoint();
}

uint32 D3D12GPUContext::GetRenderTargets(uint32 nRenderTargets, GPURenderTargetView **ppRenderTargetViews, GPUDepthStencilBufferView **ppDepthBufferView)
{
    uint32 i, j;

    for (i = 0; i < m_nCurrentRenderTargets && i < nRenderTargets; i++)
        ppRenderTargetViews[i] = m_pCurrentRenderTargetViews[i];

    for (j = i; j < nRenderTargets; j++)
        ppRenderTargetViews[j] = nullptr;

    if (ppDepthBufferView != nullptr)
        *ppDepthBufferView = m_pCurrentDepthBufferView;

    return i;
}

void D3D12GPUContext::SetRenderTargets(uint32 nRenderTargets, GPURenderTargetView **ppRenderTargets, GPUDepthStencilBufferView *pDepthBufferView)
{
    // @TODO only update pipeline on format change / # targets change

    // to system framebuffer?
    if ((nRenderTargets == 0 || (nRenderTargets == 1 && ppRenderTargets[0] == nullptr)) && pDepthBufferView == nullptr)
    {
        // change?
        if (m_nCurrentRenderTargets == 0 && m_pCurrentDepthBufferView == nullptr)
            return;

        // kill current targets
        for (uint32 i = 0; i < m_nCurrentRenderTargets; i++)
        {
            D3D12_RESOURCE_STATES defaultState = D3D12Helpers::GetResourceDefaultState(m_pCurrentRenderTargetViews[i]->GetTargetTexture());
            if (defaultState != D3D12_RESOURCE_STATE_RENDER_TARGET)
                ResourceBarrier(m_pCurrentRenderTargetViews[i]->GetD3DResource(), D3D12_RESOURCE_STATE_RENDER_TARGET, defaultState);

            m_pCurrentRenderTargetViews[i]->Release();
            m_pCurrentRenderTargetViews[i] = nullptr;
        }
        if (m_pCurrentDepthBufferView != nullptr)
        {
            D3D12_RESOURCE_STATES defaultState = D3D12Helpers::GetResourceDefaultState(m_pCurrentDepthBufferView->GetTargetTexture());
            if (defaultState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
                ResourceBarrier(m_pCurrentDepthBufferView->GetD3DResource(), D3D12_RESOURCE_STATE_DEPTH_WRITE, defaultState);

            m_pCurrentDepthBufferView->Release();
            m_pCurrentDepthBufferView = nullptr;
        }

        m_nCurrentRenderTargets = 0;
        SynchronizeRenderTargetsAndUAVs();
        UpdateScissorRect();
        m_pipelineChanged = true;
    }
    else
    {
        DebugAssert(nRenderTargets < countof(m_pCurrentRenderTargetViews));

        uint32 slot;
        uint32 newRenderTargetCount = 0;
        bool doUpdate = false;

        // transition states - unbind old targets
        // this is needed because if a target changes index it'll transition incorrectly otherwise.
        for (slot = 0; slot < m_nCurrentRenderTargets; slot++)
        {
            DebugAssert(m_pCurrentRenderTargetViews[slot] != nullptr);
            if (slot >= nRenderTargets || m_pCurrentRenderTargetViews[slot] != ppRenderTargets[slot])
            {
                D3D12_RESOURCE_STATES defaultState = D3D12Helpers::GetResourceDefaultState(m_pCurrentRenderTargetViews[slot]->GetTargetTexture());
                if (defaultState != D3D12_RESOURCE_STATE_RENDER_TARGET)
                    ResourceBarrier(m_pCurrentRenderTargetViews[slot]->GetD3DResource(), D3D12_RESOURCE_STATE_RENDER_TARGET, defaultState);
            }
        }

        // set inclusive slots
        for (slot = 0; slot < nRenderTargets; slot++)
        {
            if (m_pCurrentRenderTargetViews[slot] != ppRenderTargets[slot])
            {
                if (m_pCurrentRenderTargetViews[slot] != nullptr)
                    m_pCurrentRenderTargetViews[slot]->Release();

                if ((m_pCurrentRenderTargetViews[slot] = static_cast<D3D12GPURenderTargetView *>(ppRenderTargets[slot])) != nullptr)
                {
                    D3D12_RESOURCE_STATES defaultState = D3D12Helpers::GetResourceDefaultState(m_pCurrentRenderTargetViews[slot]->GetTargetTexture());
                    if (defaultState != D3D12_RESOURCE_STATE_RENDER_TARGET)
                        ResourceBarrier(m_pCurrentRenderTargetViews[slot]->GetD3DResource(), defaultState, D3D12_RESOURCE_STATE_RENDER_TARGET);

                    m_pCurrentRenderTargetViews[slot]->AddRef();
                }

                doUpdate = true;
            }

            if (m_pCurrentRenderTargetViews[slot] != nullptr)
                newRenderTargetCount = slot + 1;
        }

        // clear extra slots
        for (; slot < m_nCurrentRenderTargets; slot++)
        {
            if (m_pCurrentRenderTargetViews[slot] != nullptr)
            {
                m_pCurrentRenderTargetViews[slot]->Release();
                m_pCurrentRenderTargetViews[slot] = nullptr;
                doUpdate = true;
            }
        }

        // update counter
        if (newRenderTargetCount != m_nCurrentRenderTargets)
            doUpdate = true;
        m_nCurrentRenderTargets = newRenderTargetCount;

        if (m_pCurrentDepthBufferView != pDepthBufferView)
        {
            if (m_pCurrentDepthBufferView != nullptr)
            {
                D3D12_RESOURCE_STATES defaultState = D3D12Helpers::GetResourceDefaultState(m_pCurrentDepthBufferView->GetTargetTexture());
                if (defaultState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
                    ResourceBarrier(m_pCurrentDepthBufferView->GetD3DResource(), D3D12_RESOURCE_STATE_DEPTH_WRITE, defaultState);

                m_pCurrentDepthBufferView->Release();
            }

            if ((m_pCurrentDepthBufferView = static_cast<D3D12GPUDepthStencilBufferView *>(pDepthBufferView)) != nullptr)
            {
                D3D12_RESOURCE_STATES defaultState = D3D12Helpers::GetResourceDefaultState(m_pCurrentDepthBufferView->GetTargetTexture());
                if (defaultState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
                    ResourceBarrier(m_pCurrentDepthBufferView->GetD3DResource(), defaultState, D3D12_RESOURCE_STATE_DEPTH_WRITE);

                m_pCurrentDepthBufferView->AddRef();
            }

            doUpdate = true;
        }

        if (doUpdate)
        {
            SynchronizeRenderTargetsAndUAVs();
            UpdateScissorRect();
            m_pipelineChanged = true;
        }
    }
}

DRAW_TOPOLOGY D3D12GPUContext::GetDrawTopology()
{
    return m_currentTopology;
}

void D3D12GPUContext::SetDrawTopology(DRAW_TOPOLOGY topology)
{
    if (topology == m_currentTopology)
        return;

    D3D12_PRIMITIVE_TOPOLOGY D3DPrimitiveTopology = D3D12Helpers::GetD3D12PrimitiveTopology(topology);
    D3D12_PRIMITIVE_TOPOLOGY_TYPE D3DPrimitiveTopologyType = D3D12Helpers::GetD3D12PrimitiveTopologyType(topology);

    m_pCommandList->IASetPrimitiveTopology(D3DPrimitiveTopology);
    m_currentTopology = topology;

    if (D3DPrimitiveTopologyType != m_currentD3DTopologyType)
    {
        m_currentD3DTopologyType = D3DPrimitiveTopologyType;
        m_pipelineChanged = true;
    }
}

uint32 D3D12GPUContext::GetVertexBuffers(uint32 firstBuffer, uint32 nBuffers, GPUBuffer **ppVertexBuffers, uint32 *pVertexBufferOffsets, uint32 *pVertexBufferStrides)
{
    DebugAssert(firstBuffer + nBuffers < countof(m_pCurrentVertexBuffers));

    uint32 saveCount;
    for (saveCount = 0; saveCount < nBuffers; saveCount++)
    {
        if ((firstBuffer + saveCount) > m_currentVertexBufferBindCount)
            break;

        ppVertexBuffers[saveCount] = m_pCurrentVertexBuffers[firstBuffer + saveCount];
        pVertexBufferOffsets[saveCount] = m_currentVertexBufferOffsets[firstBuffer + saveCount];
        pVertexBufferStrides[saveCount] = m_currentVertexBufferStrides[firstBuffer + saveCount];
    }

    return saveCount;
}

void D3D12GPUContext::SetVertexBuffers(uint32 firstBuffer, uint32 nBuffers, GPUBuffer *const *ppVertexBuffers, const uint32 *pVertexBufferOffsets, const uint32 *pVertexBufferStrides)
{
    D3D12_VERTEX_BUFFER_VIEW vertexBufferViews[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    uint32 dirtyFirst = Y_UINT32_MAX;
    uint32 dirtyLast = 0;

    for (uint32 i = 0; i < nBuffers; i++)
    {
        D3D12GPUBuffer *pD3D12VertexBuffer = static_cast<D3D12GPUBuffer *>(ppVertexBuffers[i]);
        uint32 bufferIndex = firstBuffer + i;

        if (m_pCurrentVertexBuffers[bufferIndex] == ppVertexBuffers[i] &&
            m_currentVertexBufferOffsets[bufferIndex] == pVertexBufferOffsets[i] &&
            m_currentVertexBufferStrides[bufferIndex] == pVertexBufferStrides[i])
        {
            continue;
        }

        if (m_pCurrentVertexBuffers[bufferIndex] != nullptr)
        {
            m_pCurrentVertexBuffers[bufferIndex]->Release();
            m_pCurrentVertexBuffers[bufferIndex] = nullptr;
        }

        if ((m_pCurrentVertexBuffers[bufferIndex] = pD3D12VertexBuffer) != nullptr)
        {
            pD3D12VertexBuffer->AddRef();
            m_currentVertexBufferOffsets[bufferIndex] = pVertexBufferOffsets[i];
            m_currentVertexBufferStrides[bufferIndex] = pVertexBufferStrides[i];

            // @TODO cache this address, save virtual call?
            DebugAssert(pVertexBufferOffsets[i] < pD3D12VertexBuffer->GetDesc()->Size);
            vertexBufferViews[i].BufferLocation = pD3D12VertexBuffer->GetD3DResource()->GetGPUVirtualAddress() + pVertexBufferOffsets[i];
            vertexBufferViews[i].SizeInBytes = pD3D12VertexBuffer->GetDesc()->Size - pVertexBufferOffsets[i];
            vertexBufferViews[i].StrideInBytes = pVertexBufferStrides[i];
        }
        else
        {
            m_currentVertexBufferOffsets[bufferIndex] = 0;
            m_currentVertexBufferStrides[bufferIndex] = 0;
            Y_memzero(&vertexBufferViews[i], sizeof(vertexBufferViews[0]));
        }

        dirtyFirst = Min(dirtyFirst, bufferIndex);
        dirtyLast = Max(dirtyLast, bufferIndex);
    }

    // changes?
    if (dirtyFirst == Y_UINT32_MAX)
        return;

    // pass to command list
    // @TODO may be worth batching this to a dirty range?
    m_pCommandList->IASetVertexBuffers(dirtyFirst, dirtyLast - dirtyFirst + 1, &vertexBufferViews[dirtyFirst - firstBuffer]);

    // update new bind count
    uint32 bindCount = 0;
    uint32 searchCount = Max((firstBuffer + nBuffers), m_currentVertexBufferBindCount);
    for (uint32 i = 0; i < searchCount; i++)
    {
        if (m_pCurrentVertexBuffers[i] != nullptr)
            bindCount = i + 1;
    }
    m_currentVertexBufferBindCount = bindCount;
}

void D3D12GPUContext::SetVertexBuffer(uint32 bufferIndex, GPUBuffer *pVertexBuffer, uint32 offset, uint32 stride)
{
    if (m_pCurrentVertexBuffers[bufferIndex] == pVertexBuffer &&
        m_currentVertexBufferOffsets[bufferIndex] == offset &&
        m_currentVertexBufferStrides[bufferIndex] == stride)
    {
        return;
    }

    if (m_pCurrentVertexBuffers[bufferIndex] != pVertexBuffer)
    {
        if (m_pCurrentVertexBuffers[bufferIndex] != nullptr)
            m_pCurrentVertexBuffers[bufferIndex]->Release();

        if ((m_pCurrentVertexBuffers[bufferIndex] = static_cast<D3D12GPUBuffer *>(pVertexBuffer)) != nullptr)
            m_pCurrentVertexBuffers[bufferIndex]->AddRef();
    }

    m_currentVertexBufferOffsets[bufferIndex] = offset;
    m_currentVertexBufferStrides[bufferIndex] = stride;

    // set in command list
    if (m_pCurrentVertexBuffers[bufferIndex] != nullptr)
    {
        D3D12_VERTEX_BUFFER_VIEW bufferView;
        DebugAssert(offset < m_pCurrentVertexBuffers[bufferIndex]->GetDesc()->Size);
        bufferView.BufferLocation = m_pCurrentVertexBuffers[bufferIndex]->GetD3DResource()->GetGPUVirtualAddress() + offset;
        bufferView.SizeInBytes = m_pCurrentVertexBuffers[bufferIndex]->GetDesc()->Size - offset;
        bufferView.StrideInBytes = stride;
        m_pCommandList->IASetVertexBuffers(bufferIndex, 1, &bufferView);
    }
    else
    {
        m_pCommandList->IASetVertexBuffers(bufferIndex, 1, nullptr);
    }

    // update new bind count
    uint32 bindCount = 0;
    uint32 searchCount = Max((bufferIndex + 1), m_currentVertexBufferBindCount);
    for (uint32 i = 0; i < searchCount; i++)
    {
        if (m_pCurrentVertexBuffers[i] != nullptr)
            bindCount = i + 1;
    }
    m_currentVertexBufferBindCount = bindCount;
}

void D3D12GPUContext::GetIndexBuffer(GPUBuffer **ppBuffer, GPU_INDEX_FORMAT *pFormat, uint32 *pOffset)
{
    *ppBuffer = m_pCurrentIndexBuffer;
    *pFormat = m_currentIndexFormat;
    *pOffset = m_currentIndexBufferOffset;
}

void D3D12GPUContext::SetIndexBuffer(GPUBuffer *pBuffer, GPU_INDEX_FORMAT format, uint32 offset)
{
    if (m_pCurrentIndexBuffer == pBuffer && m_currentIndexFormat == format && m_currentIndexBufferOffset == offset)
        return;

    if (m_pCurrentIndexBuffer != pBuffer)
    {
        if (m_pCurrentIndexBuffer != nullptr)
            m_pCurrentIndexBuffer->Release();

        if ((m_pCurrentIndexBuffer = static_cast<D3D12GPUBuffer *>(pBuffer)) != nullptr)
            m_pCurrentIndexBuffer->AddRef();
    }

    m_currentIndexFormat = format;
    m_currentIndexBufferOffset = offset;

    if (m_pCurrentIndexBuffer != nullptr)
    {
        D3D12_INDEX_BUFFER_VIEW bufferView;
        DebugAssert(offset < m_pCurrentIndexBuffer->GetDesc()->Size);
        bufferView.BufferLocation = m_pCurrentIndexBuffer->GetD3DResource()->GetGPUVirtualAddress() + offset;
        bufferView.SizeInBytes = m_pCurrentIndexBuffer->GetDesc()->Size - offset;
        bufferView.Format = (format == GPU_INDEX_FORMAT_UINT16) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        m_pCommandList->IASetIndexBuffer(&bufferView);
    }
    else
    {
        m_pCommandList->IASetIndexBuffer(nullptr);
    }
}

void D3D12GPUContext::SetShaderProgram(GPUShaderProgram *pShaderProgram)
{
    if (m_pCurrentShaderProgram == pShaderProgram)
        return;

    if (m_pCurrentShaderProgram != nullptr)
        m_pCurrentShaderProgram->Release();
    if ((m_pCurrentShaderProgram = static_cast<D3D12GPUShaderProgram *>(pShaderProgram)) != nullptr)
        m_pCurrentShaderProgram->AddRef();

    g_pRenderer->GetCounters()->IncrementPipelineChangeCounter();
    m_pipelineChanged = true;
}

void D3D12GPUContext::SetShaderParameterValue(uint32 index, SHADER_PARAMETER_TYPE valueType, const void *pValue)
{
    DebugAssert(m_pCurrentShaderProgram != nullptr);
    m_pCurrentShaderProgram->InternalSetParameterValue(this, index, valueType, pValue);
}

void D3D12GPUContext::SetShaderParameterValueArray(uint32 index, SHADER_PARAMETER_TYPE valueType, const void *pValue, uint32 firstElement, uint32 numElements)
{
    DebugAssert(m_pCurrentShaderProgram != nullptr);
    m_pCurrentShaderProgram->InternalSetParameterValueArray(this, index, valueType, pValue, firstElement, numElements);
}

void D3D12GPUContext::SetShaderParameterStruct(uint32 index, const void *pValue, uint32 valueSize)
{
    DebugAssert(m_pCurrentShaderProgram != nullptr);
    m_pCurrentShaderProgram->InternalSetParameterStruct(this, index, pValue, valueSize);
}

void D3D12GPUContext::SetShaderParameterStructArray(uint32 index, const void *pValue, uint32 valueSize, uint32 firstElement, uint32 numElements)
{
    DebugAssert(m_pCurrentShaderProgram != nullptr);
    m_pCurrentShaderProgram->InternalSetParameterStructArray(this, index, pValue, valueSize, firstElement, numElements);
}

void D3D12GPUContext::SetShaderParameterResource(uint32 index, GPUResource *pResource)
{
    DebugAssert(m_pCurrentShaderProgram != nullptr);
    m_pCurrentShaderProgram->InternalSetParameterResource(this, index, pResource, nullptr);
}

void D3D12GPUContext::SetShaderParameterTexture(uint32 index, GPUTexture *pTexture, GPUSamplerState *pSamplerState)
{
    DebugAssert(m_pCurrentShaderProgram != nullptr);
    m_pCurrentShaderProgram->InternalSetParameterResource(this, index, pTexture, pSamplerState);
}

void D3D12GPUContext::WriteConstantBuffer(uint32 bufferIndex, uint32 fieldIndex, uint32 offset, uint32 count, const void *pData, bool commit /* = false */)
{
    ConstantBuffer *cbInfo = &m_constantBuffers[bufferIndex];
    if (cbInfo->pLocalMemory == nullptr)
    {
        Log_WarningPrintf("Skipping write of %u bytes to non-existant constant buffer %u", count, bufferIndex);
        return;
    }

    DebugAssert(count > 0 && (offset + count) <= cbInfo->Size);
    if (Y_memcmp(cbInfo->pLocalMemory + offset, pData, count) != 0)
    {
        Y_memcpy(cbInfo->pLocalMemory + offset, pData, count);

        if (cbInfo->DirtyUpperBounds < 0)
        {
            cbInfo->DirtyLowerBounds = offset;
            cbInfo->DirtyUpperBounds = offset + count;
        }
        else
        {
            cbInfo->DirtyLowerBounds = Min(cbInfo->DirtyLowerBounds, (int32)offset);
            cbInfo->DirtyUpperBounds = Max(cbInfo->DirtyUpperBounds, (int32)(offset + count - 1));
        }

        if (commit)
            CommitConstantBuffer(bufferIndex);
    }
}

void D3D12GPUContext::WriteConstantBufferStrided(uint32 bufferIndex, uint32 fieldIndex, uint32 offset, uint32 bufferStride, uint32 copySize, uint32 count, const void *pData, bool commit /*= false*/)
{
    ConstantBuffer *cbInfo = &m_constantBuffers[bufferIndex];
    if (cbInfo->pLocalMemory == nullptr)
    {
        Log_WarningPrintf("Skipping write of %u bytes to non-existant constant buffer %u", count, bufferIndex);
        return;
    }

    uint32 writeSize = bufferStride * count;
    DebugAssert(writeSize > 0 && (offset + writeSize) <= cbInfo->Size);

    if (Y_memcmp_stride(cbInfo->pLocalMemory + offset, bufferStride, pData, copySize, copySize, count) != 0)
    {
        Y_memcpy_stride(cbInfo->pLocalMemory + offset, bufferStride, pData, copySize, copySize, count);

        if (cbInfo->DirtyUpperBounds < 0)
        {
            cbInfo->DirtyLowerBounds = offset;
            cbInfo->DirtyUpperBounds = offset + writeSize;
        }
        else
        {
            cbInfo->DirtyLowerBounds = Min(cbInfo->DirtyLowerBounds, (int32)offset);
            cbInfo->DirtyUpperBounds = Max(cbInfo->DirtyUpperBounds, (int32)(offset + writeSize - 1));
        }

        if (commit)
            CommitConstantBuffer(bufferIndex);
    }
}

void D3D12GPUContext::CommitConstantBuffer(uint32 bufferIndex)
{
    ConstantBuffer *cbInfo = &m_constantBuffers[bufferIndex];
    if (cbInfo->pLocalMemory == nullptr || cbInfo->DirtyLowerBounds < 0)
        return;

    // work out count to modify
    uint32 modifySize = cbInfo->DirtyUpperBounds - cbInfo->DirtyLowerBounds + 1;

    // handle per-draw constant buffers
    if (!cbInfo->PerDraw)
    {
        // allocate scratch buffer memory
        ID3D12Resource *pScratchBufferResource;
        uint32 scratchBufferOffset;
        void *pCPUPointer;
        if (!AllocateScratchBufferMemory(modifySize, 0, &pScratchBufferResource, &scratchBufferOffset, &pCPUPointer, nullptr))
        {
            Log_ErrorPrintf("D3D12GPUContext::CommitConstantBuffer: Failed to allocate scratch buffer memory.");
            return;
        }

        // copy from the constant memory store to the scratch buffer
        Y_memcpy(pCPUPointer, cbInfo->pLocalMemory + cbInfo->DirtyLowerBounds, modifySize);

        // get constant buffer pointer
        ID3D12Resource *pConstantBufferResource = m_pDevice->GetConstantBufferResource(bufferIndex);
        DebugAssert(pConstantBufferResource != nullptr);

        // block predicates
        BypassPredication();

        // queue a copy to the actual buffer
        ResourceBarrier(pConstantBufferResource, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_COPY_DEST);
        m_pCommandList->CopyBufferRegion(pConstantBufferResource, cbInfo->DirtyLowerBounds, pScratchBufferResource, scratchBufferOffset, modifySize);
        m_commandCounter++;
        ResourceBarrier(pConstantBufferResource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

        // restore predicates
        RestorePredication();
    }
    else
    {
        void *pCPUPointer;
        D3D12_GPU_VIRTUAL_ADDRESS pGPUPointer;
        if (!AllocateScratchBufferMemory(cbInfo->Size, D3D12_CONSTANT_BUFFER_ALIGNMENT, nullptr, nullptr, &pCPUPointer, &pGPUPointer))
        {
            Log_ErrorPrintf("D3D12GPUContext::CommitConstantBuffer: Failed to allocate per-draw scratch buffer memory.");
            return;
        }

        // copy from the constant memory store to the scratch buffer
        Y_memcpy(pCPUPointer, cbInfo->pLocalMemory, cbInfo->Size);

        // per-draw
        cbInfo->LastAddress = pGPUPointer;
        if (m_pCurrentShaderProgram != nullptr && !m_pipelineChanged)
            m_pCurrentShaderProgram->RebindPerDrawConstantBuffer(this, bufferIndex, pGPUPointer);
    }

    // reset range
    cbInfo->DirtyLowerBounds = cbInfo->DirtyUpperBounds = -1;
}

void D3D12GPUContext::SetShaderConstantBuffers(SHADER_PROGRAM_STAGE stage, uint32 index, const D3D12DescriptorHandle &handle)
{
    ShaderStageState *state = &m_shaderStates[stage];
    if (state->ConstantBuffers[index] == handle)
        return;

    state->ConstantBuffers[index] = handle;
    state->ConstantBufferBindCount = Max(state->ConstantBufferBindCount, index + 1);
    state->ConstantBuffersDirty = true;
}

void D3D12GPUContext::SetShaderResources(SHADER_PROGRAM_STAGE stage, uint32 index, const D3D12DescriptorHandle &handle)
{
    ShaderStageState *state = &m_shaderStates[stage];
    if (state->Resources[index] == handle)
        return;

    state->Resources[index] = handle;
    if (!handle.IsNull())
    {
        // update max count
        state->ResourceBindCount = Max(state->ResourceBindCount, index + 1);
    }
    else
    {
        state->ResourceBindCount = 0;
        for (uint32 i = 0; i < D3D12_LEGACY_GRAPHICS_ROOT_SHADER_RESOURCE_SLOTS; i++)
        {
            if (!state->Resources[i].IsNull())
                state->ResourceBindCount = i + 1;
        }
    }

    state->ResourcesDirty = true;
}

void D3D12GPUContext::SetShaderSamplers(SHADER_PROGRAM_STAGE stage, uint32 index, const D3D12DescriptorHandle &handle)
{
    ShaderStageState *state = &m_shaderStates[stage];
    if (state->Samplers[index] == handle)
        return;

    state->Samplers[index] = handle;
    state->SamplerBindCount = Max(state->SamplerBindCount, index + 1);
    state->SamplersDirty = true;
}

void D3D12GPUContext::SetShaderUAVs(SHADER_PROGRAM_STAGE stage, uint32 index, const D3D12DescriptorHandle &handle)
{
    ShaderStageState *state = &m_shaderStates[stage];
    if (state->UAVs[index] == handle)
        return;

    state->UAVs[index] = handle;
    state->UAVBindCount = Max(state->UAVBindCount, index + 1);
    state->UAVsDirty = true;
}

void D3D12GPUContext::SetPerDrawConstantBuffer(uint32 index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    DebugAssert(index < m_perDrawConstantBuffers.GetSize());
    if (m_perDrawConstantBuffers[index] == address)
        return;

    m_perDrawConstantBuffers[index] = address;
    m_perDrawConstantBufferCount = Max(m_perDrawConstantBufferCount, index + 1);
    m_perDrawConstantBuffersDirty = true;
}

void D3D12GPUContext::SynchronizeRenderTargetsAndUAVs()
{
    D3D12_CPU_DESCRIPTOR_HANDLE renderTargetHandles[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
    D3D12_CPU_DESCRIPTOR_HANDLE depthStencilHandle;
    uint32 renderTargetCount = 0;
    bool hasDepthStencil = false;

    // get render target views
    if (m_nCurrentRenderTargets == 0 && m_pCurrentDepthBufferView == nullptr)
    {
        // using swap chain
        if (m_pCurrentSwapChain != nullptr)
        {
            // update the render target index
            if (m_pCurrentSwapChain->UpdateCurrentBackBuffer())
            {
                // it's changed, so we need a resource barrier
                ResourceBarrier(m_pCurrentSwapChain->GetCurrentBackBufferResource(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }

            renderTargetHandles[0] = m_pCurrentSwapChain->GetCurrentBackBufferViewDescriptorCPUHandle();
            depthStencilHandle = m_pCurrentSwapChain->GetDepthStencilBufferViewDescriptorCPUHandle();
            hasDepthStencil = m_pCurrentSwapChain->HasDepthStencilBuffer();
            renderTargetCount = 1;
        }
    }
    else
    {
        // get render target views
        for (uint32 renderTargetIndex = 0; renderTargetIndex < m_nCurrentRenderTargets; renderTargetIndex++)
        {
            if (m_pCurrentRenderTargetViews[renderTargetIndex] != nullptr)
            {
                renderTargetHandles[renderTargetIndex] = m_pCurrentRenderTargetViews[renderTargetIndex]->GetDescriptorHandle();
                renderTargetCount = renderTargetIndex + 1;
            }
            else
            {
                renderTargetHandles[renderTargetIndex].ptr = 0;
            }
        }

        // get depth stencil view
        if (m_pCurrentDepthBufferView != nullptr)
        {
            hasDepthStencil = true;
            depthStencilHandle = m_pCurrentDepthBufferView->GetDescriptorHandle();
        }
    }

    m_pCommandList->OMSetRenderTargets(renderTargetCount, (renderTargetCount > 0) ? renderTargetHandles : nullptr, FALSE, (hasDepthStencil) ? &depthStencilHandle : nullptr);
}

bool D3D12GPUContext::UpdatePipelineState(bool force)
{
    // new pipeline state required?
    force = force || m_pipelineChanged;
    if (m_pCurrentShaderProgram != nullptr)
    {
        if (m_pipelineChanged || force)
        {
            // fill pipeline state key
            D3D12GPUShaderProgram::PipelineStateKey key;
            Y_memzero(&key, sizeof(key));

            // render targets
            if (m_nCurrentRenderTargets == 0 && m_pCurrentDepthBufferView == nullptr)
            {
                if (m_pCurrentSwapChain != nullptr)
                {
                    key.RenderTargetCount = 1;
                    key.RTVFormats[0] = m_pCurrentSwapChain->GetBackBufferFormat();
                    key.DSVFormat = m_pCurrentSwapChain->GetDepthStencilBufferFormat();
                }
            }
            else
            {
                key.RenderTargetCount = m_nCurrentRenderTargets;
                for (uint32 i = 0; i < m_nCurrentRenderTargets; i++)
                    key.RTVFormats[i] = (m_pCurrentRenderTargetViews[i] != nullptr) ? m_pCurrentRenderTargetViews[i]->GetDesc()->Format : PIXEL_FORMAT_UNKNOWN;
                key.DSVFormat = (m_pCurrentDepthBufferView != nullptr) ? m_pCurrentDepthBufferView->GetDesc()->Format : PIXEL_FORMAT_UNKNOWN;
            }

            // rasterizer state
            if (m_pCurrentRasterizerState == nullptr)
                return false;
            Y_memcpy(&key.RasterizerState, m_pCurrentRasterizerState->GetD3DRasterizerStateDesc(), sizeof(key.RasterizerState));

            // depthstencil state
            if (m_pCurrentDepthStencilState == nullptr)
                return false;
            Y_memcpy(&key.DepthStencilState, m_pCurrentDepthStencilState->GetD3DDepthStencilDesc(), sizeof(key.DepthStencilState));

            // blend state
            if (m_pCurrentBlendState == nullptr)
                return false;
            Y_memcpy(&key.BlendState, m_pCurrentBlendState->GetD3DBlendDesc(), sizeof(key.BlendState));

            // topology
            key.PrimitiveTopologyType = m_currentD3DTopologyType;
            if (!m_pCurrentShaderProgram->Switch(this, m_pCommandList, &key))
                return false;

            // up-to-date
            g_pRenderer->GetCounters()->IncrementPipelineChangeCounter();
            m_pipelineChanged = false;
        }
    }
    else if (!force)
    {
        // no shader bound and not restoring
        return false;
    }

    // allocate an array of cpu pointers, uses alloca so it's at the end of the stack
    D3D12_CPU_DESCRIPTOR_HANDLE *pCPUHandles = (D3D12_CPU_DESCRIPTOR_HANDLE *)alloca(sizeof(D3D12_CPU_DESCRIPTOR_HANDLE) * 32);
    uint32 *pCPUHandleCounts = (uint32 *)alloca(sizeof(uint32) * 32);
    for (uint32 i = 0; i < 32; i++)
        pCPUHandleCounts[i] = 1;

    // update states
    for (uint32 stage = 0; stage <= SHADER_PROGRAM_STAGE_PIXEL_SHADER; stage++)
    {
        ShaderStageState *state = &m_shaderStates[stage];
        //uint32 base = (stage == SHADER_PROGRAM_STAGE_PIXEL_SHADER) ? 3 : 0;
        uint32 base = stage * 3;

        // Constant buffers
        if (state->ConstantBuffersDirty || force)
        {
            // find the new bind count
            for (uint32 i = 0; i < state->ConstantBufferBindCount; i++)
            {
                if (!state->ConstantBuffers[i].IsNull())
                    pCPUHandles[i] = state->ConstantBuffers[i];
                else
                    pCPUHandles[i] = m_pDevice->GetNullCBVDescriptorHandle();
            }
            for (uint32 i = state->ConstantBufferBindCount; i < D3D12_LEGACY_GRAPHICS_ROOT_CONSTANT_BUFFER_SLOTS; i++)
                pCPUHandles[i] = m_pDevice->GetNullCBVDescriptorHandle();

            // allocate scratch descriptors and copy
            if (AllocateScratchView(D3D12_LEGACY_GRAPHICS_ROOT_CONSTANT_BUFFER_SLOTS, &state->CBVTableCPUHandle, &state->CBVTableGPUHandle))
            {
                uint32 count = D3D12_LEGACY_GRAPHICS_ROOT_CONSTANT_BUFFER_SLOTS;
                m_pD3DDevice->CopyDescriptors(1, &state->CBVTableCPUHandle, &count, count, pCPUHandles, pCPUHandleCounts, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                m_pCommandList->SetGraphicsRootDescriptorTable(base + 0, state->CBVTableGPUHandle);
            }

            state->ConstantBuffersDirty = false;
        }

        // Resources
        if (state->ResourcesDirty || force)
        {
            // find the new bind count
            for (uint32 i = 0; i < state->ResourceBindCount; i++)
            {
                if (!state->Resources[i].IsNull())
                    pCPUHandles[i] = state->Resources[i];
                else
                    pCPUHandles[i] = m_pDevice->GetNullSRVDescriptorHandle();
            }
            for (uint32 i = state->ResourceBindCount; i < D3D12_LEGACY_GRAPHICS_ROOT_SHADER_RESOURCE_SLOTS; i++)
                pCPUHandles[i] = m_pDevice->GetNullSRVDescriptorHandle();

            // allocate scratch descriptors and copy
            if (AllocateScratchView(D3D12_LEGACY_GRAPHICS_ROOT_SHADER_RESOURCE_SLOTS, &state->SRVTableCPUHandle, &state->SRVTableGPUHandle))
            {
                uint32 count = D3D12_LEGACY_GRAPHICS_ROOT_SHADER_RESOURCE_SLOTS;
                m_pD3DDevice->CopyDescriptors(1, &state->SRVTableCPUHandle, &count, count, pCPUHandles, pCPUHandleCounts, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                m_pCommandList->SetGraphicsRootDescriptorTable(base + 1, state->SRVTableGPUHandle);
            }

            state->ResourcesDirty = false;
        }

        // Samplers
        if (state->SamplersDirty || force)
        {
            // find the new bind count
            for (uint32 i = 0; i < state->SamplerBindCount; i++)
            {
                if (!state->Samplers[i].IsNull())
                    pCPUHandles[i] = state->Samplers[i];
                else
                    pCPUHandles[i] = m_pDevice->GetNullSamplerHandle();
            }
            for (uint32 i = state->SamplerBindCount; i < D3D12_LEGACY_GRAPHICS_ROOT_SHADER_SAMPLER_SLOTS; i++)
                pCPUHandles[i] = m_pDevice->GetNullSamplerHandle();

            // allocate scratch descriptors and copy
            if (AllocateScratchSamplers(D3D12_LEGACY_GRAPHICS_ROOT_SHADER_SAMPLER_SLOTS, &state->SamplerTableCPUHandle, &state->SamplerTableGPUHandle))
            {
                uint32 count = D3D12_LEGACY_GRAPHICS_ROOT_SHADER_SAMPLER_SLOTS;
                m_pD3DDevice->CopyDescriptors(1, &state->SamplerTableCPUHandle, &count, count, pCPUHandles, pCPUHandleCounts, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
                m_pCommandList->SetGraphicsRootDescriptorTable(base + 2, state->SamplerTableGPUHandle);
            }

            state->SamplersDirty = false;
        }

//         // UAVs
//         if (stage == SHADER_PROGRAM_STAGE_PIXEL_SHADER && (state->UAVsDirty || force))
//         {
//             // find the new bind count
//             uint32 bindCount = 0;
//             for (uint32 i = 0; i < state->SamplerBindCount; i++)
//             {
//                 if (!state->UAVs[i].IsNull())
//                 {
//                     pCPUHandles[i] = state->UAVs[i];
//                     bindCount = i + 1;
//                 }
//                 else
//                 {
//                     pCPUHandles[i].ptr = 0;
//                 }
//             }
//             state->UAVBindCount = bindCount;
//             state->UAVsDirty = false;
// 
//             // allocate scratch descriptors and copy
//             if (bindCount > 0 && AllocateScratchView(bindCount, &state->UAVTableCPUHandle, &state->UAVTableGPUHandle))
//             {
//                 m_pD3DDevice->CopyDescriptors(1, &state->UAVTableCPUHandle, &bindCount, bindCount, pCPUHandles, nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
//                 m_pCommandList->SetGraphicsRootDescriptorTable(15, state->UAVTableGPUHandle);
//             }
//         }
    }

    // Commit global constant buffer
    m_pConstants->CommitGlobalConstantBufferChanges();

    // per-draw constants
    if (m_perDrawConstantBuffersDirty || force)
    {
        for (uint32 i = 0; i < m_perDrawConstantBufferCount; i++)
            m_pCommandList->SetGraphicsRootConstantBufferView(16 + i, m_perDrawConstantBuffers[i]);
        for (uint32 i = m_perDrawConstantBufferCount; i < D3D12_LEGACY_GRAPHICS_ROOT_PER_DRAW_CONSTANT_BUFFER_SLOTS; i++)
            m_pCommandList->SetGraphicsRootConstantBufferView(16 + i, m_pCurrentScratchBuffer->GetGPUAddress(0));

        m_perDrawConstantBuffersDirty = false;
    }

    return true;
}

void D3D12GPUContext::Draw(uint32 firstVertex, uint32 nVertices)
{
    if (nVertices == 0 || !UpdatePipelineState(false))
        return;
    
    m_pCommandList->DrawInstanced(nVertices, 1, firstVertex, 0);
    m_commandCounter++;

    g_pRenderer->GetCounters()->IncrementDrawCallCounter();
}

void D3D12GPUContext::DrawInstanced(uint32 firstVertex, uint32 nVertices, uint32 nInstances)
{
    if (nVertices == 0 || nInstances == 0 || !UpdatePipelineState(false))
        return;

    m_pCommandList->DrawInstanced(nVertices, nInstances, firstVertex, 0);
    m_commandCounter++;

    g_pRenderer->GetCounters()->IncrementDrawCallCounter();
}

void D3D12GPUContext::DrawIndexed(uint32 startIndex, uint32 nIndices, uint32 baseVertex)
{
    if (nIndices == 0 || !UpdatePipelineState(false))
        return;

    m_pCommandList->DrawIndexedInstanced(nIndices, 1, startIndex, baseVertex, 0);
    m_commandCounter++;

    g_pRenderer->GetCounters()->IncrementDrawCallCounter();
}

void D3D12GPUContext::DrawIndexedInstanced(uint32 startIndex, uint32 nIndices, uint32 baseVertex, uint32 nInstances)
{
    if (nIndices == 0 || !UpdatePipelineState(false))
        return;

    m_pCommandList->DrawIndexedInstanced(nIndices, nInstances, startIndex, baseVertex, 0);
    m_commandCounter++;

    g_pRenderer->GetCounters()->IncrementDrawCallCounter();
}

void D3D12GPUContext::Dispatch(uint32 threadGroupCountX, uint32 threadGroupCountY, uint32 threadGroupCountZ)
{
    if (!UpdatePipelineState(false))
        return;

    m_pCommandList->Dispatch(threadGroupCountX, threadGroupCountY, threadGroupCountZ);
    m_commandCounter++;

    g_pRenderer->GetCounters()->IncrementDrawCallCounter();
}

void D3D12GPUContext::DrawUserPointer(const void *pVertices, uint32 vertexSize, uint32 nVertices)
{
    if (!UpdatePipelineState(false))
        return;

#if 1
    // can use scratch buffer directly. 
    // this probably should use a threshold to go to an upload buffer..
    uint32 bufferSpaceRequired = vertexSize * nVertices;
    ID3D12Resource *pScratchBufferResource;
    void *pScratchBufferCPUPointer;
    D3D12_GPU_VIRTUAL_ADDRESS scratchBufferGPUPointer;
    uint32 scratchBufferOffset;
    if (!AllocateScratchBufferMemory(bufferSpaceRequired, 0, &pScratchBufferResource, &scratchBufferOffset, &pScratchBufferCPUPointer, &scratchBufferGPUPointer))
        return;

    // copy the contents in
    Y_memcpy(pScratchBufferCPUPointer, pVertices, bufferSpaceRequired);

    // set vertex buffer
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView;
    vertexBufferView.BufferLocation = scratchBufferGPUPointer;
    vertexBufferView.SizeInBytes = bufferSpaceRequired;
    vertexBufferView.StrideInBytes = vertexSize;
    m_pCommandList->IASetVertexBuffers(0, 1, &vertexBufferView);

    // invoke draw
    m_pCommandList->DrawInstanced(nVertices, 1, 0, 0);
    m_commandCounter++;

    // restore vertex buffer
    if (m_pCurrentVertexBuffers[0] != nullptr)
    {
        vertexBufferView.BufferLocation = m_pCurrentVertexBuffers[0]->GetD3DResource()->GetGPUVirtualAddress() + m_currentVertexBufferOffsets[0];
        vertexBufferView.SizeInBytes = m_pCurrentVertexBuffers[0]->GetDesc()->Size - m_currentVertexBufferOffsets[0];
        vertexBufferView.StrideInBytes = m_currentVertexBufferStrides[0];
        m_pCommandList->IASetVertexBuffers(0, 1, &vertexBufferView);
    }
    else
    {
        m_pCommandList->IASetVertexBuffers(0, 1, nullptr);
    }

#else
    uint32 bufferSpaceRequired = vertexSize * nVertices;

    ID3D12Resource *pResource;
    D3D12_HEAP_PROPERTIES heapProperties = { D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC resourceDesc = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, bufferSpaceRequired + 1, 1, 1, 1, DXGI_FORMAT_UNKNOWN,{ 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE };
    HRESULT hResult = m_pD3DDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&pResource));
    if (FAILED(hResult))
    {
        Log_ErrorPrintf("CreateCommitedResource for upload resource failed with hResult %08X", hResult);
        return;
    }

    // map the upload buffer
    void *pMappedPointer;
    D3D12_RANGE readRange = { 0, 0 };
    hResult = pResource->Map(0, D3D12_MAP_RANGE_PARAM(&readRange), &pMappedPointer);
    if (FAILED(hResult))
    {
        Log_ErrorPrintf("Failed to map upload buffer: %08X", hResult);
        pResource->Release();
        return;
    }

    // copy the contents over, and unmap the buffer
    D3D12_RANGE writeRange = { 0, bufferSpaceRequired };
    Y_memcpy(pMappedPointer, pVertices, bufferSpaceRequired);
    pResource->Unmap(0, D3D12_MAP_RANGE_PARAM(&writeRange));

    ID3D12Resource *pResource2;
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hResult = m_pD3DDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&pResource2));
    m_pCommandList->CopyBufferRegion(pResource2, 0, pResource, 0, bufferSpaceRequired);
    ResourceBarrier(pResource2, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    // set vertex buffer
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView;
    vertexBufferView.BufferLocation = pResource2->GetGPUVirtualAddress();
    vertexBufferView.SizeInBytes = bufferSpaceRequired;
    vertexBufferView.StrideInBytes = vertexSize;
    m_pCommandList->IASetVertexBuffers(0, 1, &vertexBufferView);
    m_pCommandList->DrawInstanced(nVertices, 1, 0, 0);
    D3D12RenderBackend::GetInstance()->GetGraphicsCommandQueue()->ScheduleResourceForDeletion(pResource);
    D3D12RenderBackend::GetInstance()->GetGraphicsCommandQueue()->ScheduleResourceForDeletion(pResource2);
#endif

    g_pRenderer->GetCounters()->IncrementDrawCallCounter();
}


bool D3D12GPUContext::CopyTexture(GPUTexture2D *pSourceTexture, GPUTexture2D *pDestinationTexture)
{
    // textures have to be compatible, for now this means same texture format
    D3D12GPUTexture2D *pD3D12SourceTexture = static_cast<D3D12GPUTexture2D *>(pSourceTexture);
    D3D12GPUTexture2D *pD3D12DestinationTexture = static_cast<D3D12GPUTexture2D *>(pDestinationTexture);
    if (pD3D12SourceTexture->GetDesc()->Width != pD3D12DestinationTexture->GetDesc()->Width ||
        pD3D12SourceTexture->GetDesc()->Height != pD3D12DestinationTexture->GetDesc()->Height ||
        pD3D12SourceTexture->GetDesc()->Format != pD3D12DestinationTexture->GetDesc()->Format ||
        pD3D12SourceTexture->GetDesc()->MipLevels != pD3D12DestinationTexture->GetDesc()->MipLevels)
    {
        return false;
    }

    // get old state for both
    D3D12_RESOURCE_STATES sourceResourceState = GetCurrentResourceState(pSourceTexture);
    D3D12_RESOURCE_STATES destinationResourceState = GetCurrentResourceState(pDestinationTexture);

    // switch to copy states
    ResourceBarrier(pD3D12SourceTexture->GetD3DResource(), sourceResourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ResourceBarrier(pD3D12DestinationTexture->GetD3DResource(), destinationResourceState, D3D12_RESOURCE_STATE_COPY_DEST);

    // copy each mip level
    for (uint32 i = 0; i < pD3D12SourceTexture->GetDesc()->MipLevels; i++)
    {
        D3D12_TEXTURE_COPY_LOCATION sourceLocation = { pD3D12SourceTexture->GetD3DResource(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, i };
        D3D12_TEXTURE_COPY_LOCATION destinationLocation = { pD3D12DestinationTexture->GetD3DResource(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, i };
        m_pCommandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);
        m_commandCounter++;
    }

    // switch back from copy states
    ResourceBarrier(pD3D12SourceTexture->GetD3DResource(), D3D12_RESOURCE_STATE_COPY_SOURCE, sourceResourceState);
    ResourceBarrier(pD3D12DestinationTexture->GetD3DResource(), D3D12_RESOURCE_STATE_COPY_DEST, destinationResourceState);
    return true;
}

bool D3D12GPUContext::CopyTextureRegion(GPUTexture2D *pSourceTexture, uint32 sourceX, uint32 sourceY, uint32 width, uint32 height, uint32 sourceMipLevel, GPUTexture2D *pDestinationTexture, uint32 destX, uint32 destY, uint32 destMipLevel)
{
#if 0
    // textures have to be compatible, for now this means same texture format
    D3D12GPUTexture2D *pD3D12SourceTexture = static_cast<D3D12GPUTexture2D *>(pSourceTexture);
    D3D12GPUTexture2D *pD3D12DestinationTexture = static_cast<D3D12GPUTexture2D *>(pDestinationTexture);
    if (pD3D12SourceTexture->GetDesc()->Format != pD3D12DestinationTexture->GetDesc()->Format ||
        pD3D12SourceTexture->GetDesc()->MipLevels != pD3D12DestinationTexture->GetDesc()->MipLevels)
    {
        return false;
    }

    // create source box, copy it
    D3D12_BOX sourceBox = { sourceX, sourceY, 0, sourceX + width, sourceY + height, 1 };
    m_pD3DContext->CopySubresourceRegion(pD3D12DestinationTexture->GetD3DTexture(), D3D12CalcSubresource(destMipLevel, 0, pD3D12DestinationTexture->GetDesc()->MipLevels), destX, destY, 0,
                                         pD3D12SourceTexture->GetD3DTexture(), D3D12CalcSubresource(sourceMipLevel, 0, pD3D12SourceTexture->GetDesc()->MipLevels), &sourceBox);

    return true;
#endif
    return false;
}

void D3D12GPUContext::BlitFrameBuffer(GPUTexture2D *pTexture, uint32 sourceX, uint32 sourceY, uint32 sourceWidth, uint32 sourceHeight, uint32 destX, uint32 destY, uint32 destWidth, uint32 destHeight, RENDERER_FRAMEBUFFER_BLIT_RESIZE_FILTER resizeFilter /*= RENDERER_FRAMEBUFFER_BLIT_RESIZE_FILTER_NEAREST*/)
{
#if 0
    DebugAssert(m_nCurrentRenderTargets <= 1);

    // read source formats
    DebugAssert(static_cast<D3D12GPUTexture2D *>(pTexture)->GetDesc()->Flags & GPU_TEXTURE_FLAG_SHADER_BINDABLE);
    DXGI_FORMAT sourceTextureFormat = D3D12TypeConversion::PixelFormatToDXGIFormat(static_cast<D3D12GPUTexture2D *>(pTexture)->GetDesc()->Format);
    ID3D12Resource *pSourceResource = static_cast<D3D12GPUTexture2D *>(pTexture)->GetD3DTexture();
    uint32 sourceTextureWidth = static_cast<D3D12GPUTexture2D *>(pTexture)->GetDesc()->Width;
    uint32 sourceTextureHeight = static_cast<D3D12GPUTexture2D *>(pTexture)->GetDesc()->Height;

    // read destination format
    DXGI_FORMAT destinationTextureFormat = DXGI_FORMAT_UNKNOWN;
    ID3D12Resource *pDestinationResource = NULL;
    uint32 destinationTextureWidth = 0, destinationTextureHeight = 0;
    if (m_nCurrentRenderTargets == 0)
    {
        destinationTextureFormat = static_cast<D3D12GPUOutputBuffer *>(m_pCurrentSwapChain)->GetBackBufferFormat();
        pDestinationResource = static_cast<D3D12GPUOutputBuffer *>(m_pCurrentSwapChain)->GetBackBufferTexture();
        destinationTextureWidth = static_cast<D3D12GPUOutputBuffer *>(m_pCurrentSwapChain)->GetWidth();
        destinationTextureHeight = static_cast<D3D12GPUOutputBuffer *>(m_pCurrentSwapChain)->GetHeight();
    }
    else
    {
        switch (m_pCurrentRenderTargetViews[0]->GetTargetTexture()->GetTextureType())
        {
        case TEXTURE_TYPE_2D:
            destinationTextureFormat = D3D12TypeConversion::PixelFormatToDXGIFormat(static_cast<D3D12GPUTexture2D *>(m_pCurrentRenderTargetViews[0]->GetTargetTexture())->GetDesc()->Format);
            pDestinationResource = static_cast<D3D12GPUTexture2D *>(m_pCurrentRenderTargetViews[0]->GetTargetTexture())->GetD3DTexture();
            destinationTextureWidth = static_cast<D3D12GPUTexture2D *>(m_pCurrentRenderTargetViews[0]->GetTargetTexture())->GetDesc()->Width;
            destinationTextureHeight = static_cast<D3D12GPUTexture2D *>(m_pCurrentRenderTargetViews[0]->GetTargetTexture())->GetDesc()->Height;
            break;

        default:
            Panic("D3D12GPUContext::BlitFrameBuffer: Destination is an unsupported texture type");
            return;
        }
    }

    // D3D12 can do a direct copy between identical types
    if (sourceTextureFormat == destinationTextureFormat && sourceWidth == destWidth && sourceHeight == destHeight)
    {
        // whole texture?
        if (sourceX == 0 && sourceY == 0 && destX == 0 && destY == 0 && sourceWidth == sourceTextureWidth && sourceHeight == sourceTextureHeight)
        {
            // use CopyResource
            m_pD3DContext->CopyResource(pDestinationResource, pSourceResource);
            return;
        }
        else
        {
            // use CopySubResourceRegion
            D3D12_BOX sourceBox = { sourceX, sourceY, 0, sourceX + sourceWidth, sourceY + sourceHeight, 1 };
            m_pD3DContext->CopySubresourceRegion(pDestinationResource, 0, destX, destY, 0, pSourceResource, 0, &sourceBox);
            return;
        }
    }

    // use shader
    g_pRenderer->BlitTextureUsingShader(this, pTexture, sourceX, sourceY, sourceWidth, sourceHeight, 0, destX, destY, destWidth, destHeight, resizeFilter, RENDERER_FRAMEBUFFER_BLIT_BLEND_MODE_NONE);
#endif
}

void D3D12GPUContext::GenerateMips(GPUTexture *pTexture)
{
    // @TODO has to be done using shaders :(
#if 0
    ID3D12ShaderResourceView *pSRV = nullptr;
    uint32 flags = 0;
    switch (pTexture->GetTextureType())
    {
    case TEXTURE_TYPE_1D:
        pSRV = static_cast<D3D12GPUTexture1D *>(pTexture)->GetD3DSRV();
        flags = static_cast<D3D12GPUTexture1D *>(pTexture)->GetDesc()->Flags;
        break;

    case TEXTURE_TYPE_1D_ARRAY:
        pSRV = static_cast<D3D12GPUTexture1DArray *>(pTexture)->GetD3DSRV();
        flags = static_cast<D3D12GPUTexture1DArray *>(pTexture)->GetDesc()->Flags;
        break;

    case TEXTURE_TYPE_2D:
        pSRV = static_cast<D3D12GPUTexture2D *>(pTexture)->GetD3DSRV();
        flags = static_cast<D3D12GPUTexture2D *>(pTexture)->GetDesc()->Flags;
        break;

    case TEXTURE_TYPE_2D_ARRAY:
        pSRV = static_cast<D3D12GPUTexture2DArray *>(pTexture)->GetD3DSRV();
        flags = static_cast<D3D12GPUTexture2DArray *>(pTexture)->GetDesc()->Flags;
        break;

    case TEXTURE_TYPE_3D:
        pSRV = static_cast<D3D12GPUTexture3D *>(pTexture)->GetD3DSRV();
        flags = static_cast<D3D12GPUTexture3D *>(pTexture)->GetDesc()->Flags;
        break;

    case TEXTURE_TYPE_CUBE:
        pSRV = static_cast<D3D12GPUTextureCube *>(pTexture)->GetD3DSRV();
        flags = static_cast<D3D12GPUTextureCube *>(pTexture)->GetDesc()->Flags;
        break;

    case TEXTURE_TYPE_CUBE_ARRAY:
        pSRV = static_cast<D3D12GPUTextureCubeArray *>(pTexture)->GetD3DSRV();
        flags = static_cast<D3D12GPUTextureCubeArray *>(pTexture)->GetDesc()->Flags;
        break;
    }

    if (pSRV == nullptr || !(flags & GPU_TEXTURE_FLAG_GENERATE_MIPS))
    {
        Log_ErrorPrintf("D3D12GPUContext::GenerateMips: Texture not created with GPU_TEXTURE_FLAG_GENERATE_MIPS.");
        return;
    }

    m_pD3DContext->GenerateMips(pSRV);
#endif
}

GPUCommandList *D3D12GPUContext::CreateCommandList()
{
    D3D12GPUCommandList *pCommandList = new D3D12GPUCommandList(m_pDevice, m_pD3DDevice);
    if (!pCommandList->Create(m_pGraphicsCommandQueue))
    {
        pCommandList->Release();
        return nullptr;
    }
    
    return pCommandList;
}

bool D3D12GPUContext::OpenCommandList(GPUCommandList *pCommandList)
{
    D3D12GPUCommandList *pD3D12CommandList = reinterpret_cast<D3D12GPUCommandList *>(pCommandList);
    return pD3D12CommandList->Open(m_pGraphicsCommandQueue, m_pCurrentSwapChain);
}

bool D3D12GPUContext::CloseCommandList(GPUCommandList *pCommandList)
{
    D3D12GPUCommandList *pD3D12CommandList = reinterpret_cast<D3D12GPUCommandList *>(pCommandList);

    // no errors?
    if (pD3D12CommandList->Close())
        return true;

    // release everything it's used
    pD3D12CommandList->ReleaseAllocators(m_pGraphicsCommandQueue->GetNextFenceValue());
    return false;
}

void D3D12GPUContext::ExecuteCommandList(GPUCommandList *pCommandList)
{
    D3D12GPUCommandList *pD3D12CommandList = reinterpret_cast<D3D12GPUCommandList *>(pCommandList);

    // if there was no worthwhile (draw, copy) commands issued since the last flush, don't flush this list
    if (m_commandCounter == 0)
    {
        // but still transition pending resources
        m_pDevice->TransitionPendingResources(m_pGraphicsCommandQueue);
    }
    else
    {
        // current command list has to be executed before (this will also take care of any transitions)
        CloseAndExecuteCommandList(false, false);
        ResetCommandList(true, false);
    }

    // execute on our command queue
    m_pGraphicsCommandQueue->ExecuteCommandList(pD3D12CommandList->GetD3DCommandList());
    
    // release everything the command list used
    pD3D12CommandList->ReleaseAllocators(m_pGraphicsCommandQueue->GetNextFenceValue());

    // flag all constant buffers as dirty, as the command list may have modified them
    for (ConstantBuffer &constantBuffer : m_constantBuffers)
    {
        if (constantBuffer.pLocalMemory != nullptr)
        {
            constantBuffer.DirtyLowerBounds = 0;
            constantBuffer.DirtyUpperBounds = constantBuffer.Size - 1;
        }
    }
}
