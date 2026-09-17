#pragma once
#include <memory>
#include <type_traits>
#include <unordered_map>

#include <entt/core/type_info.hpp>

#include "NoxCore/Core/core.h"

namespace Nox
{
    // Typed per-frame store for the handles features share with each other (Decision D2), e.g. GBufferOutputs written
    // by the G-buffer feature and read by lighting. Storage persists across frames (no per-frame allocation); entries
    // are only valid in the frame they were added.
    class RGBlackboard
    {
    public:
        template <typename T>
        T& Add(T value)
        {
            Holder<T>& holder = GetHolder<T>();
            holder.Value = std::move(value);
            holder.ValidFrame = m_Frame;
            return holder.Value;
        }

        template <typename T>
        T* TryGet()
        {
            auto found = m_Entries.find(entt::type_hash<T>::value());
            if (found == m_Entries.end() || found->second->ValidFrame != m_Frame)
                return nullptr;
            return &static_cast<Holder<T>*>(found->second.get())->Value;
        }

        template <typename T>
        T& Get()
        {
            T* value = TryGet<T>();
            NOX_CORE_ASSERT(value, "RGBlackboard entry was not added this frame");
            return *value;
        }

        // Invalidates every entry (called when a new graph is built).
        void NextFrame() { ++m_Frame; }

    private:
        struct HolderBase
        {
            virtual ~HolderBase() = default;
            uint64_t ValidFrame = ~0ull;
        };

        template <typename T>
        struct Holder final : HolderBase
        {
            T Value{};
        };

        template <typename T>
        Holder<T>& GetHolder()
        {
            std::unique_ptr<HolderBase>& entry = m_Entries[entt::type_hash<T>::value()];
            if (!entry)
                entry = std::make_unique<Holder<T>>();
            return static_cast<Holder<T>&>(*entry);
        }

    private:
        std::unordered_map<entt::id_type, std::unique_ptr<HolderBase>> m_Entries;
        uint64_t m_Frame = 0;
    };
}
