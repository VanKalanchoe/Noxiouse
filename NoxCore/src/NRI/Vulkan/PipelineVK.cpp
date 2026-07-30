#include "PipelineVK.h"

#include <fstream>
#include <iostream>
#include <SDL3/SDL_filesystem.h>

#include "DeviceVK.h"
#include "NoxCore/Core/core.h"
#include "NoxCore/Core/Hash.h"
#include "NoxCore/Core/Log.h"

namespace NRI
{
    static std::filesystem::path shaderFolder()
    {
        const char* basePath = SDL_GetBasePath();

        if (!basePath)
            throw std::runtime_error(SDL_GetError());

        std::filesystem::path path = std::filesystem::path(basePath) / "shaders";

        std::filesystem::create_directories(path);

        return path;
    }

    static std::filesystem::path shaderBinaryPath(const ShaderStageDesc& shaderDesc)
    {
        std::filesystem::path source(shaderDesc.sourcePath);
     
        if (!std::filesystem::exists(source)) NOX_CORE_ERROR("PipelineVK::shaderBinaryPath file not found: {}", source);
        
        auto folder = shaderFolder() / source.stem();
        std::filesystem::create_directories(folder);

        std::string stage;
        switch (shaderDesc.stage)
        {
        case ShaderStage::Vertex:
            stage = "vert";
            break;

        case ShaderStage::Fragment:
            stage = "frag";
            break;
            
        case ShaderStage::Compute:
            stage = "comp";
            break;
            
        case ShaderStage::Task:
            stage = "task";
            break;
            
        case ShaderStage::Mesh:
            stage = "mesh";
            break;

        default:
            stage = "unknown";
            break;
        }
        
        uint64_t sourceHash = Nox::Hash::computeFile(shaderDesc.sourcePath);
        
        return folder / (source.stem().string() + "." + stage + "." + std::to_string(sourceHash) +  ".bin");
    }

    static std::vector<uint8_t> loadShaderBinary(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);

        if (!file)
            throw std::runtime_error("Failed to open shader binary");

        size_t size = file.tellg();
        file.seekg(0);

        std::vector<uint8_t> data(size);

        file.read(
            reinterpret_cast<char*>(data.data()),
            size
        );

        return data;
    }
    
    PipelineVK::PipelineVK(DeviceVK& device, const PipelineDesc& desc, ShaderCompiler& compiler) : m_deviceVK(device)
    {
        std::vector<std::vector<char>> tempBytecodeStorage(desc.shaders.size());
        bool useShaderObjects = m_deviceVK.isShaderObjectExtensionEnabled();

        if (desc.type == PipelineType::Graphics)
        {
            // Verify we have shader
            if (desc.shaders.empty())
            {
                NOX_CORE_ASSERT("PipelineVK requires a shader stage");
            }

            if (useShaderObjects)
            {
                std::vector<vk::ShaderStageFlagBits> m_stages;
                // need to set all stages to VK_NULL_HANDLE 
                // because shaderbojects requires all stages even if not used by this pipeline
                
                const vk::ShaderCreateFlagsEXT commonFlags = vk::ShaderCreateFlagBitsEXT::eDescriptorHeap;

                std::vector<std::string> entryPointNames;
                std::vector<std::vector<uint8_t>> binaryStorage;

                std::vector<bool> loadedFromBinary;

                entryPointNames.reserve(desc.shaders.size());
                binaryStorage.reserve(desc.shaders.size());
                loadedFromBinary.reserve(desc.shaders.size());

                // Load cache or compile
                for (size_t i = 0; i < desc.shaders.size(); i++)
                {
                    auto& shaderDesc = desc.shaders[i];
                    
                    entryPointNames.push_back(shaderDesc.entryPoint);
                    m_stages.push_back( translateShaderStage(shaderDesc.stage));

                    auto binaryPath = shaderBinaryPath(shaderDesc);

                    if (std::filesystem::exists(binaryPath) && !desc.forceCompile)
                    {
                        NOX_CORE_INFO("PipelineVK loading shader binary: {}", binaryPath);

                        binaryStorage.push_back(
                            loadShaderBinary(binaryPath)
                        );

                        loadedFromBinary.push_back(true);
                    }
                    else
                    {
                        NOX_CORE_INFO("PipelineVK compiling shader: {}", shaderDesc.sourcePath);

                        tempBytecodeStorage[i] = compiler.compile(shaderDesc.sourcePath);

                        loadedFromBinary.push_back(false);
                    }
                }
                
                std::vector<vk::ShaderCreateInfoEXT> shaderCreateInfos;
                shaderCreateInfos.reserve(desc.shaders.size());

                size_t binaryIndex = 0;

                for (size_t i = 0; i < desc.shaders.size(); i++)
                {
                    auto& shaderDesc = desc.shaders[i];

                    vk::ShaderCreateInfoEXT info
                    {
                        .flags = commonFlags,
                        .stage = translateShaderStage(shaderDesc.stage),
                        .nextStage = determineNextStage(shaderDesc.stage),
                        .pName = entryPointNames[i].c_str(),
                        .setLayoutCount = 0,
                        .pSetLayouts = nullptr,
                        .pushConstantRangeCount = 0,
                        .pPushConstantRanges = nullptr,
                        .pSpecializationInfo = nullptr,
                    };

                    if (loadedFromBinary[i])
                    {
                        auto& binary = binaryStorage[binaryIndex++];

                        info.codeType = vk::ShaderCodeTypeEXT::eBinary;
                        info.codeSize = binary.size();
                        info.pCode = binary.data();
                    }
                    else
                    {
                        auto& spirv = tempBytecodeStorage[i];

                        info.codeType = vk::ShaderCodeTypeEXT::eSpirv;
                        info.codeSize = spirv.size();
                        info.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
                    }

                    shaderCreateInfos.push_back(info);
                }

                m_shaders = m_deviceVK.getDevice().createShadersEXT(shaderCreateInfos);
                
                if (m_shaders.empty())
                {
                    NOX_CORE_WARN("PipelineVK::PipelineVK Shader binaries incompatible (likely driver update). Falling back to SPIR-V");
                    
                    for (size_t i = 0; i < desc.shaders.size(); i++)
                    {
                        if (loadedFromBinary[i])
                        {
                            NOX_CORE_INFO("PipelineVK:PipelineVk Recompiling invalidated shader: {}", desc.shaders[i].sourcePath);
                            
                            tempBytecodeStorage[i] = compiler.compile(desc.shaders[i].sourcePath);
                            
                            shaderCreateInfos[i].codeType = vk::ShaderCodeTypeEXT::eSpirv;
                            shaderCreateInfos[i].codeSize = tempBytecodeStorage[i].size();
                            shaderCreateInfos[i].pCode = reinterpret_cast<const uint32_t*>(tempBytecodeStorage[i].data());
                            
                            loadedFromBinary[i] = false;
                        }
                        else
                        {
                            shaderCreateInfos[i].pCode = reinterpret_cast<const uint32_t*>(tempBytecodeStorage[i].data());
                        }
                    }
                }
                
                m_shaders = m_deviceVK.getDevice().createShadersEXT(shaderCreateInfos);
                
                if (m_shaders.empty())
                {
                    NOX_CORE_ASSERT("PipelineVK::PipelineVK Failed to create Binary and SPIR-V shaders");
                }
                
                m_rawShaders.assign(m_allGraphicsStages.size(), vk::ShaderEXT{});
                for (size_t i = 0; i < m_stages.size(); i++)
                {
                    auto it = std::find(m_allGraphicsStages.begin(), m_allGraphicsStages.end(), m_stages[i]);
                    size_t slot = std::distance(m_allGraphicsStages.begin(), it);
                    m_rawShaders[slot] = *m_shaders[i];
                }

                for (size_t i = 0; i < desc.shaders.size(); i++)
                {
                    if (!loadedFromBinary[i])
                    {
                        auto path = shaderBinaryPath(desc.shaders[i]);

                        NOX_CORE_INFO("PipelineVK Saving shader binary: {}", path);

                        auto data = m_shaders[i].getBinaryData();

                        std::ofstream file(path, std::ios::binary);

                        file.write(
                            reinterpret_cast<const char*>(data.data()),
                            data.size()
                        );
                    }
                }
            }
            else
            {
                // 1. Convert abstract shaders descriptors into Vulkan handles
                std::vector<vk::PipelineShaderStageCreateInfo> shaderStages;
                std::vector<vk::raii::ShaderModule> temporaryModules; // Keeps modules alive during creation

                // Reserve space so the vector doesn't reallocate and move your RAII objects around
                temporaryModules.reserve(desc.shaders.size());
                shaderStages.reserve(desc.shaders.size());

                for (const auto& shaderDesc : desc.shaders)
                {
                    tempBytecodeStorage.emplace_back(compiler.compile(shaderDesc.sourcePath));
                    temporaryModules.push_back(createShaderModule(tempBytecodeStorage.back()));

                    auto& allocatedModule = temporaryModules.back();
                    vk::ShaderStageFlagBits stageBit = translateShaderStage(shaderDesc.stage);

                    shaderStages.push_back({
                        .stage = stageBit,
                        .module = *allocatedModule,
                        .pName = shaderDesc.entryPoint.c_str()
                    });
                }

                //vk::raii::ShaderModule shaderModule = createShaderModule(readFile("../../shaders/slang.spv"));

                /*vk::PipelineShaderStageCreateInfo vertShaderStageInfo{.stage = vk::ShaderStageFlagBits::eVertex, .module = shaderModule, .pName = "vertMain"};
                vk::PipelineShaderStageCreateInfo fragShaderStageInfo{.stage = vk::ShaderStageFlagBits::eFragment, .module = shaderModule, .pName = "fragMain"};
                vk::PipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};*/

                vk::PipelineVertexInputStateCreateInfo vertexInputInfo
                {
                    .vertexBindingDescriptionCount = 0,
                    .pVertexBindingDescriptions = nullptr,
                    .vertexAttributeDescriptionCount = 0,
                    .pVertexAttributeDescriptions = nullptr
                };
                vk::PipelineInputAssemblyStateCreateInfo inputAssembly{.topology = vk::PrimitiveTopology::eTriangleList};
                vk::PipelineViewportStateCreateInfo viewportState{.viewportCount = 0, .scissorCount = 0};

                vk::PipelineRasterizationStateCreateInfo rasterizer
                {
                    .depthClampEnable = vk::False,
                    .rasterizerDiscardEnable = vk::False,
                    .polygonMode = vk::PolygonMode::eFill,
                    .cullMode = vk::CullModeFlagBits::eBack,
                    .frontFace = vk::FrontFace::eCounterClockwise,
                    .depthBiasEnable = vk::False,
                    .lineWidth = 1.0f
                };

                vk::PipelineMultisampleStateCreateInfo multisampling{.rasterizationSamples = m_deviceVK.getMSAASamples(), .sampleShadingEnable = vk::False, .minSampleShading = .2f};

                vk::PipelineDepthStencilStateCreateInfo depthStencil{
                    .depthTestEnable = vk::True,
                    .depthWriteEnable = vk::True,
                    .depthCompareOp = vk::CompareOp::eGreater,
                    .depthBoundsTestEnable = vk::False,
                    .stencilTestEnable = vk::False
                };
                
                std::vector<vk::Format> vkColorFormats;
                vkColorFormats.reserve(desc.colorFormats.size());
                
                for (auto fmt : desc.colorFormats) vkColorFormats.push_back(translateImageFormat(fmt));
                
                vk::PipelineColorBlendAttachmentState colorBlendAttachment
                {
                    .blendEnable = vk::False,
                    .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
                };
                
                std::vector<vk::PipelineColorBlendAttachmentState> colorBlendAttachmentStates(vkColorFormats.size(), colorBlendAttachment);

                vk::PipelineColorBlendStateCreateInfo colorBlending
                {
                    .logicOpEnable = vk::False, .logicOp = vk::LogicOp::eCopy, .attachmentCount = static_cast<uint32_t>(colorBlendAttachmentStates.size()), .pAttachments = colorBlendAttachmentStates.data()
                };

                /*std::vector<vk::DynamicState> dynamicStates = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
                vk::PipelineDynamicStateCreateInfo dynamicState{.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()), .pDynamicStates = dynamicStates.data()};*/

                bool hasMeshShader = false;
                for (const auto& shaderDesc : desc.shaders)
                {
                    if (shaderDesc.stage == ShaderStage::Mesh || shaderDesc.stage == ShaderStage::Task)
                    {
                        hasMeshShader = true;
                        break;
                    }
                }

                std::vector<vk::DynamicState> dynamicStates = {
                    // Replaces the old standard Viewport/Scissor
                    vk::DynamicState::eViewportWithCount,
                    vk::DynamicState::eScissorWithCount,
                };

                if (!hasMeshShader)
                {
                    dynamicStates.push_back(vk::DynamicState::eVertexInputEXT);
                    dynamicStates.push_back(vk::DynamicState::ePrimitiveTopology);
                    dynamicStates.push_back(vk::DynamicState::ePrimitiveRestartEnable);
                }

                dynamicStates.insert(dynamicStates.end(), {
                    // Rasterization
                    vk::DynamicState::eRasterizerDiscardEnable,
                    vk::DynamicState::ePolygonModeEXT,
                    vk::DynamicState::eCullMode,
                    vk::DynamicState::eFrontFace,
                    vk::DynamicState::eDepthBiasEnable,
                    vk::DynamicState::eDepthClampEnableEXT,

                    // Multisampling
                    vk::DynamicState::eRasterizationSamplesEXT,
                    vk::DynamicState::eSampleMaskEXT,
                    vk::DynamicState::eAlphaToCoverageEnableEXT,
                    vk::DynamicState::eAlphaToOneEnableEXT,

                    // Depth / Stencil
                    vk::DynamicState::eDepthTestEnable,
                    vk::DynamicState::eDepthWriteEnable,
                    vk::DynamicState::eDepthCompareOp,
                    vk::DynamicState::eDepthBoundsTestEnable,
                    vk::DynamicState::eStencilTestEnable,

                    // Color Blending
                    vk::DynamicState::eColorBlendEnableEXT,
                    vk::DynamicState::eColorBlendEquationEXT,
                    vk::DynamicState::eColorWriteMaskEXT,
                    vk::DynamicState::eLogicOpEnableEXT
                });

                vk::PipelineDynamicStateCreateInfo dynamicState{
                    .dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
                    .pDynamicStates = dynamicStates.data()
                };

                /*vk::PipelineLayoutCreateInfo pipelineLayoutInfo{.setLayoutCount = 0, .pushConstantRangeCount = 0};
                pipelineLayout = vk::raii::PipelineLayout(device, pipelineLayoutInfo);*/

                vk::Format depthFormat = m_deviceVK.getDepthFormat();

                vk::StructureChain<vk::GraphicsPipelineCreateInfo, vk::PipelineCreateFlags2CreateInfo, vk::PipelineRenderingCreateInfo> pipelineCreateInfoChain = {
                    {
                        .stageCount = static_cast<uint32_t>(shaderStages.size()),
                        .pStages = shaderStages.data(),
                        .pVertexInputState = hasMeshShader ? nullptr : &vertexInputInfo,
                        .pInputAssemblyState = hasMeshShader ? nullptr : &inputAssembly,
                        .pViewportState = &viewportState,
                        .pRasterizationState = &rasterizer,
                        .pMultisampleState = &multisampling,
                        .pDepthStencilState = &depthStencil,
                        .pColorBlendState = &colorBlending,
                        .pDynamicState = &dynamicState,
                        /*.layout = pipelineLayout,*/
                        .layout = nullptr,
                        .renderPass = nullptr
                    },
                    // With descriptor heaps we no longer need a pipeline layout
                    // This struct must be chained into pipeline creation to enable the use of heaps (allowing us to leave pipelineLayout empty)
                    {.flags = vk::PipelineCreateFlagBits2::eDescriptorHeapEXT},
                    {.colorAttachmentCount = static_cast<uint32_t>(vkColorFormats.size()), .pColorAttachmentFormats = vkColorFormats.data(), .depthAttachmentFormat = depthFormat}
                };

                m_pipeline = vk::raii::Pipeline(m_deviceVK.getDevice(), nullptr, pipelineCreateInfoChain.get<vk::GraphicsPipelineCreateInfo>());

                m_pipeline = vk::raii::Pipeline(m_deviceVK.getDevice(), nullptr, pipelineCreateInfoChain.get<vk::GraphicsPipelineCreateInfo>());
            }
        }
        else if (desc.type == PipelineType::Compute)
        {
            /*// Verify we have exactly one compute shader
            if (desc.shaders.empty())
            {
                throw std::runtime_error("Compute pipeline requires a shader stage.");
            }

            /*vk::raii::ShaderModule shaderModule = createShaderModule(readFile("../../shaders/slang.spv"));

            vk::PipelineShaderStageCreateInfo computeShaderStageInfo{.stage = vk::ShaderStageFlagBits::eCompute, .module = shaderModule, .pName = "compMain"};#1#

            const auto& computeShaderDesc = desc.shaders[0];
            auto shaderModule = createShaderModule(computeShaderDesc.bytecode);

            vk::PipelineShaderStageCreateInfo computeShaderStageInfo
            {
                .stage = vk::ShaderStageFlagBits::eCompute,
                .module = *shaderModule,
                .pName = computeShaderDesc.entryPoint.c_str()
            };

            vk::StructureChain<vk::ComputePipelineCreateInfo, vk::PipelineCreateFlags2CreateInfo> pipelineCreateInfoChain = {
                {
                    .stage = computeShaderStageInfo,
                    .layout = nullptr
                },
                // With descriptor heaps we no longer need a pipeline layout
                // This struct must be chained into pipeline creation to enable the use of heaps (allowing us to leave pipelineLayout empty)
                {.flags = vk::PipelineCreateFlagBits2::eDescriptorHeapEXT},
            };
            m_pipeline = vk::raii::Pipeline(m_deviceVK.getDevice(), nullptr, pipelineCreateInfoChain.get<vk::ComputePipelineCreateInfo>());*/
        }
    }

    [[nodiscard]] vk::raii::ShaderModule PipelineVK::createShaderModule(const std::vector<char>& code) const
    {
        vk::ShaderModuleCreateInfo createInfo{.codeSize = code.size() * sizeof(char), .pCode = reinterpret_cast<const uint32_t*>(code.data())};
        vk::raii::ShaderModule shaderModule{m_deviceVK.getDevice(), createInfo};

        return shaderModule;
    }

    vk::ShaderStageFlagBits PipelineVK::translateShaderStage(ShaderStage stage)
    {
        switch (stage)
        {
        case ShaderStage::Vertex: return vk::ShaderStageFlagBits::eVertex;
        case ShaderStage::Fragment: return vk::ShaderStageFlagBits::eFragment;
        case ShaderStage::Compute: return vk::ShaderStageFlagBits::eCompute;
        case ShaderStage::Task:     return vk::ShaderStageFlagBits::eTaskEXT;
        case ShaderStage::Mesh:     return vk::ShaderStageFlagBits::eMeshEXT;

        default:
            NOX_CORE_ASSERT("PipelineVK::translateShaderStage unsupported shader stage passed to Vulkan backend!");
        }
    }

    vk::ShaderStageFlagBits PipelineVK::determineNextStage(ShaderStage stage)
    {
        switch (stage)
        {
        case ShaderStage::Vertex: return vk::ShaderStageFlagBits::eFragment;
        case ShaderStage::Fragment: return {};
        case ShaderStage::Task: return vk::ShaderStageFlagBits::eMeshEXT;
        case ShaderStage::Mesh: return vk::ShaderStageFlagBits::eFragment;
        }
        return {};
    }
    
    vk::Format PipelineVK::translateImageFormat(ImageFormat format)
    {
        switch (format)
        {
            case ImageFormat::Surface: return m_deviceVK.getSurfaceFormat().format;
            case ImageFormat::R32SINT: return vk::Format::eR32Sint;
        }
    }
}
