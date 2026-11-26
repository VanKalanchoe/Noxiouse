#include "VfxAsset.h"

#include <fstream>
#include <yaml-cpp/yaml.h>

#include "NoxCore/Core/Log.h"

namespace Nox
{
    namespace
    {
        YAML::Emitter& EmitVec3(YAML::Emitter& out, const glm::vec3& v)
        {
            out << YAML::Flow << YAML::BeginSeq << v.x << v.y << v.z << YAML::EndSeq;
            return out;
        }

        YAML::Emitter& EmitVec4(YAML::Emitter& out, const glm::vec4& v)
        {
            out << YAML::Flow << YAML::BeginSeq << v.x << v.y << v.z << v.w << YAML::EndSeq;
            return out;
        }

        glm::vec3 ReadVec3(const YAML::Node& n, const glm::vec3& def)
        {
            if (!n || !n.IsSequence() || n.size() < 3)
                return def;
            return { n[0].as<float>(), n[1].as<float>(), n[2].as<float>() };
        }

        glm::vec4 ReadVec4(const YAML::Node& n, const glm::vec4& def)
        {
            if (!n || !n.IsSequence() || n.size() < 4)
                return def;
            return { n[0].as<float>(), n[1].as<float>(), n[2].as<float>(), n[3].as<float>() };
        }

        const char* LayerTypeToString(VfxLayerType t)
        {
            switch (t)
            {
            case VfxLayerType::Particles:  return "Particles";
            case VfxLayerType::Trail:      return "Trail";
            case VfxLayerType::Beam:       return "Beam";
            case VfxLayerType::Decal:      return "Decal";
            case VfxLayerType::Distortion: return "Distortion";
            case VfxLayerType::Volume:     return "Volume";
            case VfxLayerType::LocalFog:   return "LocalFog";
            case VfxLayerType::PointLight: return "PointLight";
            }
            return "Particles";
        }

        VfxLayerType LayerTypeFromString(const std::string& s)
        {
            if (s == "Trail")      return VfxLayerType::Trail;
            if (s == "Beam")       return VfxLayerType::Beam;
            if (s == "Decal")      return VfxLayerType::Decal;
            if (s == "Distortion") return VfxLayerType::Distortion;
            if (s == "Volume")     return VfxLayerType::Volume;
            if (s == "LocalFog")   return VfxLayerType::LocalFog;
            if (s == "PointLight") return VfxLayerType::PointLight;
            return VfxLayerType::Particles;
        }
    }

    void VfxAsset::LoadPreset(const std::string& presetName)
    {
        VfxAsset& asset = *this;
        asset.Name = presetName;
        asset.Duration = 4.0f;
        asset.Looping = true;
        asset.Parameters.clear();
        asset.Layers.clear();

        if (presetName == "Fireball")
        {
            VfxLayer sparks;
            sparks.Name = "Fire Sparks";
            sparks.Type = VfxLayerType::Particles;
            sparks.Particle.GpuSimulated = true;
            sparks.Particle.SpawnRate = 12000.0f;
            sparks.Particle.MaxParticles = 40000;
            sparks.Particle.Shape = ParticleShape::Sphere;
            sparks.Particle.ShapeSize = { 0.35f, 0.35f, 0.35f };
            sparks.Particle.Velocity = { 0.0f, 2.2f, 0.0f };
            sparks.Particle.VelocityRandom = { 1.4f, 1.2f, 1.4f };
            sparks.Particle.Gravity = -0.35f;
            sparks.Particle.SizeStart = 0.08f;
            sparks.Particle.SizeEnd = 0.01f;
            sparks.Particle.ColorStart = { 1.0f, 0.65f, 0.12f, 1.0f };
            sparks.Particle.ColorEnd = { 0.9f, 0.12f, 0.02f, 0.0f };
            sparks.Particle.Intensity = 6.0f;
            sparks.Particle.CurlNoiseEnabled = true;
            sparks.Particle.CurlNoiseStrength = 5.0f;
            sparks.Particle.CurlNoiseFrequency = 1.6f;
            asset.Layers.push_back(sparks);

            VfxLayer haze;
            haze.Name = "Heat Haze";
            haze.Type = VfxLayerType::Distortion;
            haze.Distortion.Mode = DistortionMode::HeatHaze;
            haze.Distortion.Radius = 1.8f;
            haze.Distortion.Strength = 0.03f;
            haze.Distortion.Speed = 2.0f;
            haze.Distortion.RimTint = { 1.0f, 0.45f, 0.1f, 0.15f };
            asset.Layers.push_back(haze);

            VfxLayer light;
            light.Name = "Fire Glow";
            light.Type = VfxLayerType::PointLight;
            light.Light.Color = { 1.0f, 0.5f, 0.12f };
            light.Light.Intensity = 18.0f;
            light.Light.Radius = 7.0f;
            light.Light.PulseSpeed = 8.0f;
            asset.Layers.push_back(light);
            return;
        }

        if (presetName == "Lightning")
        {
            VfxLayer bolt;
            bolt.Name = "Main Bolt";
            bolt.Type = VfxLayerType::Beam;
            bolt.LocalOffset = { 0.0f, 1.2f, 0.0f };
            bolt.Beam.TargetOffset = { 0.0f, 0.0f, 5.0f };
            bolt.Beam.Segments = 32;
            bolt.Beam.WidthStart = 0.14f;
            bolt.Beam.WidthEnd = 0.10f;
            bolt.Beam.NoiseAmplitude = 0.35f;
            bolt.Beam.NoiseSpeed = 20.0f;
            bolt.Beam.Intensity = 7.0f;
            asset.Layers.push_back(bolt);

            VfxLayer arcs;
            arcs.Name = "Electric Sparks";
            arcs.Type = VfxLayerType::Particles;
            arcs.LocalOffset = { 0.0f, 1.2f, 0.0f };
            arcs.Particle.GpuSimulated = true;
            arcs.Particle.SpawnRate = 6000.0f;
            arcs.Particle.MaxParticles = 20000;
            arcs.Particle.Shape = ParticleShape::Sphere;
            arcs.Particle.ShapeSize = { 0.4f, 0.4f, 0.4f };
            arcs.Particle.Velocity = { 0.0f, 0.5f, 4.0f };
            arcs.Particle.VelocityRandom = { 2.5f, 2.5f, 2.5f };
            arcs.Particle.LifetimeMin = 0.25f;
            arcs.Particle.LifetimeMax = 0.6f;
            arcs.Particle.SizeStart = 0.04f;
            arcs.Particle.SizeEnd = 0.005f;
            arcs.Particle.ColorStart = { 0.55f, 0.85f, 1.0f, 1.0f };
            arcs.Particle.ColorEnd = { 0.2f, 0.4f, 1.0f, 0.0f };
            arcs.Particle.Intensity = 8.0f;
            asset.Layers.push_back(arcs);

            VfxLayer light;
            light.Name = "Arc Flash";
            light.Type = VfxLayerType::PointLight;
            light.LocalOffset = { 0.0f, 1.2f, 0.0f };
            light.Light.Color = { 0.45f, 0.8f, 1.0f };
            light.Light.Intensity = 22.0f;
            light.Light.Radius = 8.0f;
            light.Light.PulseSpeed = 16.0f;
            asset.Layers.push_back(light);
            return;
        }

        // Default preset: "Teleport" (showcases GPU vortex sparks + rune decal + shockwave + glowing mist + point light)
        VfxLayer sparks;
        sparks.Name = "Vortex Sparks";
        sparks.Type = VfxLayerType::Particles;
        sparks.Particle.GpuSimulated = true;
        sparks.Particle.SpawnRate = 14000.0f;
        sparks.Particle.MaxParticles = 50000;
        sparks.Particle.Shape = ParticleShape::Cone;
        sparks.Particle.ShapeSize = { 0.9f, 0.9f, 0.9f };
        sparks.Particle.ConeAngle = 15.0f;
        sparks.Particle.Velocity = { 0.0f, 2.8f, 0.0f };
        sparks.Particle.VelocityRandom = { 0.6f, 0.5f, 0.6f };
        sparks.Particle.LifetimeMin = 0.8f;
        sparks.Particle.LifetimeMax = 1.6f;
        sparks.Particle.SizeStart = 0.05f;
        sparks.Particle.SizeEnd = 0.005f;
        sparks.Particle.ColorStart = { 0.25f, 0.8f, 1.0f, 1.0f };
        sparks.Particle.ColorEnd = { 0.45f, 0.25f, 1.0f, 0.0f };
        sparks.Particle.Intensity = 5.0f;
        sparks.Particle.VortexEnabled = true;
        sparks.Particle.VortexStrength = 10.0f;
        sparks.Particle.VortexRadialPull = 3.0f;
        sparks.Particle.VortexRadius = 3.0f;
        sparks.Particle.CurlNoiseEnabled = true;
        sparks.Particle.CurlNoiseStrength = 2.5f;
        asset.Layers.push_back(sparks);

        VfxLayer rune;
        rune.Name = "Ground Rune";
        rune.Type = VfxLayerType::Decal;
        rune.Decal.Size = { 3.2f, 1.5f, 3.2f };
        rune.Decal.Color = { 0.2f, 0.78f, 1.0f, 1.0f };
        rune.Decal.EmissiveIntensity = 4.0f;
        rune.Decal.Blend = ParticleBlend::Additive;
        asset.Layers.push_back(rune);

        VfxLayer shockwave;
        shockwave.Name = "Portal Ripple";
        shockwave.Type = VfxLayerType::Distortion;
        shockwave.LocalOffset = { 0.0f, 1.2f, 0.0f };
        shockwave.Distortion.Mode = DistortionMode::ShockwaveRing;
        shockwave.Distortion.Radius = 2.4f;
        shockwave.Distortion.Strength = 0.035f;
        shockwave.Distortion.Speed = 0.9f;
        shockwave.Distortion.RimTint = { 0.3f, 0.85f, 1.0f, 0.35f };
        asset.Layers.push_back(shockwave);

        VfxLayer mist;
        mist.Name = "Portal Mist";
        mist.Type = VfxLayerType::LocalFog;
        mist.LocalOffset = { 0.0f, 1.0f, 0.0f };
        mist.LocalFog.Shape = LocalFogShape::Cylinder;
        mist.LocalFog.Extents = { 1.4f, 1.5f, 1.4f };
        mist.LocalFog.Density = 0.35f;
        mist.LocalFog.EmissiveColor = { 0.15f, 0.65f, 1.0f };
        mist.LocalFog.EmissiveIntensity = 1.2f;
        asset.Layers.push_back(mist);

        VfxLayer light;
        light.Name = "Portal Light";
        light.Type = VfxLayerType::PointLight;
        light.LocalOffset = { 0.0f, 1.4f, 0.0f };
        light.Light.Color = { 0.25f, 0.75f, 1.0f };
        light.Light.Intensity = 16.0f;
        light.Light.Radius = 7.0f;
        light.Light.PulseSpeed = 3.5f;
        asset.Layers.push_back(light);
    }

    bool VfxAsset::Save(const std::filesystem::path& filePath) const
    {
        std::error_code ec;
        if (filePath.has_parent_path())
            std::filesystem::create_directories(filePath.parent_path(), ec);

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "VfxAsset" << YAML::Value << 1;
        out << YAML::Key << "Name" << YAML::Value << Name;
        out << YAML::Key << "Duration" << YAML::Value << Duration;
        out << YAML::Key << "Looping" << YAML::Value << Looping;

        if (!Parameters.empty())
        {
            out << YAML::Key << "Parameters" << YAML::Value << YAML::BeginSeq;
            for (const VfxParameter& p : Parameters)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Name" << YAML::Value << p.Name;
                out << YAML::Key << "Type" << YAML::Value << static_cast<int>(p.Type);
                out << YAML::Key << "Float" << YAML::Value << p.FloatValue;
                out << YAML::Key << "Vec3" << YAML::Value; EmitVec3(out, p.Vec3Value);
                out << YAML::Key << "Color" << YAML::Value; EmitVec4(out, p.ColorValue);
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
        }

        out << YAML::Key << "Layers" << YAML::Value << YAML::BeginSeq;
        for (const VfxLayer& l : Layers)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Name" << YAML::Value << l.Name;
            out << YAML::Key << "Enabled" << YAML::Value << l.Enabled;
            out << YAML::Key << "Type" << YAML::Value << LayerTypeToString(l.Type);
            out << YAML::Key << "LocalOffset" << YAML::Value; EmitVec3(out, l.LocalOffset);
            out << YAML::Key << "LocalRotation" << YAML::Value; EmitVec3(out, l.LocalRotation);
            out << YAML::Key << "StartDelay" << YAML::Value << l.StartDelay;
            out << YAML::Key << "Duration" << YAML::Value << l.Duration;

            switch (l.Type)
            {
            case VfxLayerType::Particles:
            {
                const auto& p = l.Particle;
                out << YAML::Key << "Particle" << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "GpuSimulated" << YAML::Value << p.GpuSimulated;
                out << YAML::Key << "SpawnRate" << YAML::Value << p.SpawnRate;
                out << YAML::Key << "Burst" << YAML::Value << p.Burst;
                out << YAML::Key << "MaxParticles" << YAML::Value << p.MaxParticles;
                out << YAML::Key << "Shape" << YAML::Value << static_cast<int>(p.Shape);
                out << YAML::Key << "ShapeSize" << YAML::Value; EmitVec3(out, p.ShapeSize);
                out << YAML::Key << "ConeAngle" << YAML::Value << p.ConeAngle;
                out << YAML::Key << "LifetimeMin" << YAML::Value << p.LifetimeMin;
                out << YAML::Key << "LifetimeMax" << YAML::Value << p.LifetimeMax;
                out << YAML::Key << "Velocity" << YAML::Value; EmitVec3(out, p.Velocity);
                out << YAML::Key << "VelocityRandom" << YAML::Value; EmitVec3(out, p.VelocityRandom);
                out << YAML::Key << "Gravity" << YAML::Value << p.Gravity;
                out << YAML::Key << "Drag" << YAML::Value << p.Drag;
                out << YAML::Key << "SizeStart" << YAML::Value << p.SizeStart;
                out << YAML::Key << "SizeEnd" << YAML::Value << p.SizeEnd;
                out << YAML::Key << "ColorStart" << YAML::Value; EmitVec4(out, p.ColorStart);
                out << YAML::Key << "ColorEnd" << YAML::Value; EmitVec4(out, p.ColorEnd);
                out << YAML::Key << "Intensity" << YAML::Value << p.Intensity;
                out << YAML::Key << "Blend" << YAML::Value << static_cast<int>(p.Blend);
                out << YAML::Key << "RenderMode" << YAML::Value << static_cast<int>(p.RenderMode);
                out << YAML::Key << "ParticleMesh" << YAML::Value << static_cast<uint64_t>(p.ParticleMesh);
                out << YAML::Key << "AlignToVelocity" << YAML::Value << p.AlignToVelocity;
                out << YAML::Key << "CurlNoiseEnabled" << YAML::Value << p.CurlNoiseEnabled;
                out << YAML::Key << "CurlNoiseStrength" << YAML::Value << p.CurlNoiseStrength;
                out << YAML::Key << "CurlNoiseFrequency" << YAML::Value << p.CurlNoiseFrequency;
                out << YAML::Key << "VortexEnabled" << YAML::Value << p.VortexEnabled;
                out << YAML::Key << "VortexStrength" << YAML::Value << p.VortexStrength;
                out << YAML::Key << "VortexRadialPull" << YAML::Value << p.VortexRadialPull;
                out << YAML::Key << "VortexRadius" << YAML::Value << p.VortexRadius;
                out << YAML::Key << "AttractorEnabled" << YAML::Value << p.AttractorEnabled;
                out << YAML::Key << "AttractorOffset" << YAML::Value; EmitVec3(out, p.AttractorOffset);
                out << YAML::Key << "AttractorStrength" << YAML::Value << p.AttractorStrength;
                out << YAML::Key << "DepthCollisionEnabled" << YAML::Value << p.DepthCollisionEnabled;
                out << YAML::EndMap;
                break;
            }
            case VfxLayerType::Trail:
            {
                const auto& t = l.Trail;
                out << YAML::Key << "Trail" << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "Lifetime" << YAML::Value << t.Lifetime;
                out << YAML::Key << "WidthStart" << YAML::Value << t.WidthStart;
                out << YAML::Key << "WidthEnd" << YAML::Value << t.WidthEnd;
                out << YAML::Key << "ColorStart" << YAML::Value; EmitVec4(out, t.ColorStart);
                out << YAML::Key << "ColorEnd" << YAML::Value; EmitVec4(out, t.ColorEnd);
                out << YAML::Key << "Intensity" << YAML::Value << t.Intensity;
                out << YAML::EndMap;
                break;
            }
            case VfxLayerType::Beam:
            {
                const auto& b = l.Beam;
                out << YAML::Key << "Beam" << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "TargetOffset" << YAML::Value; EmitVec3(out, b.TargetOffset);
                out << YAML::Key << "Segments" << YAML::Value << b.Segments;
                out << YAML::Key << "WidthStart" << YAML::Value << b.WidthStart;
                out << YAML::Key << "WidthEnd" << YAML::Value << b.WidthEnd;
                out << YAML::Key << "NoiseAmplitude" << YAML::Value << b.NoiseAmplitude;
                out << YAML::Key << "NoiseSpeed" << YAML::Value << b.NoiseSpeed;
                out << YAML::Key << "ColorStart" << YAML::Value; EmitVec4(out, b.ColorStart);
                out << YAML::Key << "ColorEnd" << YAML::Value; EmitVec4(out, b.ColorEnd);
                out << YAML::Key << "Intensity" << YAML::Value << b.Intensity;
                out << YAML::EndMap;
                break;
            }
            case VfxLayerType::Decal:
            {
                const auto& d = l.Decal;
                out << YAML::Key << "Decal" << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "Size" << YAML::Value; EmitVec3(out, d.Size);
                out << YAML::Key << "Color" << YAML::Value; EmitVec4(out, d.Color);
                out << YAML::Key << "EmissiveIntensity" << YAML::Value << d.EmissiveIntensity;
                out << YAML::Key << "Blend" << YAML::Value << static_cast<int>(d.Blend);
                out << YAML::Key << "Texture" << YAML::Value << static_cast<uint64_t>(d.Texture);
                out << YAML::EndMap;
                break;
            }
            case VfxLayerType::Distortion:
            {
                const auto& d = l.Distortion;
                out << YAML::Key << "Distortion" << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "Mode" << YAML::Value << static_cast<int>(d.Mode);
                out << YAML::Key << "Radius" << YAML::Value << d.Radius;
                out << YAML::Key << "Strength" << YAML::Value << d.Strength;
                out << YAML::Key << "RingWidth" << YAML::Value << d.RingWidth;
                out << YAML::Key << "Speed" << YAML::Value << d.Speed;
                out << YAML::Key << "ChromaticAberration" << YAML::Value << d.ChromaticAberration;
                out << YAML::Key << "RimTint" << YAML::Value; EmitVec4(out, d.RimTint);
                out << YAML::EndMap;
                break;
            }
            case VfxLayerType::Volume:
            {
                const auto& v = l.Volume;
                out << YAML::Key << "Volume" << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "VolumeHandle" << YAML::Value << static_cast<uint64_t>(v.Volume);
                out << YAML::Key << "DensityScale" << YAML::Value << v.DensityScale;
                out << YAML::Key << "Albedo" << YAML::Value; EmitVec3(out, v.Albedo);
                out << YAML::Key << "EmissionScale" << YAML::Value << v.EmissionScale;
                out << YAML::Key << "BlackbodyTemperature" << YAML::Value << v.BlackbodyTemperature;
                out << YAML::Key << "BoundsScale" << YAML::Value; EmitVec3(out, v.BoundsScale);
                out << YAML::EndMap;
                break;
            }
            case VfxLayerType::LocalFog:
            {
                const auto& f = l.LocalFog;
                out << YAML::Key << "LocalFog" << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "Shape" << YAML::Value << static_cast<int>(f.Shape);
                out << YAML::Key << "Extents" << YAML::Value; EmitVec3(out, f.Extents);
                out << YAML::Key << "Density" << YAML::Value << f.Density;
                out << YAML::Key << "Albedo" << YAML::Value; EmitVec3(out, f.Albedo);
                out << YAML::Key << "EmissiveColor" << YAML::Value; EmitVec3(out, f.EmissiveColor);
                out << YAML::Key << "EmissiveIntensity" << YAML::Value << f.EmissiveIntensity;
                out << YAML::EndMap;
                break;
            }
            case VfxLayerType::PointLight:
            {
                out << YAML::Key << "Light" << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "Color" << YAML::Value; EmitVec3(out, l.Light.Color);
                out << YAML::Key << "Intensity" << YAML::Value << l.Light.Intensity;
                out << YAML::Key << "Radius" << YAML::Value << l.Light.Radius;
                out << YAML::Key << "PulseSpeed" << YAML::Value << l.Light.PulseSpeed;
                out << YAML::EndMap;
                break;
            }
            }
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
        out << YAML::EndMap;

        std::ofstream fout(filePath, std::ios::trunc);
        if (!fout)
        {
            NOX_CORE_ERROR("VfxAsset::Save: failed to write '{}'", filePath.string());
            return false;
        }
        fout << out.c_str();
        return static_cast<bool>(fout);
    }

    bool VfxAsset::Load(const std::filesystem::path& filePath)
    {
        FilePath = filePath;
        try
        {
            YAML::Node root = YAML::LoadFile(filePath.string());
            if (!root || !root["VfxAsset"])
                return false;

            if (root["Name"]) Name = root["Name"].as<std::string>();
            if (root["Duration"]) Duration = root["Duration"].as<float>();
            if (root["Looping"]) Looping = root["Looping"].as<bool>();

            Parameters.clear();
            if (auto params = root["Parameters"]; params && params.IsSequence())
            {
                for (const auto& pn : params)
                {
                    VfxParameter p;
                    if (pn["Name"]) p.Name = pn["Name"].as<std::string>();
                    if (pn["Type"]) p.Type = static_cast<VfxParamType>(pn["Type"].as<int>());
                    if (pn["Float"]) p.FloatValue = pn["Float"].as<float>();
                    p.Vec3Value = ReadVec3(pn["Vec3"], p.Vec3Value);
                    p.ColorValue = ReadVec4(pn["Color"], p.ColorValue);
                    Parameters.push_back(std::move(p));
                }
            }

            Layers.clear();
            if (auto layers = root["Layers"]; layers && layers.IsSequence())
            {
                for (const auto& ln : layers)
                {
                    VfxLayer l;
                    if (ln["Name"]) l.Name = ln["Name"].as<std::string>();
                    if (ln["Enabled"]) l.Enabled = ln["Enabled"].as<bool>();
                    if (ln["Type"]) l.Type = LayerTypeFromString(ln["Type"].as<std::string>());
                    l.LocalOffset = ReadVec3(ln["LocalOffset"], l.LocalOffset);
                    l.LocalRotation = ReadVec3(ln["LocalRotation"], l.LocalRotation);
                    if (ln["StartDelay"]) l.StartDelay = ln["StartDelay"].as<float>();
                    if (ln["Duration"]) l.Duration = ln["Duration"].as<float>();

                    if (auto n = ln["Particle"]; n && n.IsMap())
                    {
                        auto& p = l.Particle;
                        if (n["GpuSimulated"]) p.GpuSimulated = n["GpuSimulated"].as<bool>();
                        if (n["SpawnRate"]) p.SpawnRate = n["SpawnRate"].as<float>();
                        if (n["Burst"]) p.Burst = n["Burst"].as<uint32_t>();
                        if (n["MaxParticles"]) p.MaxParticles = n["MaxParticles"].as<uint32_t>();
                        if (n["Shape"]) p.Shape = static_cast<ParticleShape>(n["Shape"].as<int>());
                        p.ShapeSize = ReadVec3(n["ShapeSize"], p.ShapeSize);
                        if (n["ConeAngle"]) p.ConeAngle = n["ConeAngle"].as<float>();
                        if (n["LifetimeMin"]) p.LifetimeMin = n["LifetimeMin"].as<float>();
                        if (n["LifetimeMax"]) p.LifetimeMax = n["LifetimeMax"].as<float>();
                        p.Velocity = ReadVec3(n["Velocity"], p.Velocity);
                        p.VelocityRandom = ReadVec3(n["VelocityRandom"], p.VelocityRandom);
                        if (n["Gravity"]) p.Gravity = n["Gravity"].as<float>();
                        if (n["Drag"]) p.Drag = n["Drag"].as<float>();
                        if (n["SizeStart"]) p.SizeStart = n["SizeStart"].as<float>();
                        if (n["SizeEnd"]) p.SizeEnd = n["SizeEnd"].as<float>();
                        p.ColorStart = ReadVec4(n["ColorStart"], p.ColorStart);
                        p.ColorEnd = ReadVec4(n["ColorEnd"], p.ColorEnd);
                        if (n["Intensity"]) p.Intensity = n["Intensity"].as<float>();
                        if (n["Blend"]) p.Blend = static_cast<ParticleBlend>(n["Blend"].as<int>());
                        if (n["RenderMode"]) p.RenderMode = static_cast<ParticleRenderMode>(n["RenderMode"].as<int>());
                        if (n["ParticleMesh"]) p.ParticleMesh = AssetHandle(n["ParticleMesh"].as<uint64_t>());
                        if (n["AlignToVelocity"]) p.AlignToVelocity = n["AlignToVelocity"].as<bool>();
                        if (n["CurlNoiseEnabled"]) p.CurlNoiseEnabled = n["CurlNoiseEnabled"].as<bool>();
                        if (n["CurlNoiseStrength"]) p.CurlNoiseStrength = n["CurlNoiseStrength"].as<float>();
                        if (n["CurlNoiseFrequency"]) p.CurlNoiseFrequency = n["CurlNoiseFrequency"].as<float>();
                        if (n["VortexEnabled"]) p.VortexEnabled = n["VortexEnabled"].as<bool>();
                        if (n["VortexStrength"]) p.VortexStrength = n["VortexStrength"].as<float>();
                        if (n["VortexRadialPull"]) p.VortexRadialPull = n["VortexRadialPull"].as<float>();
                        if (n["VortexRadius"]) p.VortexRadius = n["VortexRadius"].as<float>();
                        if (n["AttractorEnabled"]) p.AttractorEnabled = n["AttractorEnabled"].as<bool>();
                        p.AttractorOffset = ReadVec3(n["AttractorOffset"], p.AttractorOffset);
                        if (n["AttractorStrength"]) p.AttractorStrength = n["AttractorStrength"].as<float>();
                        if (n["DepthCollisionEnabled"]) p.DepthCollisionEnabled = n["DepthCollisionEnabled"].as<bool>();
                    }
                    if (auto n = ln["Trail"]; n && n.IsMap())
                    {
                        auto& t = l.Trail;
                        if (n["Lifetime"]) t.Lifetime = n["Lifetime"].as<float>();
                        if (n["WidthStart"]) t.WidthStart = n["WidthStart"].as<float>();
                        if (n["WidthEnd"]) t.WidthEnd = n["WidthEnd"].as<float>();
                        t.ColorStart = ReadVec4(n["ColorStart"], t.ColorStart);
                        t.ColorEnd = ReadVec4(n["ColorEnd"], t.ColorEnd);
                        if (n["Intensity"]) t.Intensity = n["Intensity"].as<float>();
                    }
                    if (auto n = ln["Beam"]; n && n.IsMap())
                    {
                        auto& b = l.Beam;
                        b.TargetOffset = ReadVec3(n["TargetOffset"], b.TargetOffset);
                        if (n["Segments"]) b.Segments = n["Segments"].as<uint32_t>();
                        if (n["WidthStart"]) b.WidthStart = n["WidthStart"].as<float>();
                        if (n["WidthEnd"]) b.WidthEnd = n["WidthEnd"].as<float>();
                        if (n["NoiseAmplitude"]) b.NoiseAmplitude = n["NoiseAmplitude"].as<float>();
                        if (n["NoiseSpeed"]) b.NoiseSpeed = n["NoiseSpeed"].as<float>();
                        b.ColorStart = ReadVec4(n["ColorStart"], b.ColorStart);
                        b.ColorEnd = ReadVec4(n["ColorEnd"], b.ColorEnd);
                        if (n["Intensity"]) b.Intensity = n["Intensity"].as<float>();
                    }
                    if (auto n = ln["Decal"]; n && n.IsMap())
                    {
                        auto& d = l.Decal;
                        d.Size = ReadVec3(n["Size"], d.Size);
                        d.Color = ReadVec4(n["Color"], d.Color);
                        if (n["EmissiveIntensity"]) d.EmissiveIntensity = n["EmissiveIntensity"].as<float>();
                        if (n["Blend"]) d.Blend = static_cast<ParticleBlend>(n["Blend"].as<int>());
                        if (n["Texture"]) d.Texture = AssetHandle(n["Texture"].as<uint64_t>());
                    }
                    if (auto n = ln["Distortion"]; n && n.IsMap())
                    {
                        auto& d = l.Distortion;
                        if (n["Mode"]) d.Mode = static_cast<DistortionMode>(n["Mode"].as<int>());
                        if (n["Radius"]) d.Radius = n["Radius"].as<float>();
                        if (n["Strength"]) d.Strength = n["Strength"].as<float>();
                        if (n["RingWidth"]) d.RingWidth = n["RingWidth"].as<float>();
                        if (n["Speed"]) d.Speed = n["Speed"].as<float>();
                        if (n["ChromaticAberration"]) d.ChromaticAberration = n["ChromaticAberration"].as<float>();
                        d.RimTint = ReadVec4(n["RimTint"], d.RimTint);
                    }
                    if (auto n = ln["Volume"]; n && n.IsMap())
                    {
                        auto& v = l.Volume;
                        if (n["VolumeHandle"]) v.Volume = AssetHandle(n["VolumeHandle"].as<uint64_t>());
                        if (n["DensityScale"]) v.DensityScale = n["DensityScale"].as<float>();
                        v.Albedo = ReadVec3(n["Albedo"], v.Albedo);
                        if (n["EmissionScale"]) v.EmissionScale = n["EmissionScale"].as<float>();
                        if (n["BlackbodyTemperature"]) v.BlackbodyTemperature = n["BlackbodyTemperature"].as<float>();
                        v.BoundsScale = ReadVec3(n["BoundsScale"], v.BoundsScale);
                    }
                    if (auto n = ln["LocalFog"]; n && n.IsMap())
                    {
                        auto& f = l.LocalFog;
                        if (n["Shape"]) f.Shape = static_cast<LocalFogShape>(n["Shape"].as<int>());
                        f.Extents = ReadVec3(n["Extents"], f.Extents);
                        if (n["Density"]) f.Density = n["Density"].as<float>();
                        f.Albedo = ReadVec3(n["Albedo"], f.Albedo);
                        f.EmissiveColor = ReadVec3(n["EmissiveColor"], f.EmissiveColor);
                        if (n["EmissiveIntensity"]) f.EmissiveIntensity = n["EmissiveIntensity"].as<float>();
                    }
                    if (auto n = ln["Light"]; n && n.IsMap())
                    {
                        l.Light.Color = ReadVec3(n["Color"], l.Light.Color);
                        if (n["Intensity"]) l.Light.Intensity = n["Intensity"].as<float>();
                        if (n["Radius"]) l.Light.Radius = n["Radius"].as<float>();
                        if (n["PulseSpeed"]) l.Light.PulseSpeed = n["PulseSpeed"].as<float>();
                    }
                    Layers.push_back(std::move(l));
                }
            }
            return true;
        }
        catch (const std::exception& e)
        {
            NOX_CORE_ERROR("VfxAsset::Load: failed to parse '{}': {}", filePath.string(), e.what());
            return false;
        }
    }
}
