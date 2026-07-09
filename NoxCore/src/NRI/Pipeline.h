#pragma once
#include <string>
#include <vector>

namespace NRI
{
    enum class PipelineType { Graphics, Compute };
    enum class ShaderStage { Vertex, Fragment, Compute };

    struct ShaderStageDesc
    {
        ShaderStage stage;
        std::string entryPoint;
        std::vector<char> bytecode; // The compiled SPV data
    };

    struct PipelineDesc
    {
        PipelineType type = PipelineType::Graphics; // default to graphics
        std::vector<ShaderStageDesc> shaders;   
    };

    class Pipeline
    {
    public:
        virtual ~Pipeline() = default;
    };
}
