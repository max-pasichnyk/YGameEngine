#include "D3D11Renderer/PrecompiledHeader.h"
#include "D3D11Renderer/D3D11GPUShaderProgram.h"
#include "D3D11Renderer/D3D11GPUContext.h"
#include "D3D11Renderer/D3D11GPUBuffer.h"
#include "D3D11Renderer/D3D11Renderer.h"
#include "Renderer/ShaderConstantBuffer.h"
Log_SetChannel(Renderer);

#define LAZY_RESOURCE_CLEANUP_AFTER_SWITCH 1

D3D11GPUShaderProgram::D3D11GPUShaderProgram()
    : m_pD3DInputLayout(nullptr),
      m_pD3DVertexShader(nullptr),
      m_pD3DHullShader(nullptr),
      m_pD3DDomainShader(nullptr),
      m_pD3DGeometryShader(nullptr),
      m_pD3DPixelShader(nullptr),
      m_pD3DComputeShader(nullptr),
      m_pBoundContext(nullptr)
{

}

D3D11GPUShaderProgram::~D3D11GPUShaderProgram()
{
    DebugAssert(m_pBoundContext == NULL);

    SAFE_RELEASE(m_pD3DComputeShader);
    SAFE_RELEASE(m_pD3DPixelShader);
    SAFE_RELEASE(m_pD3DGeometryShader);
    SAFE_RELEASE(m_pD3DDomainShader);
    SAFE_RELEASE(m_pD3DHullShader);
    SAFE_RELEASE(m_pD3DVertexShader);
    SAFE_RELEASE(m_pD3DInputLayout);

    for (uint32 i = 0; i < m_constantBuffers.GetSize(); i++)
    {
        ShaderLocalConstantBuffer *pConstantBuffer = &m_constantBuffers[i];
        
        SAFE_RELEASE(pConstantBuffer->pLocalGPUBuffer);
        if (pConstantBuffer->pLocalBuffer != nullptr)
            delete[] pConstantBuffer->pLocalBuffer;
    }
}

void D3D11GPUShaderProgram::GetMemoryUsage(uint32 *cpuMemoryUsage, uint32 *gpuMemoryUsage) const
{
    // dummy values for now
    if (cpuMemoryUsage != nullptr)
        *cpuMemoryUsage = sizeof(*this);

    if (gpuMemoryUsage != nullptr)
        *gpuMemoryUsage = 128;
}

void D3D11GPUShaderProgram::SetDebugName(const char *name)
{
    if (m_pD3DVertexShader != nullptr)
        D3D11Helpers::SetD3D11DeviceChildDebugName(m_pD3DVertexShader, name);
    if (m_pD3DHullShader != nullptr)
        D3D11Helpers::SetD3D11DeviceChildDebugName(m_pD3DHullShader, name);
    if (m_pD3DDomainShader != nullptr)
        D3D11Helpers::SetD3D11DeviceChildDebugName(m_pD3DDomainShader, name);
    if (m_pD3DGeometryShader != nullptr)
        D3D11Helpers::SetD3D11DeviceChildDebugName(m_pD3DGeometryShader, name);
    if (m_pD3DPixelShader != nullptr)
        D3D11Helpers::SetD3D11DeviceChildDebugName(m_pD3DPixelShader, name);
    if (m_pD3DComputeShader != nullptr)
        D3D11Helpers::SetD3D11DeviceChildDebugName(m_pD3DComputeShader, name);
}

bool D3D11GPUShaderProgram::Create(D3D11Renderer *pRenderer, const GPU_VERTEX_ELEMENT_DESC *pVertexAttributes, uint32 nVertexAttributes, ByteStream *pByteCodeStream)
{
    HRESULT hResult;
    ID3D11Device *pD3DDevice = pRenderer->GetD3DDevice();
    BinaryReader binaryReader(pByteCodeStream);

    // get the header of the shader cache entry
    D3D11ShaderCacheEntryHeader header;
    if (!binaryReader.SafeReadBytes(&header, sizeof(header)) ||
        header.Signature != D3D11_SHADER_CACHE_ENTRY_HEADER)
    {
        Log_ErrorPrintf("D3D11GPUShaderProgram::Create: Shader cache entry header corrupted.");
        return false;
    }

    // save a copy of the vertex shader code, we need it to validate the input layout
    byte *pVertexShaderByteCode = nullptr;
    uint32 vertexShaderByteCodeSize = 0;

    // create d3d shader objects, and arrays for them
    for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
    {
        if (header.StageSize[stageIndex] == 0)
            continue;

        // read in stage bytecode
        uint32 stageByteCodeSize = header.StageSize[stageIndex];
        byte *pStageByteCode = new byte[stageByteCodeSize];
        if (!binaryReader.SafeReadBytes(pStageByteCode, stageByteCodeSize))
        {
            delete[] pVertexShaderByteCode;
            return false;
        }

        // create d3d objects
        switch (stageIndex)
        {
        case SHADER_PROGRAM_STAGE_VERTEX_SHADER:
            {
                if ((hResult = pD3DDevice->CreateVertexShader(pStageByteCode, stageByteCodeSize, nullptr, &m_pD3DVertexShader)) != S_OK)
                {
                    Log_ErrorPrintf("D3D11ShaderProgram::Create: CreateVertexShader failed with hResult %08X.", hResult);
                    delete[] pStageByteCode;
                    return false;
                }

                // save VS bytecode
                pVertexShaderByteCode = pStageByteCode;
                vertexShaderByteCodeSize = stageByteCodeSize;
            }
            break;

        case SHADER_PROGRAM_STAGE_HULL_SHADER:
            {
                if ((hResult = pD3DDevice->CreateHullShader(pStageByteCode, stageByteCodeSize, nullptr, &m_pD3DHullShader)) != S_OK)
                {
                    Log_ErrorPrintf("D3D11ShaderProgram::Create: CreateHullShader failed with hResult %08X.", hResult);
                    delete[] pStageByteCode;
                    delete[] pVertexShaderByteCode;
                    return false;
                }

                delete[] pStageByteCode;
            }
            break;

        case SHADER_PROGRAM_STAGE_DOMAIN_SHADER:
            {
                if ((hResult = pD3DDevice->CreateDomainShader(pStageByteCode, stageByteCodeSize, nullptr, &m_pD3DDomainShader)) != S_OK)
                {
                    Log_ErrorPrintf("D3D11ShaderProgram::Create: CreateDomainShader failed with hResult %08X.", hResult);
                    delete[] pStageByteCode;
                    delete[] pVertexShaderByteCode;
                    return false;
                }

                delete[] pStageByteCode;
            }
            break;

        case SHADER_PROGRAM_STAGE_GEOMETRY_SHADER:
            {
                if ((hResult = pD3DDevice->CreateGeometryShader(pStageByteCode, stageByteCodeSize, nullptr, &m_pD3DGeometryShader)) != S_OK)
                {
                    Log_ErrorPrintf("D3D11ShaderProgram::Create: CreateGeometryShader failed with hResult %08X.", hResult);
                    delete[] pStageByteCode;
                    delete[] pVertexShaderByteCode;
                    return false;
                }

                delete[] pStageByteCode;
            }
            break;
        
        case SHADER_PROGRAM_STAGE_PIXEL_SHADER:
            {
                if ((hResult = pD3DDevice->CreatePixelShader(pStageByteCode, stageByteCodeSize, nullptr, &m_pD3DPixelShader)) != S_OK)
                {
                    Log_ErrorPrintf("D3D11ShaderProgram::Create: CreatePixelShader failed with hResult %08X.", hResult);
                    delete[] pStageByteCode;
                    delete[] pVertexShaderByteCode;
                    return false;
                }

                delete[] pStageByteCode;
            }
            break;

        case SHADER_PROGRAM_STAGE_COMPUTE_SHADER:
            {
                if ((hResult = pD3DDevice->CreateComputeShader(pStageByteCode, stageByteCodeSize, nullptr, &m_pD3DComputeShader)) != S_OK)
                {
                    Log_ErrorPrintf("D3D11ShaderProgram::Create: CreateComputeShader failed with hResult %08X.", hResult);
                    delete[] pStageByteCode;
                    delete[] pVertexShaderByteCode;
                    return false;
                }

                delete[] pStageByteCode;
            }
            break;

        default:
            UnreachableCode();
            break;
        }
    }

#ifdef Y_BUILD_CONFIG_DEBUG
//     // set debug name
//     {
//         SmallString debugName;
//         debugName.Format("%s:%u:%s:%u:%s:%u", pCacheEntryHeader->BaseShaderName, pCacheEntryHeader->BaseShaderFlags, pCacheEntryHeader->VertexFactoryName, pCacheEntryHeader->VertexFactoryFlags, pCacheEntryHeader->MaterialShaderName, pCacheEntryHeader->MaterialShaderFlags);
//         if (m_pD3DVertexShader != nullptr)
//             D3D11Helpers::SetD3D11DeviceChildDebugName(m_pD3DVertexShader, debugName);
//         if (m_pD3DHullShader != nullptr)
//             D3D11Helpers::SetD3D11DeviceChildDebugName(m_pD3DHullShader, debugName);
//         if (m_pD3DDomainShader != nullptr)
//             D3D11Helpers::SetD3D11DeviceChildDebugName(m_pD3DDomainShader, debugName);        
//         if (m_pD3DGeometryShader != nullptr)
//             D3D11Helpers::SetD3D11DeviceChildDebugName(m_pD3DGeometryShader, debugName);
//         if (m_pD3DPixelShader != nullptr)
//             D3D11Helpers::SetD3D11DeviceChildDebugName(m_pD3DPixelShader, debugName);
//         if (m_pD3DComputeShader != nullptr)
//             D3D11Helpers::SetD3D11DeviceChildDebugName(m_pD3DComputeShader, debugName);
//     }
#endif

    // load up vertex attributes
    if (header.VertexAttributeCount > 0 && vertexShaderByteCodeSize > 0)
    {
        // create a filtered list of vertex attributes provided and those used by the shader
        D3D11_INPUT_ELEMENT_DESC usedVertexAttributes[GPU_INPUT_LAYOUT_MAX_ELEMENTS];
        uint32 nUsedVertexAttributes = 0;
        for (uint32 i = 0; i < header.VertexAttributeCount; i++)
        {
            D3D11ShaderCacheEntryVertexAttribute attribute;
            if (!binaryReader.SafeReadBytes(&attribute, sizeof(attribute)))
            {
                delete[] pVertexShaderByteCode;
                return false;
            }

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
                Log_ErrorPrintf("D3D11ShaderProgram::Create: failed to match shader attribute with provided attributes");
                delete[] pVertexShaderByteCode;
                return false;
            }

            // copy info in
            usedVertexAttributes[nUsedVertexAttributes].SemanticName = D3D11TypeConversion::VertexElementSemanticToString(pVertexAttributes[listIndex].Semantic);
            usedVertexAttributes[nUsedVertexAttributes].SemanticIndex = pVertexAttributes[listIndex].SemanticIndex;
            usedVertexAttributes[nUsedVertexAttributes].Format = D3D11TypeConversion::VertexElementTypeToDXGIFormat(pVertexAttributes[listIndex].Type);
            usedVertexAttributes[nUsedVertexAttributes].InputSlot = pVertexAttributes[listIndex].StreamIndex;
            usedVertexAttributes[nUsedVertexAttributes].AlignedByteOffset = pVertexAttributes[listIndex].StreamOffset;
            usedVertexAttributes[nUsedVertexAttributes].InputSlotClass = (pVertexAttributes[listIndex].InstanceStepRate != 0) ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA;
            usedVertexAttributes[nUsedVertexAttributes].InstanceDataStepRate = pVertexAttributes[listIndex].InstanceStepRate;
            nUsedVertexAttributes++;
        }

        // create input layout
        // TODO: Cache these
        if (FAILED(hResult = pD3DDevice->CreateInputLayout(usedVertexAttributes, nUsedVertexAttributes, pVertexShaderByteCode, vertexShaderByteCodeSize, &m_pD3DInputLayout)))
        {
            Log_ErrorPrintf("D3D11ShaderProgram::Create: CreateInputLayout failed with hResult %08X", hResult);
            delete[] pVertexShaderByteCode;
            return false;
        }
    }

    // can finally free this now
    delete[] pVertexShaderByteCode;

    // load up constant buffers
    if (header.ConstantBufferCount > 0)
    {
        m_constantBuffers.Resize(header.ConstantBufferCount);
        m_constantBuffers.ZeroContents();
        for (uint32 i = 0; i < m_constantBuffers.GetSize(); i++)
        {
            D3D11ShaderCacheEntryConstantBuffer srcConstantBufferInfo;
            ShaderLocalConstantBuffer *pDstConstantBuffer = &m_constantBuffers[i];
            if (!binaryReader.SafeReadBytes(&srcConstantBufferInfo, sizeof(srcConstantBufferInfo)))
                return false;

            // constant buffer struct
            Y_strncpy(pDstConstantBuffer->Name, countof(pDstConstantBuffer->Name), srcConstantBufferInfo.Name);
            pDstConstantBuffer->Size = srcConstantBufferInfo.Size;
            pDstConstantBuffer->ParameterIndex = srcConstantBufferInfo.ParameterIndex;
            pDstConstantBuffer->EngineConstantBufferIndex = 0;
            pDstConstantBuffer->pLocalGPUBuffer = nullptr;
            pDstConstantBuffer->pLocalBuffer = nullptr;
            pDstConstantBuffer->LocalBufferDirtyLowerBounds = -1;
            pDstConstantBuffer->LocalBufferDirtyUpperBounds = -1;
        
            // create local buffers
            if (srcConstantBufferInfo.IsLocal)
            {
                // allocate local memory
                pDstConstantBuffer->pLocalBuffer = new byte[pDstConstantBuffer->Size];
                Y_memzero(pDstConstantBuffer->pLocalBuffer, pDstConstantBuffer->Size);

                // create buffer
                GPU_BUFFER_DESC bufferDesc(GPU_BUFFER_FLAG_BIND_CONSTANT_BUFFER | GPU_BUFFER_FLAG_WRITABLE, pDstConstantBuffer->Size);
                if ((pDstConstantBuffer->pLocalGPUBuffer = static_cast<D3D11GPUBuffer *>(pRenderer->CreateBuffer(&bufferDesc, pDstConstantBuffer->pLocalBuffer))) == nullptr)
                {
                    Log_ErrorPrintf("D3D11ShaderProgram::Create: Attempting to allocate local constant buffer '%s' (size %u) failed.", pDstConstantBuffer->Name, pDstConstantBuffer->Size);
                    return false;
                }
            }
            else
            {
                // lookup engine constant buffer
                const ShaderConstantBuffer *pEngineConstantBuffer = ShaderConstantBuffer::GetShaderConstantBufferByName(pDstConstantBuffer->Name, pRenderer->GetPlatform(), pRenderer->GetFeatureLevel());
                if (pEngineConstantBuffer == nullptr)
                {
                    Log_ErrorPrintf("D3D11ShaderProgram::Create: Shader is requesting unknown non-local constant buffer named '%s'.", pDstConstantBuffer->Name);
                    return false;
                }

                // store index
                pDstConstantBuffer->EngineConstantBufferIndex = pEngineConstantBuffer->GetIndex();
            }
        }
    }

    // load parameters
    if (header.ParameterCount > 0)
    {
        m_parameters.Resize(header.ParameterCount);
        for (uint32 i = 0; i < m_parameters.GetSize(); i++)
        {
            D3D11ShaderCacheEntryParameter srcParameter;
            ShaderParameter *pDstParameterInfo = &m_parameters[i];
            if (!binaryReader.SafeReadBytes(&srcParameter, sizeof(srcParameter)))
                return false;

            Y_strncpy(pDstParameterInfo->Name, countof(pDstParameterInfo->Name), srcParameter.Name);
            pDstParameterInfo->Type = srcParameter.Type;
            pDstParameterInfo->ConstantBufferIndex = srcParameter.ConstantBufferIndex;
            pDstParameterInfo->ConstantBufferOffset = srcParameter.ConstantBufferOffset;
            pDstParameterInfo->ArraySize = srcParameter.ArraySize;
            pDstParameterInfo->ArrayStride = srcParameter.ArrayStride;
            pDstParameterInfo->BindTarget = (D3D11_SHADER_BIND_TARGET)srcParameter.BindTarget;
            Y_memcpy(pDstParameterInfo->BindPoint, srcParameter.BindPoint, sizeof(pDstParameterInfo->BindPoint));
            pDstParameterInfo->LinkedSamplerIndex = srcParameter.LinkedSamplerIndex;
        }
    }
        


    // all ok
    return true;
}

void D3D11GPUShaderProgram::Bind(D3D11GPUContext *pContext)
{
    DebugAssert(m_pBoundContext == nullptr);
    ID3D11DeviceContext *pD3DDeviceContext = pContext->GetD3DContext();

    // ------------------ IA ------------------
    if (m_pD3DInputLayout != nullptr)
        pD3DDeviceContext->IASetInputLayout(m_pD3DInputLayout);

    // ------------------ VS ------------------
    if (m_pD3DVertexShader != nullptr)
        pD3DDeviceContext->VSSetShader(m_pD3DVertexShader, nullptr, 0);

    // ------------------ HS ------------------
    if (m_pD3DHullShader != nullptr)
        pD3DDeviceContext->HSSetShader(m_pD3DHullShader, nullptr, 0);

    // ------------------ DS ------------------
    if (m_pD3DDomainShader != nullptr)
        pD3DDeviceContext->DSSetShader(m_pD3DDomainShader, nullptr, 0);

    // ------------------ GS ------------------
    if (m_pD3DGeometryShader != nullptr)
        pD3DDeviceContext->GSSetShader(m_pD3DGeometryShader, nullptr, 0);

    // ------------------ PS ------------------
    if (m_pD3DPixelShader != nullptr)
        pD3DDeviceContext->PSSetShader(m_pD3DPixelShader, nullptr, 0);

    // ------------------ CS ------------------
    if (m_pD3DComputeShader != nullptr)
        pD3DDeviceContext->CSSetShader(m_pD3DComputeShader, nullptr, 0);

    // bound
    m_pBoundContext = pContext;

    // bind params
    InternalBindAutomaticParameters(pContext);
}

void D3D11GPUShaderProgram::InternalBindAutomaticParameters(D3D11GPUContext *pContext)
{
    // bind constant buffers
    for (uint32 constantBufferIndex = 0; constantBufferIndex < m_constantBuffers.GetSize(); constantBufferIndex++)
    {
        ShaderLocalConstantBuffer *pConstantBufferDef = &m_constantBuffers[constantBufferIndex];

        // local?
        D3D11GPUBuffer *pConstantBuffer = (pConstantBufferDef->pLocalGPUBuffer != nullptr) ? pConstantBufferDef->pLocalGPUBuffer : pContext->GetConstantBuffer(pConstantBufferDef->EngineConstantBufferIndex);
        InternalSetParameterResource(pContext, pConstantBufferDef->ParameterIndex, pConstantBuffer, nullptr);
    }
}

void D3D11GPUShaderProgram::Switch(D3D11GPUContext *pContext, D3D11GPUShaderProgram *pCurrentProgram)
{
#ifdef LAZY_RESOURCE_CLEANUP_AFTER_SWITCH
    ID3D11DeviceContext *pD3DDeviceContext = pContext->GetD3DContext();
    DebugAssert(m_pBoundContext == nullptr && pCurrentProgram->m_pBoundContext == pContext);

    // ------------------ IA ------------------
    if (m_pD3DInputLayout != nullptr)
        pD3DDeviceContext->IASetInputLayout(m_pD3DInputLayout);
    else if (pCurrentProgram->m_pD3DInputLayout != nullptr)
        pD3DDeviceContext->IASetInputLayout(nullptr);

    // ------------------ VS ------------------
    if (m_pD3DVertexShader != nullptr)
        pD3DDeviceContext->VSSetShader(m_pD3DVertexShader, nullptr, 0);
    else if (pCurrentProgram->m_pD3DVertexShader != nullptr)
        pD3DDeviceContext->VSSetShader(nullptr, nullptr, 0);

    // ------------------ HS ------------------
    if (m_pD3DHullShader != nullptr)
        pD3DDeviceContext->HSSetShader(m_pD3DHullShader, nullptr, 0);
    else if (pCurrentProgram->m_pD3DHullShader != nullptr)
        pD3DDeviceContext->HSSetShader(nullptr, nullptr, 0);

    // ------------------ DS ------------------
    if (m_pD3DDomainShader != nullptr)
        pD3DDeviceContext->DSSetShader(m_pD3DDomainShader, nullptr, 0);
    else if (pCurrentProgram->m_pD3DDomainShader != nullptr)
        pD3DDeviceContext->DSSetShader(nullptr, nullptr, 0);

    // ------------------ GS ------------------
    if (m_pD3DGeometryShader != nullptr)
        pD3DDeviceContext->GSSetShader(m_pD3DGeometryShader, nullptr, 0);
    else if (pCurrentProgram->m_pD3DGeometryShader != nullptr)
        pD3DDeviceContext->GSSetShader(nullptr, nullptr, 0);

    // ------------------ PS ------------------
    if (m_pD3DPixelShader != nullptr)
        pD3DDeviceContext->PSSetShader(m_pD3DPixelShader, nullptr, 0);
    else if (pCurrentProgram->m_pD3DPixelShader != nullptr)
        pD3DDeviceContext->PSSetShader(nullptr, nullptr, 0);

    // ------------------ CS ------------------
    if (m_pD3DComputeShader != nullptr)
        pD3DDeviceContext->CSSetShader(m_pD3DComputeShader, nullptr, 0);
    else if (pCurrentProgram->m_pD3DComputeShader != nullptr)
        pD3DDeviceContext->CSSetShader(nullptr, nullptr, 0);

    // swap pointers
    pCurrentProgram->m_pBoundContext = nullptr;
    m_pBoundContext = pContext;

    // bind params
    InternalBindAutomaticParameters(pContext);
#else
    pCurrentProgram->Unbind(pContext);
    Bind(pContext);
#endif
}

void D3D11GPUShaderProgram::CommitLocalConstantBuffers(D3D11GPUContext *pContext)
{
    for (uint32 i = 0; i < m_constantBuffers.GetSize(); i++)
    {
        ShaderLocalConstantBuffer *pConstantBuffer = &m_constantBuffers[i];
        if (pConstantBuffer->LocalBufferDirtyLowerBounds >= 0)
        {
            // d3d11.0 does not support partial constant buffer updates
            //pContext->WriteBuffer(pConstantBuffer->pLocalGPUBuffer, pConstantBuffer->pLocalBuffer + pConstantBuffer->LocalBufferDirtyLowerBounds, 0, pConstantBuffer->LocalBufferDirtyUpperBounds - pConstantBuffer->LocalBufferDirtyLowerBounds);
            pContext->WriteBuffer(pConstantBuffer->pLocalGPUBuffer, pConstantBuffer->pLocalBuffer, 0, pConstantBuffer->Size);
            pConstantBuffer->LocalBufferDirtyLowerBounds = pConstantBuffer->LocalBufferDirtyUpperBounds = -1;
        }
    }
}

void D3D11GPUShaderProgram::Unbind(D3D11GPUContext *pContext)
{
    ID3D11DeviceContext *pD3DDeviceContext = m_pBoundContext->GetD3DContext();
    DebugAssert(m_pBoundContext == pContext);

#ifndef LAZY_RESOURCE_CLEANUP_AFTER_SWITCH
    // unset parameters that we match against
    for (uint32 parameterIndex = 0; parameterIndex < m_parameters.GetSize(); parameterIndex++)
    {
        const ShaderParameter *parameter = &m_parameters[parameterIndex];
        for (uint32 i = 0; i < SHADER_PROGRAM_STAGE_COUNT; i++)
        {
            if (parameter->BindPoint[i] == -1)
                continue;

            switch (parameter->BindTarget)
            {
            case D3D11_SHADER_BIND_TARGET_CONSTANT_BUFFER:
                pContext->SetShaderConstantBuffers((SHADER_PROGRAM_STAGE)i, parameter->BindPoint[i], nullptr);
                break;

            case D3D11_SHADER_BIND_TARGET_RESOURCE:
                pContext->SetShaderResources((SHADER_PROGRAM_STAGE)i, parameter->BindPoint[i], nullptr);
                break;

            case D3D11_SHADER_BIND_TARGET_SAMPLER:
                pContext->SetShaderSamplers((SHADER_PROGRAM_STAGE)i, parameter->BindPoint[i], nullptr);
                break;
            }
        }
    }
#endif

    // ------------------ IA ------------------
    if (m_pD3DInputLayout != nullptr)
        pD3DDeviceContext->IASetInputLayout(nullptr);

    // ------------------ VS ------------------
    if (m_pD3DVertexShader != nullptr)
        pD3DDeviceContext->VSSetShader(nullptr, nullptr, 0);

    // ------------------ HS ------------------
    if (m_pD3DHullShader != nullptr)
        pD3DDeviceContext->HSSetShader(nullptr, nullptr, 0);

    // ------------------ DS ------------------
    if (m_pD3DDomainShader != nullptr)
        pD3DDeviceContext->DSSetShader(nullptr, nullptr, 0);

    // ------------------ GS ------------------
    if (m_pD3DGeometryShader != nullptr)
        pD3DDeviceContext->GSSetShader(nullptr, nullptr, 0);

    // ------------------ PS ------------------
    if (m_pD3DPixelShader != nullptr)
        pD3DDeviceContext->PSSetShader(nullptr, nullptr, 0);

    // ------------------ CSS ------------------
    if (m_pD3DComputeShader != nullptr)
        pD3DDeviceContext->CSSetShader(nullptr, nullptr, 0);

    m_pBoundContext = nullptr;
}

void D3D11GPUShaderProgram::InternalSetParameterValue(D3D11GPUContext *pContext, uint32 parameterIndex, SHADER_PARAMETER_TYPE valueType, const void *pValue)
{
    const ShaderParameter *parameterInfo = &m_parameters[parameterIndex];
    DebugAssert(pContext == m_pBoundContext);

    uint32 valueSize = ShaderParameterValueTypeSize(parameterInfo->Type);
    DebugAssert(parameterInfo->Type == valueType && valueSize > 0);

    // should be attached to a local constant buffer
    DebugAssert(parameterInfo->ConstantBufferIndex >= 0);

    // get the constant buffer
    ShaderLocalConstantBuffer *constantBuffer = &m_constantBuffers[parameterInfo->ConstantBufferIndex];
    DebugAssert(constantBuffer->pLocalBuffer != nullptr);

    // don't write if there is no changes
    byte *pBufferPtr = constantBuffer->pLocalBuffer + parameterInfo->ConstantBufferOffset;
    if (Y_memcmp(pBufferPtr, pValue, valueSize) != 0)
    {
        // write to the local constant buffer
        Y_memcpy(pBufferPtr, pValue, valueSize);
        if (constantBuffer->LocalBufferDirtyLowerBounds < 0)
        {
            constantBuffer->LocalBufferDirtyLowerBounds = (int32)parameterInfo->ConstantBufferOffset;
            constantBuffer->LocalBufferDirtyUpperBounds = (int32)(parameterInfo->ConstantBufferOffset + valueSize);
        }
        else
        {
            constantBuffer->LocalBufferDirtyLowerBounds = Min(constantBuffer->LocalBufferDirtyLowerBounds, (int32)parameterInfo->ConstantBufferOffset);
            constantBuffer->LocalBufferDirtyUpperBounds = Max(constantBuffer->LocalBufferDirtyUpperBounds, (int32)(parameterInfo->ConstantBufferOffset + valueSize));
        }
    }
}

void D3D11GPUShaderProgram::InternalSetParameterValueArray(D3D11GPUContext *pContext, uint32 parameterIndex, SHADER_PARAMETER_TYPE valueType, const void *pValue, uint32 firstElement, uint32 numElements)
{
    const ShaderParameter *parameterInfo = &m_parameters[parameterIndex];
    DebugAssert(pContext == m_pBoundContext);

    uint32 valueSize = ShaderParameterValueTypeSize(parameterInfo->Type);
    DebugAssert(parameterInfo->Type == valueType && valueSize > 0);
    DebugAssert(numElements > 0 && (firstElement + numElements) <= parameterInfo->ArraySize);

    // should be attached to a local constant buffer
    DebugAssert(parameterInfo->ConstantBufferIndex >= 0);

    // get the constant buffer
    ShaderLocalConstantBuffer *constantBuffer = &m_constantBuffers[parameterInfo->ConstantBufferIndex];
    DebugAssert(constantBuffer->pLocalBuffer != nullptr);

    // if there is no padding, this can be done in a single operation
    uint32 bufferOffset = parameterInfo->ConstantBufferOffset + (firstElement * parameterInfo->ArrayStride);
    byte *pBufferPtr = constantBuffer->pLocalBuffer + bufferOffset;
    if (valueSize == parameterInfo->ArrayStride)
    {
        uint32 copySize = valueSize * numElements;
        if (Y_memcmp(pBufferPtr, pValue, copySize) != 0)
        {
            // write to the local constant buffer
            Y_memcpy(pBufferPtr, pValue, copySize);
            
            // update dirty range
            if (constantBuffer->LocalBufferDirtyLowerBounds < 0)
            {
                constantBuffer->LocalBufferDirtyLowerBounds = (int32)(bufferOffset);
                constantBuffer->LocalBufferDirtyUpperBounds = (int32)(bufferOffset + copySize);
            }
            else
            {
                constantBuffer->LocalBufferDirtyLowerBounds = Min(constantBuffer->LocalBufferDirtyLowerBounds, (int32)bufferOffset);
                constantBuffer->LocalBufferDirtyUpperBounds = Max(constantBuffer->LocalBufferDirtyUpperBounds, (int32)(bufferOffset + copySize));
            }
        }
    }
    else
    {
        if (Y_memcmp_stride(pBufferPtr, parameterInfo->ArrayStride, pValue, valueSize, valueSize, numElements) != 0)
        {
            // write to the local constant buffer
            Y_memcpy_stride(pBufferPtr, parameterInfo->ArrayStride, pValue, valueSize, valueSize, numElements);
            
            // update dirty range
            uint32 writeSize = parameterInfo->ArrayStride * numElements;
            if (constantBuffer->LocalBufferDirtyLowerBounds < 0)
            {
                constantBuffer->LocalBufferDirtyLowerBounds = (int32)(bufferOffset);
                constantBuffer->LocalBufferDirtyUpperBounds = (int32)(bufferOffset + writeSize);
            }
            else
            {
                constantBuffer->LocalBufferDirtyLowerBounds = Min(constantBuffer->LocalBufferDirtyLowerBounds, (int32)bufferOffset);
                constantBuffer->LocalBufferDirtyUpperBounds = Max(constantBuffer->LocalBufferDirtyLowerBounds, (int32)(bufferOffset + writeSize));
            }
        }
    }    
}

void D3D11GPUShaderProgram::InternalSetParameterStruct(GPUContext *pContext, uint32 parameterIndex, const void *pValue, uint32 valueSize)
{
    const ShaderParameter *parameterInfo = &m_parameters[parameterIndex];
    DebugAssert(pContext == m_pBoundContext);
    DebugAssert(parameterInfo->Type == SHADER_PARAMETER_TYPE_STRUCT && valueSize <= parameterInfo->ArrayStride);

    // should be attached to a local constant buffer
    DebugAssert(parameterInfo->ConstantBufferIndex >= 0);

    // get the constant buffer
    ShaderLocalConstantBuffer *constantBuffer = &m_constantBuffers[parameterInfo->ConstantBufferIndex];
    DebugAssert(constantBuffer->pLocalBuffer != nullptr);

    // don't write if there is no changes
    byte *pBufferPtr = constantBuffer->pLocalBuffer + parameterInfo->ConstantBufferOffset;
    if (Y_memcmp(pBufferPtr, pValue, valueSize) != 0)
    {
        // write to the local constant buffer
        Y_memcpy(pBufferPtr, pValue, valueSize);
        if (constantBuffer->LocalBufferDirtyLowerBounds < 0)
        {
            constantBuffer->LocalBufferDirtyLowerBounds = (int32)parameterInfo->ConstantBufferOffset;
            constantBuffer->LocalBufferDirtyUpperBounds = (int32)(parameterInfo->ConstantBufferOffset + valueSize);
        }
        else
        {
            constantBuffer->LocalBufferDirtyLowerBounds = Min(constantBuffer->LocalBufferDirtyLowerBounds, (int32)parameterInfo->ConstantBufferOffset);
            constantBuffer->LocalBufferDirtyUpperBounds = Max(constantBuffer->LocalBufferDirtyUpperBounds, (int32)(parameterInfo->ConstantBufferOffset + valueSize));
        }
    }
}

void D3D11GPUShaderProgram::InternalSetParameterStructArray(GPUContext *pContext, uint32 parameterIndex, const void *pValue, uint32 valueSize, uint32 firstElement, uint32 numElements)
{
    const ShaderParameter *parameterInfo = &m_parameters[parameterIndex];
    DebugAssert(pContext == m_pBoundContext);
    DebugAssert(parameterInfo->Type == SHADER_PARAMETER_TYPE_STRUCT && valueSize <= parameterInfo->ArrayStride);
    DebugAssert(numElements > 0 && (firstElement + numElements) <= parameterInfo->ArraySize);

    // should be attached to a local constant buffer
    DebugAssert(parameterInfo->ConstantBufferIndex >= 0);

    // get the constant buffer
    ShaderLocalConstantBuffer *constantBuffer = &m_constantBuffers[parameterInfo->ConstantBufferIndex];
    DebugAssert(constantBuffer->pLocalBuffer != nullptr);

    // if there is no padding, this can be done in a single operation
    uint32 bufferOffset = parameterInfo->ConstantBufferOffset + (firstElement * parameterInfo->ArrayStride);
    byte *pBufferPtr = constantBuffer->pLocalBuffer + bufferOffset;
    if (valueSize == parameterInfo->ArrayStride)
    {
        uint32 copySize = valueSize * numElements;
        if (Y_memcmp(pBufferPtr, pValue, copySize) != 0)
        {
            // write to the local constant buffer
            Y_memcpy(pBufferPtr, pValue, copySize);

            // update dirty range
            if (constantBuffer->LocalBufferDirtyLowerBounds < 0)
            {
                constantBuffer->LocalBufferDirtyLowerBounds = (int32)(bufferOffset);
                constantBuffer->LocalBufferDirtyUpperBounds = (int32)(bufferOffset + copySize);
            }
            else
            {
                constantBuffer->LocalBufferDirtyLowerBounds = Min(constantBuffer->LocalBufferDirtyLowerBounds, (int32)bufferOffset);
                constantBuffer->LocalBufferDirtyUpperBounds = Max(constantBuffer->LocalBufferDirtyUpperBounds, (int32)(bufferOffset + copySize));
            }
        }
    }
    else
    {
        if (Y_memcmp_stride(pBufferPtr, parameterInfo->ArrayStride, pValue, valueSize, valueSize, numElements) != 0)
        {
            // write to the local constant buffer
            Y_memcpy_stride(pBufferPtr, parameterInfo->ArrayStride, pValue, valueSize, valueSize, numElements);

            // update dirty range
            uint32 writeSize = parameterInfo->ArrayStride * numElements;
            if (constantBuffer->LocalBufferDirtyLowerBounds < 0)
            {
                constantBuffer->LocalBufferDirtyLowerBounds = (int32)(bufferOffset);
                constantBuffer->LocalBufferDirtyUpperBounds = (int32)(bufferOffset + writeSize);
            }
            else
            {
                constantBuffer->LocalBufferDirtyLowerBounds = Min(constantBuffer->LocalBufferDirtyLowerBounds, (int32)bufferOffset);
                constantBuffer->LocalBufferDirtyUpperBounds = Max(constantBuffer->LocalBufferDirtyLowerBounds, (int32)(bufferOffset + writeSize));
            }
        }
    }
}

void D3D11GPUShaderProgram::InternalSetParameterResource(D3D11GPUContext *pContext, uint32 parameterIndex, GPUResource *pResource, GPUSamplerState *pLinkedSamplerState)
{
    const ShaderParameter *parameterInfo = &m_parameters[parameterIndex];
    DebugAssert(pContext == m_pBoundContext);
    
//     // handle type mismatches
//     if (pResource != nullptr)
//     {
//         GPU_RESOURCE_TYPE expectedResourceType = ShaderParameterResourceType(parameterInfo->Type);
//         if (pResource->GetResourceType() != expectedResourceType)
//         {
//             Log_WarningPrintf("D3D11GPUShaderProgram::InternalSetParameterResource: Parameter %u type mismatch (expecting %s, got %s for ptype %s), setting to null", parameterIndex, NameTable_GetNameString(NameTables::GPUResourceType, expectedResourceType), NameTable_GetNameString(NameTables::GPUResourceType, pResource->GetResourceType()), NameTable_GetNameString(NameTables::ShaderParameterType, parameterInfo->Type));
//             return;
//         }
//     }

    // branch out according to target
    switch (parameterInfo->BindTarget)
    {
    case D3D11_SHADER_BIND_TARGET_CONSTANT_BUFFER:
        {
            ID3D11Buffer *pD3DBuffer = (pResource != nullptr) ? static_cast<D3D11GPUBuffer *>(pResource)->GetD3DBuffer() : nullptr;
            for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
            {
                int32 bindPoint = parameterInfo->BindPoint[stageIndex];
                if (bindPoint >= 0)
                    pContext->SetShaderConstantBuffers((SHADER_PROGRAM_STAGE)stageIndex, bindPoint, pD3DBuffer);
            }
        }
        break;

    case D3D11_SHADER_BIND_TARGET_RESOURCE:
        {
            ID3D11ShaderResourceView *pD3DSRV = (pResource != nullptr) ? D3D11Helpers::GetResourceShaderResourceView(pResource) : nullptr;
            for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
            {
                int32 bindPoint = parameterInfo->BindPoint[stageIndex];
                if (bindPoint >= 0)
                    pContext->SetShaderResources((SHADER_PROGRAM_STAGE)stageIndex, bindPoint, pD3DSRV);
            }
        }
        break;

    case D3D11_SHADER_BIND_TARGET_SAMPLER:
        {
            ID3D11SamplerState *pD3DSamplerState = (pResource != nullptr) ? D3D11Helpers::GetResourceSamplerState(pResource) : nullptr;
            for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
            {
                int32 bindPoint = parameterInfo->BindPoint[stageIndex];
                if (bindPoint >= 0)
                    pContext->SetShaderSamplers((SHADER_PROGRAM_STAGE)stageIndex, bindPoint, pD3DSamplerState);
            }
        }
        break;

    case D3D11_SHADER_BIND_TARGET_UNORDERED_ACCESS_VIEW:
        {
            ID3D11UnorderedAccessView *pD3DUAV = (pResource != nullptr && pResource->GetResourceType() == GPU_RESOURCE_TYPE_COMPUTE_VIEW) ? static_cast<D3D11GPUComputeView *>(pResource)->GetD3DUAV() : nullptr;
            for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
            {
                int32 bindPoint = parameterInfo->BindPoint[stageIndex];
                if (bindPoint >= 0)
                    pContext->SetShaderUAVs((SHADER_PROGRAM_STAGE)stageIndex, bindPoint, pD3DUAV);
            }
        }
        break;
    }

    // for texture types with a linked sampler state, update it
    if (parameterInfo->LinkedSamplerIndex >= 0)
    {
        const ShaderParameter *samplerParameterInfo = &m_parameters[parameterInfo->LinkedSamplerIndex];
        DebugAssert(samplerParameterInfo->Type == SHADER_PARAMETER_TYPE_SAMPLER_STATE && samplerParameterInfo->BindTarget == D3D11_SHADER_BIND_TARGET_SAMPLER);

        // write to stages
        ID3D11SamplerState *pD3DSamplerState = (pResource != nullptr) ? D3D11Helpers::GetResourceSamplerState((pLinkedSamplerState != nullptr) ? pLinkedSamplerState : pResource) : nullptr;
        for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
        {
            int32 bindPoint = samplerParameterInfo->BindPoint[stageIndex];
            if (bindPoint >= 0)
                pContext->SetShaderSamplers((SHADER_PROGRAM_STAGE)stageIndex, bindPoint, pD3DSamplerState);
        }
    }
}

uint32 D3D11GPUShaderProgram::GetParameterCount() const
{
    return m_parameters.GetSize();
}

void D3D11GPUShaderProgram::GetParameterInformation(uint32 index, const char **name, SHADER_PARAMETER_TYPE *type, uint32 *arraySize)
{
    const ShaderParameter *parameter = &m_parameters[index];
    *name = parameter->Name;
    *type = parameter->Type;
    *arraySize = parameter->ArraySize;
}

void D3D11GPUShaderProgram::SetParameterValue(GPUContext *pContext, uint32 index, SHADER_PARAMETER_TYPE valueType, const void *pValue)
{
    InternalSetParameterValue(static_cast<D3D11GPUContext *>(pContext), index, valueType, pValue);
}

void D3D11GPUShaderProgram::SetParameterValueArray(GPUContext *pContext, uint32 index, SHADER_PARAMETER_TYPE valueType, const void *pValue, uint32 firstElement, uint32 numElements)
{
    InternalSetParameterValueArray(static_cast<D3D11GPUContext *>(pContext), index, valueType, pValue, firstElement, numElements);
}

void D3D11GPUShaderProgram::SetParameterStruct(GPUContext *pContext, uint32 index, const void *pValue, uint32 valueSize)
{
    InternalSetParameterStruct(pContext, index, pValue, valueSize);
}

void D3D11GPUShaderProgram::SetParameterStructArray(GPUContext *pContext, uint32 index, const void *pValue, uint32 valueSize, uint32 firstElement, uint32 numElements)
{
    InternalSetParameterStructArray(pContext, index, pValue, valueSize, firstElement, numElements);
}

void D3D11GPUShaderProgram::SetParameterResource(GPUContext *pContext, uint32 index, GPUResource *pResource)
{
    InternalSetParameterResource(static_cast<D3D11GPUContext *>(pContext), index, pResource, nullptr);
}

void D3D11GPUShaderProgram::SetParameterTexture(GPUContext *pContext, uint32 index, GPUTexture *pTexture, GPUSamplerState *pSamplerState)
{
    InternalSetParameterResource(static_cast<D3D11GPUContext *>(pContext), index, pTexture, pSamplerState);
}

GPUShaderProgram *D3D11Renderer::CreateGraphicsProgram(const GPU_VERTEX_ELEMENT_DESC *pVertexElements, uint32 nVertexElements, ByteStream *pByteCodeStream)
{
    D3D11GPUShaderProgram *pProgram = new D3D11GPUShaderProgram();
    if (!pProgram->Create(this, pVertexElements, nVertexElements, pByteCodeStream))
    {
        pProgram->Release();
        return nullptr;
    }

    return pProgram;
}

GPUShaderProgram *D3D11Renderer::CreateComputeProgram(ByteStream *pByteCodeStream)
{
    D3D11GPUShaderProgram *pProgram = new D3D11GPUShaderProgram();
    if (!pProgram->Create(this, nullptr, 0, pByteCodeStream))
    {
        pProgram->Release();
        return nullptr;
    }

    return pProgram;
}
