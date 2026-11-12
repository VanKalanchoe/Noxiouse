#include "ProjectSerializer.h"

#include <fstream>
#include <yaml-cpp/yaml.h>

#include "NoxCore/Core/Log.h"

namespace Nox
{
    ProjectSerializer::ProjectSerializer(Ref<Project> project) : m_Project(project)
    {
    }

    bool ProjectSerializer::Serialize(const std::filesystem::path& filePath)
    {
        const auto& config = m_Project->GetConfig();
        
        YAML::Emitter out;
        {
            out << YAML::BeginMap; // Root
            out << YAML::Key << "Project" << YAML::Value;
            {
                out << YAML::BeginMap; // Project
                out << YAML::Key << "Name" << YAML::Value << config.Name;
                out << YAML::Key << "StartScene" << YAML::Value << (uint64_t)config.StartScene;
                out << YAML::Key << "AssetDirectory" << YAML::Value << config.AssetDirectory.string();
                out << YAML::Key << "AssetRegistryPath" << YAML::Value << config.AssetRegistryPath.string();
                out << YAML::Key << "ScriptModulePath" << YAML::Value << config.ScriptModulePath.string();
                out << YAML::Key << "WorldUnitsPerMeter" << YAML::Value << config.WorldUnitsPerMeter;
                out << YAML::Key << "RenderQuality" << YAML::Value << config.RenderQuality;
                out << YAML::Key << "WindowMode" << YAML::Value << config.WindowMode;
                out << YAML::Key << "AudioMixer" << YAML::Value << config.AudioMixer.generic_string();
                out << YAML::Key << "PhysicalSurfaces" << YAML::Value << YAML::Flow << config.PhysicalSurfaces;
                const ProjectRendererSettings& renderer = config.Renderer;
                if (renderer.RayTracingResolution >= 0 || renderer.DiffuseGI >= 0 || renderer.ShadowedLights >= 0 || renderer.OIT >= 0 || renderer.TAA >= 0 ||
                    renderer.Shadows >= 0 || renderer.SunShadows >= 0 || renderer.LocalShadows >= 0 || renderer.ContactShadows >= 0 ||
                    renderer.SSAOMethod >= 0 || renderer.SSR >= 0 || renderer.SSGI >= 0 || renderer.VolumetricFog >= 0 ||
                    renderer.Bloom >= 0 || renderer.DepthOfField >= 0 || renderer.MotionBlur >= 0)
                {
                    out << YAML::Key << "Renderer" << YAML::Value << YAML::BeginMap;
                    if (renderer.RayTracingResolution >= 0) out << YAML::Key << "RayTracingResolution" << YAML::Value << renderer.RayTracingResolution;
                    if (renderer.DiffuseGI >= 0) out << YAML::Key << "DiffuseGI" << YAML::Value << renderer.DiffuseGI;
                    if (renderer.ShadowedLights >= 0) out << YAML::Key << "ShadowedLights" << YAML::Value << renderer.ShadowedLights;
                    if (renderer.OIT >= 0) out << YAML::Key << "OIT" << YAML::Value << renderer.OIT;
                    if (renderer.TAA >= 0) out << YAML::Key << "TAA" << YAML::Value << renderer.TAA;
                    if (renderer.Shadows >= 0) out << YAML::Key << "Shadows" << YAML::Value << renderer.Shadows;
                    if (renderer.SunShadows >= 0) out << YAML::Key << "SunShadows" << YAML::Value << renderer.SunShadows;
                    if (renderer.LocalShadows >= 0) out << YAML::Key << "LocalShadows" << YAML::Value << renderer.LocalShadows;
                    if (renderer.ContactShadows >= 0) out << YAML::Key << "ContactShadows" << YAML::Value << renderer.ContactShadows;
                    if (renderer.SSAOMethod >= 0) out << YAML::Key << "SSAOMethod" << YAML::Value << renderer.SSAOMethod;
                    if (renderer.SSR >= 0) out << YAML::Key << "SSR" << YAML::Value << renderer.SSR;
                    if (renderer.SSGI >= 0) out << YAML::Key << "SSGI" << YAML::Value << renderer.SSGI;
                    if (renderer.VolumetricFog >= 0) out << YAML::Key << "VolumetricFog" << YAML::Value << renderer.VolumetricFog;
                    if (renderer.Bloom >= 0) out << YAML::Key << "Bloom" << YAML::Value << renderer.Bloom;
                    if (renderer.DepthOfField >= 0) out << YAML::Key << "DepthOfField" << YAML::Value << renderer.DepthOfField;
                    if (renderer.MotionBlur >= 0) out << YAML::Key << "MotionBlur" << YAML::Value << renderer.MotionBlur;
                    out << YAML::EndMap;
                }
                out << YAML::EndMap; // Project
            }
            out << YAML::EndMap; // Root
        }
        
        std::ofstream fout(filePath);
        fout << out.c_str();

        return true;
    }

    bool ProjectSerializer::Deserialize(const std::filesystem::path& filePath)
    {
        auto& config = m_Project->GetConfig();

        YAML::Node data;
        try
        {
            data = YAML::LoadFile(filePath.string());
        }
        catch (YAML::ParserException e)
        {
            NOX_CORE_ERROR("Failed to load project file '{0}'\n    {1}", filePath.string(), e.what());
        }

        auto projectNode = data["Project"];
        if (!projectNode)
            return false;

        config.Name = projectNode["Name"].as<std::string>();
        config.StartScene = projectNode["StartScene"].as<uint64_t>();
        config.AssetDirectory = projectNode["AssetDirectory"].as<std::string>();
        if (projectNode["AssetRegistryPath"])
            config.AssetRegistryPath = projectNode["AssetRegistryPath"].as<std::string>();
        config.ScriptModulePath = projectNode["ScriptModulePath"].as<std::string>();
        if (projectNode["WorldUnitsPerMeter"])
            config.WorldUnitsPerMeter = projectNode["WorldUnitsPerMeter"].as<float>();
        if (projectNode["RenderQuality"])
            config.RenderQuality = projectNode["RenderQuality"].as<std::string>();
        if (projectNode["WindowMode"])
            config.WindowMode = projectNode["WindowMode"].as<std::string>();
        if (projectNode["AudioMixer"])
            config.AudioMixer = projectNode["AudioMixer"].as<std::string>();
        if (const YAML::Node surfaces = projectNode["PhysicalSurfaces"]; surfaces && surfaces.IsSequence())
        {
            config.PhysicalSurfaces.clear();
            for (const YAML::Node& surface : surfaces)
                config.PhysicalSurfaces.push_back(surface.as<std::string>());
        }
        if (config.PhysicalSurfaces.empty() || config.PhysicalSurfaces.front() != "Default")
        {
            std::erase(config.PhysicalSurfaces, std::string("Default"));
            config.PhysicalSurfaces.insert(config.PhysicalSurfaces.begin(), "Default");
        }
        config.Renderer = {};
        if (const YAML::Node renderer = projectNode["Renderer"]; renderer && renderer.IsMap())
        {
            auto read = [&](const char* key, int32_t& value) { if (renderer[key]) value = renderer[key].as<int32_t>(); };
            read("RayTracingResolution", config.Renderer.RayTracingResolution);
            read("DiffuseGI", config.Renderer.DiffuseGI);
            read("ShadowedLights", config.Renderer.ShadowedLights);
            read("OIT", config.Renderer.OIT);
            read("TAA", config.Renderer.TAA);
            read("Shadows", config.Renderer.Shadows);
            read("SunShadows", config.Renderer.SunShadows);
            read("LocalShadows", config.Renderer.LocalShadows);
            read("ContactShadows", config.Renderer.ContactShadows);
            read("SSAOMethod", config.Renderer.SSAOMethod);
            read("SSR", config.Renderer.SSR);
            read("SSGI", config.Renderer.SSGI);
            read("VolumetricFog", config.Renderer.VolumetricFog);
            read("Bloom", config.Renderer.Bloom);
            read("DepthOfField", config.Renderer.DepthOfField);
            read("MotionBlur", config.Renderer.MotionBlur);
        }

        return true;
    }
}
