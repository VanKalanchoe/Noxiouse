#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "NoxCore/Reflection/TypeMeta.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Widgets/UIScrollView.h"

namespace Nox::UI
{
    class UIContext;

    // Collapsible 2-column inspector grid with convenience builders and reflection-driven auto-generation.
    class UIPropertyGrid : public UIScrollView
    {
    public:
        UIPropertyGrid();

        Ref<UIStackBox> AddSection(const std::string& title, bool expanded = true);
        void AddRow(const std::string& label, const Ref<UIElement>& editor, const Ref<UIStackBox>& section = nullptr);

        void AddBool(const std::string& label, bool value, std::function<void(bool)> onChanged,
                     const Ref<UIStackBox>& section = nullptr);
        void AddFloatSlider(const std::string& label, float value, float min, float max,
                            std::function<void(float)> onChanged, const Ref<UIStackBox>& section = nullptr);
        void AddDragFloat(const std::string& label, float value, float speed,
                          std::function<void(float)> onChanged, const Ref<UIStackBox>& section = nullptr);
        void AddVec3(const std::string& label, glm::vec3 value, float speed,
                     std::function<void(const glm::vec3&)> onChanged, const Ref<UIStackBox>& section = nullptr);
        void AddString(const std::string& label, const std::string& value, const std::string& placeholder,
                       std::function<void(const std::string&)> onChanged, const Ref<UIStackBox>& section = nullptr);

        // Auto-generates rows from TypeMeta with 2-way sync and optional validity guard.
        Ref<UIStackBox> AddReflected(
            UIContext* context,
            void* instance,
            const TypeMeta& meta,
            std::function<bool()> isValid = {},
            const std::string& sectionOverride = {});

        template <typename T>
        Ref<UIStackBox> Inspect(
            UIContext* context,
            T& instance,
            std::function<bool()> isValid = {},
            const std::string& sectionOverride = {})
        {
            const TypeMeta* meta = TypeRegistry::Get().Find<T>();
            if (!meta)
                return nullptr;
            return AddReflected(context, &instance, *meta, std::move(isValid), sectionOverride);
        }

        void ClearProperties();

        float LabelRatio = 0.38f;

    protected:
        void OnUpdate(float dt) override;

    private:
        struct SyncEntry
        {
            std::function<bool()> IsValid;
            std::function<void()> SyncFromModel;
            Ref<UIElement> SectionContainer;
        };
        std::vector<SyncEntry> m_SyncEntries;
    };
}
