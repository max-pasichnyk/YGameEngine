#include "D3D12Renderer/PrecompiledHeader.h"
#include "D3D12Renderer/D3D12GPUShaderProgram.h"
#include "D3D12Renderer/D3D12GPUDevice.h"
#include "D3D12Renderer/D3D12GPUContext.h"
#include "D3D12Renderer/D3D12GPUCommandList.h"
#include "D3D12Renderer/D3D12Helpers.h"
#include "D3D12Renderer/D3D12CVars.h"
#include "Renderer/ShaderConstantBuffer.h"
#include "Engine/EngineCVars.h"
#include "Core/ThirdParty/MurmurHash3.h"
Log_SetChannel(D3D12RenderBackend);

// lookup table for types
static const DXGI_FORMAT s_GPUVertexElementTypeToDXGIFormat[GPU_VERTEX_ELEMENT_TYPE_COUNT] =
{
    DXGI_FORMAT_R8_SINT,            // GPU_VERTEX_ELEMENT_TYPE_BYTE
    DXGI_FORMAT_R8G8_SINT,          // GPU_VERTEX_ELEMENT_TYPE_BYTE2
    DXGI_FORMAT_R8G8B8A8_SINT,      // GPU_VERTEX_ELEMENT_TYPE_BYTE4
    DXGI_FORMAT_R8_UINT,            // GPU_VERTEX_ELEMENT_TYPE_UBYTE
    DXGI_FORMAT_R8G8_UINT,          // GPU_VERTEX_ELEMENT_TYPE_UBYTE2
    DXGI_FORMAT_R8G8B8A8_UINT,      // GPU_VERTEX_ELEMENT_TYPE_UBYTE4
    DXGI_FORMAT_R16_FLOAT,          // GPU_VERTEX_ELEMENT_TYPE_HALF
    DXGI_FORMAT_R16G16_FLOAT,       // GPU_VERTEX_ELEMENT_TYPE_HALF2
    DXGI_FORMAT_R16G16B16A16_FLOAT, // GPU_VERTEX_ELEMENT_TYPE_HALF4
    DXGI_FORMAT_R32_FLOAT,          // GPU_VERTEX_ELEMENT_TYPE_FLOAT
    DXGI_FORMAT_R32G32_FLOAT,       // GPU_VERTEX_ELEMENT_TYPE_FLOAT2
    DXGI_FORMAT_R32G32B32_FLOAT,    // GPU_VERTEX_ELEMENT_TYPE_FLOAT3
    DXGI_FORMAT_R32G32B32A32_FLOAT, // GPU_VERTEX_ELEMENT_TYPE_FLOAT4
    DXGI_FORMAT_R32_SINT,           // GPU_VERTEX_ELEMENT_TYPE_INT
    DXGI_FORMAT_R32G32_SINT,        // GPU_VERTEX_ELEMENT_TYPE_INT2
    DXGI_FORMAT_R32G32B32_SINT,     // GPU_VERTEX_ELEMENT_TYPE_INT3
    DXGI_FORMAT_R32G32B32A32_SINT,  // GPU_VERTEX_ELEMENT_TYPE_INT4
    DXGI_FORMAT_R32_UINT,           // GPU_VERTEX_ELEMENT_TYPE_UINT
    DXGI_FORMAT_R32G32_UINT,        // GPU_VERTEX_ELEMENT_TYPE_UINT2
    DXGI_FORMAT_R32G32B32_UINT,     // GPU_VERTEX_ELEMENT_TYPE_UINT3
    DXGI_FORMAT_R32G32B32A32_UINT,  // GPU_VERTEX_ELEMENT_TYPE_UINT4
    DXGI_FORMAT_R8G8B8A8_SNORM,     // GPU_VERTEX_ELEMENT_TYPE_SNORM4
    DXGI_FORMAT_R8G8B8A8_UNORM,     // GPU_VERTEX_ELEMENT_TYPE_UNORM4
};

D3D12GPUShaderProgram::D3D12GPUShaderProgram(D3D12GPUDevice *pDevice)
    : m_pDevice(pDevice)
{
    m_pDevice->AddRef();
    Y_memzero(m_pStageByteCode, sizeof(m_pStageByteCode));
}

D3D12GPUShaderProgram::~D3D12GPUShaderProgram()
{
    for (uint32 i = 0; i < m_pipelineStates.GetSize(); i++)
    {
        if (m_pipelineStates[i].pPipelineState != nullptr)
            m_pDevice->ScheduleResourceForDeletion(m_pipelineStates[i].pPipelineState);
    }

    for (uint32 i = 0; i < countof(m_pStageByteCode); i++)
    {
        if (m_pStageByteCode[i] != nullptr)
            m_pStageByteCode[i]->Release();
    }

    m_pDevice->Release();
}


ID3D12PipelineState *D3D12GPUShaderProgram::GetPipelineState(const PipelineStateKey *pKey)
{
    // get keys
    uint32 key1;
    uint32 key2[4];
    MurmurHash3_x86_32(pKey, sizeof(*pKey), 0x3B9AC9FC, &key1);
#ifdef Y_CPU_X86
    MurmurHash3_x86_128(pKey, sizeof(*pKey), 0xFE54B014, key2);
#else
    MurmurHash3_x64_128(pKey, sizeof(*pKey), 0xFE54B014, key2);
#endif

    // search the list
    m_pDevice->GetShaderPipelineStateLock()->LockShared();
    for (uint32 i = 0; i < m_pipelineStates.GetSize(); i++)
    {
        const PipelineState *ps = &m_pipelineStates[i];
        if (ps->Key1 > key1)
            break;
        else if (ps->Key1 < key1)
            continue;

        if (Y_memcmp(ps->Key2, key2, sizeof(key2)) == 0)
        {
            m_pDevice->GetShaderPipelineStateLock()->UnlockShared();
            return ps->pPipelineState;
        }
    }

    // move to a write lock
    m_pDevice->GetShaderPipelineStateLock()->UpgradeSharedLockToExclusive();

    // ensure another thread hasn't created the pipeline in the meantime
    for (uint32 i = 0; i < m_pipelineStates.GetSize(); i++)
    {
        const PipelineState *ps = &m_pipelineStates[i];
        if (ps->Key1 > key1)
            break;
        else if (ps->Key1 < key1)
            continue;

        if (Y_memcmp(ps->Key2, key2, sizeof(key2)) == 0)
        {
            m_pDevice->GetShaderPipelineStateLock()->UnlockExclusive();
            return ps->pPipelineState;
        }
    }

    // fill new details
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc;
    Y_memzero(&pipelineDesc, sizeof(pipelineDesc));
    pipelineDesc.pRootSignature = m_pDevice->GetLegacyGraphicsRootSignature();

    // stage bytecode
    if (m_pStageByteCode[SHADER_PROGRAM_STAGE_VERTEX_SHADER] != nullptr)
    {
        pipelineDesc.VS.pShaderBytecode = m_pStageByteCode[SHADER_PROGRAM_STAGE_VERTEX_SHADER]->GetDataPointer();
        pipelineDesc.VS.BytecodeLength = m_pStageByteCode[SHADER_PROGRAM_STAGE_VERTEX_SHADER]->GetDataSize();
    }
    else
    {
        pipelineDesc.VS.pShaderBytecode = nullptr;
        pipelineDesc.VS.BytecodeLength = 0;
    }
    if (m_pStageByteCode[SHADER_PROGRAM_STAGE_HULL_SHADER] != nullptr)
    {
        pipelineDesc.HS.pShaderBytecode = m_pStageByteCode[SHADER_PROGRAM_STAGE_HULL_SHADER]->GetDataPointer();
        pipelineDesc.HS.BytecodeLength = m_pStageByteCode[SHADER_PROGRAM_STAGE_HULL_SHADER]->GetDataSize();
    }
    else
    {
        pipelineDesc.HS.pShaderBytecode = nullptr;
        pipelineDesc.HS.BytecodeLength = 0;
    }
    if (m_pStageByteCode[SHADER_PROGRAM_STAGE_DOMAIN_SHADER] != nullptr)
    {
        pipelineDesc.DS.pShaderBytecode = m_pStageByteCode[SHADER_PROGRAM_STAGE_DOMAIN_SHADER]->GetDataPointer();
        pipelineDesc.DS.BytecodeLength = m_pStageByteCode[SHADER_PROGRAM_STAGE_DOMAIN_SHADER]->GetDataSize();
    }
    else
    {
        pipelineDesc.DS.pShaderBytecode = nullptr;
        pipelineDesc.DS.BytecodeLength = 0;
    }
    if (m_pStageByteCode[SHADER_PROGRAM_STAGE_GEOMETRY_SHADER] != nullptr)
    {
        pipelineDesc.GS.pShaderBytecode = m_pStageByteCode[SHADER_PROGRAM_STAGE_GEOMETRY_SHADER]->GetDataPointer();
        pipelineDesc.GS.BytecodeLength = m_pStageByteCode[SHADER_PROGRAM_STAGE_GEOMETRY_SHADER]->GetDataSize();
    }
    else
    {
        pipelineDesc.GS.pShaderBytecode = nullptr;
        pipelineDesc.GS.BytecodeLength = 0;
    }
    if (m_pStageByteCode[SHADER_PROGRAM_STAGE_PIXEL_SHADER] != nullptr)
    {
        pipelineDesc.PS.pShaderBytecode = m_pStageByteCode[SHADER_PROGRAM_STAGE_PIXEL_SHADER]->GetDataPointer();
        pipelineDesc.PS.BytecodeLength = m_pStageByteCode[SHADER_PROGRAM_STAGE_PIXEL_SHADER]->GetDataSize();
    }
    else
    {
        pipelineDesc.PS.pShaderBytecode = nullptr;
        pipelineDesc.PS.BytecodeLength = 0;
    }

    // streamout
    Y_memzero(&pipelineDesc.StreamOutput, sizeof(pipelineDesc.StreamOutput));

    // states
    Y_memcpy(&pipelineDesc.BlendState, &pKey->BlendState, sizeof(pipelineDesc.BlendState));
    Y_memcpy(&pipelineDesc.RasterizerState, &pKey->RasterizerState, sizeof(pipelineDesc.RasterizerState));
    Y_memcpy(&pipelineDesc.DepthStencilState, &pKey->DepthStencilState, sizeof(pipelineDesc.DepthStencilState));
    pipelineDesc.InputLayout.NumElements = m_vertexAttributes.GetSize();
    pipelineDesc.InputLayout.pInputElementDescs = m_vertexAttributes.GetBasePointer();
    pipelineDesc.SampleMask = 0xFFFFFFFF;
    pipelineDesc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
    pipelineDesc.PrimitiveTopologyType = pKey->PrimitiveTopologyType;
    pipelineDesc.NumRenderTargets = pKey->RenderTargetCount;
    for (uint32 i = 0; i < pKey->RenderTargetCount; i++)
        pipelineDesc.RTVFormats[i] = D3D12Helpers::PixelFormatToDXGIFormat(pKey->RTVFormats[i]);
    for (uint32 i = pKey->RenderTargetCount; i < 8; i++)
        pipelineDesc.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
    pipelineDesc.DSVFormat = D3D12Helpers::PixelFormatToDXGIFormat(pKey->DSVFormat);
    pipelineDesc.SampleDesc.Count = 1;
    pipelineDesc.SampleDesc.Quality = 0;
    pipelineDesc.NodeMask = 0;
    pipelineDesc.CachedPSO.pCachedBlob = nullptr;
    pipelineDesc.CachedPSO.CachedBlobSizeInBytes = 0;
    pipelineDesc.Flags = (CVars::r_use_debug_shaders.GetBool() && CVars::r_d3d12_force_warp.GetBool()) ? D3D12_PIPELINE_STATE_FLAG_TOOL_DEBUG : D3D12_PIPELINE_STATE_FLAG_NONE;

    // create it
    ID3D12PipelineState *pPipelineState;
    HRESULT hResult = m_pDevice->GetD3DDevice()->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&pPipelineState));
    if (SUCCEEDED(hResult))
    {
        // set debug name
        if (!m_debugName.IsEmpty())
            D3D12Helpers::SetD3D12ObjectName(pPipelineState, m_debugName);
    }
    else
    {
        Log_ErrorPrintf("CreateGraphicsPipelineState failed with hResult %08X", hResult);
        pPipelineState = nullptr;
    }

    // store it
    PipelineState ps;
    ps.pPipelineState = pPipelineState;
    ps.Key1 = key1;
    Y_memcpy(ps.Key2, key2, sizeof(ps.Key2));

    // insert into appropriate place in list, saving sorting
    bool inserted = false;
    for (uint32 i = 0; i < m_pipelineStates.GetSize(); i++)
    {
        if (m_pipelineStates[i].Key1 > key1)
        {
            m_pipelineStates.Insert(ps, i);
            inserted = true;
            break;
        }
    }
    if (!inserted)
        m_pipelineStates.Add(ps);

    // done
    m_pDevice->GetShaderPipelineStateLock()->UnlockExclusive();
    return ps.pPipelineState;
}

void D3D12GPUShaderProgram::GetMemoryUsage(uint32 *cpuMemoryUsage, uint32 *gpuMemoryUsage) const
{
    // dummy values for now
    if (cpuMemoryUsage != nullptr)
        *cpuMemoryUsage = sizeof(*this);

    if (gpuMemoryUsage != nullptr)
        *gpuMemoryUsage = 128;
}

void D3D12GPUShaderProgram::SetDebugName(const char *name)
{
    m_debugName = name;
}

bool D3D12GPUShaderProgram::Create(D3D12GPUDevice *pDevice, const GPU_VERTEX_ELEMENT_DESC *pVertexAttributes, uint32 nVertexAttributes, ByteStream *pByteCodeStream)
{
    BinaryReader binaryReader(pByteCodeStream);

    // get the header of the shader cache entry
    D3DShaderCacheEntryHeader header;
    if (!binaryReader.SafeReadBytes(&header, sizeof(header)) ||
        header.Signature != D3D_SHADER_CACHE_ENTRY_HEADER)
    {
        Log_ErrorPrintf("D3D12GPUShaderProgram::Create: Shader cache entry header corrupted.");
        return false;
    }

    // create d3d shader objects, and arrays for them
    for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
    {
        if (header.StageSize[stageIndex] == 0)
            continue;

        // create binary blob for bytecode
        m_pStageByteCode[stageIndex] = BinaryBlob::Allocate(header.StageSize[stageIndex]);
        if (!binaryReader.SafeReadBytes(m_pStageByteCode[stageIndex]->GetDataPointer(), header.StageSize[stageIndex]))
            return false;
    }

    // load up vertex attributes
    if (header.VertexAttributeCount > 0)
    {
        // create a filtered list of vertex attributes provided and those used by the shader
        m_vertexAttributes.Reserve(header.VertexAttributeCount);
        for (uint32 i = 0; i < header.VertexAttributeCount; i++)
        {
            D3DShaderCacheEntryVertexAttribute attribute;
            if (!binaryReader.SafeReadBytes(&attribute, sizeof(attribute)))
                return false;

            // search in the list provided
            uint32 listIndex;
            for (listIndex = 0; listIndex < nVertexAttributes; listIndex++)
            {
                if (pVertexAttributes[listIndex].Semantic == (GPU_VERTEX_ELEMENT_SEMANTIC)attribute.SemanticName &&
                    pVertexAttributes[listIndex].SemanticIndex == attribute.SemanticIndex)
                {
                    break;
                }
            }

            // make sure it exists
            if (listIndex == nVertexAttributes)
            {
                Log_ErrorPrintf("D3D12ShaderProgram::Create: failed to match shader attribute with provided attributes");
                return false;
            }

            // copy info in
            D3D12_INPUT_ELEMENT_DESC vertexAttribute;
            vertexAttribute.SemanticName = NameTable_GetNameString(NameTables::GPUVertexElementSemantic, pVertexAttributes[listIndex].Semantic);
            vertexAttribute.SemanticIndex = pVertexAttributes[listIndex].SemanticIndex;
            vertexAttribute.Format = s_GPUVertexElementTypeToDXGIFormat[pVertexAttributes[listIndex].Type];
            vertexAttribute.InputSlot = pVertexAttributes[listIndex].StreamIndex;
            vertexAttribute.AlignedByteOffset = pVertexAttributes[listIndex].StreamOffset;
            vertexAttribute.InputSlotClass = (pVertexAttributes[listIndex].InstanceStepRate != 0) ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            vertexAttribute.InstanceDataStepRate = pVertexAttributes[listIndex].InstanceStepRate;
            m_vertexAttributes.Add(vertexAttribute);
        }
    }

    // load up constant buffers
    if (header.ConstantBufferCount > 0)
    {
        m_constantBuffers.Resize(header.ConstantBufferCount);
        for (uint32 i = 0; i < m_constantBuffers.GetSize(); i++)
        {
            D3DShaderCacheEntryConstantBuffer srcConstantBufferInfo;
            ConstantBuffer *pDstConstantBuffer = &m_constantBuffers[i];
            if (!binaryReader.SafeReadBytes(&srcConstantBufferInfo, sizeof(srcConstantBufferInfo)))
                return false;

            // read name
            if (!binaryReader.SafeReadFixedString(srcConstantBufferInfo.NameLength, &pDstConstantBuffer->Name))
                return false;

            // constant buffer struct
            pDstConstantBuffer->MinimumSize = srcConstantBufferInfo.MinimumSize;
            pDstConstantBuffer->ParameterIndex = srcConstantBufferInfo.ParameterIndex;
            pDstConstantBuffer->EngineConstantBufferIndex = 0;
        
            // lookup engine constant buffer
            const ShaderConstantBuffer *pEngineConstantBuffer = ShaderConstantBuffer::GetShaderConstantBufferByName(pDstConstantBuffer->Name, RENDERER_PLATFORM_D3D11, m_pDevice->GetFeatureLevel());
            if (pEngineConstantBuffer == nullptr)
            {
                Log_ErrorPrintf("D3D12ShaderProgram::Create: Shader is requesting unknown constant buffer named '%s'.", pDstConstantBuffer->Name);
                return false;
            }

            // store index
            pDstConstantBuffer->EngineConstantBufferIndex = pEngineConstantBuffer->GetIndex();
        }
    }

    // load parameters
    if (header.ParameterCount > 0)
    {
        m_parameters.Resize(header.ParameterCount);
        for (uint32 i = 0; i < m_parameters.GetSize(); i++)
        {
            D3DShaderCacheEntryParameter srcParameter;
            ShaderParameter *pDstParameterInfo = &m_parameters[i];
            if (!binaryReader.SafeReadBytes(&srcParameter, sizeof(srcParameter)))
                return false;

            // read name
            if (!binaryReader.SafeReadFixedString(srcParameter.NameLength, &pDstParameterInfo->Name))
                return false;

            pDstParameterInfo->Type = srcParameter.Type;
            pDstParameterInfo->ConstantBufferIndex = srcParameter.ConstantBufferIndex;
            pDstParameterInfo->ConstantBufferOffset = srcParameter.ConstantBufferOffset;
            pDstParameterInfo->ArraySize = srcParameter.ArraySize;
            pDstParameterInfo->ArrayStride = srcParameter.ArrayStride;
            pDstParameterInfo->BindTarget = (D3D_SHADER_BIND_TARGET)srcParameter.BindTarget;
            Y_memcpy(pDstParameterInfo->BindPoint, srcParameter.BindPoint, sizeof(pDstParameterInfo->BindPoint));
            pDstParameterInfo->LinkedSamplerIndex = srcParameter.LinkedSamplerIndex;
        }
    }

    // all ok
    return true;
}

bool D3D12GPUShaderProgram::Switch(D3D12GPUContext *pContext, ID3D12GraphicsCommandList *pD3DCommandList, const PipelineStateKey *pKey)
{
    // get pipeline state
    ID3D12PipelineState *pPipelineState = GetPipelineState(pKey);
    if (pPipelineState == nullptr)
        return false;

    pD3DCommandList->SetPipelineState(pPipelineState);
    
    // bind constant buffers
    for (const ConstantBuffer &constantBuffer : m_constantBuffers)
    {
        const ShaderParameter &parameter = m_parameters[constantBuffer.ParameterIndex];
        DebugAssert(parameter.Type == SHADER_PARAMETER_TYPE_CONSTANT_BUFFER);

        for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
        {
            if (parameter.BindPoint[stageIndex] < 0)
                continue;

            if (parameter.BindPoint[stageIndex] < D3D12_LEGACY_GRAPHICS_ROOT_CONSTANT_BUFFER_SLOTS)
            {
                // bind to table
                const D3D12DescriptorHandle *pHandle = m_pDevice->GetConstantBufferDescriptor(constantBuffer.EngineConstantBufferIndex);
                if (pHandle != nullptr)
                    pContext->SetShaderConstantBuffers((SHADER_PROGRAM_STAGE)stageIndex, parameter.BindPoint[stageIndex], *pHandle);
            }
            else
            {
                // bind to per-draw
                D3D12_GPU_VIRTUAL_ADDRESS virtualAddress;
                if (pContext->GetPerDrawConstantBufferGPUAddress(constantBuffer.EngineConstantBufferIndex, &virtualAddress))
                    pContext->SetPerDrawConstantBuffer(parameter.BindPoint[stageIndex] - D3D12_LEGACY_GRAPHICS_ROOT_CONSTANT_BUFFER_SLOTS, virtualAddress);
            }
        }
    }

    // done
    return true;
}

bool D3D12GPUShaderProgram::Switch(D3D12GPUCommandList *pCommandList, ID3D12GraphicsCommandList *pD3DCommandList, const PipelineStateKey *pKey)
{
    // get pipeline state
    ID3D12PipelineState *pPipelineState = GetPipelineState(pKey);
    if (pPipelineState == nullptr)
        return false;

    pD3DCommandList->SetPipelineState(pPipelineState);

    // bind constant buffers
    for (const ConstantBuffer &constantBuffer : m_constantBuffers)
    {
        const ShaderParameter &parameter = m_parameters[constantBuffer.ParameterIndex];
        DebugAssert(parameter.Type == SHADER_PARAMETER_TYPE_CONSTANT_BUFFER);

        for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
        {
            if (parameter.BindPoint[stageIndex] < 0)
                continue;

            if (parameter.BindPoint[stageIndex] < D3D12_LEGACY_GRAPHICS_ROOT_CONSTANT_BUFFER_SLOTS)
            {
                // bind to table
                const D3D12DescriptorHandle *pHandle = m_pDevice->GetConstantBufferDescriptor(constantBuffer.EngineConstantBufferIndex);
                if (pHandle != nullptr)
                    pCommandList->SetShaderConstantBuffers((SHADER_PROGRAM_STAGE)stageIndex, parameter.BindPoint[stageIndex], *pHandle);
            }
            else
            {
                // bind to per-draw
                D3D12_GPU_VIRTUAL_ADDRESS virtualAddress;
                if (pCommandList->GetPerDrawConstantBufferGPUAddress(constantBuffer.EngineConstantBufferIndex, &virtualAddress))
                    pCommandList->SetPerDrawConstantBuffer(parameter.BindPoint[stageIndex] - D3D12_LEGACY_GRAPHICS_ROOT_CONSTANT_BUFFER_SLOTS, virtualAddress);
            }
        }
    }

    // done
    return true;
}

void D3D12GPUShaderProgram::RebindPerDrawConstantBuffer(D3D12GPUContext *pContext, uint32 constantBufferIndex, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    for (const ConstantBuffer &constantBuffer : m_constantBuffers)
    {
        if (constantBuffer.EngineConstantBufferIndex == constantBufferIndex)
        {
            const ShaderParameter &parameter = m_parameters[constantBuffer.ParameterIndex];
            DebugAssert(parameter.Type == SHADER_PARAMETER_TYPE_CONSTANT_BUFFER);

            for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
            {
                if (parameter.BindPoint[stageIndex] >= D3D12_LEGACY_GRAPHICS_ROOT_CONSTANT_BUFFER_SLOTS)
                    pContext->SetPerDrawConstantBuffer(parameter.BindPoint[stageIndex] - D3D12_LEGACY_GRAPHICS_ROOT_CONSTANT_BUFFER_SLOTS, address);
            }
        }
    }
}

void D3D12GPUShaderProgram::RebindPerDrawConstantBuffer(D3D12GPUCommandList *pCommandList, uint32 constantBufferIndex, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    for (const ConstantBuffer &constantBuffer : m_constantBuffers)
    {
        if (constantBuffer.EngineConstantBufferIndex == constantBufferIndex)
        {
            const ShaderParameter &parameter = m_parameters[constantBuffer.ParameterIndex];
            DebugAssert(parameter.Type == SHADER_PARAMETER_TYPE_CONSTANT_BUFFER);

            for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
            {
                if (parameter.BindPoint[stageIndex] >= D3D12_LEGACY_GRAPHICS_ROOT_CONSTANT_BUFFER_SLOTS)
                    pCommandList->SetPerDrawConstantBuffer(parameter.BindPoint[stageIndex] - D3D12_LEGACY_GRAPHICS_ROOT_CONSTANT_BUFFER_SLOTS, address);
            }
        }
    }
}

void D3D12GPUShaderProgram::InternalSetParameterValue(D3D12GPUContext *pContext, uint32 parameterIndex, SHADER_PARAMETER_TYPE valueType, const void *pValue)
{
    const ShaderParameter *parameterInfo = &m_parameters[parameterIndex];
    uint32 valueSize = ShaderParameterValueTypeSize(parameterInfo->Type);
    DebugAssert(parameterInfo->Type == valueType && valueSize > 0);

    // get constant buffer
    DebugAssert(parameterInfo->ConstantBufferIndex >= 0);
    const ConstantBuffer *constantBuffer = &m_constantBuffers[parameterInfo->ConstantBufferIndex];

    // write to the constant buffer
    pContext->WriteConstantBuffer(constantBuffer->EngineConstantBufferIndex, parameterIndex, parameterInfo->ConstantBufferOffset, valueSize, pValue, false);
}

void D3D12GPUShaderProgram::InternalSetParameterValue(D3D12GPUCommandList *pCommandList, uint32 parameterIndex, SHADER_PARAMETER_TYPE valueType, const void *pValue)
{
    const ShaderParameter *parameterInfo = &m_parameters[parameterIndex];
    uint32 valueSize = ShaderParameterValueTypeSize(parameterInfo->Type);
    DebugAssert(parameterInfo->Type == valueType && valueSize > 0);

    // get constant buffer
    DebugAssert(parameterInfo->ConstantBufferIndex >= 0);
    const ConstantBuffer *constantBuffer = &m_constantBuffers[parameterInfo->ConstantBufferIndex];

    // write to the constant buffer
    pCommandList->WriteConstantBuffer(constantBuffer->EngineConstantBufferIndex, parameterIndex, parameterInfo->ConstantBufferOffset, valueSize, pValue, false);
}

void D3D12GPUShaderProgram::InternalSetParameterValueArray(D3D12GPUContext *pContext, uint32 parameterIndex, SHADER_PARAMETER_TYPE valueType, const void *pValue, uint32 firstElement, uint32 numElements)
{
    const ShaderParameter *parameterInfo = &m_parameters[parameterIndex];
    uint32 valueSize = ShaderParameterValueTypeSize(parameterInfo->Type);
    DebugAssert(parameterInfo->Type == valueType && valueSize > 0);
    DebugAssert(numElements > 0 && (firstElement + numElements) <= parameterInfo->ArraySize);

    // get constant buffer
    DebugAssert(parameterInfo->ConstantBufferIndex >= 0);
    const ConstantBuffer *constantBuffer = &m_constantBuffers[parameterInfo->ConstantBufferIndex];

    // if there is no padding, this can be done in a single operation
    uint32 bufferOffset = parameterInfo->ConstantBufferOffset + (firstElement * parameterInfo->ArrayStride);
    if (valueSize == parameterInfo->ArrayStride)
        pContext->WriteConstantBuffer(constantBuffer->EngineConstantBufferIndex, parameterIndex, bufferOffset, valueSize * numElements, pValue);
    else
        pContext->WriteConstantBufferStrided(constantBuffer->EngineConstantBufferIndex, parameterIndex, bufferOffset, parameterInfo->ArrayStride, valueSize, numElements, pValue);
}

void D3D12GPUShaderProgram::InternalSetParameterValueArray(D3D12GPUCommandList *pCommandList, uint32 parameterIndex, SHADER_PARAMETER_TYPE valueType, const void *pValue, uint32 firstElement, uint32 numElements)
{
    const ShaderParameter *parameterInfo = &m_parameters[parameterIndex];
    uint32 valueSize = ShaderParameterValueTypeSize(parameterInfo->Type);
    DebugAssert(parameterInfo->Type == valueType && valueSize > 0);
    DebugAssert(numElements > 0 && (firstElement + numElements) <= parameterInfo->ArraySize);

    // get constant buffer
    DebugAssert(parameterInfo->ConstantBufferIndex >= 0);
    const ConstantBuffer *constantBuffer = &m_constantBuffers[parameterInfo->ConstantBufferIndex];

    // if there is no padding, this can be done in a single operation
    uint32 bufferOffset = parameterInfo->ConstantBufferOffset + (firstElement * parameterInfo->ArrayStride);
    if (valueSize == parameterInfo->ArrayStride)
        pCommandList->WriteConstantBuffer(constantBuffer->EngineConstantBufferIndex, parameterIndex, bufferOffset, valueSize * numElements, pValue);
    else
        pCommandList->WriteConstantBufferStrided(constantBuffer->EngineConstantBufferIndex, parameterIndex, bufferOffset, parameterInfo->ArrayStride, valueSize, numElements, pValue);
}

void D3D12GPUShaderProgram::InternalSetParameterStruct(D3D12GPUContext *pContext, uint32 parameterIndex, const void *pValue, uint32 valueSize)
{
    const ShaderParameter *parameterInfo = &m_parameters[parameterIndex];
    DebugAssert(parameterInfo->Type == SHADER_PARAMETER_TYPE_STRUCT && valueSize <= parameterInfo->ArrayStride);

    // get constant buffer
    DebugAssert(parameterInfo->ConstantBufferIndex >= 0);
    const ConstantBuffer *constantBuffer = &m_constantBuffers[parameterInfo->ConstantBufferIndex];

    // write to the constant buffer
    pContext->WriteConstantBuffer(constantBuffer->EngineConstantBufferIndex, parameterIndex, parameterInfo->ConstantBufferOffset, valueSize, pValue, false);
}

void D3D12GPUShaderProgram::InternalSetParameterStruct(D3D12GPUCommandList *pCommandList, uint32 parameterIndex, const void *pValue, uint32 valueSize)
{
    const ShaderParameter *parameterInfo = &m_parameters[parameterIndex];
    DebugAssert(parameterInfo->Type == SHADER_PARAMETER_TYPE_STRUCT && valueSize <= parameterInfo->ArrayStride);

    // get constant buffer
    DebugAssert(parameterInfo->ConstantBufferIndex >= 0);
    const ConstantBuffer *constantBuffer = &m_constantBuffers[parameterInfo->ConstantBufferIndex];

    // write to the constant buffer
    pCommandList->WriteConstantBuffer(constantBuffer->EngineConstantBufferIndex, parameterIndex, parameterInfo->ConstantBufferOffset, valueSize, pValue, false);
}

void D3D12GPUShaderProgram::InternalSetParameterStructArray(D3D12GPUContext *pContext, uint32 parameterIndex, const void *pValue, uint32 valueSize, uint32 firstElement, uint32 numElements)
{
    const ShaderParameter *parameterInfo = &m_parameters[parameterIndex];
    DebugAssert(parameterInfo->Type == SHADER_PARAMETER_TYPE_STRUCT && valueSize <= parameterInfo->ArrayStride);
    DebugAssert(numElements > 0 && (firstElement + numElements) <= parameterInfo->ArraySize);

    // get constant buffer
    DebugAssert(parameterInfo->ConstantBufferIndex >= 0);
    const ConstantBuffer *constantBuffer = &m_constantBuffers[parameterInfo->ConstantBufferIndex];

    // if there is no padding, this can be done in a single operation
    uint32 bufferOffset = parameterInfo->ConstantBufferOffset + (firstElement * parameterInfo->ArrayStride);
    if (valueSize == parameterInfo->ArrayStride)
        pContext->WriteConstantBuffer(constantBuffer->EngineConstantBufferIndex, parameterIndex, bufferOffset, valueSize * numElements, pValue);
    else
        pContext->WriteConstantBufferStrided(constantBuffer->EngineConstantBufferIndex, parameterIndex, bufferOffset, parameterInfo->ArrayStride, valueSize, numElements, pValue);
}

void D3D12GPUShaderProgram::InternalSetParameterStructArray(D3D12GPUCommandList *pCommandList, uint32 parameterIndex, const void *pValue, uint32 valueSize, uint32 firstElement, uint32 numElements)
{
    const ShaderParameter *parameterInfo = &m_parameters[parameterIndex];
    DebugAssert(parameterInfo->Type == SHADER_PARAMETER_TYPE_STRUCT && valueSize <= parameterInfo->ArrayStride);
    DebugAssert(numElements > 0 && (firstElement + numElements) <= parameterInfo->ArraySize);

    // get constant buffer
    DebugAssert(parameterInfo->ConstantBufferIndex >= 0);
    const ConstantBuffer *constantBuffer = &m_constantBuffers[parameterInfo->ConstantBufferIndex];

    // if there is no padding, this can be done in a single operation
    uint32 bufferOffset = parameterInfo->ConstantBufferOffset + (firstElement * parameterInfo->ArrayStride);
    if (valueSize == parameterInfo->ArrayStride)
        pCommandList->WriteConstantBuffer(constantBuffer->EngineConstantBufferIndex, parameterIndex, bufferOffset, valueSize * numElements, pValue);
    else
        pCommandList->WriteConstantBufferStrided(constantBuffer->EngineConstantBufferIndex, parameterIndex, bufferOffset, parameterInfo->ArrayStride, valueSize, numElements, pValue);
}

void D3D12GPUShaderProgram::InternalSetParameterResource(D3D12GPUContext *pContext, uint32 parameterIndex, GPUResource *pResource, GPUSamplerState *pLinkedSamplerState)
{
    const ShaderParameter *parameterInfo = &m_parameters[parameterIndex];

    // branch out according to target
    switch (parameterInfo->BindTarget)
    {
    case D3D_SHADER_BIND_TARGET_RESOURCE:
        {
            D3D12DescriptorHandle handle;
            D3D12Helpers::GetResourceSRVHandle(pResource, &handle);

            for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
            {
                int32 bindPoint = parameterInfo->BindPoint[stageIndex];
                if (bindPoint >= 0)
                    pContext->SetShaderResources((SHADER_PROGRAM_STAGE)stageIndex, bindPoint, handle);
            }
        }
        break;

    case D3D_SHADER_BIND_TARGET_SAMPLER:
        {
            D3D12DescriptorHandle handle;
            D3D12Helpers::GetResourceSamplerHandle(pResource, &handle);

            for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
            {
                int32 bindPoint = parameterInfo->BindPoint[stageIndex];
                if (bindPoint >= 0)
                    pContext->SetShaderSamplers((SHADER_PROGRAM_STAGE)stageIndex, bindPoint, handle);
            }
        }
        break;

    case D3D_SHADER_BIND_TARGET_UNORDERED_ACCESS_VIEW:
        {
//             ID3D12UnorderedAccessView *pD3DUAV = (pResource != nullptr && pResource->GetResourceType() == GPU_RESOURCE_TYPE_COMPUTE_VIEW) ? static_cast<D3D12GPUComputeView *>(pResource)->GetD3DUAV() : nullptr;
//             for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
//             {
//                 int32 bindPoint = parameterInfo->BindPoint[stageIndex];
//                 if (bindPoint >= 0)
//                     pContext->SetShaderUAVs((SHADER_PROGRAM_STAGE)stageIndex, bindPoint, pD3DUAV);
//             }
        }
        break;
    }

    // for texture types with a linked sampler state, update it
    if (parameterInfo->LinkedSamplerIndex >= 0)
    {
        const ShaderParameter *samplerParameterInfo = &m_parameters[parameterInfo->LinkedSamplerIndex];
        DebugAssert(samplerParameterInfo->Type == SHADER_PARAMETER_TYPE_SAMPLER_STATE && samplerParameterInfo->BindTarget == D3D_SHADER_BIND_TARGET_SAMPLER);

        // get handle - might look nasty, but handle is nulled in the constructor.
        D3D12DescriptorHandle handle;
        D3D12Helpers::GetResourceSamplerHandle((pLinkedSamplerState != nullptr) ? static_cast<GPUResource *>(pLinkedSamplerState) : pResource, &handle);

        // write to stages
        for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
        {
            int32 bindPoint = samplerParameterInfo->BindPoint[stageIndex];
            if (bindPoint >= 0)
                pContext->SetShaderSamplers((SHADER_PROGRAM_STAGE)stageIndex, bindPoint, handle);
        }
    }
}

void D3D12GPUShaderProgram::InternalSetParameterResource(D3D12GPUCommandList *pCommandList, uint32 parameterIndex, GPUResource *pResource, GPUSamplerState *pLinkedSamplerState)
{
    const ShaderParameter *parameterInfo = &m_parameters[parameterIndex];

    // branch out according to target
    switch (parameterInfo->BindTarget)
    {
    case D3D_SHADER_BIND_TARGET_RESOURCE:
        {
            D3D12DescriptorHandle handle;
            D3D12Helpers::GetResourceSRVHandle(pResource, &handle);

            for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
            {
                int32 bindPoint = parameterInfo->BindPoint[stageIndex];
                if (bindPoint >= 0)
                    pCommandList->SetShaderResources((SHADER_PROGRAM_STAGE)stageIndex, bindPoint, handle);
            }
        }
        break;

    case D3D_SHADER_BIND_TARGET_SAMPLER:
        {
            D3D12DescriptorHandle handle;
            D3D12Helpers::GetResourceSamplerHandle(pResource, &handle);

            for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
            {
                int32 bindPoint = parameterInfo->BindPoint[stageIndex];
                if (bindPoint >= 0)
                    pCommandList->SetShaderSamplers((SHADER_PROGRAM_STAGE)stageIndex, bindPoint, handle);
            }
        }
        break;

    case D3D_SHADER_BIND_TARGET_UNORDERED_ACCESS_VIEW:
        {
//             ID3D12UnorderedAccessView *pD3DUAV = (pResource != nullptr && pResource->GetResourceType() == GPU_RESOURCE_TYPE_COMPUTE_VIEW) ? static_cast<D3D12GPUComputeView *>(pResource)->GetD3DUAV() : nullptr;
//             for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
//             {
//                 int32 bindPoint = parameterInfo->BindPoint[stageIndex];
//                 if (bindPoint >= 0)
//                     pContext->SetShaderUAVs((SHADER_PROGRAM_STAGE)stageIndex, bindPoint, pD3DUAV);
//             }
        }
        break;
    }

    // for texture types with a linked sampler state, update it
    if (parameterInfo->LinkedSamplerIndex >= 0)
    {
        const ShaderParameter *samplerParameterInfo = &m_parameters[parameterInfo->LinkedSamplerIndex];
        DebugAssert(samplerParameterInfo->Type == SHADER_PARAMETER_TYPE_SAMPLER_STATE && samplerParameterInfo->BindTarget == D3D_SHADER_BIND_TARGET_SAMPLER);

        // get handle - might look nasty, but handle is nulled in the constructor.
        D3D12DescriptorHandle handle;
        D3D12Helpers::GetResourceSamplerHandle((pLinkedSamplerState != nullptr) ? static_cast<GPUResource *>(pLinkedSamplerState) : pResource, &handle);

        // write to stages
        for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
        {
            int32 bindPoint = samplerParameterInfo->BindPoint[stageIndex];
            if (bindPoint >= 0)
                pCommandList->SetShaderSamplers((SHADER_PROGRAM_STAGE)stageIndex, bindPoint, handle);
        }
    }
}

uint32 D3D12GPUShaderProgram::GetParameterCount() const
{
    return m_parameters.GetSize();
}

void D3D12GPUShaderProgram::GetParameterInformation(uint32 index, const char **name, SHADER_PARAMETER_TYPE *type, uint32 *arraySize)
{
    const ShaderParameter *parameter = &m_parameters[index];
    *name = parameter->Name;
    *type = parameter->Type;
    *arraySize = parameter->ArraySize;
}

GPUShaderProgram *D3D12GPUDevice::CreateGraphicsProgram(const GPU_VERTEX_ELEMENT_DESC *pVertexElements, uint32 nVertexElements, ByteStream *pByteCodeStream)
{
    D3D12GPUShaderProgram *pProgram = new D3D12GPUShaderProgram(this);
    if (!pProgram->Create(this, pVertexElements, nVertexElements, pByteCodeStream))
    {
        pProgram->Release();
        return nullptr;
    }

    return pProgram;
}

GPUShaderProgram *D3D12GPUDevice::CreateComputeProgram(ByteStream *pByteCodeStream)
{
    D3D12GPUShaderProgram *pProgram = new D3D12GPUShaderProgram(this);
    if (!pProgram->Create(this, nullptr, 0, pByteCodeStream))
    {
        pProgram->Release();
        return nullptr;
    }

    return pProgram;
}
