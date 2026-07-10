#include "PipelineVK.h"

#include "DeviceVK.h"

namespace NRI
{
    PipelineVK::PipelineVK(DeviceVK& device, const PipelineDesc& desc) : m_deviceVK(device)
    {
        bool useShaderObjects = m_deviceVK.isShaderObjectExtensionEnabled();

        if (desc.type == PipelineType::Graphics)
        {
            // Verify we have shader
            if (desc.shaders.empty())
            {
                throw std::runtime_error("Graphics pipeline requires a shader stage.");
            }

            if (useShaderObjects)
            {
                const vk::ShaderCreateFlagsEXT commonFlags = vk::ShaderCreateFlagBitsEXT::eDescriptorHeap;

                std::vector<std::string> entryPointNames;
                entryPointNames.reserve(desc.shaders.size());

                std::vector<vk::ShaderCreateInfoEXT> shaderCreateInfos;
                shaderCreateInfos.reserve(desc.shaders.size());

                for (const auto& shaderDesc : desc.shaders)
                {
                    if (shaderDesc.bytecode.empty())
                        throw std::runtime_error("Empty shader bytecode");

                    entryPointNames.push_back(shaderDesc.entryPoint);
                    vk::ShaderStageFlagBits stageBit = translateShaderStage(shaderDesc.stage);
                    m_stages.push_back(stageBit);

                    shaderCreateInfos.push_back({
                        .flags = commonFlags,
                        .stage = translateShaderStage(shaderDesc.stage),
                        .nextStage = determineNextStage(shaderDesc.stage),
                        .codeType = vk::ShaderCodeTypeEXT::eSpirv,
                        .codeSize = shaderDesc.bytecode.size() * sizeof(char),
                        .pCode = reinterpret_cast<const uint32_t*>(shaderDesc.bytecode.data()),
                        .pName = entryPointNames.back().c_str(),
                        .setLayoutCount = 0, // Descriptor heap: no descriptor set layouts
                        .pSetLayouts = nullptr,
                        .pushConstantRangeCount = 0, // Push data (vkCmdPushDataEXT) is used instead
                        .pPushConstantRanges = nullptr,
                        .pSpecializationInfo = nullptr, // Vertex shader has no spec constants
                    });
                }

                m_shaders = m_deviceVK.getDevice().createShadersEXT(shaderCreateInfos);
                m_rawShaders.reserve(m_shaders.size());
                for (const auto& raiiShader : m_shaders)
                {
                    m_rawShaders.push_back(*raiiShader);
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
                    temporaryModules.push_back(createShaderModule(shaderDesc.bytecode));

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

                vk::PipelineVertexInputStateCreateInfo vertexInputInfo{
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
                    .depthCompareOp = vk::CompareOp::eLess,
                    .depthBoundsTestEnable = vk::False,
                    .stencilTestEnable = vk::False
                };
                vk::PipelineColorBlendAttachmentState colorBlendAttachment{
                    .blendEnable = vk::False,
                    .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
                };

                vk::PipelineColorBlendStateCreateInfo colorBlending{
                    .logicOpEnable = vk::False, .logicOp = vk::LogicOp::eCopy, .attachmentCount = 1, .pAttachments = &colorBlendAttachment
                };

                /*std::vector<vk::DynamicState> dynamicStates = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
                vk::PipelineDynamicStateCreateInfo dynamicState{.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()), .pDynamicStates = dynamicStates.data()};*/
                
                std::vector<vk::DynamicState> dynamicStates = {
                    // Replaces the old standard Viewport/Scissor
                    vk::DynamicState::eViewportWithCount,
                    vk::DynamicState::eScissorWithCount,
    
                    // Vertex Input
                    vk::DynamicState::eVertexInputEXT,
    
                    // Input Assembly
                    vk::DynamicState::ePrimitiveTopology,
                    vk::DynamicState::ePrimitiveRestartEnable,
    
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
                };

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
                        .pVertexInputState = &vertexInputInfo,
                        .pInputAssemblyState = &inputAssembly,
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
                    {.colorAttachmentCount = 1, .pColorAttachmentFormats = &m_deviceVK.getSurfaceFormat().format, .depthAttachmentFormat = depthFormat}
                };

                m_pipeline = vk::raii::Pipeline(m_deviceVK.getDevice(), nullptr, pipelineCreateInfoChain.get<vk::GraphicsPipelineCreateInfo>());
            }
        }
        else if (desc.type == PipelineType::Compute)
        {
            // Verify we have exactly one compute shader
            if (desc.shaders.empty())
            {
                throw std::runtime_error("Compute pipeline requires a shader stage.");
            }

            /*vk::raii::ShaderModule shaderModule = createShaderModule(readFile("../../shaders/slang.spv"));

            vk::PipelineShaderStageCreateInfo computeShaderStageInfo{.stage = vk::ShaderStageFlagBits::eCompute, .module = shaderModule, .pName = "compMain"};*/

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
            m_pipeline = vk::raii::Pipeline(m_deviceVK.getDevice(), nullptr, pipelineCreateInfoChain.get<vk::ComputePipelineCreateInfo>());
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

        // Future-proofing is now trivial to add:
        // case ShaderStage::Task:     return vk::ShaderStageFlagBits::eTaskEXT;
        // case ShaderStage::Mesh:     return vk::ShaderStageFlagBits::eMeshEXT;
        // case ShaderStage::RayGen:   return vk::ShaderStageFlagBits::eRaygenKHR;

        default:
            throw std::runtime_error("Unsupported shader stage passed to Vulkan backend!");
        }
    }

    vk::ShaderStageFlagBits PipelineVK::determineNextStage(ShaderStage stage)
    {
        switch (stage)
        {
        case ShaderStage::Vertex: return vk::ShaderStageFlagBits::eFragment;
        case ShaderStage::Fragment: return {};
        }
        return {};
    }
}
