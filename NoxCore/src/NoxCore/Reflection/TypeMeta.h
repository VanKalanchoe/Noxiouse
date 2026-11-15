#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace Nox
{
    enum class PropertyType : uint8_t
    {
        Bool = 0,
        Int,
        UInt32,
        Float,
        Vec2,
        Vec3,
        Vec4,
        Color3,
        Color4,
        String
    };

    struct PropertyMeta
    {
        std::string Name;
        PropertyType Type = PropertyType::Float;
        float Speed = 0.1f;
        float Min = 0.0f;
        float Max = 0.0f;
        std::string Placeholder;

        std::function<float(const void*)> DynamicMin;
        std::function<float(const void*)> DynamicMax;
        std::function<void(void* outValue, const void* instance)> Read;
        std::function<void(void* instance, const void* inValue)> Write;
    };

    struct TypeMeta
    {
        std::string DisplayName;
        std::vector<PropertyMeta> Properties;
        std::function<void(void* instance)> OnPostEdit;
    };

    template <typename T>
    class TypeBuilder
    {
    public:
        explicit TypeBuilder(TypeMeta& meta) : m_Meta(meta) {}

        TypeBuilder& Bool(std::string name, bool T::*member)
        {
            return AddScalar<bool>(std::move(name), PropertyType::Bool, member, 0.0f, 0.0f, 0.0f);
        }

        TypeBuilder& Int(std::string name, int T::*member, float speed = 1.0f, int min = 0, int max = 0)
        {
            return AddScalar<int>(std::move(name), PropertyType::Int, member, speed,
                                   static_cast<float>(min), static_cast<float>(max));
        }

        TypeBuilder& UInt32(std::string name, uint32_t T::*member, float speed = 1.0f, uint32_t min = 0, uint32_t max = 0)
        {
            return AddScalar<uint32_t>(std::move(name), PropertyType::UInt32, member, speed,
                                        static_cast<float>(min), static_cast<float>(max));
        }

        TypeBuilder& Float(std::string name, float T::*member, float speed = 0.1f, float min = 0.0f, float max = 0.0f)
        {
            return AddScalar<float>(std::move(name), PropertyType::Float, member, speed, min, max);
        }

        TypeBuilder& Vec2(std::string name, glm::vec2 T::*member, float speed = 0.1f)
        {
            return AddScalar<glm::vec2>(std::move(name), PropertyType::Vec2, member, speed, 0.0f, 0.0f);
        }

        TypeBuilder& Vec3(std::string name, glm::vec3 T::*member, float speed = 0.1f)
        {
            return AddScalar<glm::vec3>(std::move(name), PropertyType::Vec3, member, speed, 0.0f, 0.0f);
        }

        TypeBuilder& Vec4(std::string name, glm::vec4 T::*member, float speed = 0.1f)
        {
            return AddScalar<glm::vec4>(std::move(name), PropertyType::Vec4, member, speed, 0.0f, 0.0f);
        }

        TypeBuilder& Color3(std::string name, glm::vec3 T::*member)
        {
            return AddScalar<glm::vec3>(std::move(name), PropertyType::Color3, member, 0.0f, 0.0f, 1.0f);
        }

        TypeBuilder& Color4(std::string name, glm::vec4 T::*member)
        {
            return AddScalar<glm::vec4>(std::move(name), PropertyType::Color4, member, 0.0f, 0.0f, 1.0f);
        }

        TypeBuilder& String(std::string name, std::string T::*member, std::string placeholder = {})
        {
            PropertyMeta prop{};
            prop.Name = std::move(name);
            prop.Type = PropertyType::String;
            prop.Placeholder = std::move(placeholder);
            prop.Read = [member](void* out, const void* inst)
            {
                *static_cast<std::string*>(out) = static_cast<const T*>(inst)->*member;
            };
            prop.Write = [member](void* inst, const void* in)
            {
                static_cast<T*>(inst)->*member = *static_cast<const std::string*>(in);
            };
            m_Meta.Properties.push_back(std::move(prop));
            return *this;
        }

        TypeBuilder& MinFrom(float T::*otherMember)
        {
            if (!m_Meta.Properties.empty())
            {
                m_Meta.Properties.back().DynamicMin = [otherMember](const void* inst) -> float
                {
                    return static_cast<const T*>(inst)->*otherMember;
                };
            }
            return *this;
        }

        TypeBuilder& MaxFrom(float T::*otherMember)
        {
            if (!m_Meta.Properties.empty())
            {
                m_Meta.Properties.back().DynamicMax = [otherMember](const void* inst) -> float
                {
                    return static_cast<const T*>(inst)->*otherMember;
                };
            }
            return *this;
        }

        TypeBuilder& OnPostEdit(std::function<void(T&)> fn)
        {
            m_Meta.OnPostEdit = [cb = std::move(fn)](void* inst)
            {
                if (cb)
                    cb(*static_cast<T*>(inst));
            };
            return *this;
        }

        const TypeMeta& Meta() const { return m_Meta; }

    private:
        template <typename FieldT>
        TypeBuilder& AddScalar(std::string name, PropertyType type, FieldT T::*member, float speed, float min, float max)
        {
            PropertyMeta prop{};
            prop.Name = std::move(name);
            prop.Type = type;
            prop.Speed = speed;
            prop.Min = min;
            prop.Max = max;
            prop.Read = [member](void* out, const void* inst)
            {
                *static_cast<FieldT*>(out) = static_cast<const T*>(inst)->*member;
            };
            prop.Write = [member](void* inst, const void* in)
            {
                static_cast<T*>(inst)->*member = *static_cast<const FieldT*>(in);
            };
            m_Meta.Properties.push_back(std::move(prop));
            return *this;
        }

        TypeMeta& m_Meta;
    };

    class TypeRegistry
    {
    public:
        static TypeRegistry& Get();

        template <typename T>
        TypeBuilder<T> Register(std::string displayName)
        {
            const std::type_index key(typeid(T));
            auto& slot = m_Types[key];
            if (!slot)
                slot = std::make_unique<TypeMeta>();
            slot->DisplayName = std::move(displayName);
            slot->Properties.clear();
            slot->OnPostEdit = nullptr;
            return TypeBuilder<T>(*slot);
        }

        template <typename T>
        const TypeMeta* Find() const
        {
            auto it = m_Types.find(std::type_index(typeid(T)));
            return it != m_Types.end() ? it->second.get() : nullptr;
        }

    private:
        std::unordered_map<std::type_index, std::unique_ptr<TypeMeta>> m_Types;
    };

    template <typename T>
    TypeBuilder<T> ReflectType(std::string displayName)
    {
        return TypeRegistry::Get().Register<T>(std::move(displayName));
    }
}
