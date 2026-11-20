#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/Renderer/DataTypes.h"
#include "NoxCore/UI/Core/UIElement.h"
#include "NoxCore/UI/Layout/UILayout.h"

namespace Nox
{
    namespace UI
    {
        class UIContext;
        class UIDragScalar;
        class UIScrollView;
        class UIStackBox;
    }

    struct LengthUnitInfo
    {
        float Factor = 1.0f;
        const char* Suffix = "m";
        bool Meters = true;
    };

    LengthUnitInfo GetLengthUnitInfo(float representativeWorldValue);
    bool ScriptUnitFactor(const std::string& unit, float& worldUnitsPerDisplayUnit);
    std::string AssetDisplayName(AssetHandle handle, const char* emptyText = "None");
    std::string ToLowerStr(std::string s);

    struct AssetPickerOption
    {
        std::string Label;
        std::string Detail;
        AssetHandle Handle = 0;
    };

    std::vector<AssetPickerOption> CollectAssetPickerOptions(const std::vector<AssetType>& acceptedTypes);

    // Opens a fast, searchable, viewport-clamped asset picker popup anchored to triggerBounds.
    void ShowSearchableAssetPicker(
        UI::UIContext& context,
        const UI::UIRect& triggerBounds,
        const std::string& emptyLabel,
        AssetHandle currentHandle,
        std::vector<AssetPickerOption> options,
        std::function<void(AssetHandle)> onPick,
        bool alignRight = false);

    // Sleek pill switch used in the entity header for InactiveComponent toggling.
    class EntityActiveSwitch final : public UI::UIElement
    {
    public:
        explicit EntityActiveSwitch(bool active = true, std::function<void(bool)> onChanged = {});

        bool Checked = true;
        std::function<void(bool)> OnChanged;

        bool OnPointerUp(const UI::UIPointerEvent& e) override;

    protected:
        UI::UIVec2 ComputeDesiredSize() override;
        void OnPaint(UI::UIDrawList& drawList, const UI::UITheme& theme) override;
    };

    struct InspectorCardResult
    {
        Ref<UI::UIStackBox> Card;
        Ref<UI::UIStackBox> Body;
    };

    // Builds a sleek collapsible component card with a vector chevron and right-aligned three-dots menu.
    InspectorCardResult CreateCollapsibleCard(
        const std::string& title,
        bool expanded,
        std::function<void(bool)> onExpandChanged,
        bool removable,
        std::function<void()> onReset,
        std::function<void()> onRemove);

    // The small chevron that opens what is under a row (a material's properties under its slot): it flips its own state and
    // calls onToggle(expanded) -- the caller shows or hides the section, no rebuild of the panel. The three dots open a row's
    // menu (onOpen gets the button's bounds, to anchor the menu).
    Ref<UI::UIElement> CreateExpandToggle(bool expanded, std::function<void(bool)> onToggle, const std::string& tooltip = {});
    Ref<UI::UIElement> CreateMoreButton(std::function<void(const UI::UIRect&)> onOpen, const std::string& tooltip = "Options");

    // A 0..1 (or min..max) value as one field: its fill bar is the slider (drag across it), a click types the exact value.
    Ref<UI::UIDragScalar> CreateSliderField(float value, float minV, float maxV, std::function<void(float)> onChanged);

    // Two-column property row builder with live two-way sync callbacks. The editors it makes keep copies of what they call
    // (OnMutate), never a pointer to the builder: builders are locals of the function that fills a card.
    // The element a Ref or a UI builder holds, by raw pointer: a sync (UIElement::OnSync) lives in the element itself, so it must not
    // hold a Ref to it.
    template <typename Holder>
    auto* Raw(const Holder& holder)
    {
        return holder.operator->();
    }

    struct InspectorCardBuilder
    {
        Ref<UI::UIStackBox> Body;
        std::function<void(bool)> OnMutate;
        UI::UIContext* Context = nullptr;

        static constexpr float kLabelRatio = 0.39f;

        void AddRow(const std::string& labelText, const Ref<UI::UIElement>& editor, const std::string& tooltip = {});
        void AddSubheader(const std::string& title, const std::string& tooltip = {});
        void AddSeparator();
        void AddStatusNote(const std::string& text, const glm::vec4& color = { 0.60f, 0.66f, 0.76f, 1.0f });
        void AddBool(const std::string& label, bool* target, std::function<void()> onChange = {}, const std::string& tooltip = {});
        void AddFloat(const std::string& label, float* target, float speed = 0.1f, float minV = 0.0f, float maxV = 0.0f,
                      const char* fmt = "{:.3f}", std::function<void()> onChange = {}, const std::string& tooltip = {});
        void AddSlider(const std::string& label, float* target, float minV, float maxV,
                       std::function<void()> onChange = {}, const std::string& tooltip = {});
        void AddUInt32(const std::string& label, uint32_t* target, uint32_t minV = 0, uint32_t maxV = 0,
                       std::function<void()> onChange = {}, const std::string& tooltip = {});
        void AddInt32(const std::string& label, int32_t* target, int32_t minV = 0, int32_t maxV = 0,
                      std::function<void()> onChange = {}, const std::string& tooltip = {});
        void AddLength(const std::string& label, float* worldValue, float minWorld = 0.0f, float maxWorld = 0.0f,
                       const char* rateSuffix = "", std::function<void()> onChange = {}, const std::string& tooltip = {});
        void AddVec2(const std::string& label, glm::vec2* target, float speed = 0.05f,
                     std::function<void()> onChange = {}, const std::string& tooltip = {});
        void AddVec3(const std::string& label, glm::vec3* target, float speed = 0.05f,
                     std::function<void()> onChange = {}, const std::string& tooltip = {});
        void AddLengthVec3(const std::string& label, glm::vec3* worldVec,
                           std::function<void()> onChange = {}, const std::string& tooltip = {});
        void AddColor3(const std::string& label, glm::vec3* target, std::function<void()> onChange = {});
        void AddColor4(const std::string& label, glm::vec4* target, std::function<void()> onChange = {});
        void AddString(const std::string& label, std::string* target, const std::string& placeholder = {},
                       std::function<void()> onChange = {}, const std::string& tooltip = {});
        void AddReadonly(const std::string& label, const std::string& value);
        void AddEnum(const std::string& label, std::vector<std::string> items, int selectedIdx,
                     std::function<void(int)> onSelect, const std::string& tooltip = {});
        void AddAssetSlot(const std::string& label, AssetHandle current,
                          std::vector<AssetType> acceptedTypes,
                          std::function<void(AssetHandle)> onAssign,
                          const char* emptyText = "None");
    };

    struct MaterialPropertiesCallbacks
    {
        std::function<void(AssetHandle)> OpenAsset;
        std::function<void()> RequestRebuild;
        // A line at the top, shown while the material is unsaved: Save / Revert (the Inspector's card; the Material window has
        // its own toolbar).
        bool InlineSave = false;
    };

    // An edit of a material's values: drawn with them now, marked unsaved (MaterialEdits; nothing is written per change).
    void NotifyMaterialEdited(AssetHandle handle);

    // Populates full native Nox::UI material properties (used by Quick Edit in Inspector AND standalone Material Editor V2 windows).
    void PopulateMaterialPropertiesUI(
        AssetHandle handle,
        MaterialData& data,
        InspectorCardBuilder& c,
        std::unordered_map<std::string, bool>& expandedSections,
        const MaterialPropertiesCallbacks& callbacks = {});
}
