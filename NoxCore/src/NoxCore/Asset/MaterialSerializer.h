#pragma once

#include <filesystem>
#include <fstream>
#include <istream>
#include <memory>
#include <ostream>

#include "NoxCore/Core/VirtualFileSystem.h"
#include "NoxCore/Renderer/DataTypes.h"

namespace Nox
{
    // A material's fields, as a .nmat file holds them and a cooked mesh embeds its materials (MeshSerializer): one writer for both,
    // so the two carry the same fields. A file in an older layout is not read (its material is made again from the source model).
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

            stream.write(Magic, 4);
            WriteFields(stream, material);
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
            if (std::string(magic, 4) != std::string(Magic, 4))
                return false;
            ReadFields(stream, material);
            return !stream.fail();
        }

        static void WriteFields(std::ostream& stream, const MaterialData& material)
        {
            WriteString(stream, material.Name);
            Write(stream, material.Workflow);
            Write(stream, material.DiffuseFactor);
            Write(stream, material.SpecularFactor);
            Write(stream, material.BaseColorFactor);
            WriteTexture(stream, material.BaseColorTexturePath, material.BaseColorTextureSet);
            Write(stream, material.MetallicFactor);
            Write(stream, material.RoughnessFactor);
            WriteTexture(stream, material.MetallicRoughnessTexturePath, material.PhysicalDescriptorTextureSet);
            WriteTexture(stream, material.NormalTexturePath, material.NormalTextureSet);
            WriteTexture(stream, material.OcclusionTexturePath, material.OcclusionTextureSet);
            Write(stream, material.EmissiveFactor);
            WriteTexture(stream, material.EmissiveTexturePath, material.EmissiveTextureSet);
            Write(stream, material.emissiveStrength);
            Write(stream, material.TransmissionFactor);
            WriteTexture(stream, material.TransmissionTexturePath, material.TransmissionTextureSet);
            Write(stream, material.IOR);
            Write(stream, material.Thickness);
            Write(stream, material.AttenuationColor);
            Write(stream, material.AttenuationDistance);
            Write(stream, material.DiffuseTransmission);
            Write(stream, material.PathTracerFlags);
            Write(stream, material.DepthBias);
            Write(stream, static_cast<uint32_t>(material.Mode));
            Write(stream, material.AlphaMaskCutoff);
            Write(stream, static_cast<uint8_t>(material.DoubleSided ? 1 : 0));
            Write(stream, static_cast<uint8_t>(material.Unlit ? 1 : 0));

            Write(stream, material.ClearcoatFactor);
            WriteTexture(stream, material.ClearcoatTexture);
            Write(stream, material.ClearcoatRoughnessFactor);
            WriteTexture(stream, material.ClearcoatRoughnessTexture);
            WriteTexture(stream, material.ClearcoatNormalTexture);
            Write(stream, material.SheenColorFactor);
            WriteTexture(stream, material.SheenColorTexture);
            Write(stream, material.SheenRoughnessFactor);
            WriteTexture(stream, material.SheenRoughnessTexture);
            Write(stream, material.SpecularStrength);
            WriteTexture(stream, material.SpecularStrengthTexture);
            Write(stream, material.SpecularColorFactor);
            WriteTexture(stream, material.SpecularColorTexture);
            Write(stream, material.IridescenceFactor);
            WriteTexture(stream, material.IridescenceTexture);
            Write(stream, material.IridescenceIOR);
            Write(stream, material.IridescenceThicknessMinimum);
            Write(stream, material.IridescenceThicknessMaximum);
            WriteTexture(stream, material.IridescenceThicknessTexture);
            Write(stream, material.AnisotropyStrength);
            Write(stream, material.AnisotropyRotation);
            WriteTexture(stream, material.AnisotropyTexture);
            Write(stream, material.Dispersion);
        }

        static void ReadFields(std::istream& stream, MaterialData& material)
        {
            ReadString(stream, material.Name);
            Read(stream, material.Workflow);
            Read(stream, material.DiffuseFactor);
            Read(stream, material.SpecularFactor);
            Read(stream, material.BaseColorFactor);
            ReadTexture(stream, material.BaseColorTexturePath, material.BaseColorTextureSet);
            Read(stream, material.MetallicFactor);
            Read(stream, material.RoughnessFactor);
            ReadTexture(stream, material.MetallicRoughnessTexturePath, material.PhysicalDescriptorTextureSet);
            ReadTexture(stream, material.NormalTexturePath, material.NormalTextureSet);
            ReadTexture(stream, material.OcclusionTexturePath, material.OcclusionTextureSet);
            Read(stream, material.EmissiveFactor);
            ReadTexture(stream, material.EmissiveTexturePath, material.EmissiveTextureSet);
            Read(stream, material.emissiveStrength);
            Read(stream, material.TransmissionFactor);
            ReadTexture(stream, material.TransmissionTexturePath, material.TransmissionTextureSet);
            Read(stream, material.IOR);
            Read(stream, material.Thickness);
            Read(stream, material.AttenuationColor);
            Read(stream, material.AttenuationDistance);
            Read(stream, material.DiffuseTransmission);
            Read(stream, material.PathTracerFlags);
            Read(stream, material.DepthBias);
            uint32_t mode = 0;
            Read(stream, mode);
            material.Mode = static_cast<AlphaMode>(mode);
            Read(stream, material.AlphaMaskCutoff);
            uint8_t doubleSided = 0, unlit = 0;
            Read(stream, doubleSided);
            Read(stream, unlit);
            material.DoubleSided = doubleSided != 0;
            material.Unlit = unlit != 0;

            Read(stream, material.ClearcoatFactor);
            ReadTexture(stream, material.ClearcoatTexture);
            Read(stream, material.ClearcoatRoughnessFactor);
            ReadTexture(stream, material.ClearcoatRoughnessTexture);
            ReadTexture(stream, material.ClearcoatNormalTexture);
            Read(stream, material.SheenColorFactor);
            ReadTexture(stream, material.SheenColorTexture);
            Read(stream, material.SheenRoughnessFactor);
            ReadTexture(stream, material.SheenRoughnessTexture);
            Read(stream, material.SpecularStrength);
            ReadTexture(stream, material.SpecularStrengthTexture);
            Read(stream, material.SpecularColorFactor);
            ReadTexture(stream, material.SpecularColorTexture);
            Read(stream, material.IridescenceFactor);
            ReadTexture(stream, material.IridescenceTexture);
            Read(stream, material.IridescenceIOR);
            Read(stream, material.IridescenceThicknessMinimum);
            Read(stream, material.IridescenceThicknessMaximum);
            ReadTexture(stream, material.IridescenceThicknessTexture);
            Read(stream, material.AnisotropyStrength);
            Read(stream, material.AnisotropyRotation);
            ReadTexture(stream, material.AnisotropyTexture);
            Read(stream, material.Dispersion);
        }

    private:
        // "NMA2": the layout with every field (lobes included); "NMAT" files from before are made again.
        static constexpr char Magic[4] = { 'N', 'M', 'A', '2' };

        template<typename T>
        static void Write(std::ostream& stream, const T& value)
        {
            stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
        }

        template<typename T>
        static void Read(std::istream& stream, T& value)
        {
            stream.read(reinterpret_cast<char*>(&value), sizeof(T));
        }

        static void WriteString(std::ostream& stream, const std::string& value)
        {
            uint32_t length = static_cast<uint32_t>(value.size());
            Write(stream, length);
            stream.write(value.data(), length);
        }

        static void ReadString(std::istream& stream, std::string& value)
        {
            uint32_t length = 0;
            Read(stream, length);
            if (stream.fail())
                return;
            value.resize(length);
            if (length > 0)
                stream.read(value.data(), length);
        }

        static void WriteTexture(std::ostream& stream, const std::string& path, int32_t set)
        {
            WriteString(stream, path);
            Write(stream, set);
        }

        static void ReadTexture(std::istream& stream, std::string& path, int32_t& set)
        {
            ReadString(stream, path);
            Read(stream, set);
        }

        static void WriteTexture(std::ostream& stream, const MaterialTexture& texture) { WriteTexture(stream, texture.Path, texture.Set); }
        static void ReadTexture(std::istream& stream, MaterialTexture& texture) { ReadTexture(stream, texture.Path, texture.Set); }
    };
}
