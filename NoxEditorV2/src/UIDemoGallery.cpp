#include "UIDemoGallery.h"

#include <cmath>
#include <cstdio>
#include <memory>

#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Layout/UIWindow.h"
#include "NoxCore/UI/Layout/UIWrapBox.h"
#include "NoxCore/UI/Widgets/UIBadge.h"
#include "NoxCore/UI/Widgets/UIBreadcrumb.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UICheckBox.h"
#include "NoxCore/UI/Widgets/UICollapsingHeader.h"
#include "NoxCore/UI/Widgets/UIColorPicker.h"
#include "NoxCore/UI/Widgets/UIDragScalar.h"
#include "NoxCore/UI/Widgets/UIDropdown.h"
#include "NoxCore/UI/Widgets/UIImage.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UILogView.h"
#include "NoxCore/UI/Widgets/UIMenu.h"
#include "NoxCore/UI/Widgets/UIPlotGraph.h"
#include "NoxCore/UI/Widgets/UIProgressBar.h"
#include "NoxCore/UI/Widgets/UIPropertyGrid.h"
#include "NoxCore/UI/Widgets/UIRadioGroup.h"
#include "NoxCore/UI/Widgets/UIScrollView.h"
#include "NoxCore/UI/Widgets/UISeparator.h"
#include "NoxCore/UI/Widgets/UISlider.h"
#include "NoxCore/UI/Widgets/UITableView.h"
#include "NoxCore/UI/Widgets/UITextInput.h"
#include "NoxCore/UI/Widgets/UIToggleSwitch.h"
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

        // 9. PlotGraph (Line & Histogram) + Right-Click Context Menu
        auto plotPanel = MakeUI<UIStackBox>(UIOrientation::Vertical, 8.0f);
        plotPanel->StyleClass = "Panel";
        plotPanel->DrawBackground = true;
        plotPanel->Padding = UIThickness::All(12.0f);
        plotPanel->AddChild(MakeUI<UILabel>("8. UIPlotGraph (Line / Histogram) & Right-Click Context Menu", 14.5f));

        auto plot = MakeUI<UIPlotGraph>("Frame Time / Signal (ms)", UIPlotMode::Line);
        plot->HeightMode = UISizeMode::Fixed;
        plot->FixedSize.y = 130.0f;
        std::vector<float> initialWave;
        for (int i = 0; i < 48; ++i)
            initialWave.push_back(16.6f + std::sin(static_cast<float>(i) * 0.32f) * 4.5f + std::cos(static_cast<float>(i) * 0.7f) * 1.8f);
        plot->SetValues(std::move(initialWave));

        auto phase = std::make_shared<int>(48);
        auto plotControls = MakeUI<UIStackBox>(UIOrientation::Horizontal, 8.0f);
        auto pushBtn = MakeUI<UIButton>("Push Sample", [plot, phase, report]()
        {
            ++(*phase);
            const float val = 16.6f + std::sin(static_cast<float>(*phase) * 0.32f) * 5.0f;
            plot->PushSample(val);
            report("Pushed plot sample: " + std::to_string(val));
        });
        auto modeToggle = MakeUI<UICheckBox>("Histogram Mode", false, [plot](bool hist)
        {
            plot->SetMode(hist ? UIPlotMode::Histogram : UIPlotMode::Line);
        });
        modeToggle->VAlign = UIAlignment::Center;

        auto ctxArea = MakeUI<UIButton>("Right-Click Me (Context Menu)");
        ctxArea->Tooltip = "Right-click anywhere on this button to open a context menu";
        ctxArea->OnMouseDown = [&context, plot, report](const UIPointerEvent& e) -> bool
        {
            if (e.Button == 3)
            {
                UIMenuPopup::ShowContextMenu(context, e.Position, {
                    { "Switch to Line", "", [plot, report]() { plot->SetMode(UIPlotMode::Line); report("Context Menu: Line"); } },
                    { "Switch to Histogram", "", [plot, report]() { plot->SetMode(UIPlotMode::Histogram); report("Context Menu: Histogram"); } },
                    { "", "", {}, true },
                    { "Reset Plot", "", [plot, report]() { plot->Clear(); report("Context Menu: Cleared Plot"); } }
                });
                return true;
            }
            return false;
        };

        plotControls->AddChild(pushBtn);
        plotControls->AddChild(modeToggle);
        plotControls->AddChild(ctxArea);
        plotPanel->AddChild(plotControls);
        plotPanel->AddChild(plot);
        scroll->AddChild(plotPanel);

        // 10. Draggable/Resizable UIWindow & Toast Notifications
        auto winPanel = MakeUI<UIStackBox>(UIOrientation::Vertical, 8.0f);
        winPanel->StyleClass = "Panel";
        winPanel->DrawBackground = true;
        winPanel->Padding = UIThickness::All(12.0f);
        winPanel->AddChild(MakeUI<UILabel>("9. Floating UIWindow (Drag & Resize) & Toast Notifications", 14.5f));

        auto winRow = MakeUI<UIStackBox>(UIOrientation::Horizontal, 8.0f);
        auto toastCount = std::make_shared<int>(0);
        auto toastBtn = MakeUI<UIButton>("Push Toast Notification", [&context, toastCount, report]()
        {
            ++(*toastCount);
            context.PushToast("Toast #" + std::to_string(*toastCount) + "  -  Saved & Ready!", 3.0f, { 0.24f, 0.76f, 0.48f, 1.0f });
            report("Pushed Toast #" + std::to_string(*toastCount));
        });
        toastBtn->StyleVariant = "primary";

        auto spawnWinBtn = MakeUI<UIButton>("Open Floating UIWindow", [&context, report]()
        {
            auto host = MakeUI<UIAnchorBox>();
            host->HitTestMode = UIHitTestMode::PassThrough;

            auto win = MakeUI<UIWindow>("Floating Tool Window", [&context, report]()
            {
                context.ClearOverlay();
                report("Closed Floating UIWindow");
            });
            win->AddContent(MakeUI<UILabel>("Drag the title bar to move me.", 13.5f));
            win->AddContent(MakeUI<UILabel>("Drag the bottom-right corner to resize.", 13.5f));
            win->AddContent(MakeUI<UISlider>(0.5f, 0.0f, 1.0f, [&context](float v)
            {
                context.PushToast("Window Slider: " + std::to_string(static_cast<int>(v * 100.0f)) + "%", 1.5f);
            }));
            host->AddChild(win);
            context.SetOverlay(host);
        });

        winRow->AddChild(toastBtn);
        winRow->AddChild(spawnWinBtn);
        winPanel->AddChild(winRow);
        scroll->AddChild(winPanel);

        // 11. Filterable UILogView & Keyboard Tab/Escape Focus
        auto logPanel = MakeUI<UIStackBox>(UIOrientation::Vertical, 8.0f);
        logPanel->StyleClass = "Panel";
        logPanel->DrawBackground = true;
        logPanel->Padding = UIThickness::All(12.0f);
        logPanel->AddChild(MakeUI<UILabel>("10. Filterable UILogView & Tab / Shift+Tab / Esc Navigation", 14.5f));

        auto logView = MakeUI<UILogView>();
        logView->HeightMode = UISizeMode::Fixed;
        logView->FixedSize.y = 180.0f;
        logView->AddLog(UILogLevel::Info, "Nox::UI initialized (Tab / Shift+Tab cycles focus, Esc closes overlays)");
        logView->AddLog(UILogLevel::Warning, "Sample warning: shader permutation cache miss");
        logView->AddLog(UILogLevel::Error, "Sample error: failed to load optional asset");

        auto logBtns = MakeUI<UIStackBox>(UIOrientation::Horizontal, 8.0f);
        logBtns->AddChild(MakeUI<UIButton>("+ Info", [logView]() { logView->AddLog(UILogLevel::Info, "User triggered Info log entry"); }));
        logBtns->AddChild(MakeUI<UIButton>("+ Warn", [logView]() { logView->AddLog(UILogLevel::Warning, "User triggered Warning log entry"); }));
        logBtns->AddChild(MakeUI<UIButton>("+ Error", [logView]() { logView->AddLog(UILogLevel::Error, "User triggered Error log entry"); }));

        logPanel->AddChild(logBtns);
        logPanel->AddChild(logView);
        scroll->AddChild(logPanel);

        // 12. Animated UIToggleSwitch & UIBadge Pills
        auto switchPanel = MakeUI<UIStackBox>(UIOrientation::Vertical, 8.0f);
        switchPanel->StyleClass = "Panel";
        switchPanel->DrawBackground = true;
        switchPanel->Padding = UIThickness::All(12.0f);
        switchPanel->AddChild(MakeUI<UILabel>("11. Animated UIToggleSwitch & UIBadge Pills", 14.5f));

        auto switchRow = MakeUI<UIStackBox>(UIOrientation::Horizontal, 14.0f);
        auto sw1 = MakeUI<UIToggleSwitch>("VSync", true, [report](bool on) { report(std::string("VSync: ") + (on ? "ON" : "OFF")); });
        auto sw2 = MakeUI<UIToggleSwitch>("Path Tracer", false, [report](bool on) { report(std::string("Path Tracer: ") + (on ? "ON" : "OFF")); });
        sw2->OnColor = { 0.24f, 0.76f, 0.48f, 1.0f };

        auto badge1 = MakeUI<UIBadge>("SDF UI", UIColor{ 0.24f, 0.56f, 0.96f, 1.0f });
        auto badge2 = MakeUI<UIBadge>("READY", UIColor{ 0.22f, 0.76f, 0.46f, 1.0f });
        auto badge3 = MakeUI<UIBadge>("BETA", UIColor{ 0.92f, 0.62f, 0.16f, 1.0f });
        badge1->VAlign = UIAlignment::Center;
        badge2->VAlign = UIAlignment::Center;
        badge3->VAlign = UIAlignment::Center;

        switchRow->AddChild(sw1);
        switchRow->AddChild(sw2);
        switchRow->AddChild(badge1);
        switchRow->AddChild(badge2);
        switchRow->AddChild(badge3);
        switchPanel->AddChild(switchRow);
        scroll->AddChild(switchPanel);

        // 13. UIBreadcrumb & Responsive UIWrapBox
        auto flowPanel = MakeUI<UIStackBox>(UIOrientation::Vertical, 8.0f);
        flowPanel->StyleClass = "Panel";
        flowPanel->DrawBackground = true;
        flowPanel->Padding = UIThickness::All(12.0f);
        flowPanel->AddChild(MakeUI<UILabel>("12. UIBreadcrumb & Responsive UIWrapBox", 14.5f));

        auto crumb = MakeUI<UIBreadcrumb>(
            std::vector<std::string>{ "Engine", "Nox::UI", "Widgets", "Gallery" },
            [&context, report](int idx)
            {
                context.PushToast("Breadcrumb segment #" + std::to_string(idx) + " clicked", 1.8f);
                report("Breadcrumb navigated to index " + std::to_string(idx));
            }
        );
        flowPanel->AddChild(crumb);

        auto wrap = MakeUI<UIWrapBox>(6.0f, 6.0f);
        static const char* kTags[] = {
            "Bindless SDF", "Retained Mode", "Tab Navigation", "Toast Alerts",
            "HSVA Picker", "Sortable Table", "Live Plot", "Node Graph",
            "3D Gizmo", "Hot-Reload .nstyle", "Virtual Tree", "Floating Window"
        };
        for (const char* tag : kTags)
        {
            wrap->AddChild(MakeUI<UIButton>(tag, [tag, report]()
            {
                report(std::string("WrapBox tag clicked: ") + tag);
            }));
        }
        flowPanel->AddChild(wrap);

        auto imgRow = MakeUI<UIStackBox>(UIOrientation::Horizontal, 10.0f);
        auto imgPreview = MakeUI<UIImage>(~0u, UIVec2{ 56.0f, 56.0f });
        imgPreview->CornerRadius = UICornerRadius::All(8.0f);
        imgPreview->Tooltip = "UIImage widget (displays any bindless texture descriptor slot, or fallback card when ~0u)";
        auto imgLabel = MakeUI<UILabel>("UIImage (Bindless Texture Slot / Rounded Fallback Card)", 13.0f);
        imgLabel->VAlign = UIAlignment::Center;
        imgRow->AddChild(imgPreview);
        imgRow->AddChild(imgLabel);
        flowPanel->AddChild(imgRow);

        scroll->AddChild(flowPanel);

        // 14. Drag & Drop Payloads
        auto dndPanel = MakeUI<UIStackBox>(UIOrientation::Vertical, 8.0f);
        dndPanel->StyleClass = "Panel";
        dndPanel->DrawBackground = true;
        dndPanel->Padding = UIThickness::All(12.0f);
        dndPanel->AddChild(MakeUI<UILabel>("13. Drag & Drop Payloads (UIElement::OnDragStart / OnDrop)", 14.5f));

        auto dndRow = MakeUI<UIStackBox>(UIOrientation::Horizontal, 10.0f);
        auto src1 = MakeUI<UIButton>("Drag: Material_Gold");
        src1->OnDragStart = []() -> std::optional<UIDragPayload>
        {
            return UIDragPayload{ "DemoAsset", "Material_Gold", 101 };
        };
        auto src2 = MakeUI<UIButton>("Drag: Mesh_Sphere");
        src2->OnDragStart = []() -> std::optional<UIDragPayload>
        {
            return UIDragPayload{ "DemoAsset", "Mesh_Sphere", 102 };
        };

        auto dropTarget = MakeUI<UIButton>("Drop Asset Here");
        dropTarget->StyleClass = "TextInput";
        dropTarget->WidthMode = UISizeMode::Fill;
        dropTarget->AcceptedDropType = "DemoAsset";
        UIButton* dropPtr = dropTarget.get();
        dropTarget->OnDrop = [&context, dropPtr, report](const UIDragPayload& payload) -> bool
        {
            dropPtr->SetText("Dropped: " + payload.Text + " (ID " + std::to_string(payload.UserId) + ")");
            context.PushToast("Dropped '" + payload.Text + "'", 2.0f, { 0.24f, 0.76f, 0.48f, 1.0f });
            report("Dropped payload: " + payload.Text);
            return true;
        };

        dndRow->AddChild(src1);
        dndRow->AddChild(src2);
        dndRow->AddChild(dropTarget);
        dndPanel->AddChild(dndRow);
        scroll->AddChild(dndPanel);

        return scroll;
    }
}
