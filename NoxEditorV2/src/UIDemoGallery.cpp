#include "UIDemoGallery.h"

#include <cstdio>
#include <memory>

#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UICheckBox.h"
#include "NoxCore/UI/Widgets/UICollapsingHeader.h"
#include "NoxCore/UI/Widgets/UIColorPicker.h"
#include "NoxCore/UI/Widgets/UIDragScalar.h"
#include "NoxCore/UI/Widgets/UIDropdown.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIProgressBar.h"
#include "NoxCore/UI/Widgets/UIPropertyGrid.h"
#include "NoxCore/UI/Widgets/UIRadioGroup.h"
#include "NoxCore/UI/Widgets/UIScrollView.h"
#include "NoxCore/UI/Widgets/UISeparator.h"
#include "NoxCore/UI/Widgets/UISlider.h"
#include "NoxCore/UI/Widgets/UITableView.h"
#include "NoxCore/UI/Widgets/UITextInput.h"
#include "NoxCore/UI/Widgets/UITreeView.h"

namespace Nox::UI
{
    namespace
    {
        template <typename T, typename... Args>
        Ref<T> MakeUI(Args&&... args)
        {
            return Ref<T>(new T(std::forward<Args>(args)...));
        }
    }

    Ref<UIElement> CreateUIDemoGallery(UIContext& context, const std::function<void(const std::string&)>& onStatus)
    {
        auto scroll = MakeUI<UIScrollView>();
        scroll->ChildSpacing = 12.0f;
        scroll->Padding = UIThickness::All(8.0f);

        auto report = [onStatus](const std::string& msg)
        {
            if (onStatus)
                onStatus(msg);
        };

        // 1. Header & Modal Overlay Demo
        auto headerCard = MakeUI<UIStackBox>(UIOrientation::Horizontal, 10.0f);
        headerCard->StyleClass = "TopBar";
        headerCard->DrawBackground = true;
        headerCard->Padding = UIThickness::Symmetric(14.0f, 10.0f);

        auto headerText = MakeUI<UILabel>("Nox::UI Demo Gallery  -  All Retained Widgets, Layouts & SDF Styling", 15.5f);
        headerText->WidthMode = UISizeMode::Fill;
        headerText->VAlign = UIAlignment::Center;
        headerCard->AddChild(headerText);

        auto modalBtn = MakeUI<UIButton>("Open Modal Overlay", [&context, report]()
        {
            auto backdrop = MakeUI<UIAnchorBox>();
            backdrop->StyleClass = "Background";
            backdrop->DrawBackground = true;
            backdrop->HitTestMode = UIHitTestMode::Self;

            auto dialog = MakeUI<UIStackBox>(UIOrientation::Vertical, 12.0f);
            dialog->StyleClass = "Panel";
            dialog->DrawBackground = true;
            dialog->WidthMode = UISizeMode::Fixed;
            dialog->HeightMode = UISizeMode::Fixed;
            dialog->FixedSize = { 380.0f, 175.0f };
            dialog->AnchorMin = { 0.5f, 0.5f };
            dialog->AnchorMax = { 0.5f, 0.5f };
            dialog->AnchorOffsets = { -190.0f, -87.5f, 0.0f, 0.0f };
            dialog->Padding = UIThickness::All(18.0f);

            dialog->AddChild(MakeUI<UILabel>("Modal Dialog (UIContext::SetOverlay)", 16.0f));
            dialog->AddChild(MakeUI<UILabel>("Click outside or press Close to dismiss.", 13.5f));

            auto btnRow = MakeUI<UIStackBox>(UIOrientation::Horizontal, 8.0f);
            btnRow->HAlign = UIAlignment::End;
            auto closeBtn = MakeUI<UIButton>("Close", [&context, report]()
            {
                context.ClearOverlay();
                report("Demo Modal closed");
            });
            closeBtn->StyleVariant = "primary";
            btnRow->AddChild(closeBtn);
            dialog->AddChild(btnRow);

            backdrop->AddChild(dialog);
            context.SetOverlay(backdrop);
        });
        modalBtn->StyleVariant = "primary";
        headerCard->AddChild(modalBtn);
        scroll->AddChild(headerCard);

        // 2. Buttons & States
        auto btnPanel = MakeUI<UIStackBox>(UIOrientation::Vertical, 8.0f);
        btnPanel->StyleClass = "Panel";
        btnPanel->DrawBackground = true;
        btnPanel->Padding = UIThickness::All(12.0f);
        btnPanel->AddChild(MakeUI<UILabel>("1. Buttons & States (Normal / Primary / Disabled)", 14.5f));

        auto btnRow = MakeUI<UIStackBox>(UIOrientation::Horizontal, 8.0f);
        auto count = std::make_shared<int>(0);
        auto defaultBtn = MakeUI<UIButton>("Default Button", [count, report]()
        {
            ++(*count);
            report("Default Button clicked (" + std::to_string(*count) + ")");
        });
        auto primaryBtn = MakeUI<UIButton>("Primary Button", [count, report]()
        {
            ++(*count);
            report("Primary Button clicked (" + std::to_string(*count) + ")");
        });
        primaryBtn->StyleVariant = "primary";

        auto disabledBtn = MakeUI<UIButton>("Disabled Button");
        disabledBtn->SetStateFlag(UIState::Disabled, true);

        auto enableToggle = MakeUI<UICheckBox>("Enable 3rd Button", false, [disabledBtn](bool checked)
        {
            disabledBtn->SetStateFlag(UIState::Disabled, !checked);
        });
        enableToggle->VAlign = UIAlignment::Center;

        btnRow->AddChild(defaultBtn);
        btnRow->AddChild(primaryBtn);
        btnRow->AddChild(disabledBtn);
        btnRow->AddChild(enableToggle);
        btnPanel->AddChild(btnRow);
        scroll->AddChild(btnPanel);

        // 3. Inputs, Sliders & Drag Scalars
        auto inputPanel = MakeUI<UIStackBox>(UIOrientation::Vertical, 8.0f);
        inputPanel->StyleClass = "Panel";
        inputPanel->DrawBackground = true;
        inputPanel->Padding = UIThickness::All(12.0f);
        inputPanel->AddChild(MakeUI<UILabel>("2. Text Input, Sliders & Drag Scalars", 14.5f));

        auto textInput = MakeUI<UITextInput>("", "Type here and press Enter...");
        textInput->OnChanged = [report](const std::string& t) { report("TextInput: " + t); };
        textInput->OnSubmit = [report](const std::string& t) { report("Submitted: " + t); };
        inputPanel->AddChild(textInput);

        auto slider = MakeUI<UISlider>(0.42f, 0.0f, 1.0f, [report](float v)
        {
            report("Slider: " + std::to_string(v));
        });
        inputPanel->AddChild(slider);

        auto steppedSlider = MakeUI<UISlider>(50.0f, 0.0f, 100.0f, [report](float v)
        {
            report("Stepped Slider (step=10): " + std::to_string(static_cast<int>(v)));
        });
        steppedSlider->Step = 10.0f;
        steppedSlider->Format = "{:.0f}";
        inputPanel->AddChild(steppedSlider);

        auto dragRow = MakeUI<UIStackBox>(UIOrientation::Horizontal, 8.0f);
        auto dX = MakeUI<UIDragScalar>(1.25f, 0.02f);
        dX->Prefix = "X";
        dX->PrefixColor = { 0.80f, 0.26f, 0.26f, 1.0f };
        dX->WidthMode = UISizeMode::Fill;
        auto dY = MakeUI<UIDragScalar>(2.50f, 0.02f);
        dY->Prefix = "Y";
        dY->PrefixColor = { 0.25f, 0.68f, 0.34f, 1.0f };
        dY->WidthMode = UISizeMode::Fill;
        auto dZ = MakeUI<UIDragScalar>(-0.75f, 0.02f);
        dZ->Prefix = "Z";
        dZ->PrefixColor = { 0.24f, 0.52f, 0.88f, 1.0f };
        dZ->WidthMode = UISizeMode::Fill;
        dragRow->AddChild(dX);
        dragRow->AddChild(dY);
        dragRow->AddChild(dZ);
        inputPanel->AddChild(dragRow);
        scroll->AddChild(inputPanel);

        // 4. UIGridBox Showcase
        auto gridPanel = MakeUI<UIStackBox>(UIOrientation::Vertical, 8.0f);
        gridPanel->StyleClass = "Panel";
        gridPanel->DrawBackground = true;
        gridPanel->Padding = UIThickness::All(12.0f);
        gridPanel->AddChild(MakeUI<UILabel>("3. UIGridBox (3-Column Auto Layout)", 14.5f));

        auto grid = MakeUI<UIGridBox>(3, 8.0f, 8.0f);
        for (int i = 1; i <= 6; ++i)
        {
            auto cell = MakeUI<UIButton>("Grid Item " + std::to_string(i), [i, report]()
            {
                report("Clicked Grid Item " + std::to_string(i));
            });
            grid->AddChild(cell);
        }
        gridPanel->AddChild(grid);
        scroll->AddChild(gridPanel);

        // 5. Virtualized Tree Stress Test (2,000 rows)
        auto treePanel = MakeUI<UIStackBox>(UIOrientation::Vertical, 8.0f);
        treePanel->StyleClass = "Panel";
        treePanel->DrawBackground = true;
        treePanel->Padding = UIThickness::All(12.0f);
        treePanel->AddChild(MakeUI<UILabel>("4. Virtualized UITreeView (2,000 Items, O(visible) Paint)", 14.5f));

        auto demoTree = MakeUI<UITreeView>();
        demoTree->HeightMode = UISizeMode::Fixed;
        demoTree->FixedSize.y = 200.0f;
        UITreeNode rootNode{ 1, "StressTestRoot (2,000)", true, { 0.24f, 0.56f, 0.96f, 1.0f }, {} };
        for (int i = 0; i < 2000; ++i)
        {
            rootNode.Children.push_back({
                static_cast<uint64_t>(10 + i),
                "VirtualizedRow_" + std::to_string(i),
                false,
                { 0.35f, 0.80f, 0.60f, 1.0f },
                {}
            });
        }
        demoTree->SetNodes({ std::move(rootNode) });
        demoTree->OnSelect = [report](uint64_t id) { report("DemoTree selected ID: " + std::to_string(id)); };
        treePanel->AddChild(demoTree);
        scroll->AddChild(treePanel);

        // 6. Dropdown, ProgressBar, Separator & CollapsingHeader
        auto extraPanel = MakeUI<UIStackBox>(UIOrientation::Vertical, 8.0f);
        extraPanel->StyleClass = "Panel";
        extraPanel->DrawBackground = true;
        extraPanel->Padding = UIThickness::All(12.0f);
        extraPanel->AddChild(MakeUI<UILabel>("5. Dropdown, ProgressBar, Separator & CollapsingHeader", 14.5f));
        extraPanel->AddChild(MakeUI<UISeparator>());

        auto dropRow = MakeUI<UIStackBox>(UIOrientation::Horizontal, 10.0f);
        auto dropLabel = MakeUI<UILabel>("Render Mode:", 13.5f);
        dropLabel->VAlign = UIAlignment::Center;
        auto dropdown = MakeUI<UIDropdown>(
            &context,
            std::vector<std::string>{ "Deferred VisBuffer", "Forward Lit", "Path Tracer", "Wireframe" },
            0,
            [report](int idx) { report("Dropdown selected index: " + std::to_string(idx)); }
        );
        dropdown->WidthMode = UISizeMode::Fill;
        dropRow->AddChild(dropLabel);
        dropRow->AddChild(dropdown);
        extraPanel->AddChild(dropRow);

        auto progressBar = MakeUI<UIProgressBar>(0.65f);
        auto progressSlider = MakeUI<UISlider>(0.65f, 0.0f, 1.0f, [progressBar, report](float v)
        {
            progressBar->SetFraction(v);
            report("ProgressBar: " + std::to_string(static_cast<int>(v * 100.0f)) + "%");
        });
        extraPanel->AddChild(progressBar);
        extraPanel->AddChild(progressSlider);

        extraPanel->AddChild(MakeUI<UISeparator>());

        auto section = MakeUI<UICollapsingHeader>("Collapsible Details Section", true);
        section->AddContent(MakeUI<UILabel>("This content toggles cleanly via UICollapsingHeader.", 13.0f));
        section->AddContent(MakeUI<UIButton>("Nested Action", [report]() { report("Collapsed section button clicked"); }));
        extraPanel->AddChild(section);

        scroll->AddChild(extraPanel);

        // 7. Color Edit, Inline Color Picker & Tooltips
        auto colorPanel = MakeUI<UIStackBox>(UIOrientation::Vertical, 8.0f);
        colorPanel->StyleClass = "Panel";
        colorPanel->DrawBackground = true;
        colorPanel->Padding = UIThickness::All(12.0f);
        colorPanel->AddChild(MakeUI<UILabel>("6. Color Edit, HSVA Color Picker & Hover Tooltips", 14.5f));

        auto colorRow = MakeUI<UIStackBox>(UIOrientation::Horizontal, 12.0f);
        auto inlinePicker = MakeUI<UIColorPicker>(UIColor{ 0.24f, 0.56f, 0.96f, 1.0f });
        inlinePicker->Tooltip = "Drag SV square, Hue bar, or Alpha bar";

        auto colorEdit = MakeUI<UIColorEdit>(&context, UIColor{ 0.24f, 0.56f, 0.96f, 1.0f }, [inlinePicker, report](const UIColor& c)
        {
            inlinePicker->SetColor(c, false);
            report("ColorEdit: " + inlinePicker->GetHexString());
        });
        colorEdit->Tooltip = "Click to open floating HSVA Color Picker popover";
        colorEdit->VAlign = UIAlignment::Start;

        inlinePicker->OnColorChanged = [colorEdit, progressBar, report](const UIColor& c)
        {
            colorEdit->SetColor(c, false);
            progressBar->FillColor = c;
            report("Color updated (synced to ProgressBar fill)");
        };

        colorRow->AddChild(inlinePicker);
        colorRow->AddChild(colorEdit);
        colorPanel->AddChild(colorRow);
        scroll->AddChild(colorPanel);

        // 8. RadioGroup, Live Theme Switcher & Virtualized Sortable UITableView
        auto tablePanel = MakeUI<UIStackBox>(UIOrientation::Vertical, 8.0f);
        tablePanel->StyleClass = "Panel";
        tablePanel->DrawBackground = true;
        tablePanel->Padding = UIThickness::All(12.0f);
        tablePanel->AddChild(MakeUI<UILabel>("7. RadioGroup, Live Theme Switcher & Sortable UITableView (500 Rows)", 14.5f));

        auto radio = MakeUI<UIRadioGroup>(
            std::vector<std::string>{ "Default Dark", "Midnight Cyan", "Warm Amber" },
            0,
            [&context, report](int idx)
            {
                auto theme = UITheme::CreateDefault();
                if (idx == 1)
                {
                    auto panel = theme->Resolve("Panel", "", UIState::Normal);
                    panel.FillColor = { 0.06f, 0.09f, 0.14f, 0.96f };
                    panel.BorderColor = { 0.14f, 0.28f, 0.42f, 1.0f };
                    theme->SetStyle("Panel", panel);

                    auto primary = theme->Resolve("Button", "primary", UIState::Normal);
                    primary.FillColor = { 0.08f, 0.56f, 0.72f, 1.0f };
                    theme->SetStyle("Button.primary", primary);
                }
                else if (idx == 2)
                {
                    auto panel = theme->Resolve("Panel", "", UIState::Normal);
                    panel.FillColor = { 0.12f, 0.10f, 0.09f, 0.96f };
                    panel.BorderColor = { 0.36f, 0.27f, 0.18f, 1.0f };
                    theme->SetStyle("Panel", panel);

                    auto primary = theme->Resolve("Button", "primary", UIState::Normal);
                    primary.FillColor = { 0.80f, 0.52f, 0.14f, 1.0f };
                    theme->SetStyle("Button.primary", primary);
                }
                context.SetTheme(theme);
                report("Theme preset switched (" + std::to_string(idx) + ")");
            },
            UIOrientation::Horizontal
        );
        tablePanel->AddChild(radio);

        auto table = MakeUI<UITableView>();
        table->HeightMode = UISizeMode::Fixed;
        table->FixedSize.y = 200.0f;
        table->SetColumns({
            { "ID", 70.0f, 0.0f, true },
            { "Entity / Asset Name", 0.0f, 1.6f, true },
            { "Category", 0.0f, 1.0f, true }
        });
        std::vector<UITableRow> rows;
        rows.reserve(500);
        static const char* kCategories[] = { "Mesh", "Material", "Light", "Shader", "Audio" };
        for (int i = 0; i < 500; ++i)
        {
            char idBuf[16]{};
            std::snprintf(idBuf, sizeof(idBuf), "%04d", i + 1);
            rows.push_back({
                static_cast<uint64_t>(i + 1),
                { idBuf, "DemoItem_" + std::to_string(i + 1), kCategories[i % 5] }
            });
        }
        table->SetRows(std::move(rows));
        table->OnSelect = [report](uint64_t rowId, int)
        {
            report("UITableView selected row ID: " + std::to_string(rowId));
        };
        tablePanel->AddChild(table);
        scroll->AddChild(tablePanel);

        return scroll;
    }
}
