#include "ResourceCompiler/PrecompiledHeader.h"
#include "ResourceCompiler/ShaderCompilerD3D.h"

#ifdef Y_PLATFORM_WINDOWS
#include "ResourceCompiler/ShaderGraphCompilerHLSL.h"
#include "Renderer/Renderer.h"
Log_SetChannel(ShaderCompilerD3D);

#include <d3d11.h>
#include "D3D11Renderer/D3DShaderCacheEntry.h"
#include "D3D11Renderer/D3D11Defines.h"
#pragma comment(lib, "d3dcompiler.lib")

struct PlatformProfileInfo
{
    const char *StageProfiles[SHADER_PROGRAM_STAGE_COUNT];
};

static const PlatformProfileInfo s_InternalProfileInfo[RENDERER_FEATURE_LEVEL_COUNT] =
{
    //vs profile          hs profile          ds profile          gs profile          ps profile      cs profile  
    { "vs_4_0",           NULL,               NULL,               NULL,               "ps_4_0",       NULL        },          // RENDERER_FEATURE_LEVEL_ES2
    { "vs_4_0",           NULL,               NULL,               NULL,               "ps_4_0",       NULL        },          // RENDERER_FEATURE_LEVEL_ES3
    { "vs_4_0",           NULL,               NULL,               "gs_4_0",           "ps_4_0",       "cs_4_0"    },          // RENDERER_FEATURE_LEVEL_SM4
    { "vs_5_0",           "hs_5_0",           "ds_5_0",           "gs_5_0",           "ps_5_0",       "cs_5_0"    },          // RENDERER_FEATURE_LEVEL_SM5
};


// d3d11 include interface
struct D3DIncludeInterface : public ID3DInclude
{
    D3DIncludeInterface(ShaderCompilerD3D *pParent) : m_pParent(pParent) {}

    STDOVERRIDEMETHODIMP Open(THIS_ D3D_INCLUDE_TYPE IncludeType, LPCSTR pFileName, LPCVOID pParentData, LPCVOID *ppData, UINT *pBytes)
    {
        return (m_pParent->ReadIncludeFile((IncludeType == D3D_INCLUDE_SYSTEM), pFileName, const_cast<LPVOID *>(ppData), pBytes)) ? S_OK : E_FAIL;
    }

    STDOVERRIDEMETHODIMP Close(THIS_ LPCVOID pData)
    {
        m_pParent->FreeIncludeFile(const_cast<LPVOID>(pData));
        return S_OK;
    }

private:
    ShaderCompilerD3D *m_pParent;
};

ShaderCompilerD3D::ShaderCompilerD3D(ResourceCompilerCallbacks *pCallbacks, const ShaderCompilerParameters *pParameters)
    : ShaderCompiler(pCallbacks, pParameters)
{

}

ShaderCompilerD3D::~ShaderCompilerD3D()
{

}

void ShaderCompilerD3D::BuildD3DDefineList(SHADER_PROGRAM_STAGE stage, MemArray<D3D_SHADER_MACRO> &D3DMacroArray)
{
#define ADD_D3D_SHADER_MACRO(MacroName, MacroDefinition) MULTI_STATEMENT_MACRO_BEGIN \
                                                    Macro.Name = MacroName; \
                                                    Macro.Definition = MacroDefinition; \
                                                    D3DMacroArray.Add(Macro); \
                                               MULTI_STATEMENT_MACRO_END

    D3D_SHADER_MACRO Macro;  

    // stage-specific defines
    switch (stage)
    {
    case SHADER_PROGRAM_STAGE_VERTEX_SHADER:
        ADD_D3D_SHADER_MACRO("VERTEX_SHADER",        "1");
        break;

    case SHADER_PROGRAM_STAGE_HULL_SHADER:
        ADD_D3D_SHADER_MACRO("HULL_SHADER",          "1");
        break;

    case SHADER_PROGRAM_STAGE_DOMAIN_SHADER:
        ADD_D3D_SHADER_MACRO("DOMAIN_SHADER",        "1");
        break;

    case SHADER_PROGRAM_STAGE_GEOMETRY_SHADER:
        ADD_D3D_SHADER_MACRO("GEOMETRY_SHADER",      "1");
        break;

    case SHADER_PROGRAM_STAGE_PIXEL_SHADER:
        ADD_D3D_SHADER_MACRO("PIXEL_SHADER",         "1");
        break;

    case SHADER_PROGRAM_STAGE_COMPUTE_SHADER:
        ADD_D3D_SHADER_MACRO("COMPUTE_SHADER",       "1");
        break;
    }

    // build the d3d list
    uint32 i;
    for (i = 0; i < m_CompileMacros.GetSize(); i++)
        ADD_D3D_SHADER_MACRO(m_CompileMacros[i].Key, m_CompileMacros[i].Value);

    // NULL end-macro
    ADD_D3D_SHADER_MACRO(NULL, NULL);
#undef ADD_D3D_SHADER_MACRO
}

bool ShaderCompilerD3D::CompileShaderStage(SHADER_PROGRAM_STAGE stage, byte **ppShaderByteCode, uint32 *pShaderByteCodeSize)
{
    D3DIncludeInterface includeInterface(this);
    HRESULT hResult;
    
    // get source filename
    const PlatformProfileInfo *pPlatformProfileInfo = &s_InternalProfileInfo[m_eRendererFeatureLevel];
    if (m_StageFileNames[stage].IsEmpty() || m_StageEntryPoints[stage].IsEmpty())
        return false;

    // open the source file
    BinaryBlob *pSourceBlob = m_pResourceCompilerCallbacks->GetFileContents(m_StageFileNames[stage]);
    if (pSourceBlob == nullptr)
    {
        Log_ErrorPrintf("ShaderCompilerD3D::CompileShaderStage: Could not open shader source file '%s'.", m_StageFileNames[stage].GetCharArray());
        return false;
    }

    // get define list
    MemArray<D3D_SHADER_MACRO> D3DMacroArray;
    BuildD3DDefineList(stage, D3DMacroArray);

    // flags
    uint32 D3DCompileFlags = D3DCOMPILE_PACK_MATRIX_ROW_MAJOR;

    // debug flags
    if (m_iShaderCompilerFlags & SHADER_COMPILER_FLAG_ENABLE_DEBUG_INFO)
        D3DCompileFlags |= D3DCOMPILE_DEBUG;

    // optimization flags
    if (m_iShaderCompilerFlags & SHADER_COMPILER_FLAG_DISABLE_OPTIMIZATIONS)
        D3DCompileFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
    else
        D3DCompileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;

    ID3DBlob *pCodeBlob;
    ID3DBlob *pErrorBlob;

    // shader dumping
    if (m_pDumpWriter != nullptr)
    {
        ID3DBlob *pCodeText;
        hResult = D3DPreprocess(pSourceBlob->GetDataPointer(), pSourceBlob->GetDataSize(),
                                m_StageFileNames[stage],
                                D3DMacroArray.GetBasePointer(),
                                &includeInterface,
                                &pCodeText,
                                &pErrorBlob);

        if (FAILED(hResult))
        {
            Log_ErrorPrintf("CompileShaderStage: D3DPreprocess failed with HResult %08X", hResult);
            if (pErrorBlob != NULL)
            {
                Log_ErrorPrint((const char *)pErrorBlob->GetBufferPointer());
                pErrorBlob->Release();
            }

            pSourceBlob->Release();
            return false;
        }

        if (pErrorBlob != NULL)
        {
            Log_WarningPrintf("CompileShaderStage: Preprocess succeeded with warnings.");
            Log_WarningPrint((const char *)pErrorBlob->GetBufferPointer());
            pErrorBlob->Release();
        }

        m_pDumpWriter->WriteFormattedLine("Preprocessed shader %s:", pPlatformProfileInfo->StageProfiles[stage]);
        m_pDumpWriter->WriteWithLineNumbers(reinterpret_cast<const char *>(pCodeText->GetBufferPointer()), (uint32)pCodeText->GetBufferSize());
        pCodeText->Release();
    }

    // invoke compiler
    hResult = D3DCompile(pSourceBlob->GetDataPointer(), pSourceBlob->GetDataSize(),
                         m_StageFileNames[stage],
                         D3DMacroArray.GetBasePointer(),
                         &includeInterface,
                         m_StageEntryPoints[stage],
                         pPlatformProfileInfo->StageProfiles[stage],
                         D3DCompileFlags,
                         0,
                         &pCodeBlob,
                         &pErrorBlob);

    if (FAILED(hResult))
    {
        Log_ErrorPrintf("CompileShaderStage: D3DCompile for %s failed with HResult %08X", pPlatformProfileInfo->StageProfiles[stage], hResult);
        if (pErrorBlob != NULL)
        {
            if (m_pDumpWriter != NULL)
            {
                m_pDumpWriter->WriteFormattedLine("CompileShaderStage: D3DCompile for %s failed with HResult %08X", pPlatformProfileInfo->StageProfiles[stage], hResult);
                m_pDumpWriter->Write((const char *)pErrorBlob->GetBufferPointer(), (uint32)pErrorBlob->GetBufferSize());
                m_pDumpWriter->WriteLine("");
            }

            Log_ErrorPrint((const char *)pErrorBlob->GetBufferPointer());
            pErrorBlob->Release();
        }

        pSourceBlob->Release();
        return false;
    }

    if (pErrorBlob != NULL)
    {
        if (m_pDumpWriter != NULL)
        {
            m_pDumpWriter->WriteLine("CompileShaderStage: Compile succeeded with warnings.");
            m_pDumpWriter->Write((const char *)pErrorBlob->GetBufferPointer(), (uint32)pErrorBlob->GetBufferSize());
            m_pDumpWriter->WriteLine("");
        }

        Log_WarningPrintf("CompileShaderStage: Compile succeeded with warnings.");
        Log_WarningPrint((const char *)pErrorBlob->GetBufferPointer());
        pErrorBlob->Release();
    }

    if (m_pDumpWriter != NULL)
    {
        ID3DBlob *pDisasmBlob;
        hResult = D3DDisassemble(pCodeBlob->GetBufferPointer(), pCodeBlob->GetBufferSize(), D3D_DISASM_ENABLE_INSTRUCTION_NUMBERING, NULL, &pDisasmBlob);
        if (FAILED(hResult))
        {
            Log_WarningPrintf("D3DDisassemble failed with hResult %08X", hResult);
            m_pDumpWriter->WriteFormattedLine("D3DDisassemble failed with hResult %08X", hResult);
        }
        else
        {
            m_pDumpWriter->WriteFormattedLine("%s shader disassembly:", pPlatformProfileInfo->StageProfiles[stage]);
            m_pDumpWriter->Write((const char *)pDisasmBlob->GetBufferPointer(), (uint32)pDisasmBlob->GetBufferSize());
            pDisasmBlob->Release();
        }
    }

    // set blob
    Log_DevPrintf("CompileShaderStage: %s compiled to %u bytes.", pPlatformProfileInfo->StageProfiles[stage], pCodeBlob->GetBufferSize());

    // release source
    pSourceBlob->Release();

    // copy code
    *ppShaderByteCode = (byte *)Y_malloc(pCodeBlob->GetBufferSize());
    Y_memcpy(*ppShaderByteCode, pCodeBlob->GetBufferPointer(), (uint32)pCodeBlob->GetBufferSize());
    *pShaderByteCodeSize = static_cast<uint32>(pCodeBlob->GetBufferSize());
    FAST_RELEASE(pCodeBlob);
    return true;
}

static bool MapD3D11ShaderTypeDescToParameterType(const D3D11_SHADER_TYPE_DESC *pVariableTypeDesc, SHADER_PARAMETER_TYPE *pParameterType)
{
    struct TypeTranslation { SHADER_PARAMETER_TYPE UniformType; D3D_SHADER_VARIABLE_CLASS D3DClass; D3D_SHADER_VARIABLE_TYPE D3DType; uint32 Rows; uint32 Cols; };
    static const TypeTranslation TranslationTable[] =
    {
        { SHADER_PARAMETER_TYPE_BOOL,               D3D_SVC_SCALAR,             D3D_SVT_BOOL,           1,      1   },
        { SHADER_PARAMETER_TYPE_INT,                D3D_SVC_SCALAR,             D3D_SVT_INT,            1,      1   },
        { SHADER_PARAMETER_TYPE_INT2,               D3D_SVC_SCALAR,             D3D_SVT_INT,            1,      2   },
        { SHADER_PARAMETER_TYPE_INT3,               D3D_SVC_SCALAR,             D3D_SVT_INT,            1,      3   },
        { SHADER_PARAMETER_TYPE_INT4,               D3D_SVC_SCALAR,             D3D_SVT_INT,            1,      4   },
        { SHADER_PARAMETER_TYPE_UINT,               D3D_SVC_SCALAR,             D3D_SVT_UINT,           1,      1   },
        { SHADER_PARAMETER_TYPE_UINT2,              D3D_SVC_SCALAR,             D3D_SVT_UINT,           1,      2   },
        { SHADER_PARAMETER_TYPE_UINT3,              D3D_SVC_SCALAR,             D3D_SVT_UINT,           1,      3   },
        { SHADER_PARAMETER_TYPE_UINT4,              D3D_SVC_SCALAR,             D3D_SVT_UINT,           1,      4   },
        { SHADER_PARAMETER_TYPE_FLOAT,              D3D_SVC_SCALAR,             D3D_SVT_FLOAT,          1,      1   },
        { SHADER_PARAMETER_TYPE_FLOAT2,             D3D_SVC_VECTOR,             D3D_SVT_FLOAT,          1,      2   },
        { SHADER_PARAMETER_TYPE_FLOAT3,             D3D_SVC_VECTOR,             D3D_SVT_FLOAT,          1,      3   },
        { SHADER_PARAMETER_TYPE_FLOAT4,             D3D_SVC_VECTOR,             D3D_SVT_FLOAT,          1,      4   },
        { SHADER_PARAMETER_TYPE_FLOAT2X2,           D3D_SVC_MATRIX_ROWS,        D3D_SVT_FLOAT,          2,      2   },
        { SHADER_PARAMETER_TYPE_FLOAT3X3,           D3D_SVC_MATRIX_ROWS,        D3D_SVT_FLOAT,          3,      3   },
        { SHADER_PARAMETER_TYPE_FLOAT3X4,           D3D_SVC_MATRIX_ROWS,        D3D_SVT_FLOAT,          3,      4   },
        { SHADER_PARAMETER_TYPE_FLOAT4X4,           D3D_SVC_MATRIX_ROWS,        D3D_SVT_FLOAT,          4,      4   },
    };

    for (uint32 i = 0; i < countof(TranslationTable); i++)
    {
        const TypeTranslation *pCur = &TranslationTable[i];
        if (pCur->D3DClass == pVariableTypeDesc->Class &&
            pCur->D3DType == pVariableTypeDesc->Type &&
            pCur->Rows == pVariableTypeDesc->Rows &&
            pCur->Cols == pVariableTypeDesc->Columns)
        {
            *pParameterType = pCur->UniformType;
            return true;
        }
    }

    return false;
}

static bool MapD3D11ShaderInputBindDescToParameterType(const D3D11_SHADER_INPUT_BIND_DESC *pInputBindDesc, SHADER_PARAMETER_TYPE *pParameterType, D3D_SHADER_BIND_TARGET *pBindTarget)
{
    if (pInputBindDesc->Type == D3D_SIT_CBUFFER)
    {
        // map to constant buffer
        *pParameterType = SHADER_PARAMETER_TYPE_CONSTANT_BUFFER;
        *pBindTarget = D3D_SHADER_BIND_TARGET_CONSTANT_BUFFER;
        return true;
    }
    else if (pInputBindDesc->Type == D3D_SIT_SAMPLER)
    {
        // map to sampler state
        *pParameterType = SHADER_PARAMETER_TYPE_SAMPLER_STATE;
        *pBindTarget = D3D_SHADER_BIND_TARGET_SAMPLER;
        return true;
    }
    else
    {
        // figure out the dimension
        switch (pInputBindDesc->Dimension)
        {
        case D3D_SRV_DIMENSION_TEXTURE1D:           *pParameterType = SHADER_PARAMETER_TYPE_TEXTURE1D;          break;
        case D3D_SRV_DIMENSION_TEXTURE1DARRAY:      *pParameterType = SHADER_PARAMETER_TYPE_TEXTURE1DARRAY;     break;
        case D3D_SRV_DIMENSION_TEXTURE2D:           *pParameterType = SHADER_PARAMETER_TYPE_TEXTURE2D;          break;
        case D3D_SRV_DIMENSION_TEXTURE2DARRAY:      *pParameterType = SHADER_PARAMETER_TYPE_TEXTURE2DARRAY;     break;
        case D3D_SRV_DIMENSION_TEXTURE2DMS:         *pParameterType = SHADER_PARAMETER_TYPE_TEXTURE2DMS;        break;
        case D3D_SRV_DIMENSION_TEXTURE2DMSARRAY:    *pParameterType = SHADER_PARAMETER_TYPE_TEXTURE2DMSARRAY;   break;
        case D3D_SRV_DIMENSION_TEXTURE3D:           *pParameterType = SHADER_PARAMETER_TYPE_TEXTURE3D;          break;
        case D3D_SRV_DIMENSION_TEXTURECUBE:         *pParameterType = SHADER_PARAMETER_TYPE_TEXTURECUBE;        break;
        case D3D_SRV_DIMENSION_TEXTURECUBEARRAY:    *pParameterType = SHADER_PARAMETER_TYPE_TEXTURECUBEARRAY;   break;
        case D3D_SRV_DIMENSION_BUFFER:              *pParameterType = SHADER_PARAMETER_TYPE_BUFFER;             break;
        case D3D_SRV_DIMENSION_BUFFEREX:            *pParameterType = SHADER_PARAMETER_TYPE_BUFFER;             break;
        default:
            return false;
        }

        // work out the binding type
        if (pInputBindDesc->Type == D3D_SIT_TEXTURE)
            *pBindTarget = D3D_SHADER_BIND_TARGET_RESOURCE;
        else if (pInputBindDesc->Type >= D3D_SIT_UAV_RWTYPED && pInputBindDesc->Type <= D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER)
            *pBindTarget = D3D_SHADER_BIND_TARGET_UNORDERED_ACCESS_VIEW;
        else
            return false;

        // done
        return true;
    }
}

// TODO move into class
static bool ReflectShader(SHADER_PROGRAM_STAGE stage,
                          MemArray<D3DShaderCacheEntryVertexAttribute> &outVertexAttributes,
                          MemArray<D3DShaderCacheEntryConstantBuffer> &outConstantBuffers,
                          MemArray<D3DShaderCacheEntryParameter> &outParameters,
                          const byte *pByteCode, uint32 byteCodeSize)
{
    ID3D11ShaderReflection *pReflectionPointer;
    HRESULT hResult = D3DReflect(pByteCode, byteCodeSize, IID_ID3D11ShaderReflection, (void **)&pReflectionPointer);
    if (FAILED(hResult))
    {
        Log_ErrorPrintf("D3DReflect failed with hResult %08X", hResult);
        return false;
    }

    // construct an autorelease to make stuff neater
    AutoReleasePtr<ID3D11ShaderReflection> pReflection = pReflectionPointer;

    // get shader descriptor
    D3D11_SHADER_DESC shaderDesc;
    hResult = pReflection->GetDesc(&shaderDesc);
    DebugAssert(SUCCEEDED(hResult));

//     ID3DBlob *pDisassembly;
//     hResult = D3DDisassemble(pByteCode, byteCodeSize, 0, NULL, &pDisassembly);
//     DebugAssert(hResult == S_OK);
//     Log_DevPrint("Shader Disassembly");
//     Log_DevPrint((const char *)pDisassembly->GetBufferPointer());
//     pDisassembly->Release();
// 
    // parse input format
    if (stage == SHADER_PROGRAM_STAGE_VERTEX_SHADER)
    {
        for (uint32 i = 0; i < shaderDesc.InputParameters; i++)
        {
            D3D11_SIGNATURE_PARAMETER_DESC signatureParameterDesc;
            hResult = pReflection->GetInputParameterDesc(i, &signatureParameterDesc);
            DebugAssert(hResult == S_OK);

            // skip system values
            if (signatureParameterDesc.SystemValueType != D3D_NAME_UNDEFINED)
                continue;

            D3DShaderCacheEntryVertexAttribute vertexAttribute;
            if (!NameTable_TranslateType(NameTables::GPUVertexElementSemantic, signatureParameterDesc.SemanticName, &vertexAttribute.SemanticName, true))
            {
                Log_ErrorPrintf("ReflectShader: failed to find mapping of semantic name '%s'", signatureParameterDesc.SemanticName);
                return false;
            }

            vertexAttribute.SemanticIndex = signatureParameterDesc.SemanticIndex;
            outVertexAttributes.Add(vertexAttribute);
        }
    }

    // parse constant buffers
    if (shaderDesc.ConstantBuffers > 0)
    {
        for (uint32 constantBufferIndex = 0; constantBufferIndex < shaderDesc.ConstantBuffers; constantBufferIndex++)
        {
            ID3D11ShaderReflectionConstantBuffer *pConstantBufferReflection = pReflection->GetConstantBufferByIndex(constantBufferIndex);
            DebugAssert(pConstantBufferReflection != NULL);

            D3D11_SHADER_BUFFER_DESC constantBufferDesc;
            hResult = pConstantBufferReflection->GetDesc(&constantBufferDesc);
            DebugAssert(SUCCEEDED(hResult));

//             // is the constant buffer used by the shader?
//             D3D11_SHADER_INPUT_BIND_DESC inputBindDesc;
//             hResult = pReflection->GetResourceBindingDescByName(constantBufferDesc.Name, &inputBindDesc);
//             if (FAILED(hResult))
//                 continue;

            // currently, only the $Globals constant buffer is made local
            bool isLocal = (Y_strcmp(constantBufferDesc.Name, "$Globals") == 0 || Y_strncmp(constantBufferDesc.Name, "__LocalConstantBuffer_", 22) == 0);

            // do we have an entry for it?
            uint32 outConstantBufferIndex;
            for (outConstantBufferIndex = 0; outConstantBufferIndex < outConstantBuffers.GetSize(); outConstantBufferIndex++)
            {
                if (Y_strcmp(outConstantBuffers[outConstantBufferIndex].Name, constantBufferDesc.Name) == 0)
                {
                    // found it
                    break;
                }
            }

            // not present?
            if (outConstantBufferIndex == outConstantBuffers.GetSize())
            {
                // add the constant buffer entry
                D3DShaderCacheEntryConstantBuffer cbDecl;
                Y_strncpy(cbDecl.Name, countof(cbDecl.Name), constantBufferDesc.Name);
                cbDecl.Size = constantBufferDesc.Size;
                cbDecl.ParameterIndex = 0xFFFFFFFF;
                cbDecl.IsLocal = isLocal;
                outConstantBuffers.Add(cbDecl);
            }

//             // write the binding point to the output constant buffers
//             DebugAssert(outConstantBuffers[outConstantBufferIndex].BindPoint[stage] == -1);
//             outConstantBuffers[outConstantBufferIndex].BindPoint[stage] = inputBindDesc.BindPoint;

            // find variables for this constant buffer if it is local, since they cannot be set any other way
            if (outConstantBuffers[outConstantBufferIndex].IsLocal)
            {
                for (uint32 variableIndex = 0; variableIndex < constantBufferDesc.Variables; variableIndex++)
                {
                    ID3D11ShaderReflectionVariable *pVariableReflection = pConstantBufferReflection->GetVariableByIndex(variableIndex);

                    // query variable 
                    D3D11_SHADER_VARIABLE_DESC variableDesc;
                    hResult = pVariableReflection->GetDesc(&variableDesc);
                    DebugAssert(SUCCEEDED(hResult));

                    // query variable type
                    ID3D11ShaderReflectionType *pVariableType = pVariableReflection->GetType();
                    D3D11_SHADER_TYPE_DESC typeDesc;
                    hResult = pVariableType->GetDesc(&typeDesc);
                    DebugAssert(SUCCEEDED(hResult));

                    // skip over unused variables
                    if (!(variableDesc.uFlags & D3D_SVF_USED))
                        continue;

                    // have we already done this parameter?
                    uint32 outParameterIndex;
                    for (outParameterIndex = 0; outParameterIndex < outParameters.GetSize(); outParameterIndex++)
                    {
                        if (Y_strcmp(outParameters[outParameterIndex].Name, variableDesc.Name) == 0)
                            break;
                    }

                    // skip already-done parameters
                    if (outParameterIndex != outParameters.GetSize())
                        continue;

                    // handle structs
                    SHADER_PARAMETER_TYPE parameterType;
                    uint32 uniformSize;
                    if (typeDesc.Class == D3D_SVC_STRUCT)
                    {
                        // columns == size in floats?
                        parameterType = SHADER_PARAMETER_TYPE_STRUCT;
                        uniformSize = typeDesc.Columns * sizeof(float);
                    }
                    else
                    {
                        // determine type
                        if (!MapD3D11ShaderTypeDescToParameterType(&typeDesc, &parameterType))
                        {
                            Log_ErrorPrintf("ReflectShader: Failed to map variable type");
                            return false;
                        }

                        // get type size
                        uniformSize = ShaderParameterValueTypeSize(parameterType);
                    }

                    // determine array size
                    uint32 uniformArraySize = Max((uint32)1, (uint32)typeDesc.Elements);
                    uint32 uniformArrayStride = uniformSize;
                    if ((uniformArrayStride * uniformArraySize) != variableDesc.Size)
                    {
                        // Basically, every row gets aligned to a float4.
                        // However it seems that the last element is not padded.
                        switch (parameterType)
                        {
                        case SHADER_PARAMETER_TYPE_BOOL:
                            uniformArraySize = sizeof(BOOL) * 4;
                            break;

                        case SHADER_PARAMETER_TYPE_INT:
                        case SHADER_PARAMETER_TYPE_INT2:
                        case SHADER_PARAMETER_TYPE_INT3:
                            uniformArrayStride = sizeof(int32) * 4;
                            break;

                        case SHADER_PARAMETER_TYPE_FLOAT:
                        case SHADER_PARAMETER_TYPE_FLOAT2:
                        case SHADER_PARAMETER_TYPE_FLOAT3:
                            uniformArrayStride = sizeof(float) * 4;
                            break;

                        default:
                            UnreachableCode();
                            break;
                        }

                        // should've fixed it
                        DebugAssert((uniformArrayStride * (uniformArraySize - 1) + ShaderParameterValueTypeSize(parameterType)) == variableDesc.Size);
                    }

                    // create parameter
                    D3DShaderCacheEntryParameter paramDecl;
                    Y_strncpy(paramDecl.Name, countof(paramDecl.Name), variableDesc.Name);
                    paramDecl.Type = parameterType;
                    paramDecl.ConstantBufferIndex = outConstantBufferIndex;
                    paramDecl.ConstantBufferOffset = variableDesc.StartOffset;
                    paramDecl.ArraySize = uniformArraySize;
                    paramDecl.ArrayStride = uniformArrayStride;
                    paramDecl.BindTarget = D3D_SHADER_BIND_TARGET_COUNT;
                    for (uint32 i = 0; i < SHADER_PROGRAM_STAGE_COUNT; i++)
                        paramDecl.BindPoint[i] = -1;
                    paramDecl.LinkedSamplerIndex = -1;
                    outParameters.Add(paramDecl);
                }
            }
        }
    }

    // parse samplers/resources
    if (shaderDesc.BoundResources > 0)
    {
        // work out number of samplers/resources
        for (uint32 resourceIndex = 0; resourceIndex < shaderDesc.BoundResources; resourceIndex++)
        {
            D3D11_SHADER_INPUT_BIND_DESC inputBindDesc;
            hResult = pReflection->GetResourceBindingDesc(resourceIndex, &inputBindDesc);
            DebugAssert(SUCCEEDED(hResult));

            // map to parameter info
            SHADER_PARAMETER_TYPE parameterType;
            D3D_SHADER_BIND_TARGET bindTarget;
            if (!MapD3D11ShaderInputBindDescToParameterType(&inputBindDesc, &parameterType, &bindTarget))
            {
                Log_ErrorPrintf("ReflectShader: Failed to map resource input type");
                return false;
            }

            // parameter with this name exists?
            uint32 outParameterIndex;
            for (outParameterIndex = 0; outParameterIndex < outParameters.GetSize(); outParameterIndex++)
            {
                if (Y_strcmp(outParameters[outParameterIndex].Name, inputBindDesc.Name) == 0)
                    break;
            }

            // if not, create it
            if (outParameterIndex == outParameters.GetSize())
            {
                D3DShaderCacheEntryParameter paramDecl;
                Y_strncpy(paramDecl.Name, countof(paramDecl.Name), inputBindDesc.Name);
                paramDecl.Type = parameterType;
                paramDecl.ConstantBufferIndex = -1;
                paramDecl.ConstantBufferOffset = 0;
                paramDecl.ArraySize = 0;
                paramDecl.ArrayStride = 0;
                paramDecl.BindTarget = bindTarget;
                for (uint32 i = 0; i < SHADER_PROGRAM_STAGE_COUNT; i++)
                    paramDecl.BindPoint[i] = -1;
                paramDecl.LinkedSamplerIndex = -1;
                outParameters.Add(paramDecl);
            }

            // it would be very concerning if these didn't match... aliased variables with different types
            DebugAssert(outParameters[outParameterIndex].Type == parameterType);
            DebugAssert(outParameters[outParameterIndex].BindTarget == (uint32)bindTarget);

            // set the bind point
            DebugAssert(outParameters[outParameterIndex].BindPoint[stage] == -1);
            outParameters[outParameterIndex].BindPoint[stage] = inputBindDesc.BindPoint;
        }
    }

    return true;
}

static bool LinkResourceSamplerParameters(MemArray<D3DShaderCacheEntryParameter> &parameters)
{
    SmallString samplerParameterName;

    // find any resources with a matching _SamplerState suffix sampler
    for (uint32 parameterIndex = 0; parameterIndex < parameters.GetSize(); parameterIndex++)
    {
        D3DShaderCacheEntryParameter *parameter = &parameters[parameterIndex];
        if (parameter->BindTarget == D3D_SHADER_BIND_TARGET_RESOURCE && 
            parameter->Type >= SHADER_PARAMETER_TYPE_TEXTURE1D && parameter->Type <= SHADER_PARAMETER_TYPE_TEXTURECUBEARRAY)
        {
            samplerParameterName.Clear();
            samplerParameterName.AppendString(parameters[parameterIndex].Name);
            samplerParameterName.AppendString("_SamplerState");

            for (uint32 samplerParameterIndex = 0; samplerParameterIndex < parameters.GetSize(); samplerParameterIndex++)
            {
                if (samplerParameterIndex == parameterIndex)
                    continue;

                const D3DShaderCacheEntryParameter *samplerParameter = &parameters[samplerParameterIndex];
                if (samplerParameter->Type == SHADER_PARAMETER_TYPE_SAMPLER_STATE && samplerParameterName.Compare(samplerParameter->Name))
                {
                    // found a match
                    parameters[parameterIndex].LinkedSamplerIndex = samplerParameterIndex;
                    Log_DevPrintf("LinkResourceSamplerParameters: Linked '%s' to '%s'", parameter->Name, samplerParameter->Name);
                    break;
                }
            }
        }
    }

    return true;
}

static bool LinkLocalConstantBuffersToParameters(MemArray<D3DShaderCacheEntryConstantBuffer> &constantBuffers, const MemArray<D3DShaderCacheEntryParameter> &parameters)
{
    // match the constant buffers to parameters
    for (uint32 constantBufferIndex = 0; constantBufferIndex < constantBuffers.GetSize();)
    {
        D3DShaderCacheEntryConstantBuffer *constantBuffer = &constantBuffers[constantBufferIndex];

        uint32 parameterIndex;
        for (parameterIndex = 0; parameterIndex < parameters.GetSize(); parameterIndex++)
        {
            const D3DShaderCacheEntryParameter *parameter = &parameters[parameterIndex];
            if (parameter->Type == SHADER_PARAMETER_TYPE_CONSTANT_BUFFER && Y_strcmp(constantBuffer->Name, parameter->Name) == 0)
                break;
        }

        if (parameterIndex == parameters.GetSize())
        {
            Log_WarningPrintf("LinkLocalConstantBuffersToParameters: Constant buffer '%s' does not map to a parameter, dropping it.", constantBuffer->Name);
            constantBuffers.OrderedRemove(constantBufferIndex);
            continue;
        }

        // map it
        constantBuffer->ParameterIndex = parameterIndex;
        constantBufferIndex++;
    }

    return true;
}


bool ShaderCompilerD3D::InternalCompile(ByteStream *pByteCodeStream, ByteStream *pInfoLogStream)
{
    MemArray<D3DShaderCacheEntryVertexAttribute> vertexAttributes;
    MemArray<D3DShaderCacheEntryConstantBuffer> constantBuffers;
    MemArray<D3DShaderCacheEntryParameter> parameters;
    SmallString tempString;
    Timer compileTimer, stageCompileTimer;
    float compileTimeCompile, compileTimeReflect;
    bool result = false;

    // Create code arrays.
    byte *shaderByteCode[SHADER_PROGRAM_STAGE_COUNT];
    uint32 shaderByteCodeSize[SHADER_PROGRAM_STAGE_COUNT];
    Y_memzero(shaderByteCode, sizeof(shaderByteCode));
    Y_memzero(shaderByteCodeSize, sizeof(shaderByteCodeSize));
    stageCompileTimer.Reset();

    // compile stages
    for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
    {
        if (!m_StageEntryPoints[stageIndex].IsEmpty() && !CompileShaderStage((SHADER_PROGRAM_STAGE)stageIndex, &shaderByteCode[stageIndex], &shaderByteCodeSize[stageIndex]))
        {
            Log_ErrorPrintf("Failed to compile stage %u.", stageIndex);
            goto CLEANUP;
        }
    }
    compileTimeCompile = (float)stageCompileTimer.GetTimeMilliseconds();
    stageCompileTimer.Reset();

    // Reflect each present shader stage.
    for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
    {
        if (shaderByteCode[stageIndex] != nullptr && !ReflectShader((SHADER_PROGRAM_STAGE)stageIndex, vertexAttributes, constantBuffers, parameters, shaderByteCode[stageIndex], shaderByteCodeSize[stageIndex]))
        {
            Log_ErrorPrintf("Failed to reflect stage %u.", stageIndex);
            goto CLEANUP;
        }
    }

    // Link sampler states to resources
    if (!LinkResourceSamplerParameters(parameters))
        goto CLEANUP;

    // Link local constant buffers to parameters
    if (!LinkLocalConstantBuffersToParameters(constantBuffers, parameters))
        goto CLEANUP;

#if 1
    // Dump out variables
    for (uint32 constantBufferIndex = 0; constantBufferIndex < constantBuffers.GetSize(); constantBufferIndex++)
        Log_DevPrintf("Shader Constant Buffer [%u] : %s, %u bytes, local: %s", constantBufferIndex, constantBuffers[constantBufferIndex].Name, constantBuffers[constantBufferIndex].Size, (constantBuffers[constantBufferIndex].IsLocal) ? "yes" : "no");
    for (uint32 parameterIndex = 0; parameterIndex < parameters.GetSize(); parameterIndex++)
        Log_DevPrintf("Shader Parameter [%u] : %s, type %s", parameterIndex, parameters[parameterIndex].Name, NameTable_GetNameString(NameTables::ShaderParameterType, parameters[parameterIndex].Type));
#endif

    // Create the header of the shader data.
    // The zeros on the strings prevent stack memory getting dropped into the output.
    D3DShaderCacheEntryHeader cacheEntryHeader;
    Y_memzero(&cacheEntryHeader, sizeof(cacheEntryHeader));
    cacheEntryHeader.Signature = D3D_SHADER_CACHE_ENTRY_HEADER;
    cacheEntryHeader.FeatureLevel = m_eRendererFeatureLevel;

    // Store shader stage sizes.
    for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
        cacheEntryHeader.StageSize[stageIndex] = shaderByteCodeSize[stageIndex];

    // Store counts.
    cacheEntryHeader.VertexAttributeCount = vertexAttributes.GetSize();
    cacheEntryHeader.ConstantBufferCount = constantBuffers.GetSize();
    cacheEntryHeader.ParameterCount = parameters.GetSize();

    // Write header.
    pByteCodeStream->Write(&cacheEntryHeader, sizeof(cacheEntryHeader));

    // Write each of the shader stage's bytecodes out.
    for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
    {
        if (shaderByteCode[stageIndex] != nullptr)
        {
            // write stage bytecode
            pByteCodeStream->Write(shaderByteCode[stageIndex], shaderByteCodeSize[stageIndex]);
        }
    }

    // Write out declarations.
    if (vertexAttributes.GetSize() > 0)
        pByteCodeStream->Write(vertexAttributes.GetBasePointer(), sizeof(D3DShaderCacheEntryVertexAttribute) * vertexAttributes.GetSize());
    if (constantBuffers.GetSize() > 0)
        pByteCodeStream->Write(constantBuffers.GetBasePointer(), sizeof(D3DShaderCacheEntryConstantBuffer) * constantBuffers.GetSize());
    if (parameters.GetSize() > 0)
        pByteCodeStream->Write(parameters.GetBasePointer(), sizeof(D3DShaderCacheEntryParameter) * parameters.GetSize());

    // update timer
    compileTimeReflect = (float)stageCompileTimer.GetTimeMilliseconds();

    // Compile succeded.
    result = true;
    Log_DevPrintf("Shader successfully compiled. Took %.3f msec (Compile: %.3f msec, Reflect+Write: %.3f msec)",
        (float)compileTimer.GetTimeMilliseconds(), compileTimeCompile, compileTimeReflect);

CLEANUP:
    for (uint32 stageIndex = 0; stageIndex < SHADER_PROGRAM_STAGE_COUNT; stageIndex++)
    {
        if (shaderByteCode[stageIndex] != nullptr)
            Y_free(shaderByteCode[stageIndex]);
    }

    return result;
}

ShaderCompiler *ShaderCompiler::CreateD3D11ShaderCompiler(ResourceCompilerCallbacks *pCallbacks, const ShaderCompilerParameters *pParameters)
{
    return new ShaderCompilerD3D(pCallbacks, pParameters);
}

#endif      // Y_PLATFORM_WINDOWS
