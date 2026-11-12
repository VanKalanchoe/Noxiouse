#pragma once

#include <atomic>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>

#include "VulkanCommon.h"
#include "../Pipeline.h"

namespace NRI
{
    class ShaderCompiler;
    class DeviceVK; // Forward declaration
    
    class PipelineVK final : public Pipeline
    {
    public:
        // A shader-object pipeline is built on worker threads (the start-up's ~200 pipelines at once, compiling sources that have no
        // cached binary there too); every accessor waits for it to be done. FinishPendingBuilds waits for all of them (end of the
        // renderer's start-up), drawing the compile progress meanwhile.
        PipelineVK(DeviceVK& device, const PipelineDesc& desc, ShaderCompiler& compiler);
        ~PipelineVK() override;

        struct BuildStats
        {
            uint32_t Parallel = 0; // built on the workers from cached binaries
            uint32_t Compiled = 0; // built on the workers, compiling at least one source
            uint32_t Failed = 0;
        };
        static BuildStats FinishPendingBuilds();
        // Built on the pool (FinishPendingBuilds reports it to the progress as created), or here.
        bool isBuiltOnPool() const;
        
        bool isShaderObject() const { ensureBuilt(); return !m_shaders.empty(); }
        const vk::raii::Pipeline& getNativePipeline() const { ensureBuilt(); return m_pipeline; }
        
        const std::vector<vk::raii::ShaderEXT>& getShaders() const { ensureBuilt(); return m_shaders; }
        const std::vector<vk::ShaderStageFlagBits>& getStages() const { ensureBuilt(); return m_stages; }
        const std::vector<vk::ShaderEXT>& getRawShaders() const { ensureBuilt(); return m_rawShaders; }
        
        // Ray Tracing Pipeline accessors
        const std::vector<uint8_t>& getShaderGroupHandles() const override { ensureBuilt(); return m_shaderGroupHandles; }
        uint32_t getShaderGroupHandleSize() const override { ensureBuilt(); return m_shaderGroupHandleSize; }
        uint32_t getShaderGroupBaseAlignment() const override { ensureBuilt(); return m_shaderGroupBaseAlignment; }
        uint32_t getRayGenShaderGroupIndex() const override { ensureBuilt(); return m_rayGenShaderGroupIndex; }
        const std::vector<uint32_t>& getMissShaderGroupIndices() const override { ensureBuilt(); return m_missShaderGroupIndices; }
        const std::vector<uint32_t>& getHitShaderGroupIndices() const override { ensureBuilt(); return m_hitShaderGroupIndices; }
        const std::vector<uint32_t>& getCallableShaderGroupIndices() const override { ensureBuilt(); return m_callableShaderGroupIndices; }
        
    private:
        void build(const PipelineDesc& desc, ShaderCompiler& compiler);
        void ensureBuilt() const;
        // A stage's SPIR-V: its cached file, else the shader compiler (each source once, compileModuleOnce).
        std::vector<char> compileShader(const ShaderStageDesc& shaderDesc, ShaderCompiler& compiler, bool forceCompile);

        struct AsyncBuild
        {
            PipelineDesc Desc; // a copy: the caller's goes away
            ShaderCompiler* Compiler = nullptr;
            std::mutex Mutex;
            std::condition_variable Finished;
            bool Done = false;
            bool Compiled = false; // the build compiled a source (BuildStats)
            std::exception_ptr Error;
        };
        std::shared_ptr<AsyncBuild> m_async;
        mutable std::atomic<bool> m_ready{ false };

        [[nodiscard]] vk::raii::ShaderModule createShaderModule(const std::vector<char>& code) const;
        vk::ShaderStageFlagBits translateShaderStage(ShaderStage stage);
        vk::ShaderStageFlagBits determineNextStage(ShaderStage stage);
        vk::Format translateImageFormat(ImageFormat format);

    private:
        DeviceVK& m_deviceVK;
        
        // Monolithic path
        vk::raii::Pipeline m_pipeline = nullptr;
        
        // Shader Object path
        std::vector<vk::raii::ShaderEXT> m_shaders;
        std::vector<vk::ShaderStageFlagBits> m_stages;
        std::vector<vk::ShaderEXT> m_rawShaders;

        // Ray Tracing Pipeline path
        std::vector<uint8_t> m_shaderGroupHandles;
        uint32_t m_shaderGroupHandleSize = 0;
        uint32_t m_shaderGroupBaseAlignment = 0;
        uint32_t m_rayGenShaderGroupIndex = 0;
        std::vector<uint32_t> m_missShaderGroupIndices;
        std::vector<uint32_t> m_hitShaderGroupIndices;
        std::vector<uint32_t> m_callableShaderGroupIndices;
    };
}