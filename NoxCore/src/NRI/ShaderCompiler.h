#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <functional>

namespace NRI
{
    enum class GraphicsAPI : uint8_t;

    // Startup progress (the renderer's splash while shaders compile): who is told when a source really compiles (a cached binary is
    // not a compile; any thread: pipelines build on workers), when a pipeline is done (main thread), and that the main thread is
    // waiting for the workers (OnIdle, every few tens of ms: draw, answer the OS). Null outside the renderer's start.
    struct ShaderProgress
    {
        std::function<void(const std::string& source)> OnCompile;
        std::function<void()> OnPipelineCreated;
        std::function<void()> OnIdle;
    };
    void SetShaderProgress(ShaderProgress* progress);
    ShaderProgress* GetShaderProgress();

    class ShaderCompiler
    {
    public:
        virtual ~ShaderCompiler() = default;
        
        virtual std::vector<char> compile(const std::string& path) = 0;
        // Hash of the shader source and every file it includes, so a cached binary is only reused while all of them match.
        virtual uint64_t sourceHash(const std::string& path) = 0;
        // Whether compile() can run here: false in a packaged game shipped without the compiler (precompiled shaders only).
        virtual bool canCompile() = 0;
        virtual void setTargetAPI(GraphicsAPI api) {}
        virtual GraphicsAPI getTargetAPI() const { return static_cast<GraphicsAPI>(0); }
    };
    
    std::unique_ptr<ShaderCompiler> CreateSlangCompiler();
}
