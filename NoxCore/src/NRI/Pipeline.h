#pragma once
#include <string>
#include <vector>

namespace NRI
{
    enum class ImageFormat;

    enum class PipelineType { Graphics, Compute, RayTracing };
    enum class ShaderStage { Vertex, Fragment, Compute, Task, Mesh, RayGen, Miss, ClosestHit, AnyHit, Intersection, Callable };

    struct ShaderStageDesc
    {
        ShaderStage stage;
        std::string entryPoint;
        std::string sourcePath;
    };

    struct RayTracingHitGroupDesc
    {
        uint32_t closestHitShaderIndex = ~0u; // index into PipelineDesc::shaders, or ~0u
        uint32_t anyHitShaderIndex = ~0u;     // index into PipelineDesc::shaders, or ~0u
        uint32_t intersectionShaderIndex = ~0u; // index into PipelineDesc::shaders, or ~0u
    };

    struct PipelineDesc
    {
        PipelineType type = PipelineType::Graphics; // default to graphics
        std::vector<ShaderStageDesc> shaders;
        
        std::vector<ImageFormat> colorFormats;
        std::vector<RayTracingHitGroupDesc> hitGroups;
        uint32_t maxRecursionDepth = 1;
        
        bool forceCompile = false;
    };

    class Pipeline
    {
    public:
        virtual ~Pipeline() = default;
        virtual const std::vector<uint8_t>& getShaderGroupHandles() const { static const std::vector<uint8_t> empty; return empty; }
        virtual uint32_t getShaderGroupHandleSize() const { return 0; }
        virtual uint32_t getShaderGroupBaseAlignment() const { return 0; }
        virtual uint32_t getRayGenShaderGroupIndex() const { return 0; }
        virtual const std::vector<uint32_t>& getMissShaderGroupIndices() const { static const std::vector<uint32_t> empty; return empty; }
        virtual const std::vector<uint32_t>& getHitShaderGroupIndices() const { static const std::vector<uint32_t> empty; return empty; }
        virtual const std::vector<uint32_t>& getCallableShaderGroupIndices() const { static const std::vector<uint32_t> empty; return empty; }
    };
}
