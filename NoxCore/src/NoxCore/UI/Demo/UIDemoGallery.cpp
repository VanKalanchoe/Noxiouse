#include "NoxCore/UI/Demo/UIDemoGallery.h"

#include <cmath>
#include <cstdio>
#include <memory>

#include <SDL3/SDL_scancode.h>

#include "NoxCore/Reflection/TypeMeta.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Layout/UIAccordion.h"
#include "NoxCore/UI/Layout/UIGroupBox.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Layout/UITabPanel.h"
#include "NoxCore/UI/Layout/UIWrapBox.h"
#include "NoxCore/UI/Widgets/UIAlertBanner.h"
#include "NoxCore/UI/Widgets/UIBadge.h"
#include "NoxCore/UI/Widgets/UIBreadcrumb.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UICheckBox.h"
#include "NoxCore/UI/Widgets/UICollapsingHeader.h"
#include "NoxCore/UI/Widgets/UIColorPicker.h"
#include "NoxCore/UI/Widgets/UICommandPalette.h"
#include "NoxCore/UI/Widgets/UICurveEditor.h"
#include "NoxCore/UI/Widgets/UIDragScalar.h"
#include "NoxCore/UI/Widgets/UIDropdown.h"
#include "NoxCore/UI/Widgets/UIKeybindButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UILogView.h"
#include "NoxCore/UI/Widgets/UIMenu.h"
#include "NoxCore/UI/Widgets/UIModal.h"
#include "NoxCore/UI/Widgets/UIPagination.h"
#include "NoxCore/UI/Widgets/UIPlotGraph.h"
#include "NoxCore/UI/Widgets/UIProgressBar.h"
#include "NoxCore/UI/Widgets/UIPropertyGrid.h"
#include "NoxCore/UI/Widgets/UIRadioGroup.h"
#include "NoxCore/UI/Widgets/UIRangeSlider.h"
#include "NoxCore/UI/Widgets/UIRotaryKnob.h"
#include "NoxCore/UI/Widgets/UIScrollView.h"
#include "NoxCore/UI/Widgets/UISearchBar.h"
#include "NoxCore/UI/Widgets/UISegmentedControl.h"
#include "NoxCore/UI/Widgets/UISeparator.h"
#include "NoxCore/UI/Widgets/UISlider.h"
#include "NoxCore/UI/Widgets/UISpinBox.h"
#include "NoxCore/UI/Widgets/UISpinner.h"
#include "NoxCore/UI/Widgets/UISplitButton.h"
#include "NoxCore/UI/Widgets/UIStarRating.h"
#include "NoxCore/UI/Widgets/UIStatCard.h"
#include "NoxCore/UI/Widgets/UITableView.h"
#include "NoxCore/UI/Widgets/UITagInput.h"
#include "NoxCore/UI/Widgets/UITextArea.h"
#include "NoxCore/UI/Widgets/UITextInput.h"
#include "NoxCore/UI/Widgets/UITimeline.h"
#include "NoxCore/UI/Widgets/UIToggleSwitch.h"
#include "NoxCore/UI/Widgets/UITreeView.h"
#include "NoxCore/UI/Widgets/UIVectorEdit.h"

namespace Nox::UI
{
    namespace
    {
        Ref<UIScrollView> MakeScrollPage()
        {
            auto scroll = MakeUI<UIScrollView>();
            scroll->WidthMode = UISizeMode::Fill;
            scroll->HeightMode = UISizeMode::Fill;
            scroll->Padding = UIThickness::All(12.0f);
            scroll->ChildSpacing = 12.0f;
            return scroll;
        }

        UIColor GetToken(const UIContext& ctx, const char* name, const UIColor& fallback)
        {
            const auto& theme = ctx.GetTheme();
            return theme ? theme->GetToken(name, fallback) : fallback;
        }
    }

    std::vector<UIDemoPanel> CreateUIDemoPanels(UIContext& context, const std::function<void(const std::string&)>& onStatus)
    {
        auto reportStatus = [onStatus](const std::string& msg)
        {
            if (onStatus)
                onStatus(msg);
        };

        const UIColor accent = GetToken(context, "Accent", { 0.259f, 0.529f, 0.961f, 1.0f });
        const UIColor success = GetToken(context, "Success", { 0.235f, 0.678f, 0.482f, 1.0f });
        const UIColor warning = GetToken(context, "Warning", { 0.851f, 0.616f, 0.224f, 1.0f });
        const UIColor muted = GetToken(context, "TextMuted", { 0.580f, 0.616f, 0.686f, 1.0f });

        std::vector<UIDemoPanel> panels;

        // =====================================================================
        // Tab 1: Controls
        // =====================================================================
        {
            auto page = MakeScrollPage();
            auto grid = MakeUI<UIGridBox>(2, 12.0f, 12.0f);
            grid->WidthMode = UISizeMode::Fill;
            grid->MinColumnWidth = 300.0f;

            // Actions & States
            auto actionsCard = MakeUI<UIGroupBox>("Actions & Keybinds");
            auto btnWrap = MakeUI<UIWrapBox>(8.0f, 8.0f);

            auto primaryBtn = MakeUI<UIButton>("Primary Action", [reportStatus, &context]()
            {
                reportStatus("Triggered primary action");
                context.PushToast("Primary action executed");
            });
            primaryBtn->StyleVariant = "primary";
            primaryBtn->Tooltip = "Primary styled button";

            auto secondaryBtn = MakeUI<UIButton>("Secondary", [reportStatus]()
            {
                reportStatus("Clicked secondary button");
            });

            auto disabledBtn = MakeUI<UIButton>("Disabled");
            disabledBtn->SetEnabled(false);

            auto splitBtn = MakeUI<UISplitButton>(
                context,
                "Build",
                [reportStatus]() { reportStatus("Build started"); },
                std::vector<UIMenuItem>{
                    { "Build Debug", "", [reportStatus]() { reportStatus("Build Debug"); } },
                    { "Build Release", "", [reportStatus]() { reportStatus("Build Release"); } },
                    { "Clean & Rebuild", "", [reportStatus]() { reportStatus("Clean & Rebuild"); } }
                }
            );

            auto keybind = MakeUI<UIKeybindButton>(SDL_SCANCODE_F, 0, [reportStatus](int scancode, uint16_t)
            {
                reportStatus("Bound key scancode: " + std::to_string(scancode));
            });

            btnWrap->AddChild(primaryBtn);
            btnWrap->AddChild(secondaryBtn);
            btnWrap->AddChild(disabledBtn);
            btnWrap->AddChild(splitBtn);
            btnWrap->AddChild(keybind);
            actionsCard->AddContent(btnWrap);
            grid->AddChild(actionsCard);

            // Selection & Toggles
            auto selectCard = MakeUI<UIGroupBox>("Selection & Toggles");
            auto toggleRow = MakeUI<UIWrapBox>(14.0f, 8.0f);
            toggleRow->AddChild(MakeUI<UICheckBox>("Enable Shadows", true, [reportStatus](bool v)
            {
                reportStatus(std::string("Shadows: ") + (v ? "On" : "Off"));
            }));
            toggleRow->AddChild(MakeUI<UIToggleSwitch>("Auto-Save", true, [reportStatus](bool v)
            {
                reportStatus(std::string("Auto-Save: ") + (v ? "On" : "Off"));
            }));
            selectCard->AddContent(toggleRow);

            auto pickersRow = MakeUI<UIStackBox>(UIOrientation::Horizontal, 8.0f);
            pickersRow->WidthMode = UISizeMode::Fill;
            auto dropdown = MakeUI<UIDropdown>(&context, std::vector<std::string>{ "Perspective", "Orthographic", "Top", "Front" }, 0);
            dropdown->WidthMode = UISizeMode::Fill;
            dropdown->OnSelectionChanged = [dropdown, reportStatus](int)
            {
                reportStatus("Camera: " + dropdown->GetSelectedText());
            };
            auto segmented = MakeUI<UISegmentedControl>(
                std::vector<std::string>{ "Local", "World" },
                0,
                [reportStatus](int idx) { reportStatus(idx == 0 ? "Space: Local" : "Space: World"); }
            );
            pickersRow->AddChild(dropdown);
            pickersRow->AddChild(segmented);
            selectCard->AddContent(pickersRow);

            auto radioRatingRow = MakeUI<UIStackBox>(UIOrientation::Horizontal, 12.0f);
            radioRatingRow->WidthMode = UISizeMode::Fill;
            auto radio = MakeUI<UIRadioGroup>(
                std::vector<std::string>{ "Low", "Medium", "High" },
                1,
                [reportStatus](int idx) { reportStatus("Quality preset #" + std::to_string(idx)); },
                UIOrientation::Horizontal
            );
            radio->WidthMode = UISizeMode::Fill;
            auto stars = MakeUI<UIStarRating>(4, 5, [reportStatus](int r)
            {
                reportStatus("Rating: " + std::to_string(r) + "/5");
            });
            stars->VAlign = UIAlignment::Center;
            radioRatingRow->AddChild(radio);
            radioRatingRow->AddChild(stars);
            selectCard->AddContent(radioRatingRow);
            grid->AddChild(selectCard);

            // Sliders & Numeric Inputs
            auto numbersCard = MakeUI<UIGroupBox>("Sliders & Numeric Inputs");
            auto progress = MakeUI<UIProgressBar>(0.65f);

            auto slider = MakeUI<UISlider>(0.65f, 0.0f, 1.0f, [progress, reportStatus](float v)
            {
                progress->SetFraction(v);
                reportStatus("Exposure: " + std::to_string(v));
            });
            slider->WidthMode = UISizeMode::Fill;
            numbersCard->AddContent(slider);
            numbersCard->AddContent(progress);

            auto rangeSlider = MakeUI<UIRangeSlider>(0.2f, 0.8f, 0.0f, 1.0f, [reportStatus](float lo, float hi)
            {
                char buf[64]{};
                std::snprintf(buf, sizeof(buf), "Range: %.2f .. %.2f", lo, hi);
                reportStatus(buf);
            });
            numbersCard->AddContent(rangeSlider);

            auto numRow = MakeUI<UIStackBox>(UIOrientation::Horizontal, 8.0f);
            numRow->WidthMode = UISizeMode::Fill;
            auto spin = MakeUI<UISpinBox>(4.0f, 1.0f, 0.0f, 32.0f, [reportStatus](float v)
            {
                reportStatus("Samples: " + std::to_string(static_cast<int>(v)));
            });
            auto scalar = MakeUI<UIDragScalar>(1.0f, 0.05f);
            scalar->Prefix = "x";
            scalar->PrefixColor = accent;
            scalar->Min = 0.1f;
            scalar->Max = 10.0f;
            scalar->OnChanged = [reportStatus](float v) { reportStatus("Scale: " + std::to_string(v)); };
            numRow->AddChild(spin);
            numRow->AddChild(scalar);
            numbersCard->AddContent(numRow);

            auto vecEdit = MakeUI<UIVectorEdit>(3, glm::vec4(1.0f, 2.5f, 0.0f, 0.0f), 0.1f, [reportStatus](const glm::vec4& v)
            {
                char buf[64]{};
                std::snprintf(buf, sizeof(buf), "Vec3: (%.1f, %.1f, %.1f)", v.x, v.y, v.z);
                reportStatus(buf);
            });
            numbersCard->AddContent(vecEdit);
            grid->AddChild(numbersCard);

            // Knobs & Indicators
            auto miscCard = MakeUI<UIGroupBox>("Dials & Status");
            auto dialRow = MakeUI<UIWrapBox>(16.0f, 8.0f);
            dialRow->AddChild(MakeUI<UIRotaryKnob>("Gain", 0.5f, 0.0f, 1.0f, [reportStatus](float v)
            {
                reportStatus("Gain: " + std::to_string(v));
            }));
            dialRow->AddChild(MakeUI<UIRotaryKnob>("Roughness", 0.35f, 0.0f, 1.0f, [reportStatus](float v)
            {
                reportStatus("Roughness: " + std::to_string(v));
            }));
            auto spinner = MakeUI<UISpinner>();
            spinner->VAlign = UIAlignment::Center;
            dialRow->AddChild(spinner);
            miscCard->AddContent(dialRow);

            auto badgeRow = MakeUI<UIWrapBox>(8.0f, 8.0f);
            badgeRow->AddChild(MakeUI<UIBadge>("Default", accent));
            badgeRow->AddChild(MakeUI<UIBadge>("Synced", success));
            badgeRow->AddChild(MakeUI<UIBadge>("Draft", warning));
            miscCard->AddContent(badgeRow);
            grid->AddChild(miscCard);

            page->AddChild(grid);
            panels.push_back({ "Controls", page });
        }

        // =====================================================================
        // Tab 2: Inputs & Data
        // =====================================================================
        {
            auto page = MakeScrollPage();
            auto grid = MakeUI<UIGridBox>(2, 12.0f, 12.0f);
            grid->WidthMode = UISizeMode::Fill;
            grid->MinColumnWidth = 300.0f;

            // Text & Color Inputs
            auto textCard = MakeUI<UIGroupBox>("Text, Search & Color");
            auto textInput = MakeUI<UITextInput>("Scene_Main", "Enter asset name...");
            textInput->WidthMode = UISizeMode::Fill;
            textInput->OnChanged = [reportStatus](const std::string& t) { reportStatus("Name: " + t); };
            textCard->AddContent(textInput);

            auto search = MakeUI<UISearchBar>("Search assets...", [reportStatus](const std::string& q)
            {
                reportStatus(q.empty() ? "Search cleared" : "Searching: " + q);
            });
            search->SetResultCount(12);
            textCard->AddContent(search);

            auto tags = MakeUI<UITagInput>(std::vector<std::string>{ "PBR", "Static", "LOD0" });
            textCard->AddContent(tags);

            auto colorEdit = MakeUI<UIColorEdit>(&context, accent, [reportStatus](const UIColor&)
            {
                reportStatus("Updated tint color");
            });
            textCard->AddContent(colorEdit);

            auto textArea = MakeUI<UITextArea>("// Notes\nAuto-scaling multiline editor.\nSupports selection and scrolling.", "Notes...");
            textArea->WidthMode = UISizeMode::Fill;
            textCard->AddContent(textArea);
            grid->AddChild(textCard);

            // Property Grid
            auto propCard = MakeUI<UIGroupBox>("Property Grid");
            auto props = MakeUI<UIPropertyGrid>();
            props->WidthMode = UISizeMode::Fill;
            auto genSec = props->AddSection("Transform", true);
            props->AddString("Name", "MainCamera", "Entity name", [reportStatus](const std::string& s) { reportStatus("Entity: " + s); }, genSec);
            props->AddBool("Visible", true, [reportStatus](bool v) { reportStatus(v ? "Visible" : "Hidden"); }, genSec);
            props->AddVec3("Position", { 0.0f, 1.5f, -4.0f }, 0.1f, [reportStatus](const glm::vec3&) { reportStatus("Moved Position"); }, genSec);

            auto matSec = props->AddSection("Material", true);
            props->AddFloatSlider("Metallic", 0.1f, 0.0f, 1.0f, [reportStatus](float v) { reportStatus("Metallic: " + std::to_string(v)); }, matSec);
            props->AddFloatSlider("Roughness", 0.45f, 0.0f, 1.0f, [reportStatus](float v) { reportStatus("Roughness: " + std::to_string(v)); }, matSec);

            if (!TypeRegistry::Get().Find<SpotLightComponent>())
            {
                ReflectType<SpotLightComponent>("Spot Light (Auto-Generated)")
                    .Color3("Color", &SpotLightComponent::Color)
                    .Float("Intensity", &SpotLightComponent::Intensity, 0.5f, 0.0f, 1000.0f)
                    .Float("Range", &SpotLightComponent::Range, 0.25f, 0.1f, 500.0f)
                    .Float("Inner Angle", &SpotLightComponent::InnerAngle, 0.5f, 0.0f, 180.0f)
                        .MaxFrom(&SpotLightComponent::OuterAngle)
                    .Float("Outer Angle", &SpotLightComponent::OuterAngle, 0.5f, 0.0f, 180.0f)
                    .Float("Radius", &SpotLightComponent::Radius, 0.01f, 0.0f, 5.0f)
                    .UInt32("Shadow Samples", &SpotLightComponent::ShadowSamples, 1.0f, 1, 16)
                    .OnPostEdit([](SpotLightComponent& c)
                    {
                        c.InnerAngle = std::min(c.InnerAngle, c.OuterAngle);
                    });
            }
            auto demoSpotLight = std::make_shared<SpotLightComponent>();
            props->Inspect(&context, *demoSpotLight, [demoSpotLight]() { return true; });

            propCard->AddContent(props);
            grid->AddChild(propCard);

            // Table View
            auto tableCard = MakeUI<UIGroupBox>("Table View");
            auto table = MakeUI<UITableView>();
            table->WidthMode = UISizeMode::Fill;
            table->SetColumns({
                { "ID", 0.0f, 0.5f, true },
                { "Asset", 0.0f, 1.6f, true },
                { "Type", 0.0f, 1.0f, true }
            });
            table->SetRows({
                { 1, { "01", "Env_Studio", "HDRI" } },
                { 2, { "02", "Metal_Brushed", "Material" } },
                { 3, { "03", "Hero_Mesh", "StaticMesh" } },
                { 4, { "04", "DefaultDark", "Theme" } }
            });
            table->OnSelect = [reportStatus](uint64_t id, int) { reportStatus("Selected row #" + std::to_string(id)); };
            tableCard->AddContent(table);
            grid->AddChild(tableCard);

            // Tree & Log View
            auto treeCard = MakeUI<UIGroupBox>("Tree & Log View");
            auto tree = MakeUI<UITreeView>();
            tree->WidthMode = UISizeMode::Fill;
            UITreeNode rootNode{ 1, "Root", true, accent, {
                { 2, "DirectionalLight", true, warning, {} },
                { 3, "CameraRig", true, success, {} }
            } };
            tree->SetNodes({ rootNode });
            tree->OnSelect = [reportStatus](uint64_t id) { reportStatus("Selected node #" + std::to_string(id)); };
            treeCard->AddContent(tree);

            auto logView = MakeUI<UILogView>();
            logView->AddLog(UILogLevel::Info, "Renderer initialized");
            logView->AddLog(UILogLevel::Warning, "Shader cache warmed");
            logView->AddLog(UILogLevel::Info, "UI theme active");
            treeCard->AddContent(logView);
            grid->AddChild(treeCard);

            page->AddChild(grid);
            panels.push_back({ "Inputs & Data", page });
        }

        // =====================================================================
        // Tab 3: Layout & Graphs
        // =====================================================================
        {
            auto page = MakeScrollPage();

            // Stat Cards row
            auto statsRow = MakeUI<UIStackBox>(UIOrientation::Horizontal, 12.0f);
            statsRow->WidthMode = UISizeMode::Fill;
            statsRow->AddChild(MakeUI<UIStatCard>("Frame Time", "6.2 ms", "60+ FPS", success));
            statsRow->AddChild(MakeUI<UIStatCard>("Draw Calls", "42", "Batched", accent));
            statsRow->AddChild(MakeUI<UIStatCard>("VRAM Used", "1.4 GB", "Nominal", warning));
            page->AddChild(statsRow);

            auto grid = MakeUI<UIGridBox>(2, 12.0f, 12.0f);
            grid->WidthMode = UISizeMode::Fill;
            grid->MinColumnWidth = 300.0f;

            // Containers
            auto containerCard = MakeUI<UIGroupBox>("Navigation & Disclosure");
            auto breadcrumb = MakeUI<UIBreadcrumb>(
                std::vector<std::string>{ "Assets", "UI", "Themes" },
                [reportStatus](int idx) { reportStatus("Breadcrumb index: " + std::to_string(idx)); }
            );
            containerCard->AddContent(breadcrumb);

            auto accordion = MakeUI<UIAccordion>(false);
            accordion->AddSection("General", MakeUI<UILabel>("Auto-sized accordion section content."), true);
            accordion->AddSection("Advanced", MakeUI<UILabel>("Second collapsible section."), false);
            containerCard->AddContent(accordion);

            auto collapse = MakeUI<UICollapsingHeader>("Details", false);
            collapse->AddContent(MakeUI<UILabel>("Hidden until expanded."));
            containerCard->AddContent(collapse);

            auto pagination = MakeUI<UIPagination>(9, 1, [reportStatus](int p)
            {
                reportStatus("Page " + std::to_string(p));
            });
            containerCard->AddContent(pagination);
            grid->AddChild(containerCard);

            // Graphs & Timeline
            auto graphCard = MakeUI<UIGroupBox>("Plots, Curves & Timeline");
            auto plot = MakeUI<UIPlotGraph>("Frame Time (ms)", UIPlotMode::Line);
            plot->WidthMode = UISizeMode::Fill;
            plot->LineColor = accent;
            plot->SetValues({ 6.4f, 6.1f, 6.8f, 5.9f, 6.0f, 6.3f, 6.1f, 5.8f, 6.2f });
            graphCard->AddContent(plot);

            auto curve = MakeUI<UICurveEditor>();
            curve->WidthMode = UISizeMode::Fill;
            graphCard->AddContent(curve);

            auto timeline = MakeUI<UITimeline>(10.0f);
            timeline->SetKeyframes({ 1.5f, 4.0f, 7.5f });
            timeline->OnScrub = [reportStatus](float t)
            {
                char buf[48]{};
                std::snprintf(buf, sizeof(buf), "Timeline: %.2fs", t);
                reportStatus(buf);
            };
            graphCard->AddContent(timeline);
            grid->AddChild(graphCard);

            page->AddChild(grid);
            panels.push_back({ "Layout & Graphs", page });
        }

        // =====================================================================
        // Tab 4: Overlays & Theme
        // =====================================================================
        {
            auto page = MakeScrollPage();
            auto grid = MakeUI<UIGridBox>(2, 12.0f, 12.0f);
            grid->WidthMode = UISizeMode::Fill;
            grid->MinColumnWidth = 300.0f;

            auto overlayCard = MakeUI<UIGroupBox>("Modals, Palette & Toasts");
            auto overlayWrap = MakeUI<UIWrapBox>(8.0f, 8.0f);

            auto confirmBtn = MakeUI<UIButton>("Confirm Modal", [&context, reportStatus]()
            {
                UIModal::ShowConfirm(
                    context,
                    "Reset Layout",
                    "Restore default dock panels?",
                    [reportStatus]() { reportStatus("Confirmed"); },
                    [reportStatus]() { reportStatus("Cancelled"); }
                );
            });
            auto promptBtn = MakeUI<UIButton>("Prompt Modal", [&context, reportStatus]()
            {
                UIModal::ShowPrompt(
                    context,
                    "Rename Item",
                    "Enter a new name:",
                    "Untitled",
                    "Name...",
                    [reportStatus](const std::string& val) { reportStatus("Renamed to: " + val); }
                );
            });
            auto paletteBtn = MakeUI<UIButton>("Command Palette", [&context, reportStatus]()
            {
                UICommandPalette::Open(context, {
                    { "File", "New Scene", "Ctrl+N", [reportStatus]() { reportStatus("New Scene"); } },
                    { "View", "Reset Layout", "", [reportStatus]() { reportStatus("Reset Layout"); } },
                    { "Theme", "Reload DefaultDark", "", [reportStatus]() { reportStatus("Reloaded Theme"); } }
                });
            });
            auto toastBtn = MakeUI<UIButton>("Show Toast", [&context, success]()
            {
                context.PushToast("Saved changes successfully", 2.5f, success);
            });

            overlayWrap->AddChild(confirmBtn);
            overlayWrap->AddChild(promptBtn);
            overlayWrap->AddChild(paletteBtn);
            overlayWrap->AddChild(toastBtn);
            overlayCard->AddContent(overlayWrap);

            overlayCard->AddContent(MakeUI<UIAlertBanner>(UIAlertSeverity::Info, "Info: Auto-scaling layout active.", "Dismiss", nullptr, true));
            overlayCard->AddContent(MakeUI<UIAlertBanner>(UIAlertSeverity::Success, "Success: Theme tokens synchronized.", "", nullptr, false));
            overlayCard->AddContent(MakeUI<UIAlertBanner>(UIAlertSeverity::Warning, "Warning: Unsaved changes in draft.", "", nullptr, false));
            grid->AddChild(overlayCard);

            auto themeCard = MakeUI<UIGroupBox>("Theme Presets");
            auto themeDesc = MakeUI<UILabel>("Switch accent & surface contrast cleanly without restarting:");
            themeDesc->UseColorOverride = true;
            themeDesc->ColorOverride = muted;
            themeCard->AddContent(themeDesc);

            auto themeRadio = MakeUI<UIRadioGroup>(
                std::vector<std::string>{ "Slate Blue (Default)", "Muted Teal", "Warm Graphite" },
                0,
                [&context, reportStatus](int idx)
                {
                    auto t = UITheme::CreateDefault();
                    if (idx == 1)
                    {
                        t->SetToken("Accent", { 0.212f, 0.659f, 0.635f, 1.0f });
                        t->SetToken("AccentHover", { 0.282f, 0.729f, 0.706f, 1.0f });
                    }
                    else if (idx == 2)
                    {
                        t->SetToken("Background", { 0.094f, 0.094f, 0.098f, 1.0f });
                        t->SetToken("Surface0", { 0.125f, 0.125f, 0.133f, 0.98f });
                        t->SetToken("Surface1", { 0.165f, 0.165f, 0.173f, 0.98f });
                        t->SetToken("Accent", { 0.800f, 0.565f, 0.267f, 1.0f });
                        t->SetToken("AccentHover", { 0.867f, 0.635f, 0.341f, 1.0f });
                    }
                    auto btnPrimary = t->Resolve("Button", "primary", UIState::Normal);
                    btnPrimary.FillColor = t->GetToken("Accent");
                    btnPrimary.FillColorBottom = btnPrimary.FillColor;
                    btnPrimary.BorderColor = t->GetToken("AccentHover");
                    t->SetStyle("Button.primary", btnPrimary);
                    context.SetTheme(t);
                    reportStatus("Applied theme preset #" + std::to_string(idx + 1));
                },
                UIOrientation::Vertical
            );
            themeCard->AddContent(themeRadio);
            grid->AddChild(themeCard);

            page->AddChild(grid);
            panels.push_back({ "Overlays & Theme", page });
        }

        return panels;
    }

    Ref<UIElement> CreateUIDemoGallery(UIContext& context, const std::function<void(const std::string&)>& onStatus)
    {
        auto root = MakeUI<UIStackBox>(UIOrientation::Vertical, 10.0f);
        root->WidthMode = UISizeMode::Fill;
        root->HeightMode = UISizeMode::Fill;

        auto statusText = MakeUI<UILabel>("Ready");
        statusText->VAlign = UIAlignment::Center;
        auto reportStatus = [onStatus, statusText](const std::string& msg)
        {
            statusText->Text = msg;
            if (onStatus)
                onStatus(msg);
        };

        const UIColor accent = GetToken(context, "Accent", { 0.259f, 0.529f, 0.961f, 1.0f });
        const UIColor muted = GetToken(context, "TextMuted", { 0.580f, 0.616f, 0.686f, 1.0f });

        auto header = MakeUI<UIStackBox>(UIOrientation::Horizontal, 10.0f);
        header->StyleClass = "TopBar";
        header->DrawBackground = true;
        header->WidthMode = UISizeMode::Fill;
        header->Padding = UIThickness::Symmetric(14.0f, 10.0f);

        auto titleCol = MakeUI<UIStackBox>(UIOrientation::Vertical, 2.0f);
        titleCol->WidthMode = UISizeMode::Fill;
        titleCol->AddChild(MakeUI<UILabel>("Nox::UI Component Gallery"));
        auto sub = MakeUI<UILabel>("Auto-scaling retained UI toolkit");
        sub->UseColorOverride = true;
        sub->ColorOverride = muted;
        titleCol->AddChild(sub);
        header->AddChild(titleCol);

        auto statusBadge = MakeUI<UIBadge>("Live", accent);
        statusBadge->VAlign = UIAlignment::Center;
        header->AddChild(statusBadge);
        header->AddChild(statusText);
        root->AddChild(header);

        auto tabs = MakeUI<UITabPanel>();
        tabs->WidthMode = UISizeMode::Fill;
        tabs->HeightMode = UISizeMode::Fill;
        for (const auto& panel : CreateUIDemoPanels(context, reportStatus))
            tabs->AddTab(panel.Title, panel.Content);

        root->AddChild(tabs);
        return root;
    }
}
