#include "InspectorWidgetsV2.h"
#include "MaterialEdits.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>

#include <SDL3/SDL_misc.h>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Asset/Material.h"
#include "NoxCore/Asset/MaterialSerializer.h"
#include "NoxCore/Audio/SpatialAudio.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/NodeGraph/NodeGraphAsset.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/MaterialGraph.h"
#include "NoxCore/Renderer/Renderer.h"
#include "Panels/AssetPreviewRenderer.h"
#include "NoxCore/UI/Widgets/UIPropertyRows.h"
#include "NoxCore/UI/Widgets/UISearchPicker.h"
#include "NoxCore/UI/Widgets/UIIconButton.h"
#include "NoxCore/UI/Core/UIBuilder.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Rendering/UIIcons.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UICheckBox.h"
#include "NoxCore/UI/Widgets/UIColorPicker.h"
#include "NoxCore/UI/Widgets/UIDragScalar.h"
#include "NoxCore/UI/Widgets/UIDropdown.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIMenu.h"
#include "NoxCore/UI/Widgets/UIScrollView.h"
#include "NoxCore/UI/Widgets/UISlider.h"
#include "NoxCore/UI/Widgets/UITextInput.h"
#include "NoxCore/UI/Widgets/UIVectorEdit.h"

namespace Nox
{
    namespace
    {
        // The "..." button of a card or row (opens a menu under itself).
        Ref<UI::UIIconButton> MakeMoreButton(std::function<void(const UI::UIRect&)> onOpen, std::string tooltip)
        {
            Ref<UI::UIIconButton> button(new UI::UIIconButton([](UI::UIDrawList& drawList, const UI::UIRect& b, const UI::UIColor& color)
            {
                UI::Icons::DrawMoreDots(drawList, b.Center(), 12.0f, color, true);
            }, std::move(tooltip)));
            button->FixedSize = { 24.0f, 22.0f };
            button->OnClickAt = std::move(onOpen);
            return button;
        }



        // A small chevron that opens and closes what is under its row (a material's properties under its slot).


        std::string TexturePathDisplayName(const std::string& path, const char* emptyText = "None")
        {
            if (path.empty())
                return emptyText;
            return std::filesystem::path(path).filename().string();
        }
    }

    std::string ToLowerStr(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    LengthUnitInfo GetLengthUnitInfo(float representativeWorldValue)
    {
        const WorldUnits::DisplayUnit displayUnit = WorldUnits::GetDisplayUnit();
        bool useMeters = displayUnit == WorldUnits::DisplayUnit::Meters;
        if (displayUnit == WorldUnits::DisplayUnit::Auto)
            useMeters = WorldUnits::ToMeters(std::abs(representativeWorldValue)) >= 1.0f;
        return useMeters
            ? LengthUnitInfo{ WorldUnits::FromMeters(1.0f), "m", true }
            : LengthUnitInfo{ WorldUnits::FromCentimeters(1.0f), "cm", false };
    }

    bool ScriptUnitFactor(const std::string& unit, float& worldUnitsPerDisplayUnit)
    {
        if (unit == "cm" || unit == "cm/s") { worldUnitsPerDisplayUnit = WorldUnits::FromCentimeters(1.0f); return true; }
        if (unit == "m" || unit == "m/s") { worldUnitsPerDisplayUnit = WorldUnits::FromMeters(1.0f); return true; }
        if (unit == "mm") { worldUnitsPerDisplayUnit = WorldUnits::FromMeters(0.001f); return true; }
        if (unit == "km/h") { worldUnitsPerDisplayUnit = WorldUnits::FromMeters(1000.0f) / 3600.0f; return true; }
        return false;
    }

    std::string AssetDisplayName(AssetHandle handle, const char* emptyText)
    {
        if (handle == 0)
            return emptyText;
        if (!AssetManager::IsAssetHandleValid(handle) || !Project::GetActive() || !Project::GetActive()->GetEditorAssetManager())
            return "Invalid Asset";
        const AssetMetadata meta = Project::GetActive()->GetEditorAssetManager()->GetMetadata(handle);
        if (meta.FilePath.empty())
            return "(built at run time)";
        return meta.FilePath.filename().string();
    }

    std::vector<AssetPickerOption> CollectAssetPickerOptions(const std::vector<AssetType>& acceptedTypes)
    {
        std::vector<AssetPickerOption> out;
        if (!Project::GetActive() || !Project::GetActive()->GetEditorAssetManager())
            return out;
        for (const auto& [handle, meta] : Project::GetActive()->GetEditorAssetManager()->GetAssetRegistry())
        {
            if (std::find(acceptedTypes.begin(), acceptedTypes.end(), meta.Type) == acceptedTypes.end())
                continue;
            AssetPickerOption opt;
            opt.Handle = handle;
            if (meta.FilePath.empty())
            {
                opt.Label = std::to_string(static_cast<uint64_t>(handle));
            }
            else
            {
                opt.Label = meta.FilePath.filename().string();
                const std::string parent = meta.FilePath.parent_path().generic_string();
                if (!parent.empty())
                    opt.Detail = parent;
            }
            out.push_back(std::move(opt));
        }
        std::sort(out.begin(), out.end(), [](const AssetPickerOption& a, const AssetPickerOption& b)
        {
            const int cmp = ToLowerStr(a.Label).compare(ToLowerStr(b.Label));
            if (cmp != 0)
                return cmp < 0;
            return a.Detail < b.Detail;
        });
        return out;
    }

    void ShowSearchableAssetPicker(
        UI::UIContext& context,
        const UI::UIRect& triggerBounds,
        const std::string& emptyLabel,
        AssetHandle currentHandle,
        std::vector<AssetPickerOption> options,
        std::function<void(AssetHandle)> onPick,
        bool alignRight)
    {
        // The library's searchable popup (UI::UISearchPicker) over the assets; Id is the handle.
        std::vector<UI::UISearchPickerOption> rows;
        rows.reserve(options.size());
        for (AssetPickerOption& option : options)
            rows.push_back({ std::move(option.Label), std::move(option.Detail), static_cast<uint64_t>(option.Handle) });
        UI::UISearchPickerSettings settings;
        settings.Placeholder = "Filter assets...";
        settings.NoneLabel = emptyLabel;
        settings.NoMatches = "No matching assets";
        settings.AlignRight = alignRight;
        UI::UISearchPicker::Show(context, triggerBounds, std::move(rows), static_cast<uint64_t>(currentHandle),
                                 [onPick = std::move(onPick)](uint64_t id) { if (onPick) onPick(AssetHandle(id)); }, std::move(settings));
    }

    EntityActiveSwitch::EntityActiveSwitch(bool active, std::function<void(bool)> onChanged)
        : Checked(active), OnChanged(std::move(onChanged))
    {
        HitTestMode = UI::UIHitTestMode::Self;
        Focusable = true;
    }

    bool EntityActiveSwitch::OnPointerUp(const UI::UIPointerEvent& e)
    {
        if (e.Button == 1 && IsEnabled() && GetBounds().Contains(e.Position))
        {
            Checked = !Checked;
            if (OnChanged)
                OnChanged(Checked);
            return true;
        }
        return false;
    }

    UI::UIVec2 EntityActiveSwitch::ComputeDesiredSize()
    {
        return { 36.0f, 20.0f };
    }

    void EntityActiveSwitch::OnPaint(UI::UIDrawList& drawList, const UI::UITheme& theme)
    {
        const UI::UIRect& b = GetBounds();
        const bool hovered = UI::HasState(GetState(), UI::UIState::Hovered);
        const float trackW = 34.0f;
        const float trackH = 18.0f;
        const float x0 = b.Min.x + (b.Width() - trackW) * 0.5f;
        const float y0 = b.Min.y + (b.Height() - trackH) * 0.5f;
        const UI::UIVec2 tMin{ x0, y0 };
        const UI::UIVec2 tMax{ x0 + trackW, y0 + trackH };
        const float radius = trackH * 0.5f;

        const UI::UIColor accent = theme.GetToken("Success", { 0.235f, 0.678f, 0.482f, 1.0f });
        const UI::UIColor trackOff = theme.GetToken("Surface2", { 0.165f, 0.192f, 0.267f, 1.0f });
        const UI::UIColor borderCol = hovered
            ? theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f })
            : (Checked ? accent : theme.GetToken("Border", { 0.20f, 0.24f, 0.32f, 1.0f }));
        const UI::UIColor trackFill = Checked ? UI::UIColor{ accent.r, accent.g, accent.b, 0.92f } : trackOff;

        drawList.AddRoundedRect(tMin, tMax, trackFill, glm::vec4(radius), 1.0f, borderCol);

        const float knobSize = 12.0f;
        const float knobY = y0 + (trackH - knobSize) * 0.5f;
        const float knobX = Checked ? (tMax.x - knobSize - 3.0f) : (tMin.x + 3.0f);
        const UI::UIColor knobColor = Checked
            ? UI::UIColor{ 0.96f, 0.98f, 1.0f, 1.0f }
            : UI::UIColor{ 0.60f, 0.65f, 0.75f, 1.0f };
        drawList.AddRoundedRect(
            { knobX, knobY },
            { knobX + knobSize, knobY + knobSize },
            knobColor,
            glm::vec4(knobSize * 0.5f)
        );
    }

    InspectorCardResult CreateCollapsibleCard(
        const std::string& title,
        bool expanded,
        std::function<void(bool)> onExpandChanged,
        bool removable,
        std::function<void()> onReset,
        std::function<void()> onRemove)
    {
        // The library's card (UI::UICollapsibleCard); its "..." menu: Reset Component / Remove Component.
        std::function<std::vector<UI::UIMenuItem>()> menu;
        if (onReset || (removable && onRemove))
            menu = [onReset, removable, onRemove]()
            {
                std::vector<UI::UIMenuItem> items;
                if (onReset)
                    items.push_back({ "Reset Component", "", onReset });
                if (removable && onRemove)
                {
                    if (!items.empty())
                        items.push_back({ "", "", {}, true });
                    items.push_back({ "Remove Component", "", onRemove });
                }
                return items;
            };
        UI::UICollapsibleCard card = UI::UICollapsibleCard::Create(title, expanded, std::move(onExpandChanged), std::move(menu), "Component options");
        return { card.Card, card.Body };
    }

    Ref<UI::UIElement> CreateExpandToggle(bool expanded, std::function<void(bool)> onToggle, const std::string& tooltip)
    {
        // A chevron that opens or closes something: the accent colour while open or under the pointer.
        Ref<UI::UIIconButton> button(new UI::UIIconButton({}, tooltip));
        UI::UIIconButton* raw = button.get(); // (its Active is the open state)
        button->DrawIcon = [raw](UI::UIDrawList& drawList, const UI::UIRect& b, const UI::UIColor& color)
        {
            UI::Icons::DrawChevron(drawList, b.Center(), 8.0f, raw->Active, color, 1.7f);
        };
        button->FixedSize = { 22.0f, 24.0f };
        button->Toggle = true;
        button->Active = expanded;
        button->ActiveStyle = UI::UIIconButton::ActiveLook::Tint;
        button->OnToggled = std::move(onToggle);
        return button;
    }

    Ref<UI::UIElement> CreateMoreButton(std::function<void(const UI::UIRect&)> onOpen, const std::string& tooltip)
    {
        return MakeMoreButton(std::move(onOpen), tooltip);
    }

    void InspectorCardBuilder::AddRow(const std::string& labelText, const Ref<UI::UIElement>& editor, const std::string& tooltip)
    {
        UI::UIPropertyRows(Body, Context).Row(labelText, editor, tooltip);
    }

    void InspectorCardBuilder::AddSubheader(const std::string& title, const std::string& tooltip)
    {
        UI::UIPropertyRows(Body, Context).Header(title, tooltip);
    }

    void InspectorCardBuilder::AddSeparator()
    {
        UI::UIPropertyRows(Body, Context).Separator();
    }

    void InspectorCardBuilder::AddStatusNote(const std::string& text, const glm::vec4& color)
    {
        auto lbl = UI::Label(text);
        lbl->FontSizeOverride = 13.5f;
        lbl->UseColorOverride = true;
        lbl->ColorOverride = color;
        Body->AddChild(lbl);
    }

    void InspectorCardBuilder::AddBool(const std::string& label, bool* target, std::function<void()> onChange, const std::string& tooltip)
    {
        auto cb = UI::CheckBox("", *target, [mutate = OnMutate, target, onChange](bool v)
        {
            *target = v;
            if (onChange) onChange();
            if (mutate) mutate(false);
        });
        AddRow(label, cb, tooltip);
        UI::AppendSync(*Raw(cb), [cb = Raw(cb), target]()
        {
            if (!UI::HasState(cb->GetState(), UI::UIState::Pressed))
                cb->Checked = *target;
        });
    }

    void InspectorCardBuilder::AddFloat(const std::string& label, float* target, float speed, float minV, float maxV,
                                        const char* fmt, std::function<void()> onChange, const std::string& tooltip)
    {
        auto drag = UI::MakeUI<UI::UIDragScalar>(*target, speed, [mutate = OnMutate, target, onChange](float v)
        {
            *target = v;
            if (onChange) onChange();
            if (mutate) mutate(false);
        });
        drag->WidthMode = UI::UISizeMode::Fill;
        drag->Min = minV;
        drag->Max = maxV;
        drag->Format = fmt;
        AddRow(label, drag, tooltip);
        UI::AppendSync(*Raw(drag), [drag = Raw(drag), target]()
        {
            if (!drag->IsInteracting())
                drag->Value = *target;
        });
    }

    Ref<UI::UIDragScalar> CreateSliderField(float value, float minV, float maxV, std::function<void(float)> onChanged)
    {
        return UI::UIPropertyRows::SliderField(value, minV, maxV, std::move(onChanged));
    }

    void InspectorCardBuilder::AddSlider(const std::string& label, float* target, float minV, float maxV,
                                         std::function<void()> onChange, const std::string& tooltip)
    {
        // A slider you can also type into: the number field with its fill bar (Unreal's numeric entry box).
        auto slider = CreateSliderField(*target, minV, maxV, [mutate = OnMutate, target, onChange](float v)
        {
            *target = v;
            if (onChange) onChange();
            if (mutate) mutate(false);
        });
        if (!tooltip.empty())
            slider->Tooltip = tooltip;
        AddRow(label, slider, tooltip);
        UI::AppendSync(*Raw(slider), [slider = Raw(slider), target]()
        {
            if (!slider->IsInteracting())
                slider->Value = *target;
        });
    }

    void InspectorCardBuilder::AddUInt32(const std::string& label, uint32_t* target, uint32_t minV, uint32_t maxV,
                                         std::function<void()> onChange, const std::string& tooltip)
    {
        auto drag = UI::MakeUI<UI::UIDragScalar>(static_cast<float>(*target), 0.25f, [mutate = OnMutate, target, minV, maxV, onChange](float v)
        {
            uint32_t next = static_cast<uint32_t>(std::max(0.0f, std::round(v)));
            if (maxV > minV)
                next = std::clamp(next, minV, maxV);
            *target = next;
            if (onChange) onChange();
            if (mutate) mutate(false);
        });
        drag->WidthMode = UI::UISizeMode::Fill;
        drag->Format = "{:.0f}";
        drag->Min = static_cast<float>(minV);
        drag->Max = static_cast<float>(maxV);
        AddRow(label, drag, tooltip);
        UI::AppendSync(*Raw(drag), [drag = Raw(drag), target]()
        {
            if (!drag->IsInteracting())
                drag->Value = static_cast<float>(*target);
        });
    }

    void InspectorCardBuilder::AddInt32(const std::string& label, int32_t* target, int32_t minV, int32_t maxV,
                                        std::function<void()> onChange, const std::string& tooltip)
    {
        auto drag = UI::MakeUI<UI::UIDragScalar>(static_cast<float>(*target), 0.25f, [mutate = OnMutate, target, minV, maxV, onChange](float v)
        {
            int32_t next = static_cast<int32_t>(std::round(v));
            if (maxV > minV)
                next = std::clamp(next, minV, maxV);
            *target = next;
            if (onChange) onChange();
            if (mutate) mutate(false);
        });
        drag->WidthMode = UI::UISizeMode::Fill;
        drag->Format = "{:.0f}";
        drag->Min = static_cast<float>(minV);
        drag->Max = static_cast<float>(maxV);
        AddRow(label, drag, tooltip);
        UI::AppendSync(*Raw(drag), [drag = Raw(drag), target]()
        {
            if (!drag->IsInteracting())
                drag->Value = static_cast<float>(*target);
        });
    }

    void InspectorCardBuilder::AddLength(const std::string& label, float* worldValue, float minWorld, float maxWorld,
                                         const char* rateSuffix, std::function<void()> onChange, const std::string& tooltip)
    {
        const LengthUnitInfo unit = GetLengthUnitInfo(*worldValue);
        const float displayVal = *worldValue / unit.Factor;
        const float speed = std::max(unit.Meters ? 0.005f : 0.5f, std::abs(displayVal) * 0.02f);
        auto drag = UI::MakeUI<UI::UIDragScalar>(displayVal, speed, [mutate = OnMutate, worldValue, factor = unit.Factor, onChange](float d)
        {
            *worldValue = d * factor;
            if (onChange) onChange();
            if (mutate) mutate(false);
        });
        drag->WidthMode = UI::UISizeMode::Fill;
        drag->Min = minWorld / unit.Factor;
        drag->Max = maxWorld / unit.Factor;
        AddRow(label + " (" + unit.Suffix + rateSuffix + ")", drag, tooltip);
        UI::AppendSync(*Raw(drag), [drag = Raw(drag), worldValue, factor = unit.Factor]()
        {
            if (!drag->IsInteracting())
                drag->Value = *worldValue / factor;
        });
    }

    void InspectorCardBuilder::AddVec2(const std::string& label, glm::vec2* target, float speed,
                                       std::function<void()> onChange, const std::string& tooltip)
    {
        auto vec = UI::MakeUI<UI::UIVectorEdit>(2, glm::vec4(target->x, target->y, 0.0f, 0.0f), speed,
            [mutate = OnMutate, target, onChange](const glm::vec4& v)
            {
                target->x = v.x;
                target->y = v.y;
                if (onChange) onChange();
                if (mutate) mutate(false);
            });
        AddRow(label, vec, tooltip);
        UI::AppendSync(*Raw(vec), [vec = Raw(vec), target]()
        {
            if (!vec->IsEditing())
                vec->SetValue({ target->x, target->y, 0.0f, 0.0f }, false);
        });
    }

    void InspectorCardBuilder::AddVec3(const std::string& label, glm::vec3* target, float speed,
                                       std::function<void()> onChange, const std::string& tooltip)
    {
        auto vec = UI::MakeUI<UI::UIVectorEdit>(3, glm::vec4(*target, 0.0f), speed,
            [mutate = OnMutate, target, onChange](const glm::vec4& v)
            {
                target->x = v.x;
                target->y = v.y;
                target->z = v.z;
                if (onChange) onChange();
                if (mutate) mutate(false);
            });
        AddRow(label, vec, tooltip);
        UI::AppendSync(*Raw(vec), [vec = Raw(vec), target]()
        {
            if (!vec->IsEditing())
                vec->SetValue(glm::vec4(*target, 0.0f), false);
        });
    }

    void InspectorCardBuilder::AddLengthVec3(const std::string& label, glm::vec3* worldVec,
                                             std::function<void()> onChange, const std::string& tooltip)
    {
        const float largest = std::max({ std::abs(worldVec->x), std::abs(worldVec->y), std::abs(worldVec->z) });
        const LengthUnitInfo unit = GetLengthUnitInfo(largest);
        const float speed = unit.Meters ? 0.05f : 5.0f;
        const glm::vec3 initial = *worldVec / unit.Factor;
        auto vec = UI::MakeUI<UI::UIVectorEdit>(3, glm::vec4(initial, 0.0f), speed,
            [mutate = OnMutate, worldVec, factor = unit.Factor, onChange](const glm::vec4& v)
            {
                *worldVec = glm::vec3(v.x, v.y, v.z) * factor;
                if (onChange) onChange();
                if (mutate) mutate(false);
            });
        AddRow(label + " (" + unit.Suffix + ")", vec, tooltip);
        UI::AppendSync(*Raw(vec), [vec = Raw(vec), worldVec, factor = unit.Factor]()
        {
            if (!vec->IsEditing())
                vec->SetValue(glm::vec4(*worldVec / factor, 0.0f), false);
        });
    }

    void InspectorCardBuilder::AddColor3(const std::string& label, glm::vec3* target, std::function<void()> onChange)
    {
        UI::UIPropertyRows(Body, Context).Color3(label, target, [mutate = OnMutate, onChange]()
        {
            if (onChange) onChange();
            if (mutate) mutate(false);
        });
    }

    void InspectorCardBuilder::AddColor4(const std::string& label, glm::vec4* target, std::function<void()> onChange)
    {
        UI::UIPropertyRows(Body, Context).Color4(label, target, [mutate = OnMutate, onChange]()
        {
            if (onChange) onChange();
            if (mutate) mutate(false);
        });
    }

    void InspectorCardBuilder::AddString(const std::string& label, std::string* target, const std::string& placeholder,
                                         std::function<void()> onChange, const std::string& tooltip)
    {
        UI::UIPropertyRows(Body, Context).String(label, target, placeholder, [mutate = OnMutate, onChange]()
        {
            if (onChange) onChange();
            if (mutate) mutate(false);
        }, tooltip);
    }

    void InspectorCardBuilder::AddReadonly(const std::string& label, const std::string& value)
    {
        auto lbl = UI::Label(value);
        lbl->FontSizeOverride = 14.5f;
        lbl->UseColorOverride = true;
        lbl->ColorOverride = { 0.64f, 0.70f, 0.80f, 1.0f };
        AddRow(label, lbl);
    }

    void InspectorCardBuilder::AddEnum(const std::string& label, std::vector<std::string> items, int selectedIdx,
                                       std::function<void(int)> onSelect, const std::string& tooltip)
    {
        auto dd = UI::MakeUI<UI::UIDropdown>(Context, std::move(items), selectedIdx,
            [mutate = OnMutate, onSelect](int idx)
            {
                if (onSelect) onSelect(idx);
                if (mutate) mutate(true);
            });
        dd->WidthMode = UI::UISizeMode::Fill;
        AddRow(label, dd, tooltip);
    }

    void InspectorCardBuilder::AddAssetSlot(const std::string& label, AssetHandle current,
                                            std::vector<AssetType> acceptedTypes,
                                            std::function<void(AssetHandle)> onAssign,
                                            const char* emptyText)
    {
        auto slotRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 4.0f);
        slotRow->WidthMode = UI::UISizeMode::Fill;
        slotRow->VAlign = UI::UIAlignment::Center;

        auto pickBtn = UI::Button(AssetDisplayName(current, emptyText));
        pickBtn->WidthMode = UI::UISizeMode::Fill;
        pickBtn->FlexWeight = 1.0f;
        pickBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
        pickBtn->TextElide = UI::UITextElide::Middle; // an asset's name keeps its start and its end
        UI::UIButton* btnPtr = pickBtn.Get().get();
        std::string emptyStr = emptyText ? emptyText : "None";
        pickBtn->OnClick = [mutate = OnMutate, btnPtr, current, emptyStr, acceptedTypes = std::move(acceptedTypes), onAssign]()
        {
            UI::UIContext* ctx = UI::UIContext::GetCurrent();
            if (!ctx)
                return;
            ShowSearchableAssetPicker(
                *ctx,
                btnPtr->GetBounds(),
                emptyStr,
                current,
                CollectAssetPickerOptions(acceptedTypes),
                [mutate, onAssign](AssetHandle picked)
                {
                    if (onAssign) onAssign(picked);
                    if (mutate) mutate(true);
                },
                false);
        };
        slotRow->AddChild(pickBtn);

        if (current != 0)
        {
            auto clearBtn = UI::Button("x", [mutate = OnMutate, onAssign]()
            {
                if (onAssign) onAssign(AssetHandle(0));
                if (mutate) mutate(true);
            }).Tooltip("Clear asset");
            clearBtn->WidthMode = UI::UISizeMode::Fixed;
            clearBtn->HeightMode = UI::UISizeMode::Fixed;
            clearBtn->FixedSize = { 24.0f, 24.0f };
            clearBtn->Padding = UI::UIThickness::All(0.0f);
            slotRow->AddChild(clearBtn);
        }

        AddRow(label, slotRow);
    }

    void NotifyMaterialEdited(AssetHandle handle)
    {
        // Changed in memory and marked unsaved; the Material window's Save (or the editor's Save All / "Save changes?") writes it.
        MaterialEdits::MarkEdited(handle);
    }

    void PopulateMaterialPropertiesUI(
        AssetHandle handle,
        MaterialData& data,
        InspectorCardBuilder& c,
        std::unordered_map<std::string, bool>& expandedSections,
        const MaterialPropertiesCallbacks& callbacks)
    {
        if (callbacks.InlineSave)
        {
            auto line = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 6.0f);
            line->WidthMode = UI::UISizeMode::Fill;
            auto label = UI::Label("Unsaved changes", 13.0f);
            label->UseColorOverride = true;
            label->ColorOverride = { 0.95f, 0.65f, 0.3f, 1.0f };
            label->VAlign = UI::UIAlignment::Center;
            line->AddChild(label);
            auto save = UI::Button("Save", [handle]() { MaterialEdits::Save(handle); });
            save->StyleVariant = "primary";
            save->Tooltip = "Write the material to its file (File > Save All saves every unsaved material)";
            line->AddChild(save);
            auto revert = UI::Button("Revert", [handle, rebuild = callbacks.RequestRebuild]()
            {
                if (MaterialEdits::Revert(handle) && rebuild)
                    rebuild(); // (the rows pointed at the values that were replaced)
            });
            revert->Tooltip = "Drop the changes: the material as its file has it";
            line->AddChild(revert);
            Ref<UI::UIStackBox> lineRef = line;
            c.Body->AddChild(lineRef);
            UI::AppendSync(*lineRef, [line = lineRef.get(), handle]() { line->Visible = MaterialEdits::IsUnsaved(handle); });
        }

        auto notifySaved = [handle]() { NotifyMaterialEdited(handle); };
        // By value: the callbacks are a temporary of the caller, and these run long after this function returned.
        auto notifySavedRebuild = [handle, requestRebuild = callbacks.RequestRebuild]()
        {
            NotifyMaterialEdited(handle);
            if (requestRebuild)
                requestRebuild();
        };

        if (handle != 0 && Project::GetActive() && Project::GetActive()->GetEditorAssetManager())
        {
            auto assets = Project::GetActive()->GetEditorAssetManager();
            const AcousticMaterial chosen = assets->GetMetadata(handle).AudioSettings.Acoustic;
            const AcousticMaterial resolved = ResolveAcousticMaterial(chosen, data.MetallicFactor, data.TransmissionFactor, data.Mode == AlphaMode::Blend);
            std::vector<std::string> acousticNames;
            for (uint8_t i = 0; i < static_cast<uint8_t>(AcousticMaterial::Count); ++i)
            {
                const AcousticMaterial opt = static_cast<AcousticMaterial>(i);
                if (opt == AcousticMaterial::Auto)
                    acousticNames.push_back("Auto (" + std::string(AcousticMaterialToString(resolved)) + ")");
                else
                    acousticNames.push_back(std::string(AcousticMaterialToString(opt)));
            }
            c.AddEnum("Acoustic Material", std::move(acousticNames), static_cast<int>(chosen),
                [assets, handle](int idx)
                {
                    assets->SetAcousticMaterial(handle, static_cast<AcousticMaterial>(idx));
                    SpatialAudio::MaterialsChanged();
                },
                "How this surface transmits and absorbs sound in Steam Audio.");

            const auto& surfaces = Project::GetActive()->GetConfig().PhysicalSurfaces;
            const std::string curSurf = assets->GetMetadata(handle).PhysicalSurface;
            std::vector<std::string> surfItems = { "Default" };
            int surfIdx = 0;
            for (const std::string& s : surfaces)
            {
                if (s.empty() || s == "Default")
                    continue;
                if (s == curSurf)
                    surfIdx = static_cast<int>(surfItems.size());
                surfItems.push_back(s);
            }
            c.AddEnum("Physical Surface", std::move(surfItems), surfIdx,
                [assets, handle, surfaces](int idx)
                {
                    std::string picked;
                    if (idx > 0)
                    {
                        int cursor = 1;
                        for (const std::string& s : surfaces)
                        {
                            if (s.empty() || s == "Default")
                                continue;
                            if (cursor == idx) { picked = s; break; }
                            ++cursor;
                        }
                    }
                    assets->SetPhysicalSurface(handle, picked);
                },
                "Surface type for footsteps / physics sounds.");
            c.AddSeparator();
        }

        auto addTextureSlot = [&](const std::string& label, std::string& texturePath, const std::string& tooltip = {})
        {
            auto texRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 4.0f);
            texRow->WidthMode = UI::UISizeMode::Fill;
            texRow->VAlign = UI::UIAlignment::Center;

            auto assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr;
            const AssetHandle currentHandle = (assets && !texturePath.empty())
                ? assets->FindHandleByPath(texturePath, AssetType::Texture2D)
                : AssetHandle(0);

            auto pickBtn = UI::Button(TexturePathDisplayName(texturePath));
            pickBtn->WidthMode = UI::UISizeMode::Fill;
            pickBtn->FlexWeight = 1.0f;
            pickBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
            pickBtn->TextElide = UI::UITextElide::Middle;
        pickBtn->TextElide = UI::UITextElide::Middle; // an asset's name keeps its start and its end
            UI::UIButton* btnPtr = pickBtn.Get().get();
            pickBtn->OnClick = [btnPtr, currentHandle, &texturePath, notifySavedRebuild]()
            {
                UI::UIContext* ctx = UI::UIContext::GetCurrent();
                auto mgr = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr;
                if (!ctx || !mgr)
                    return;
                ShowSearchableAssetPicker(
                    *ctx,
                    btnPtr->GetBounds(),
                    "None",
                    currentHandle,
                    CollectAssetPickerOptions({ AssetType::Texture2D }),
                    [mgr, &texturePath, notifySavedRebuild](AssetHandle picked)
                    {
                        if (picked == 0)
                            texturePath.clear();
                        else
                        {
                            const auto& m = mgr->GetMetadata(picked);
                            texturePath = m.SourceFilePath.empty()
                                ? m.FilePath.generic_string()
                                : m.SourceFilePath.generic_string();
                        }
                        notifySavedRebuild();
                    },
                    false);
            };
            texRow->AddChild(pickBtn);

            if (!texturePath.empty())
            {
                auto clearBtn = UI::Button("x", [&texturePath, notifySavedRebuild]()
                {
                    texturePath.clear();
                    notifySavedRebuild();
                }).Tooltip("Clear texture");
                clearBtn->WidthMode = UI::UISizeMode::Fixed;
                clearBtn->HeightMode = UI::UISizeMode::Fixed;
                clearBtn->FixedSize = { 24.0f, 24.0f };
                clearBtn->Padding = UI::UIThickness::All(0.0f);
                texRow->AddChild(clearBtn);
            }
            c.AddRow(label, texRow, tooltip);
        };

        const bool specularGlossiness = (data.Workflow == 1.0f);
        if (specularGlossiness)
        {
            c.AddReadonly("Workflow", "Specular-Glossiness");
            c.AddColor4("Diffuse", &data.DiffuseFactor, notifySaved);
            c.AddColor3("Specular", reinterpret_cast<glm::vec3*>(&data.SpecularFactor), notifySaved);
            c.AddSlider("Glossiness", &data.SpecularFactor.a, 0.0f, 1.0f, notifySaved);
        }
        else
        {
            c.AddReadonly("Workflow", "Metallic-Roughness");
            c.AddColor4("Base Color", &data.BaseColorFactor, notifySaved);
            c.AddSlider("Metallic", &data.MetallicFactor, 0.0f, 1.0f, notifySaved);
            c.AddSlider("Roughness", &data.RoughnessFactor, 0.0f, 1.0f, notifySaved);
        }

        c.AddColor3("Emissive", &data.EmissiveFactor, notifySaved);
        c.AddFloat("Emissive Strength", &data.emissiveStrength, 0.05f, 0.0f, 100.0f, "{:.2f}", notifySaved);
        c.AddSlider("Transmission", &data.TransmissionFactor, 0.0f, 1.0f, notifySaved);
        c.AddFloat("IOR", &data.IOR, 0.01f, 1.0f, 3.0f, "{:.2f}", notifySaved);
        c.AddFloat("Thickness", &data.Thickness, 0.01f, 0.0f, 10.0f, "{:.2f}", notifySaved);
        c.AddColor3("Attenuation Color", &data.AttenuationColor, notifySaved);
        c.AddFloat("Attenuation Dist", &data.AttenuationDistance, 0.05f, 0.0f, 100000.0f, "{:.2f}", notifySaved);
        c.AddSlider("Diffuse Trans", &data.DiffuseTransmission, 0.0f, 1.0f, notifySaved);
        c.AddFloat("Depth Bias", &data.DepthBias, 0.00001f, 0.0f, 1.0f, "{:.5f}", notifySaved);

        c.AddSeparator();
        addTextureSlot(specularGlossiness ? "Diffuse Tex" : "Base Color Tex", data.BaseColorTexturePath);
        addTextureSlot(specularGlossiness ? "Spec Gloss Tex" : "Metal Rough Tex", data.MetallicRoughnessTexturePath);
        addTextureSlot("Normal Tex", data.NormalTexturePath);
        c.AddFloat("Normal Strength", &data.NormalScale, 0.02f, 0.0f, 4.0f, "{:.2f}", notifySaved);
        addTextureSlot("Occlusion Tex", data.OcclusionTexturePath);
        c.AddSlider("AO Strength", &data.OcclusionStrength, 0.0f, 1.0f, notifySaved);
        addTextureSlot("Emissive Tex", data.EmissiveTexturePath);
        addTextureSlot("Transmission Tex", data.TransmissionTexturePath);

        if (!specularGlossiness)
        {
            const std::string layersKey = "MatLayers_" + std::to_string(static_cast<uint64_t>(handle));
            bool layersOpen = expandedSections.contains(layersKey) ? expandedSections[layersKey] : false;
            auto layersCard = CreateCollapsibleCard("Material Layers", layersOpen,
                [&expandedSections, layersKey](bool next) { expandedSections[layersKey] = next; }, false, {}, {});
            InspectorCardBuilder lc{ layersCard.Body, c.OnMutate, c.Context };
            lc.AddSubheader("Clear Coat");
            lc.AddSlider("Clear Coat", &data.ClearcoatFactor, 0.0f, 1.0f, notifySaved);
            lc.AddSlider("Coat Roughness", &data.ClearcoatRoughnessFactor, 0.0f, 1.0f, notifySaved);
            lc.AddFloat("Coat Normal Scale", &data.ClearcoatNormalScale, 0.02f, 0.0f, 4.0f, "{:.2f}", notifySaved);
            lc.AddSubheader("Sheen");
            lc.AddColor3("Sheen Color", &data.SheenColorFactor, notifySaved);
            lc.AddSlider("Sheen Roughness", &data.SheenRoughnessFactor, 0.0f, 1.0f, notifySaved);
            lc.AddSubheader("Specular & Iridescence");
            lc.AddSlider("Specular", &data.SpecularStrength, 0.0f, 1.0f, notifySaved);
            lc.AddColor3("Specular Color", &data.SpecularColorFactor, notifySaved);
            lc.AddSlider("Iridescence", &data.IridescenceFactor, 0.0f, 1.0f, notifySaved);
            lc.AddFloat("Film IOR", &data.IridescenceIOR, 0.01f, 1.0f, 3.0f, "{:.2f}", notifySaved);
            lc.AddFloat("Film Min (nm)", &data.IridescenceThicknessMinimum, 5.0f, 0.0f, 2000.0f, "{:.0f}", notifySaved);
            lc.AddFloat("Film Max (nm)", &data.IridescenceThicknessMaximum, 5.0f, 0.0f, 2000.0f, "{:.0f}", notifySaved);
            lc.AddSubheader("Anisotropy & Dispersion");
            lc.AddSlider("Anisotropy", &data.AnisotropyStrength, 0.0f, 1.0f, notifySaved);
            lc.AddFloat("Dispersion", &data.Dispersion, 0.01f, 0.0f, 3.0f, "{:.2f}", notifySaved);
            c.Body->AddChild(layersCard.Card);
        }

        const std::string uvKey = "MatUV_" + std::to_string(static_cast<uint64_t>(handle));
        bool uvOpen = expandedSections.contains(uvKey) ? expandedSections[uvKey] : false;
        auto uvCard = CreateCollapsibleCard("UV Channels", uvOpen,
            [&expandedSections, uvKey](bool next) { expandedSections[uvKey] = next; }, false, {}, {});
        InspectorCardBuilder uvc{ uvCard.Body, c.OnMutate, c.Context };
        for (int ch = 0; ch < MaxMaterialUVChannels; ++ch)
        {
            MaterialUVChannel& channel = data.UVChannels[ch];
            uvc.AddSubheader("Channel " + std::to_string(ch));
            uvc.AddInt32("Mesh UV Set", &channel.Source, 0, 1, notifySaved);
            uvc.AddVec2("Tiling", &channel.Scale, 0.02f, notifySaved);
            uvc.AddVec2("Offset", &channel.Offset, 0.02f, notifySaved);
        }
        c.Body->AddChild(uvCard.Card);

        c.AddSeparator();
        c.AddEnum("Blend Mode", { "Opaque", "Masked", "Translucent" }, static_cast<int>(data.Mode),
            [&data, notifySavedRebuild](int idx)
            {
                data.Mode = static_cast<AlphaMode>(idx);
                notifySavedRebuild();
            });
        if (data.Mode == AlphaMode::Mask)
            c.AddFloat("Alpha Cutoff", &data.AlphaMaskCutoff, 0.005f, 0.0f, 1.0f, "{:.3f}", notifySaved);
        c.AddBool("Two Sided", &data.DoubleSided, notifySaved);

        c.AddEnum("Shading Model", { "Default Lit", "Unlit", "Toon", "Custom Lit" }, static_cast<int>(data.ShadingModel),
            [&data, notifySavedRebuild](int idx)
            {
                data.ShadingModel = static_cast<MaterialShadingModel>(idx);
                notifySavedRebuild();
            });

        if (data.ShadingModel == MaterialShadingModel::Toon)
        {
            c.AddSubheader("Toon Shading");
            c.AddInt32("Bands", &data.ToonBands, 1, 8, notifySaved);
            c.AddSlider("Band Softness", &data.ToonBandSoftness, 0.0f, 1.0f, notifySaved);
            addTextureSlot("Ramp Texture", data.ToonRampTexture.Path);
            c.AddSlider("Spec Size", &data.ToonSpecularSize, 0.0f, 1.0f, notifySaved);
            c.AddSlider("Spec Softness", &data.ToonSpecularSoftness, 0.0f, 1.0f, notifySaved);
            c.AddSlider("Spec Intensity", &data.ToonSpecularIntensity, 0.0f, 8.0f, notifySaved);
            c.AddColor3("Rim Color", &data.ToonRimColor, notifySaved);
            c.AddSlider("Rim Width", &data.ToonRimWidth, 0.0f, 1.0f, notifySaved);
            c.AddSlider("Rim Intensity", &data.ToonRimIntensity, 0.0f, 8.0f, notifySaved);
        }
        else if (data.ShadingModel == MaterialShadingModel::CustomLit)
        {
            c.AddSubheader("Custom Lit Shader");
            auto assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr;
            const AssetHandle currentShader = (assets && !data.CustomShader.empty())
                ? assets->FindHandleByPath(data.CustomShader, AssetType::MaterialShader)
                : AssetHandle(0);

            auto shRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 4.0f);
            shRow->WidthMode = UI::UISizeMode::Fill;
            auto pickBtn = UI::Button(TexturePathDisplayName(data.CustomShader));
            pickBtn->WidthMode = UI::UISizeMode::Fill;
            pickBtn->FlexWeight = 1.0f;
            pickBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
            pickBtn->TextElide = UI::UITextElide::Middle;
        pickBtn->TextElide = UI::UITextElide::Middle; // an asset's name keeps its start and its end
            UI::UIButton* shBtnPtr = pickBtn.Get().get();
            pickBtn->OnClick = [shBtnPtr, currentShader, assets, &data, notifySavedRebuild]()
            {
                UI::UIContext* ctx = UI::UIContext::GetCurrent();
                if (!ctx || !assets)
                    return;
                ShowSearchableAssetPicker(
                    *ctx, shBtnPtr->GetBounds(), "None", currentShader,
                    CollectAssetPickerOptions({ AssetType::MaterialShader }),
                    [assets, &data, notifySavedRebuild](AssetHandle picked)
                    {
                        data.CustomShader = picked != 0 ? assets->GetMetadata(picked).FilePath.generic_string() : std::string();
                        notifySavedRebuild();
                    });
            };
            shRow->AddChild(pickBtn);
            if (!data.CustomShader.empty())
            {
                auto openExtBtn = UI::Button("Open", [&data, assets, currentShader]()
                {
                    const std::string rel = currentShader != 0 && assets
                        ? assets->GetMetadata(currentShader).FilePath.generic_string()
                        : data.CustomShader;
                    const std::string url = "file:///" + std::filesystem::absolute(Project::GetActiveAssetDirectory() / rel).generic_string();
                    SDL_OpenURL(url.c_str());
                }).Tooltip("Open .slang file in external code editor");
                openExtBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                shRow->AddChild(openExtBtn);
            }
            c.AddRow("Material Shader", shRow);

            for (int p = 0; p < MaterialCustomParameterCount; ++p)
                c.AddColor4("Parameter " + std::to_string(p + 1), &data.CustomParameters[p], notifySaved);
            addTextureSlot("Custom Tex 1 (sRGB)", data.CustomTexture1.Path);
            addTextureSlot("Custom Tex 2 (lin)", data.CustomTexture2.Path);
        }

        // Material Graph section
        {
            c.AddSubheader("Material Graph");
            auto assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr;
            const AssetHandle currentGraph = (assets && !data.SurfaceGraph.empty())
                ? assets->FindHandleByPath(data.SurfaceGraph, AssetType::MaterialGraph)
                : AssetHandle(0);

            auto grRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 4.0f);
            grRow->WidthMode = UI::UISizeMode::Fill;
            auto pickBtn = UI::Button(TexturePathDisplayName(data.SurfaceGraph));
            pickBtn->WidthMode = UI::UISizeMode::Fill;
            pickBtn->FlexWeight = 1.0f;
            pickBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
            pickBtn->TextElide = UI::UITextElide::Middle;
        pickBtn->TextElide = UI::UITextElide::Middle; // an asset's name keeps its start and its end
            UI::UIButton* grBtnPtr = pickBtn.Get().get();
            pickBtn->OnClick = [grBtnPtr, currentGraph, assets, &data, notifySavedRebuild]()
            {
                UI::UIContext* ctx = UI::UIContext::GetCurrent();
                if (!ctx || !assets)
                    return;
                ShowSearchableAssetPicker(
                    *ctx, grBtnPtr->GetBounds(), "None", currentGraph,
                    CollectAssetPickerOptions({ AssetType::MaterialGraph }),
                    [assets, &data, notifySavedRebuild](AssetHandle picked)
                    {
                        data.SurfaceGraph = picked != 0 ? assets->GetMetadata(picked).FilePath.generic_string() : std::string();
                        notifySavedRebuild();
                    });
            };
            grRow->AddChild(pickBtn);
            if (currentGraph != 0 && callbacks.OpenAsset)
            {
                auto openBtn = UI::Button("Open", [callbacks, currentGraph]() { callbacks.OpenAsset(currentGraph); });
                openBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                grRow->AddChild(openBtn);
            }
            if (!data.SurfaceGraph.empty())
            {
                auto clrBtn = UI::Button("x", [&data, notifySavedRebuild]()
                {
                    data.SurfaceGraph.clear();
                    notifySavedRebuild();
                }).Tooltip("Clear surface graph");
                clrBtn->WidthMode = UI::UISizeMode::Fixed;
                clrBtn->HeightMode = UI::UISizeMode::Fixed;
                clrBtn->FixedSize = { 24.0f, 24.0f };
                clrBtn->Padding = UI::UIThickness::All(0.0f);
                grRow->AddChild(clrBtn);
            }
            c.AddRow("Surface Graph", grRow);

            if (!data.SurfaceGraph.empty() && currentGraph != 0)
            {
                const Ref<NodeGraphAsset> graphAsset = AssetManager::GetAsset<NodeGraphAsset>(currentGraph);
                const MaterialGraphProgram program = graphAsset ? CompileMaterialGraph(graphAsset->Graph) : MaterialGraphProgram{};
                if (!graphAsset)
                    c.AddStatusNote("Graph asset not loaded in project Assets.", { 0.95f, 0.75f, 0.25f, 1.0f });
                else if (!program.Valid)
                    c.AddStatusNote("Graph compile error: " + program.Error, { 0.95f, 0.35f, 0.30f, 1.0f });
                else
                {
                    for (const MaterialGraphParameter& param : program.Parameters)
                    {
                        if (param.Name.starts_with('#'))
                            continue;
                        const bool isTex = (param.Type == MaterialGraphParameter::Kind::Texture);
                        auto it = std::find_if(data.GraphParameters.begin(), data.GraphParameters.end(), [&](const MaterialGraphParameterValue& v)
                        {
                            return v.Name == param.Name && v.IsTexture == isTex;
                        });
                        const bool overridden = (it != data.GraphParameters.end());

                        auto paramRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 4.0f);
                        paramRow->WidthMode = UI::UISizeMode::Fill;
                        paramRow->VAlign = UI::UIAlignment::Center;
                        auto ovCb = UI::CheckBox("", overridden, [assets, &data, param, isTex, notifySavedRebuild](bool next)
                        {
                            auto found = std::find_if(data.GraphParameters.begin(), data.GraphParameters.end(), [&](const MaterialGraphParameterValue& v)
                            {
                                return v.Name == param.Name && v.IsTexture == isTex;
                            });
                            if (next && found == data.GraphParameters.end())
                            {
                                MaterialGraphParameterValue added;
                                added.Name = param.Name;
                                added.IsTexture = isTex;
                                added.Value = param.Default;
                                if (isTex && param.DefaultTexture != 0 && AssetManager::IsAssetHandleValid(param.DefaultTexture) && assets)
                                    added.Texture = assets->GetMetadata(param.DefaultTexture).FilePath.generic_string();
                                data.GraphParameters.push_back(std::move(added));
                            }
                            else if (!next && found != data.GraphParameters.end())
                            {
                                data.GraphParameters.erase(found);
                            }
                            notifySavedRebuild();
                        }).Tooltip("Checked: override graph default");
                        paramRow->AddChild(ovCb);

                        if (isTex)
                        {
                            if (overridden)
                            {
                                const AssetHandle texHandle = assets ? assets->FindHandleByPath(it->Texture, AssetType::Texture2D) : AssetHandle(0);
                                auto texBtn = UI::Button(TexturePathDisplayName(it->Texture));
                                texBtn->WidthMode = UI::UISizeMode::Fill;
                                texBtn->FlexWeight = 1.0f;
                                texBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                                UI::UIButton* tbPtr = texBtn.Get().get();
                                texBtn->OnClick = [tbPtr, texHandle, assets, &data, paramName = param.Name, notifySavedRebuild]()
                                {
                                    UI::UIContext* ctx = UI::UIContext::GetCurrent();
                                    if (!ctx || !assets) return;
                                    ShowSearchableAssetPicker(*ctx, tbPtr->GetBounds(), "None", texHandle,
                                        CollectAssetPickerOptions({ AssetType::Texture2D }),
                                        [assets, &data, paramName, notifySavedRebuild](AssetHandle h)
                                        {
                                            for (auto& gp : data.GraphParameters)
                                            {
                                                if (gp.Name == paramName && gp.IsTexture)
                                                {
                                                    gp.Texture = h != 0 ? assets->GetMetadata(h).FilePath.generic_string() : std::string();
                                                    break;
                                                }
                                            }
                                            notifySavedRebuild();
                                        });
                                };
                                paramRow->AddChild(texBtn);
                            }
                            else
                            {
                                auto lbl = UI::Label(AssetDisplayName(param.DefaultTexture) + " (default)");
                                lbl->FontSizeOverride = 14.0f;
                                lbl->UseColorOverride = true;
                                lbl->ColorOverride = { 0.56f, 0.62f, 0.72f, 1.0f };
                                paramRow->AddChild(lbl);
                            }
                        }
                        else if (param.Type == MaterialGraphParameter::Kind::Scalar)
                        {
                            auto drag = UI::MakeUI<UI::UIDragScalar>(overridden ? it->Value.x : param.Default.x, 0.01f,
                                [&data, paramName = param.Name, overridden, notifySaved](float v)
                                {
                                    if (!overridden) return;
                                    for (auto& gp : data.GraphParameters)
                                    {
                                        if (gp.Name == paramName && !gp.IsTexture)
                                        {
                                            gp.Value.x = v;
                                            break;
                                        }
                                    }
                                    notifySaved();
                                });
                            drag->WidthMode = UI::UISizeMode::Fill;
                            drag->FlexWeight = 1.0f;
                            drag->SetEnabled(overridden);
                            paramRow->AddChild(drag);
                        }
                        else
                        {
                            auto col = UI::MakeUI<UI::UIColorEdit>(c.Context, overridden ? it->Value : param.Default,
                                [&data, paramName = param.Name, overridden, notifySaved](const UI::UIColor& v)
                                {
                                    if (!overridden) return;
                                    for (auto& gp : data.GraphParameters)
                                    {
                                        if (gp.Name == paramName && !gp.IsTexture)
                                        {
                                            gp.Value = v;
                                            break;
                                        }
                                    }
                                    notifySaved();
                                });
                            col->WidthMode = UI::UISizeMode::Fill;
                            col->FlexWeight = 1.0f;
                            col->SetEnabled(overridden);
                            paramRow->AddChild(col);
                        }
                        c.AddRow(param.Name, paramRow);
                    }
                }
            }
        }
    }
}
