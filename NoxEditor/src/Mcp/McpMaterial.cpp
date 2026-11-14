#include "McpMaterial.h"

#include <algorithm>
#include <format>
#include <functional>
#include <vector>

namespace Nox::Mcp
{
    namespace
    {
        Json ToJson(float value) { return value; }
        Json ToJson(int32_t value) { return value; }
        Json ToJson(uint32_t value) { return value; }
        Json ToJson(bool value) { return value; }
        Json ToJson(const std::string& value) { return value; }
        Json ToJson(const glm::vec2& value) { return Json::array({ value.x, value.y }); }
        Json ToJson(const glm::vec3& value) { return Json::array({ value.x, value.y, value.z }); }
        Json ToJson(const glm::vec4& value) { return Json::array({ value.x, value.y, value.z, value.w }); }
        Json ToJson(const MaterialTexture& value) { return Json{ { "Path", value.Path }, { "Set", value.Set } }; }
        Json ToJson(AlphaMode value)
        {
            return value == AlphaMode::Mask ? "Mask" : value == AlphaMode::Blend ? "Blend" : "Opaque";
        }

        std::string FromJson(const Json& value, float& out)
        {
            if (!value.is_number())
                return "must be a number";
            out = value.get<float>();
            return {};
        }
        std::string FromJson(const Json& value, int32_t& out)
        {
            if (!value.is_number() || value.get<double>() != static_cast<double>(static_cast<int64_t>(value.get<double>())))
                return "must be a whole number";
            out = value.get<int32_t>();
            return {};
        }
        std::string FromJson(const Json& value, uint32_t& out)
        {
            if (!value.is_number() || value.get<double>() < 0.0 || value.get<double>() != static_cast<double>(static_cast<int64_t>(value.get<double>())))
                return "must be a whole number, 0 or more";
            out = value.get<uint32_t>();
            return {};
        }
        std::string FromJson(const Json& value, bool& out)
        {
            if (!value.is_boolean())
                return "must be true or false";
            out = value.get<bool>();
            return {};
        }
        std::string FromJson(const Json& value, std::string& out)
        {
            if (!value.is_string())
                return "must be a string";
            out = value.get<std::string>();
            return {};
        }
        template<int N, typename Vector>
        std::string FromJsonVector(const Json& value, Vector& out)
        {
            if (!value.is_array() || value.size() != N)
                return std::format("must be {} numbers", N);
            for (int i = 0; i < N; ++i)
            {
                if (!value[i].is_number())
                    return std::format("must be {} numbers", N);
                out[i] = value[i].get<float>();
            }
            return {};
        }
        std::string FromJson(const Json& value, glm::vec2& out) { return FromJsonVector<2>(value, out); }
        std::string FromJson(const Json& value, glm::vec3& out) { return FromJsonVector<3>(value, out); }
        std::string FromJson(const Json& value, glm::vec4& out) { return FromJsonVector<4>(value, out); }
        std::string FromJson(const Json& value, MaterialTexture& out)
        {
            if (!value.is_object())
                return "must be { Path, Set }";
            MaterialTexture texture = out;
            for (auto it = value.begin(); it != value.end(); ++it)
            {
                std::string problem = it.key() == "Path" ? FromJson(it.value(), texture.Path)
                                    : it.key() == "Set"  ? FromJson(it.value(), texture.Set)
                                                         : std::format("has no field '{}' (Path, Set)", it.key());
                if (!problem.empty())
                    return it.key() == "Path" || it.key() == "Set" ? std::format("{}: {}", it.key(), problem) : problem;
            }
            out = texture;
            return {};
        }
        std::string FromJson(const Json& value, AlphaMode& out)
        {
            const std::string name = value.is_string() ? value.get<std::string>() : std::string();
            if (name == "Opaque")
                out = AlphaMode::Opaque;
            else if (name == "Mask")
                out = AlphaMode::Mask;
            else if (name == "Blend")
                out = AlphaMode::Blend;
            else
                return "must be \"Opaque\", \"Mask\" or \"Blend\"";
            return {};
        }

        struct Field
        {
            const char* Name;
            std::function<Json(const MaterialData&)> Get;
            std::function<std::string(const Json&, MaterialData&)> Set;
        };

#define NOX_MATERIAL_FIELD(member) \
        Field{ #member, [](const MaterialData& m) { return ToJson(m.member); }, [](const Json& v, MaterialData& m) { return FromJson(v, m.member); } }

        const std::vector<Field>& Fields()
        {
            static const std::vector<Field> fields = {
                NOX_MATERIAL_FIELD(Name),
                NOX_MATERIAL_FIELD(Workflow),
                NOX_MATERIAL_FIELD(DiffuseFactor),
                NOX_MATERIAL_FIELD(SpecularFactor),
                NOX_MATERIAL_FIELD(BaseColorFactor),
                NOX_MATERIAL_FIELD(BaseColorTexturePath),
                NOX_MATERIAL_FIELD(BaseColorTextureSet),
                NOX_MATERIAL_FIELD(MetallicFactor),
                NOX_MATERIAL_FIELD(RoughnessFactor),
                NOX_MATERIAL_FIELD(MetallicRoughnessTexturePath),
                NOX_MATERIAL_FIELD(PhysicalDescriptorTextureSet),
                NOX_MATERIAL_FIELD(NormalTexturePath),
                NOX_MATERIAL_FIELD(NormalTextureSet),
                NOX_MATERIAL_FIELD(NormalScale),
                NOX_MATERIAL_FIELD(OcclusionTexturePath),
                NOX_MATERIAL_FIELD(OcclusionTextureSet),
                NOX_MATERIAL_FIELD(OcclusionStrength),
                NOX_MATERIAL_FIELD(EmissiveFactor),
                NOX_MATERIAL_FIELD(EmissiveTexturePath),
                NOX_MATERIAL_FIELD(EmissiveTextureSet),
                NOX_MATERIAL_FIELD(emissiveStrength),
                NOX_MATERIAL_FIELD(TransmissionFactor),
                NOX_MATERIAL_FIELD(TransmissionTexturePath),
                NOX_MATERIAL_FIELD(TransmissionTextureSet),
                NOX_MATERIAL_FIELD(IOR),
                NOX_MATERIAL_FIELD(Thickness),
                NOX_MATERIAL_FIELD(AttenuationColor),
                NOX_MATERIAL_FIELD(AttenuationDistance),
                NOX_MATERIAL_FIELD(DiffuseTransmission),
                NOX_MATERIAL_FIELD(PathTracerFlags),
                NOX_MATERIAL_FIELD(DepthBias),
                NOX_MATERIAL_FIELD(Mode),
                NOX_MATERIAL_FIELD(AlphaMaskCutoff),
                NOX_MATERIAL_FIELD(DoubleSided),
                NOX_MATERIAL_FIELD(Unlit),
                NOX_MATERIAL_FIELD(ClearcoatFactor),
                NOX_MATERIAL_FIELD(ClearcoatTexture),
                NOX_MATERIAL_FIELD(ClearcoatRoughnessFactor),
                NOX_MATERIAL_FIELD(ClearcoatRoughnessTexture),
                NOX_MATERIAL_FIELD(ClearcoatNormalTexture),
                NOX_MATERIAL_FIELD(ClearcoatNormalScale),
                NOX_MATERIAL_FIELD(SheenColorFactor),
                NOX_MATERIAL_FIELD(SheenColorTexture),
                NOX_MATERIAL_FIELD(SheenRoughnessFactor),
                NOX_MATERIAL_FIELD(SheenRoughnessTexture),
                NOX_MATERIAL_FIELD(SpecularStrength),
                NOX_MATERIAL_FIELD(SpecularStrengthTexture),
                NOX_MATERIAL_FIELD(SpecularColorFactor),
                NOX_MATERIAL_FIELD(SpecularColorTexture),
                NOX_MATERIAL_FIELD(IridescenceFactor),
                NOX_MATERIAL_FIELD(IridescenceTexture),
                NOX_MATERIAL_FIELD(IridescenceIOR),
                NOX_MATERIAL_FIELD(IridescenceThicknessMinimum),
                NOX_MATERIAL_FIELD(IridescenceThicknessMaximum),
                NOX_MATERIAL_FIELD(IridescenceThicknessTexture),
                NOX_MATERIAL_FIELD(AnisotropyStrength),
                NOX_MATERIAL_FIELD(AnisotropyRotation),
                NOX_MATERIAL_FIELD(AnisotropyTexture),
                NOX_MATERIAL_FIELD(Dispersion),
            };
            return fields;
        }
#undef NOX_MATERIAL_FIELD

        Json ChannelToJson(const MaterialUVChannel& channel)
        {
            return Json{ { "Source", channel.Source }, { "Offset", ToJson(channel.Offset) }, { "Rotation", channel.Rotation }, { "Scale", ToJson(channel.Scale) } };
        }

        std::string ChannelFromJson(const Json& value, MaterialUVChannel& channel)
        {
            if (!value.is_object())
                return "must be { Source, Offset, Rotation, Scale }";
            MaterialUVChannel result = channel;
            for (auto it = value.begin(); it != value.end(); ++it)
            {
                std::string problem;
                if (it.key() == "Source")
                {
                    problem = FromJson(it.value(), result.Source);
                    if (problem.empty() && (result.Source < 0 || result.Source > 1))
                        problem = "must be 0 or 1 (the mesh's UV set)";
                }
                else if (it.key() == "Offset")
                    problem = FromJson(it.value(), result.Offset);
                else if (it.key() == "Rotation")
                    problem = FromJson(it.value(), result.Rotation);
                else if (it.key() == "Scale")
                    problem = FromJson(it.value(), result.Scale);
                else
                    return std::format("has no field '{}' (Source, Offset, Rotation, Scale)", it.key());
                if (!problem.empty())
                    return std::format("{}: {}", it.key(), problem);
            }
            channel = result;
            return {};
        }
    }

    Json MaterialToJson(const MaterialData& material)
    {
        Json json = Json::object();
        for (const Field& field : Fields())
            json[field.Name] = field.Get(material);
        Json channels = Json::array();
        for (const MaterialUVChannel& channel : material.UVChannels)
            channels.push_back(ChannelToJson(channel));
        json["UVChannels"] = channels;
        return json;
    }

    std::string MaterialFromJson(const Json& value, MaterialData& material)
    {
        if (!value.is_object())
            return "the material's values must be an object";
        MaterialData result = material; // (all or nothing)
        for (auto it = value.begin(); it != value.end(); ++it)
        {
            if (it.key() == "UVChannels")
            {
                if (!it.value().is_array() || it.value().size() > static_cast<size_t>(MaxMaterialUVChannels))
                    return std::format("'UVChannels' must be a list of at most {} channels", MaxMaterialUVChannels);
                for (size_t index = 0; index < it.value().size(); ++index)
                    if (std::string problem = ChannelFromJson(it.value()[index], result.UVChannels[index]); !problem.empty())
                        return std::format("'UVChannels/{}' {}", index, problem);
                continue;
            }
            const auto& fields = Fields();
            const auto field = std::find_if(fields.begin(), fields.end(), [&](const Field& f) { return it.key() == f.Name; });
            if (field == fields.end())
                return std::format("unknown material field '{}' (get_material lists them)", it.key());
            if (std::string problem = field->Set(it.value(), result); !problem.empty())
                return std::format("'{}' {}", it.key(), problem);
        }
        material = std::move(result);
        return {};
    }

    std::vector<std::pair<std::string, std::string>> MaterialTexturePaths(const MaterialData& material)
    {
        std::vector<std::pair<std::string, std::string>> paths;
        const Json json = MaterialToJson(material);
        for (auto it = json.begin(); it != json.end(); ++it)
        {
            const std::string& key = it.key();
            if (key.size() > 11 && key.ends_with("TexturePath") && it.value().is_string() && !it.value().get<std::string>().empty())
                paths.emplace_back(key, it.value().get<std::string>());
            else if (it.value().is_object() && it.value().contains("Path") && it.value()["Path"].is_string() && !it.value()["Path"].get<std::string>().empty())
                paths.emplace_back(key + "/Path", it.value()["Path"].get<std::string>());
        }
        return paths;
    }
}
