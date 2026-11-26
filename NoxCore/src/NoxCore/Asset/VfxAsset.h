#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Scene/Components.h"

namespace Nox
{
    enum class VfxLayerType : uint8_t
    {
        Particles  = 0,
        Trail      = 1,
        Beam       = 2,
        Decal      = 3,
        Distortion = 4,
        Volume     = 5,
        LocalFog   = 6,
        PointLight = 7
    };

    enum class VfxParamType : uint8_t
    {
        Float = 0,
        Vec3  = 1,
        Color = 2
    };

    struct VfxParameter
    {
        std::string Name = "Intensity";
        VfxParamType Type = VfxParamType::Float;
        float FloatValue = 1.0f;
        glm::vec3 Vec3Value{ 0.0f };
        glm::vec4 ColorValue{ 1.0f };
    };

    struct VfxLightLayerSettings
    {
        glm::vec3 Color{ 0.35f, 0.75f, 1.0f };
        float Intensity = 12.0f;
        float Radius = 6.0f;
        float PulseSpeed = 3.0f;
    };

    struct VfxLayer
    {
        std::string Name = "Layer";
        bool Enabled = true;
        VfxLayerType Type = VfxLayerType::Particles;
        glm::vec3 LocalOffset{ 0.0f };   // meters
        glm::vec3 LocalRotation{ 0.0f }; // degrees
        float StartDelay = 0.0f;         // seconds
        float Duration = 0.0f;           // 0 = active for entire system duration

        ParticleEmitterComponent Particle{};
        TrailRendererComponent Trail{};
        BeamRendererComponent Beam{};
        DecalComponent Decal{};
        DistortionComponent Distortion{};
        VolumeComponent Volume{};
        LocalFogVolumeComponent LocalFog{};
        VfxLightLayerSettings Light{};
    };

    // Unified Multi-Layer VFX Asset (.nvfx, docs/VFX_And_Volumes_Plan_2026-11.md step V4).
    // Combines GPU particle emitters, mesh particles, ribbons/trails, lightning beams, decals,
    // screen-space distortion, heterogeneous volumes, local fog, and dynamic point lights in one reusable asset.
    class VfxAsset : public Asset
    {
    public:
        VfxAsset() = default;
        ~VfxAsset() override = default;

        AssetType GetType() const override { return AssetType::Vfx; }
        static AssetType GetStaticType() { return AssetType::Vfx; }

        bool Load(const std::filesystem::path& filePath);
        bool Save(const std::filesystem::path& filePath) const;

        // Populate this asset with a ready-to-use multi-layer preset ("Teleport", "Fireball", "Lightning").
        void LoadPreset(const std::string& presetName);

        std::string Name = "New VFX";
        float Duration = 5.0f;
        bool Looping = true;
        std::vector<VfxParameter> Parameters;
        std::vector<VfxLayer> Layers;
        std::filesystem::path FilePath;
    };
}
