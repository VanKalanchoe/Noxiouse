#pragma once

#include <functional>
#include <string>

#include <glm/glm.hpp>

#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Widgets/UIScrollView.h"

namespace Nox::UI
{
    // Collapsible 2-column inspector grid with convenience builders for common property types.
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

        void ClearProperties();

        float LabelRatio = 0.38f;
    };
}
