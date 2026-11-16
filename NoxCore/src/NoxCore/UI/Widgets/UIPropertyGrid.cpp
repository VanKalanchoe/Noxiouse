#include "UIPropertyGrid.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UICheckBox.h"
#include "NoxCore/UI/Widgets/UIColorPicker.h"
#include "NoxCore/UI/Widgets/UIDragScalar.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UISlider.h"
#include "NoxCore/UI/Widgets/UITextInput.h"
#include "NoxCore/UI/Widgets/UIVectorEdit.h"

namespace Nox::UI
{
    UIPropertyGrid::UIPropertyGrid()
    {
        StyleClass = "PropertyGrid";
        ChildSpacing = 6.0f;
    }

    void UIPropertyGrid::ClearProperties()
    {
        m_SyncEntries.clear();
        ClearChildren();
    }

    void UIPropertyGrid::OnUpdate(float dt)
    {
        UIScrollView::OnUpdate(dt);

        for (size_t i = 0; i < m_SyncEntries.size();)
        {
            auto& entry = m_SyncEntries[i];
            if (entry.IsValid && !entry.IsValid())
            {
                if (entry.SectionContainer)
                    RemoveChild(entry.SectionContainer);
                m_SyncEntries.erase(m_SyncEntries.begin() + static_cast<ptrdiff_t>(i));
                continue;
            }
            if (entry.SyncFromModel)
                entry.SyncFromModel();
            ++i;
        }
    }

    Ref<UIStackBox> UIPropertyGrid::AddReflected(
        UIContext* context,
        void* instance,
        const TypeMeta& meta,
        std::function<bool()> isValid,
        const std::string& sectionOverride)
    {
        if (!instance)
            return nullptr;

        const std::string& title = sectionOverride.empty() ? meta.DisplayName : sectionOverride;
        Ref<UIStackBox> section = AddSection(title, true);
        Ref<UIElement> sectionContainer;
        if (!GetChildren().empty())
            sectionContainer = GetChildren().back();

        auto postEdit = meta.OnPostEdit;
        std::vector<std::function<void()>> rowSyncs;

        for (const PropertyMeta& prop : meta.Properties)
        {
            switch (prop.Type)
            {
            case PropertyType::Bool:
            {
                bool val = false;
                prop.Read(&val, instance);
                Ref<UICheckBox> cb(new UICheckBox({}, val, [instance, prop, postEdit](bool next)
                {
                    prop.Write(instance, &next);
                    if (postEdit) postEdit(instance);
                }));
                AddRow(prop.Name, cb, section);
                rowSyncs.push_back([cb, instance, prop]()
                {
                    if (HasState(cb->GetState(), UIState::Pressed)) return;
                    bool cur = false;
                    prop.Read(&cur, instance);
                    cb->Checked = cur;
                });
                break;
            }
            case PropertyType::Int:
            {
                int val = 0;
                prop.Read(&val, instance);
                Ref<UIDragScalar> drag(new UIDragScalar(static_cast<float>(val), prop.Speed, [instance, prop, postEdit](float next)
                {
                    int iv = static_cast<int>(std::round(next));
                    prop.Write(instance, &iv);
                    if (postEdit) postEdit(instance);
                }));
                drag->Format = "{:.0f}";
                drag->Min = prop.Min;
                drag->Max = prop.Max;
                AddRow(prop.Name, drag, section);
                rowSyncs.push_back([drag, instance, prop]()
                {
                    if (HasState(drag->GetState(), UIState::Focused) || HasState(drag->GetState(), UIState::Pressed)) return;
                    int cur = 0;
                    prop.Read(&cur, instance);
                    drag->Value = static_cast<float>(cur);
                });
                break;
            }
            case PropertyType::UInt32:
            {
                uint32_t val = 0;
                prop.Read(&val, instance);
                Ref<UIDragScalar> drag(new UIDragScalar(static_cast<float>(val), prop.Speed, [instance, prop, postEdit](float next)
                {
                    uint32_t uv = static_cast<uint32_t>(std::max(0.0f, std::round(next)));
                    prop.Write(instance, &uv);
                    if (postEdit) postEdit(instance);
                }));
                drag->Format = "{:.0f}";
                drag->Min = prop.Min;
                drag->Max = prop.Max;
                AddRow(prop.Name, drag, section);
                rowSyncs.push_back([drag, instance, prop]()
                {
                    if (HasState(drag->GetState(), UIState::Focused) || HasState(drag->GetState(), UIState::Pressed)) return;
                    uint32_t cur = 0;
                    prop.Read(&cur, instance);
                    drag->Value = static_cast<float>(cur);
                });
                break;
            }
            case PropertyType::Float:
            {
                float val = 0.0f;
                prop.Read(&val, instance);
                Ref<UIDragScalar> drag(new UIDragScalar(val, prop.Speed, [instance, prop, postEdit](float next)
                {
                    prop.Write(instance, &next);
                    if (postEdit) postEdit(instance);
                }));
                drag->Min = prop.DynamicMin ? prop.DynamicMin(instance) : prop.Min;
                drag->Max = prop.DynamicMax ? prop.DynamicMax(instance) : prop.Max;
                AddRow(prop.Name, drag, section);
                rowSyncs.push_back([drag, instance, prop]()
                {
                    if (prop.DynamicMin) drag->Min = prop.DynamicMin(instance);
                    if (prop.DynamicMax) drag->Max = prop.DynamicMax(instance);
                    if (HasState(drag->GetState(), UIState::Focused) || HasState(drag->GetState(), UIState::Pressed)) return;
                    float cur = 0.0f;
                    prop.Read(&cur, instance);
                    drag->Value = cur;
                });
                break;
            }
            case PropertyType::Vec2:
            {
                glm::vec2 val(0.0f);
                prop.Read(&val, instance);
                Ref<UIVectorEdit> vec(new UIVectorEdit(2, { val.x, val.y, 0.0f, 0.0f }, prop.Speed, [instance, prop, postEdit](const glm::vec4& next)
                {
                    glm::vec2 v2(next.x, next.y);
                    prop.Write(instance, &v2);
                    if (postEdit) postEdit(instance);
                }));
                AddRow(prop.Name, vec, section);
                rowSyncs.push_back([vec, instance, prop]()
                {
                    if (vec->IsEditing()) return;
                    glm::vec2 cur(0.0f);
                    prop.Read(&cur, instance);
                    vec->SetValue({ cur.x, cur.y, 0.0f, 0.0f }, false);
                });
                break;
            }
            case PropertyType::Vec3:
            {
                glm::vec3 val(0.0f);
                prop.Read(&val, instance);
                Ref<UIVectorEdit> vec(new UIVectorEdit(3, { val.x, val.y, val.z, 0.0f }, prop.Speed, [instance, prop, postEdit](const glm::vec4& next)
                {
                    glm::vec3 v3(next.x, next.y, next.z);
                    prop.Write(instance, &v3);
                    if (postEdit) postEdit(instance);
                }));
                AddRow(prop.Name, vec, section);
                rowSyncs.push_back([vec, instance, prop]()
                {
                    if (vec->IsEditing()) return;
                    glm::vec3 cur(0.0f);
                    prop.Read(&cur, instance);
                    vec->SetValue({ cur.x, cur.y, cur.z, 0.0f }, false);
                });
                break;
            }
            case PropertyType::Vec4:
            {
                glm::vec4 val(0.0f);
                prop.Read(&val, instance);
                Ref<UIVectorEdit> vec(new UIVectorEdit(4, val, prop.Speed, [instance, prop, postEdit](const glm::vec4& next)
                {
                    prop.Write(instance, &next);
                    if (postEdit) postEdit(instance);
                }));
                AddRow(prop.Name, vec, section);
                rowSyncs.push_back([vec, instance, prop]()
                {
                    if (vec->IsEditing()) return;
                    glm::vec4 cur(0.0f);
                    prop.Read(&cur, instance);
                    vec->SetValue(cur, false);
                });
                break;
            }
            case PropertyType::Color3:
            {
                glm::vec3 val(1.0f);
                prop.Read(&val, instance);
                Ref<UIColorEdit> color(new UIColorEdit(context, { val.r, val.g, val.b, 1.0f }, [instance, prop, postEdit](const UIColor& c)
                {
                    glm::vec3 v3(c.r, c.g, c.b);
                    prop.Write(instance, &v3);
                    if (postEdit) postEdit(instance);
                }));
                AddRow(prop.Name, color, section);
                rowSyncs.push_back([color, instance, prop]()
                {
                    glm::vec3 cur(1.0f);
                    prop.Read(&cur, instance);
                    const UIColor curC = color->GetColor();
                    if (std::abs(curC.r - cur.r) > 1e-4f || std::abs(curC.g - cur.g) > 1e-4f || std::abs(curC.b - cur.b) > 1e-4f)
                        color->SetColor({ cur.r, cur.g, cur.b, 1.0f }, false);
                });
                break;
            }
            case PropertyType::Color4:
            {
                glm::vec4 val(1.0f);
                prop.Read(&val, instance);
                Ref<UIColorEdit> color(new UIColorEdit(context, val, [instance, prop, postEdit](const UIColor& c)
                {
                    prop.Write(instance, &c);
                    if (postEdit) postEdit(instance);
                }));
                AddRow(prop.Name, color, section);
                rowSyncs.push_back([color, instance, prop]()
                {
                    glm::vec4 cur(1.0f);
                    prop.Read(&cur, instance);
                    if (glm::length(color->GetColor() - cur) > 1e-4f)
                        color->SetColor(cur, false);
                });
                break;
            }
            case PropertyType::String:
            {
                std::string val;
                prop.Read(&val, instance);
                Ref<UITextInput> input(new UITextInput(val, prop.Placeholder));
                input->OnChanged = [instance, prop, postEdit](const std::string& next)
                {
                    prop.Write(instance, &next);
                    if (postEdit) postEdit(instance);
                };
                AddRow(prop.Name, input, section);
                rowSyncs.push_back([input, instance, prop]()
                {
                    if (HasState(input->GetState(), UIState::Focused)) return;
                    std::string cur;
                    prop.Read(&cur, instance);
                    if (input->Text != cur)
                        input->Text = cur;
                });
                break;
            }
            }
        }

        m_SyncEntries.push_back({
            std::move(isValid),
            [syncs = std::move(rowSyncs)]()
            {
                for (const auto& fn : syncs)
                    fn();
            },
            sectionContainer
        });

        return section;
    }

    Ref<UIStackBox> UIPropertyGrid::AddSection(const std::string& title, bool expanded)
    {
        Ref<UIStackBox> sectionContainer(new UIStackBox(UIOrientation::Vertical, 4.0f));
        sectionContainer->WidthMode = UISizeMode::Fill;

        Ref<UIStackBox> body(new UIStackBox(UIOrientation::Vertical, 6.0f));
        body->WidthMode = UISizeMode::Fill;
        body->Padding = { 6.0f, 4.0f, 2.0f, 6.0f };
        body->Visible = expanded;

        const std::string prefix = expanded ? "v  " : ">  ";
        Ref<UIButton> header(new UIButton(prefix + title));
        header->StyleClass = "SectionHeader";
        header->WidthMode = UISizeMode::Fill;
        header->HeightMode = UISizeMode::Auto;

        UIButton* headerPtr = header.get();
        UIStackBox* bodyPtr = body.get();
        header->OnClick = [headerPtr, bodyPtr, title]()
        {
            bodyPtr->Visible = !bodyPtr->Visible;
            headerPtr->SetText((bodyPtr->Visible ? "v  " : ">  ") + title);
        };

        sectionContainer->AddChild(header);
        sectionContainer->AddChild(body);
        AddChild(sectionContainer);
        return body;
    }

    void UIPropertyGrid::AddRow(const std::string& label, const Ref<UIElement>& editor, const Ref<UIStackBox>& section)
    {
        Ref<UIStackBox> row(new UIStackBox(UIOrientation::Horizontal, 8.0f));
        row->WidthMode = UISizeMode::Fill;
        row->VAlign = UIAlignment::Center;

        Ref<UILabel> nameLabel(new UILabel(label));
        nameLabel->WidthMode = UISizeMode::Fill;
        nameLabel->FlexWeight = std::clamp(LabelRatio, 0.1f, 0.8f);
        nameLabel->VAlign = UIAlignment::Center;
        nameLabel->UseColorOverride = true;
        nameLabel->ColorOverride = { 0.72f, 0.77f, 0.86f, 1.0f };

        if (editor)
        {
            editor->WidthMode = UISizeMode::Fill;
            editor->FlexWeight = std::max(0.2f, 1.0f - LabelRatio);
            editor->VAlign = UIAlignment::Center;
        }

        row->AddChild(nameLabel);
        if (editor)
            row->AddChild(editor);

        if (section)
            section->AddChild(row);
        else
            AddChild(row);
    }

    void UIPropertyGrid::AddBool(const std::string& label, bool value, std::function<void(bool)> onChanged,
                                 const Ref<UIStackBox>& section)
    {
        Ref<UIStackBox> wrap(new UIStackBox(UIOrientation::Horizontal, 0.0f));
        Ref<UICheckBox> cb(new UICheckBox({}, value, std::move(onChanged)));
        wrap->AddChild(cb);
        AddRow(label, wrap, section);
    }

    void UIPropertyGrid::AddFloatSlider(const std::string& label, float value, float min, float max,
                                        std::function<void(float)> onChanged, const Ref<UIStackBox>& section)
    {
        Ref<UISlider> slider(new UISlider(value, min, max, std::move(onChanged)));
        AddRow(label, slider, section);
    }

    void UIPropertyGrid::AddDragFloat(const std::string& label, float value, float speed,
                                      std::function<void(float)> onChanged, const Ref<UIStackBox>& section)
    {
        Ref<UIDragScalar> drag(new UIDragScalar(value, speed, std::move(onChanged)));
        AddRow(label, drag, section);
    }

    void UIPropertyGrid::AddVec3(const std::string& label, glm::vec3 value, float speed,
                                 std::function<void(const glm::vec3&)> onChanged, const Ref<UIStackBox>& section)
    {
        auto state = std::make_shared<glm::vec3>(value);
        Ref<UIStackBox> vecRow(new UIStackBox(UIOrientation::Horizontal, 4.0f));

        auto addAxis = [&](const char* axis, const UIColor& color, float* comp)
        {
            Ref<UIDragScalar> drag(new UIDragScalar(*comp, speed, [state, comp, onChanged](float v)
            {
                *comp = v;
                if (onChanged)
                    onChanged(*state);
            }));
            drag->Prefix = axis;
            drag->PrefixColor = color;
            drag->WidthMode = UISizeMode::Fill;
            vecRow->AddChild(drag);
        };

        addAxis("X", { 0.80f, 0.26f, 0.26f, 1.0f }, &state->x);
        addAxis("Y", { 0.25f, 0.68f, 0.34f, 1.0f }, &state->y);
        addAxis("Z", { 0.24f, 0.52f, 0.88f, 1.0f }, &state->z);

        AddRow(label, vecRow, section);
    }

    void UIPropertyGrid::AddString(const std::string& label, const std::string& value, const std::string& placeholder,
                                   std::function<void(const std::string&)> onChanged, const Ref<UIStackBox>& section)
    {
        Ref<UITextInput> input(new UITextInput(value, placeholder));
        input->OnChanged = std::move(onChanged);
        AddRow(label, input, section);
    }
}
