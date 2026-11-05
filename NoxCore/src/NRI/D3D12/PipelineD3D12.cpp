#include "PipelineD3D12.h"
#include "DeviceD3D12.h"
#include "TextureD3D12.h"
#include "../ShaderCompiler.h"

namespace NRI
{
    PipelineD3D12::PipelineD3D12(DeviceD3D12& device, const PipelineDesc& desc, ShaderCompiler& compiler)
        : m_deviceD3D12(device), m_type(desc.type)
    {
        switch (desc.type)
        {
        case PipelineType::Graphics:
            createGraphicsPipeline(desc, compiler);
            break;
        case PipelineType::Compute:
            createComputePipeline(desc, compiler);
            break;
        case PipelineType::RayTracing:
            createRayTracingPipeline(desc, compiler);
            break;
        }
    }

    void PipelineD3D12::createGraphicsPipeline(const PipelineDesc& desc, ShaderCompiler& compiler)
    {
        std::vector<char> vsBytecode, psBytecode, asBytecode, msBytecode;
        bool hasMeshShaders = false;

        for (const auto& s : desc.shaders)
        {
            auto code = compiler.compile(s.sourcePath);
            if (s.stage == ShaderStage::Vertex) vsBytecode = std::move(code);
            else if (s.stage == ShaderStage::Fragment) psBytecode = std::move(code);
            else if (s.stage == ShaderStage::Task) { asBytecode = std::move(code); hasMeshShaders = true; }
            else if (s.stage == ShaderStage::Mesh) { msBytecode = std::move(code); hasMeshShaders = true; }
        }

        if (hasMeshShaders)
        {
            struct alignas(void*) MeshPipelineStateStream
            {
                struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE }; ID3D12RootSignature* pRootSignature{ nullptr }; } RootSig;
                struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS }; D3D12_SHADER_BYTECODE bytecode{}; } AS;
                struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS }; D3D12_SHADER_BYTECODE bytecode{}; } MS;
                struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS }; D3D12_SHADER_BYTECODE bytecode{}; } PS;
                struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER }; D3D12_RASTERIZER_DESC desc{}; } Rasterizer;
                struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL }; D3D12_DEPTH_STENCIL_DESC desc{}; } DepthStencil;
                struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS }; D3D12_RT_FORMAT_ARRAY rts{}; } RTVFormats;
                struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT }; DXGI_FORMAT format{}; } DSVFormat;
            } stream;

            stream.RootSig.pRootSignature = m_deviceD3D12.getGlobalRootSignature();
            if (!asBytecode.empty())
                stream.AS.bytecode = { asBytecode.data(), asBytecode.size() };
            stream.MS.bytecode = { msBytecode.data(), msBytecode.size() };
            if (!psBytecode.empty())
                stream.PS.bytecode = { psBytecode.data(), psBytecode.size() };

            stream.Rasterizer.desc.FillMode = D3D12_FILL_MODE_SOLID;
            stream.Rasterizer.desc.CullMode = D3D12_CULL_MODE_BACK;
            stream.Rasterizer.desc.DepthClipEnable = TRUE;

            stream.DepthStencil.desc.DepthEnable = TRUE;
            stream.DepthStencil.desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            stream.DepthStencil.desc.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL; // Reverse-Z

            stream.RTVFormats.rts.NumRenderTargets = static_cast<UINT>(desc.colorFormats.size());
            for (size_t i = 0; i < desc.colorFormats.size(); ++i)
                stream.RTVFormats.rts.RTFormats[i] = TranslateImageFormat(desc.colorFormats[i]);
            stream.DSVFormat.format = DXGI_FORMAT_D32_FLOAT;

            D3D12_PIPELINE_STATE_STREAM_DESC streamDesc{ sizeof(stream), &stream };
            ComPtr<ID3D12Device2> dev2;
            if (SUCCEEDED(m_deviceD3D12.getNativeDevice()->QueryInterface(IID_PPV_ARGS(&dev2))))
            {
                ThrowIfFailed(dev2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&m_pipeline)), "CreatePipelineState (Mesh)");
            }
        }
        else
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
            psoDesc.pRootSignature = m_deviceD3D12.getGlobalRootSignature();
            if (!vsBytecode.empty())
                psoDesc.VS = { vsBytecode.data(), vsBytecode.size() };
            if (!psBytecode.empty())
                psoDesc.PS = { psBytecode.data(), psBytecode.size() };

            psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
            psoDesc.RasterizerState.DepthClipEnable = TRUE;

            psoDesc.DepthStencilState.DepthEnable = TRUE;
            psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;

            psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            psoDesc.SampleMask = UINT_MAX;
            psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            psoDesc.NumRenderTargets = static_cast<UINT>(desc.colorFormats.size());
            for (size_t i = 0; i < desc.colorFormats.size(); ++i)
                psoDesc.RTVFormats[i] = TranslateImageFormat(desc.colorFormats[i]);
            psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
            psoDesc.SampleDesc.Count = 1;

            ThrowIfFailed(m_deviceD3D12.getNativeDevice()->CreateGraphicsPipelineState(
                &psoDesc, IID_PPV_ARGS(&m_pipeline)), "CreateGraphicsPipelineState");
        }
    }

    void PipelineD3D12::createComputePipeline(const PipelineDesc& desc, ShaderCompiler& compiler)
    {
        std::vector<char> csBytecode;
        for (const auto& s : desc.shaders)
        {
            if (s.stage == ShaderStage::Compute)
            {
                csBytecode = compiler.compile(s.sourcePath);
                break;
            }
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = m_deviceD3D12.getGlobalRootSignature();
        psoDesc.CS = { csBytecode.data(), csBytecode.size() };

        ThrowIfFailed(m_deviceD3D12.getNativeDevice()->CreateComputePipelineState(
            &psoDesc, IID_PPV_ARGS(&m_pipeline)), "CreateComputePipelineState");
    }

    void PipelineD3D12::createRayTracingPipeline(const PipelineDesc& desc, ShaderCompiler& compiler)
    {
        // Setup DXR state object
        std::vector<D3D12_STATE_SUBOBJECT> subobjects;

        // Global root signature
        D3D12_GLOBAL_ROOT_SIGNATURE globalRootSigDesc{};
        globalRootSigDesc.pGlobalRootSignature = m_deviceD3D12.getGlobalRootSignature();
        D3D12_STATE_SUBOBJECT globalRootSigSubobject{};
        globalRootSigSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
        globalRootSigSubobject.pDesc = &globalRootSigDesc;
        subobjects.push_back(globalRootSigSubobject);

        // Raytracing pipeline config
        D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{};
        pipelineConfig.MaxTraceRecursionDepth = desc.maxRecursionDepth;
        D3D12_STATE_SUBOBJECT configSubobject{};
        configSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
        configSubobject.pDesc = &pipelineConfig;
        subobjects.push_back(configSubobject);

        D3D12_STATE_OBJECT_DESC stateObjectDesc{};
        stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
        stateObjectDesc.NumSubobjects = static_cast<UINT>(subobjects.size());
        stateObjectDesc.pSubobjects = subobjects.data();

        ThrowIfFailed(m_deviceD3D12.getNativeDevice()->CreateStateObject(
            &stateObjectDesc, IID_PPV_ARGS(&m_stateObject)), "CreateStateObject (RTPSO)");

        m_shaderGroupHandles.resize(D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
    }
}
