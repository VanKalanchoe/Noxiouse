#pragma once

#include <filesystem>
#include <fstream>
#include <memory>

#include "NoxCore/Core/VirtualFileSystem.h"
#include "NoxCore/Renderer/DataTypes.h"

namespace Nox
{
    class MaterialSerializer
    {
    public:
        static bool Serialize(const std::filesystem::path& path, const MaterialData& material)
        {
            if (!path.parent_path().empty())
                std::filesystem::create_directories(path.parent_path());

            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            if (!stream.is_open())
                return false;

            stream.write("NMAT", 4);
            WriteString(stream, material.Name);
            stream.write(reinterpret_cast<const char*>(&material.Workflow), sizeof(float));
            stream.write(reinterpret_cast<const char*>(&material.DiffuseFactor), sizeof(glm::vec4));
            stream.write(reinterpret_cast<const char*>(&material.SpecularFactor), sizeof(glm::vec4));
            stream.write(reinterpret_cast<const char*>(&material.BaseColorFactor), sizeof(glm::vec4));
            WriteString(stream, material.BaseColorTexturePath);
            stream.write(reinterpret_cast<const char*>(&material.BaseColorTextureSet), sizeof(int32_t));
            stream.write(reinterpret_cast<const char*>(&material.MetallicFactor), sizeof(float));
            stream.write(reinterpret_cast<const char*>(&material.RoughnessFactor), sizeof(float));
            WriteString(stream, material.MetallicRoughnessTexturePath);
            stream.write(reinterpret_cast<const char*>(&material.PhysicalDescriptorTextureSet), sizeof(int32_t));
            WriteString(stream, material.NormalTexturePath);
            stream.write(reinterpret_cast<const char*>(&material.NormalTextureSet), sizeof(int32_t));
            WriteString(stream, material.OcclusionTexturePath);
            stream.write(reinterpret_cast<const char*>(&material.OcclusionTextureSet), sizeof(int32_t));
            stream.write(reinterpret_cast<const char*>(&material.EmissiveFactor), sizeof(glm::vec3));
            WriteString(stream, material.EmissiveTexturePath);
            stream.write(reinterpret_cast<const char*>(&material.EmissiveTextureSet), sizeof(int32_t));
            stream.write(reinterpret_cast<const char*>(&material.emissiveStrength), sizeof(float));
            stream.write(reinterpret_cast<const char*>(&material.TransmissionFactor), sizeof(float));
            WriteString(stream, material.TransmissionTexturePath);
            stream.write(reinterpret_cast<const char*>(&material.TransmissionTextureSet), sizeof(int32_t));
            uint32_t mode = static_cast<uint32_t>(material.Mode);
            stream.write(reinterpret_cast<const char*>(&mode), sizeof(uint32_t));
            stream.write(reinterpret_cast<const char*>(&material.AlphaMaskCutoff), sizeof(float));
            uint8_t doubleSided = material.DoubleSided ? 1 : 0;
            uint8_t unlit = material.Unlit ? 1 : 0;
            stream.write(reinterpret_cast<const char*>(&doubleSided), sizeof(uint8_t));
            stream.write(reinterpret_cast<const char*>(&unlit), sizeof(uint8_t));
            stream.write(reinterpret_cast<const char*>(&material.IOR), sizeof(float));
            stream.write(reinterpret_cast<const char*>(&material.Thickness), sizeof(float));
            stream.write(reinterpret_cast<const char*>(&material.AttenuationColor), sizeof(glm::vec3));
            stream.write(reinterpret_cast<const char*>(&material.AttenuationDistance), sizeof(float));
            stream.write(reinterpret_cast<const char*>(&material.DiffuseTransmission), sizeof(float));
            stream.write(reinterpret_cast<const char*>(&material.PathTracerFlags), sizeof(uint32_t));
            return stream.good();
        }

        static bool Deserialize(const std::filesystem::path& path, MaterialData& material)
        {
            // A loose file, else a mounted pack's entry (VirtualFileSystem.h).
            std::unique_ptr<std::istream> file = VFS::Open(path);
            if (!file)
                return false;
            std::istream& stream = *file;

            char magic[4]{};
            stream.read(magic, 4);
            if (std::string(magic, 4) != "NMAT")
                return false;

            ReadString(stream, material.Name);
            stream.read(reinterpret_cast<char*>(&material.Workflow), sizeof(float));
            stream.read(reinterpret_cast<char*>(&material.DiffuseFactor), sizeof(glm::vec4));
            stream.read(reinterpret_cast<char*>(&material.SpecularFactor), sizeof(glm::vec4));
            stream.read(reinterpret_cast<char*>(&material.BaseColorFactor), sizeof(glm::vec4));
            ReadString(stream, material.BaseColorTexturePath);
            stream.read(reinterpret_cast<char*>(&material.BaseColorTextureSet), sizeof(int32_t));
            stream.read(reinterpret_cast<char*>(&material.MetallicFactor), sizeof(float));
            stream.read(reinterpret_cast<char*>(&material.RoughnessFactor), sizeof(float));
            ReadString(stream, material.MetallicRoughnessTexturePath);
            stream.read(reinterpret_cast<char*>(&material.PhysicalDescriptorTextureSet), sizeof(int32_t));
            ReadString(stream, material.NormalTexturePath);
            stream.read(reinterpret_cast<char*>(&material.NormalTextureSet), sizeof(int32_t));
            ReadString(stream, material.OcclusionTexturePath);
            stream.read(reinterpret_cast<char*>(&material.OcclusionTextureSet), sizeof(int32_t));
            stream.read(reinterpret_cast<char*>(&material.EmissiveFactor), sizeof(glm::vec3));
            ReadString(stream, material.EmissiveTexturePath);
            stream.read(reinterpret_cast<char*>(&material.EmissiveTextureSet), sizeof(int32_t));
            stream.read(reinterpret_cast<char*>(&material.emissiveStrength), sizeof(float));
            stream.read(reinterpret_cast<char*>(&material.TransmissionFactor), sizeof(float));
            ReadString(stream, material.TransmissionTexturePath);
            stream.read(reinterpret_cast<char*>(&material.TransmissionTextureSet), sizeof(int32_t));
            uint32_t mode = 0;
            stream.read(reinterpret_cast<char*>(&mode), sizeof(uint32_t));
            material.Mode = static_cast<AlphaMode>(mode);
            stream.read(reinterpret_cast<char*>(&material.AlphaMaskCutoff), sizeof(float));
            uint8_t doubleSided = 0, unlit = 0;
            stream.read(reinterpret_cast<char*>(&doubleSided), sizeof(uint8_t));
            stream.read(reinterpret_cast<char*>(&unlit), sizeof(uint8_t));
            material.DoubleSided = doubleSided != 0;
            material.Unlit = unlit != 0;
            stream.read(reinterpret_cast<char*>(&material.IOR), sizeof(float));
            stream.read(reinterpret_cast<char*>(&material.Thickness), sizeof(float));
            if (!stream.good())
                return false;
            // Written since volume absorption: a file from before ends here (no absorption).
            glm::vec3 attenuationColor(1.0f);
            float attenuationDistance = 0.0f;
            if (stream.read(reinterpret_cast<char*>(&attenuationColor), sizeof(glm::vec3)) &&
                stream.read(reinterpret_cast<char*>(&attenuationDistance), sizeof(float)))
            {
                material.AttenuationColor = attenuationColor;
                material.AttenuationDistance = attenuationDistance;
                // Written since diffuse transmission: older files end before it (none).
                float diffuseTransmission = 0.0f;
                if (stream.read(reinterpret_cast<char*>(&diffuseTransmission), sizeof(float)))
                {
                    material.DiffuseTransmission = diffuseTransmission;
                    // Written since the path tracer flags: older files end before them (none).
                    uint32_t pathTracerFlags = 0;
                    if (stream.read(reinterpret_cast<char*>(&pathTracerFlags), sizeof(uint32_t)))
                        material.PathTracerFlags = pathTracerFlags;
                }
            }
            stream.clear();

            // Older transmission-only imports inherited the glTF parser's synthetic 1/1 PBR defaults. RTXPT/Donut
            // uses 0/0 when the PBR object is absent. A fully metallic material cannot transmit, so repair this exact
            // textureless legacy combination in Nox without changing the vendored parser.
            if (material.TransmissionFactor > 0.0f && material.MetallicFactor == 1.0f &&
                material.RoughnessFactor == 1.0f && material.BaseColorTexturePath.empty() &&
                material.MetallicRoughnessTexturePath.empty())
            {
                material.MetallicFactor = 0.0f;
                material.RoughnessFactor = 0.0f;
            }
            return stream.good();
        }

    private:
        static void WriteString(std::ofstream& stream, const std::string& value)
        {
            uint32_t length = static_cast<uint32_t>(value.size());
            stream.write(reinterpret_cast<const char*>(&length), sizeof(uint32_t));
            stream.write(value.data(), length);
        }

        static void ReadString(std::istream& stream, std::string& value)
        {
            uint32_t length = 0;
            stream.read(reinterpret_cast<char*>(&length), sizeof(uint32_t));
            value.resize(length);
            if (length > 0)
                stream.read(value.data(), length);
        }
    };
}
