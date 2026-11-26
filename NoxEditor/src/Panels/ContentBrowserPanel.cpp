#include "ContentBrowserPanel.h"
#include <NoxUI/Rendering/UIFont.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_scancode.h>

#include "AssetPreviewRenderer.h"
#include "NoxCore/Animation/AnimationGraphNodes.h"
#include "NoxCore/Animation/Timeline.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Asset/MaterialSerializer.h"
#include "NoxCore/Asset/NodeGraphSerializer.h"
#include "NoxCore/Asset/TimelineImporter.h"
#include "NoxCore/Asset/MeshImporter.h"
#include "NoxCore/Asset/VoxImporter.h"
#include "NoxCore/Asset/VfxAsset.h"
#include "NoxCore/Audio/AudioGraph.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/UI/Core/UILayer.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/CustomLitShaders.h"
#include "NoxCore/Renderer/MaterialGraph.h"
#include <NoxUI/Core/UIContext.h>
#include <NoxUI/Layout/UILayout.h>
#include <NoxUI/Rendering/UIDrawList.h>
#include <NoxUI/Rendering/UIIcons.h>
#include <NoxUI/Widgets/UIBreadcrumb.h>
#include <NoxUI/Widgets/UIButton.h>
#include <NoxUI/Widgets/UICheckBox.h>
#include <NoxUI/Widgets/UIDragScalar.h>
#include <NoxUI/Widgets/UIDropdown.h>
#include <NoxUI/Widgets/UILabel.h>
#include <NoxUI/Widgets/UIMenu.h>
#include <NoxUI/Widgets/UIModal.h>
#include <NoxUI/Widgets/UIScrollBar.h>
#include <NoxUI/Widgets/UIScrollView.h>
#include <NoxUI/Widgets/UISearchBar.h>
#include <NoxUI/Widgets/UITextInput.h>
#include <NoxUI/Widgets/UITreeView.h>
#include "NoxCore/Utils/Utils.h"

namespace Nox
{
    // ------------------------------------------------------------------------------------------------------------------------------
    // Asset type visuals
    // ------------------------------------------------------------------------------------------------------------------------------

    AssetTypeVisual GetAssetTypeVisual(AssetType type)
    {
        // The colours follow Unreal's where an asset kind exists there (static mesh cyan, skeletal mesh pink, material green,
        // texture red, level orange, animation turquoise, sound blue), so the line under a tile tells the kind at a glance.
        switch (type)
        {
        case AssetType::Scene:             return { "Scene",          NoxUI::ThemeColor("AssetScene", { 0.96f, 0.58f, 0.18f, 1.0f }) };
        case AssetType::Texture2D:         return { "Texture",        NoxUI::ThemeColor("AssetTexture", { 0.88f, 0.32f, 0.30f, 1.0f }) };
        case AssetType::MeshSource:        return { "Model File",     NoxUI::ThemeColor("AssetModelFile", { 0.55f, 0.66f, 0.72f, 1.0f }) };
        case AssetType::Mesh:              return { "Skeletal Mesh",  NoxUI::ThemeColor("AssetSkeletalMesh", { 0.90f, 0.40f, 0.74f, 1.0f }) };
        case AssetType::SkeletalMesh:      return { "Skeletal Mesh",  NoxUI::ThemeColor("AssetSkeletalMesh", { 0.90f, 0.40f, 0.74f, 1.0f }) };
        case AssetType::StaticMesh:        return { "Static Mesh",    NoxUI::ThemeColor("AssetStaticMesh", { 0.20f, 0.76f, 0.90f, 1.0f }) };
        case AssetType::Material:          return { "Material",       NoxUI::ThemeColor("AssetMaterial", { 0.40f, 0.80f, 0.34f, 1.0f }) };
        case AssetType::MaterialGraph:     return { "Material Graph", NoxUI::ThemeColor("AssetMaterialGraph", { 0.62f, 0.88f, 0.42f, 1.0f }) };
        case AssetType::MaterialShader:    return { "Material Shader",NoxUI::ThemeColor("AssetMaterialShader", { 0.78f, 0.82f, 0.32f, 1.0f }) };
        case AssetType::Skeleton:          return { "Skeleton",       NoxUI::ThemeColor("AssetSkeleton", { 0.88f, 0.80f, 0.58f, 1.0f }) };
        case AssetType::AnimationSequence: return { "Animation",      NoxUI::ThemeColor("AssetAnimation", { 0.22f, 0.86f, 0.72f, 1.0f }) };
        case AssetType::AnimationGraph:    return { "Anim Graph",     NoxUI::ThemeColor("AssetAnimGraph", { 0.52f, 0.58f, 0.98f, 1.0f }) };
        case AssetType::Prefab:            return { "Prefab",         NoxUI::ThemeColor("AssetPrefab", { 0.32f, 0.58f, 1.00f, 1.0f }) };
        case AssetType::Timeline:          return { "Timeline",       NoxUI::ThemeColor("AssetTimeline", { 0.98f, 0.78f, 0.28f, 1.0f }) };
        case AssetType::Font:              return { "Font",           NoxUI::ThemeColor("AssetFont", { 0.74f, 0.76f, 0.82f, 1.0f }) };
        case AssetType::AudioClip:         return { "Sound",          NoxUI::ThemeColor("AssetSound", { 0.38f, 0.72f, 1.00f, 1.0f }) };
        case AssetType::AudioGraph:        return { "Audio Graph",    NoxUI::ThemeColor("AssetAudioGraph", { 0.54f, 0.68f, 1.00f, 1.0f }) };
        case AssetType::AudioMixer:        return { "Audio Mixer",    NoxUI::ThemeColor("AssetAudioMixer", { 0.60f, 0.64f, 0.84f, 1.0f }) };
        case AssetType::AcousticData:      return { "Acoustic Data",  NoxUI::ThemeColor("AssetAcousticData", { 0.44f, 0.78f, 0.82f, 1.0f }) };
        case AssetType::Volume:            return { "Volume",         NoxUI::ThemeColor("AssetVolume", { 0.72f, 0.44f, 0.96f, 1.0f }) };
        case AssetType::Vfx:               return { "VFX",            NoxUI::ThemeColor("AssetVfx", { 1.00f, 0.45f, 0.28f, 1.0f }) };
        default:                           return { "File",           NoxUI::ThemeColor("AssetFile", { 0.62f, 0.66f, 0.74f, 1.0f }) };
        }
    }

    namespace
    {
        using NoxUI::UIColor;
        using NoxUI::UIVec2;

        void Ring(NoxUI::UIDrawList& d, const UIVec2& c, float r, float thickness, const UIColor& color)
        {
            d.AddRoundedRect(c - UIVec2{ r }, c + UIVec2{ r }, { 0.0f, 0.0f, 0.0f, 0.0f }, glm::vec4(r), thickness, color);
        }

        void Disc(NoxUI::UIDrawList& d, const UIVec2& c, float r, const UIColor& color)
        {
            d.AddRoundedRect(c - UIVec2{ r }, c + UIVec2{ r }, color, glm::vec4(r));
        }

        void Line(NoxUI::UIDrawList& d, const UIVec2& a, const UIVec2& b, float thickness, const UIColor& color)
        {
            NoxUI::Icons::DrawSegment(d, a, b, thickness, color);
        }

        UIColor Darker(const UIColor& c, float f) { return { c.r * f, c.g * f, c.b * f, c.a }; }

        void IsoCube(NoxUI::UIDrawList& d, const UIVec2& c, float s, float t, const UIColor& color)
        {
            // An outlined cube seen from above at an angle.
            const UIVec2 top{ c.x, c.y - s * 0.50f }, left{ c.x - s * 0.44f, c.y - s * 0.25f }, right{ c.x + s * 0.44f, c.y - s * 0.25f };
            const UIVec2 mid{ c.x, c.y }, botLeft{ c.x - s * 0.44f, c.y + s * 0.25f }, botRight{ c.x + s * 0.44f, c.y + s * 0.25f };
            const UIVec2 bottom{ c.x, c.y + s * 0.50f };
            Line(d, top, left, t, color); Line(d, top, right, t, color);
            Line(d, left, mid, t, color); Line(d, right, mid, t, color);
            Line(d, left, botLeft, t, color); Line(d, right, botRight, t, color);
            Line(d, mid, bottom, t, color); Line(d, botLeft, bottom, t, color); Line(d, botRight, bottom, t, color);
        }

        void NodeGraphGlyph(NoxUI::UIDrawList& d, const UIVec2& c, float s, float t, const UIColor& color)
        {
            const float w = s * 0.30f, h = s * 0.20f;
            const UIVec2 a{ c.x - s * 0.34f, c.y - s * 0.22f }, b{ c.x - s * 0.34f, c.y + s * 0.24f }, out{ c.x + s * 0.32f, c.y };
            auto node = [&](const UIVec2& p) { d.AddRoundedRect(p - UIVec2{ w * 0.5f, h * 0.5f }, p + UIVec2{ w * 0.5f, h * 0.5f }, Darker(color, 0.35f), glm::vec4(3.0f), t, color); };
            d.AddBezier({ a.x + w * 0.5f, a.y }, { a.x + s * 0.30f, a.y }, { out.x - s * 0.30f, out.y }, { out.x - w * 0.5f, out.y }, t, color);
            d.AddBezier({ b.x + w * 0.5f, b.y }, { b.x + s * 0.30f, b.y }, { out.x - s * 0.30f, out.y }, { out.x - w * 0.5f, out.y }, t, color);
            node(a); node(b); node(out);
        }
    }

    void DrawFolderIcon(NoxUI::UIDrawList& d, const UIVec2& c, float s, const UIColor& color)
    {
        // A folder: its tab, then the front a little lighter, like a manila folder.
        const float w = s, h = s * 0.74f;
        const UIVec2 min{ c.x - w * 0.5f, c.y - h * 0.5f };
        d.AddRoundedRect({ min.x, min.y }, { min.x + w * 0.42f, min.y + h * 0.22f }, Darker(color, 0.78f), glm::vec4(s * 0.05f));
        d.AddRoundedRect({ min.x, min.y + h * 0.12f }, { min.x + w, min.y + h }, Darker(color, 0.78f), glm::vec4(s * 0.06f));
        d.AddRoundedRectGradient({ min.x, min.y + h * 0.26f }, { min.x + w, min.y + h }, color, Darker(color, 0.88f), glm::vec4(s * 0.06f));
    }

    void DrawAssetTypeIcon(NoxUI::UIDrawList& d, AssetType type, const UIVec2& c, float s, const UIColor& color)
    {
        const float t = std::max(1.5f, s * 0.055f);
        switch (type)
        {
        case AssetType::StaticMesh:
        case AssetType::MeshSource:
            IsoCube(d, c, s * 0.86f, t, color);
            break;
        case AssetType::Mesh:
        case AssetType::SkeletalMesh:
        {
            // A figure: head, body, arms, legs.
            Disc(d, { c.x, c.y - s * 0.30f }, s * 0.11f, color);
            Line(d, { c.x, c.y - s * 0.17f }, { c.x, c.y + s * 0.12f }, t * 1.3f, color);
            Line(d, { c.x - s * 0.22f, c.y - s * 0.06f }, { c.x + s * 0.22f, c.y - s * 0.06f }, t * 1.2f, color);
            Line(d, { c.x, c.y + s * 0.12f }, { c.x - s * 0.15f, c.y + s * 0.40f }, t * 1.2f, color);
            Line(d, { c.x, c.y + s * 0.12f }, { c.x + s * 0.15f, c.y + s * 0.40f }, t * 1.2f, color);
            break;
        }
        case AssetType::Material:
        {
            // A lit sphere.
            const float r = s * 0.38f;
            d.AddRoundedRectGradient(c - UIVec2{ r }, c + UIVec2{ r }, color, Darker(color, 0.30f), glm::vec4(r));
            Disc(d, { c.x - r * 0.35f, c.y - r * 0.38f }, r * 0.22f, NoxUI::ThemeColor("AssetIconHighlight", { 1.0f, 1.0f, 1.0f, 0.55f }));
            break;
        }
        case AssetType::Texture2D:
        {
            // A picture: frame, a mountain, the sun.
            const UIVec2 min{ c.x - s * 0.40f, c.y - s * 0.32f }, max{ c.x + s * 0.40f, c.y + s * 0.32f };
            d.AddRoundedRect(min, max, Darker(color, 0.25f), glm::vec4(s * 0.06f), t, color);
            Line(d, { min.x + s * 0.06f, max.y - s * 0.08f }, { c.x - s * 0.08f, c.y - s * 0.04f }, t, color);
            Line(d, { c.x - s * 0.08f, c.y - s * 0.04f }, { c.x + s * 0.10f, max.y - s * 0.12f }, t, color);
            Line(d, { c.x + s * 0.04f, c.y + s * 0.06f }, { c.x + s * 0.16f, c.y - s * 0.02f }, t, color);
            Line(d, { c.x + s * 0.16f, c.y - s * 0.02f }, { max.x - s * 0.06f, max.y - s * 0.08f }, t, color);
            Disc(d, { c.x + s * 0.20f, c.y - s * 0.16f }, s * 0.06f, color);
            break;
        }
        case AssetType::Scene:
        {
            // A globe.
            const float r = s * 0.38f;
            Ring(d, c, r, t, color);
            d.AddRoundedRect({ c.x - r * 0.45f, c.y - r }, { c.x + r * 0.45f, c.y + r }, { 0, 0, 0, 0 }, glm::vec4(r * 0.45f, r * 0.45f, r * 0.45f, r * 0.45f), t, color);
            Line(d, { c.x - r, c.y }, { c.x + r, c.y }, t, color);
            Line(d, { c.x - r * 0.86f, c.y - r * 0.48f }, { c.x + r * 0.86f, c.y - r * 0.48f }, t * 0.8f, color);
            Line(d, { c.x - r * 0.86f, c.y + r * 0.48f }, { c.x + r * 0.86f, c.y + r * 0.48f }, t * 0.8f, color);
            break;
        }
        case AssetType::Prefab:
        {
            // A box with its faces shaded.
            const float h = s * 0.42f;
            d.AddRoundedRect({ c.x - h, c.y - h * 0.55f }, { c.x + h, c.y + h }, Darker(color, 0.35f), glm::vec4(s * 0.04f), t, color);
            d.AddRoundedRect({ c.x - h * 1.08f, c.y - h }, { c.x + h * 1.08f, c.y - h * 0.45f }, color, glm::vec4(s * 0.04f));
            Line(d, { c.x, c.y - h * 0.45f }, { c.x, c.y + h }, t, color);
            break;
        }
        case AssetType::Skeleton:
        {
            // A bone.
            const UIVec2 a{ c.x - s * 0.30f, c.y + s * 0.26f }, b{ c.x + s * 0.30f, c.y - s * 0.26f };
            Line(d, a, b, t * 2.4f, color);
            Disc(d, a + UIVec2{ -s * 0.04f, -s * 0.07f }, s * 0.08f, color); Disc(d, a + UIVec2{ s * 0.06f, s * 0.05f }, s * 0.08f, color);
            Disc(d, b + UIVec2{ -s * 0.06f, -s * 0.05f }, s * 0.08f, color); Disc(d, b + UIVec2{ s * 0.04f, s * 0.07f }, s * 0.08f, color);
            break;
        }
        case AssetType::AnimationSequence:
        {
            // Play, in a ring.
            const float r = s * 0.38f;
            Ring(d, c, r, t, color);
            Line(d, { c.x - r * 0.30f, c.y - r * 0.45f }, { c.x - r * 0.30f, c.y + r * 0.45f }, t * 1.3f, color);
            Line(d, { c.x - r * 0.30f, c.y - r * 0.45f }, { c.x + r * 0.50f, c.y }, t * 1.3f, color);
            Line(d, { c.x - r * 0.30f, c.y + r * 0.45f }, { c.x + r * 0.50f, c.y }, t * 1.3f, color);
            break;
        }
        case AssetType::AnimationGraph:
        case AssetType::MaterialGraph:
        case AssetType::AudioGraph:
            NodeGraphGlyph(d, c, s, t, color);
            break;
        case AssetType::Timeline:
        {
            // A film strip with key frames.
            const UIVec2 min{ c.x - s * 0.42f, c.y - s * 0.26f }, max{ c.x + s * 0.42f, c.y + s * 0.26f };
            d.AddRoundedRect(min, max, Darker(color, 0.30f), glm::vec4(s * 0.04f), t, color);
            for (int i = 0; i < 5; ++i)
            {
                const float x = min.x + s * 0.10f + static_cast<float>(i) * s * 0.16f;
                d.AddRoundedRect({ x, min.y + s * 0.04f }, { x + s * 0.07f, min.y + s * 0.10f }, color, glm::vec4(1.0f));
                d.AddRoundedRect({ x, max.y - s * 0.10f }, { x + s * 0.07f, max.y - s * 0.04f }, color, glm::vec4(1.0f));
            }
            Disc(d, { c.x - s * 0.14f, c.y }, s * 0.05f, color);
            Disc(d, { c.x + s * 0.14f, c.y }, s * 0.05f, color);
            break;
        }
        case AssetType::Font:
        {
            const float size = s * 0.62f;
            const UIVec2 extent = NoxUI::UILabel::MeasureText("Aa", nullptr, size);
            d.AddText("Aa", nullptr, { c.x - extent.x * 0.5f, c.y - extent.y * 0.5f }, size, color);
            break;
        }
        case AssetType::AudioClip:
        case AssetType::AcousticData:
        {
            // A speaker and its waves.
            d.AddRoundedRect({ c.x - s * 0.36f, c.y - s * 0.10f }, { c.x - s * 0.20f, c.y + s * 0.10f }, color, glm::vec4(2.0f));
            Line(d, { c.x - s * 0.20f, c.y - s * 0.10f }, { c.x - s * 0.02f, c.y - s * 0.26f }, t, color);
            Line(d, { c.x - s * 0.20f, c.y + s * 0.10f }, { c.x - s * 0.02f, c.y + s * 0.26f }, t, color);
            Line(d, { c.x - s * 0.02f, c.y - s * 0.26f }, { c.x - s * 0.02f, c.y + s * 0.26f }, t, color);
            d.AddBezier({ c.x + s * 0.10f, c.y - s * 0.14f }, { c.x + s * 0.18f, c.y - s * 0.06f }, { c.x + s * 0.18f, c.y + s * 0.06f }, { c.x + s * 0.10f, c.y + s * 0.14f }, t, color);
            d.AddBezier({ c.x + s * 0.20f, c.y - s * 0.26f }, { c.x + s * 0.34f, c.y - s * 0.10f }, { c.x + s * 0.34f, c.y + s * 0.10f }, { c.x + s * 0.20f, c.y + s * 0.26f }, t, color);
            break;
        }
        case AssetType::AudioMixer:
        {
            for (int i = 0; i < 3; ++i)
            {
                const float x = c.x + (static_cast<float>(i) - 1.0f) * s * 0.24f;
                const float knob = c.y + (i == 1 ? -0.14f : (i == 0 ? 0.10f : 0.02f)) * s;
                Line(d, { x, c.y - s * 0.36f }, { x, c.y + s * 0.36f }, t, color);
                d.AddRoundedRect({ x - s * 0.08f, knob - s * 0.05f }, { x + s * 0.08f, knob + s * 0.05f }, color, glm::vec4(2.0f));
            }
            break;
        }
        case AssetType::MaterialShader:
        default:
        {
            // A page with "</>" (shader code, a script) or folded corner lines (any other file).
            const UIVec2 min{ c.x - s * 0.30f, c.y - s * 0.40f }, max{ c.x + s * 0.30f, c.y + s * 0.40f };
            d.AddRoundedRect(min, max, Darker(color, 0.25f), glm::vec4(s * 0.05f), t, color);
            if (type == AssetType::MaterialShader)
            {
                Line(d, { c.x - s * 0.06f, c.y - s * 0.10f }, { c.x - s * 0.16f, c.y }, t, color);
                Line(d, { c.x - s * 0.16f, c.y }, { c.x - s * 0.06f, c.y + s * 0.10f }, t, color);
                Line(d, { c.x + s * 0.06f, c.y - s * 0.10f }, { c.x + s * 0.16f, c.y }, t, color);
                Line(d, { c.x + s * 0.16f, c.y }, { c.x + s * 0.06f, c.y + s * 0.10f }, t, color);
            }
            else
            {
                for (int i = 0; i < 3; ++i)
                    Line(d, { min.x + s * 0.12f, c.y - s * 0.12f + static_cast<float>(i) * s * 0.12f }, { max.x - s * 0.12f, c.y - s * 0.12f + static_cast<float>(i) * s * 0.12f }, t * 0.8f, color);
            }
            break;
        }
        }
    }

    namespace
    {
        constexpr NoxUI::UIThemeToken kFolderColor{ "ContentFolder", { 0.86f, 0.70f, 0.38f, 1.0f } };
        constexpr NoxUI::UIThemeToken kScriptColor{ "ContentScript", { 0.44f, 0.62f, 0.95f, 1.0f } };

        constexpr const char* kFilterNames[] = { "All Types", "Meshes", "Materials", "Textures", "Prefabs", "Scenes", "Animation", "Audio", "Scripts", "Volumes", "VFX" };

        std::string ToLower(std::string_view text)
        {
            std::string lower(text);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return lower;
        }

        bool IsInside(const std::filesystem::path& path, const std::filesystem::path& folder)
        {
            const std::filesystem::path relative = path.lexically_relative(folder);
            return !relative.empty() && *relative.begin() != "..";
        }

        EditorAssetManager* Assets()
        {
            return Project::GetActive() ? Project::GetActive()->GetEditorAssetManager().get() : nullptr;
        }

        uint64_t FolderId(const std::filesystem::path& absolute)
        {
            const uint64_t id = std::hash<std::string>{}(absolute.lexically_normal().generic_string());
            return id == 0 ? 1 : id;
        }

        std::filesystem::path UniquePath(const std::filesystem::path& folder, const std::string& stem, const std::string& extension)
        {
            std::filesystem::path path = folder / (stem + extension);
            for (int n = 2; std::filesystem::exists(path); ++n)
                path = folder / (stem + " " + std::to_string(n) + extension);
            return path;
        }
    }

    // ------------------------------------------------------------------------------------------------------------------------------
    // The path bar
    // ------------------------------------------------------------------------------------------------------------------------------

    // Where you are, as Unreal shows it: "Assets > Models > bistro" in plain text with chevrons, one click on a folder goes there.
    // As high as the toolbar's buttons; when the path is longer than the bar, its first folders give way to "...".
    class ContentPathBar final : public NoxUI::UIElement
    {
    public:
        ContentPathBar()
        {
            HitTestMode = NoxUI::UIHitTestMode::Self;
            WidthMode = NoxUI::UISizeMode::Fill;
            HeightMode = NoxUI::UISizeMode::Fixed;
            FixedSize = { 0.0f, 26.0f };
            VAlign = NoxUI::UIAlignment::Center;
        }

        std::function<void(int segment)> OnNavigate;

        void SetSegments(std::vector<std::string> segments)
        {
            m_Segments = std::move(segments);
            m_Hovered = -1;
        }
        const std::vector<std::string>& GetSegments() const { return m_Segments; }

        bool OnPointerMove(const NoxUI::UIPointerEvent& e) override
        {
            m_Hovered = SegmentAt(e.Position);
            return true;
        }
        bool OnPointerLeave(const NoxUI::UIPointerEvent&) override
        {
            m_Hovered = -1;
            return false;
        }
        bool OnPointerDown(const NoxUI::UIPointerEvent& e) override
        {
            if (e.Button != 1)
                return false;
            const int segment = SegmentAt(e.Position);
            if (segment >= 0 && segment + 1 < static_cast<int>(m_Segments.size()) && OnNavigate)
                OnNavigate(segment);
            return true;
        }

    protected:
        UIVec2 ComputeDesiredSize() override { return { 80.0f, 26.0f }; }

        void OnPaint(NoxUI::UIDrawList& d, const NoxUI::UITheme& theme) override
        {
            Place();
            const NoxUI::UIRect& b = GetBounds();
            const UIColor text = theme.GetToken("Text", { 0.92f, 0.93f, 0.95f, 1.0f });
            const UIColor muted = theme.GetToken("TextMuted", { 0.58f, 0.62f, 0.69f, 1.0f });
            d.AddRoundedRect(b.Min, b.Max, theme.GetToken("Background", { 0.063f, 0.075f, 0.106f, 1.0f }), glm::vec4(4.0f), 1.0f,
                             theme.GetToken("Border", { 0.20f, 0.235f, 0.32f, 1.0f }));
            d.PushClipRect(b.Min, b.Max);
            const float cy = b.Center().y;
            if (m_Elided)
            {
                const UIVec2 size = NoxUI::UILabel::MeasureText("...", nullptr, kFont);
                d.AddText("...", nullptr, { b.Min.x + kInset, cy - size.y * 0.5f }, kFont, muted);
                const float x = b.Min.x + kInset + size.x + kChevron * 0.5f;
                NoxUI::Icons::DrawSegment(d, { x - 2.0f, cy - 4.0f }, { x + 2.0f, cy }, 1.5f, muted);
                NoxUI::Icons::DrawSegment(d, { x + 2.0f, cy }, { x - 2.0f, cy + 4.0f }, 1.5f, muted);
            }
            for (size_t i = 0; i < m_Placed.size(); ++i)
            {
                const Placed& p = m_Placed[i];
                const bool last = p.Index + 1 == static_cast<int>(m_Segments.size());
                if (p.Index == m_Hovered && !last)
                    d.AddRoundedRect({ p.X0, b.Min.y + 3.0f }, { p.X1, b.Max.y - 3.0f }, NoxUI::ThemeColor("BreadcrumbHover", { 1.0f, 1.0f, 1.0f, 0.08f }), glm::vec4(3.0f));
                const std::string& label = m_Segments[static_cast<size_t>(p.Index)];
                const UIVec2 size = NoxUI::UILabel::MeasureText(label, nullptr, kFont);
                d.AddText(label, nullptr, { p.X0 + kPad, cy - size.y * 0.5f }, kFont, last || p.Index == m_Hovered ? text : muted);
                if (!last)
                {
                    // The chevron after it.
                    const float x = p.X1 + kChevron * 0.5f;
                    NoxUI::Icons::DrawSegment(d, { x - 2.0f, cy - 4.0f }, { x + 2.0f, cy }, 1.5f, muted);
                    NoxUI::Icons::DrawSegment(d, { x + 2.0f, cy }, { x - 2.0f, cy + 4.0f }, 1.5f, muted);
                }
            }
            d.PopClipRect();
        }

    private:
        static constexpr float kFont = 14.0f;
        static constexpr float kPad = 6.0f;
        static constexpr float kChevron = 14.0f;
        static constexpr float kInset = 6.0f;

        struct Placed
        {
            int Index = 0;
            float X0 = 0.0f, X1 = 0.0f;
        };

        // The segments that fit, the last one always; the ones that do not are left out from the front.
        void Place()
        {
            m_Placed.clear();
            m_Elided = false;
            const NoxUI::UIRect& b = GetBounds();
            const float available = b.Width() - kInset * 2.0f;
            std::vector<float> widths;
            for (const std::string& segment : m_Segments)
                widths.push_back(NoxUI::UILabel::MeasureText(segment, nullptr, kFont).x + kPad * 2.0f);
            const float ellipsis = NoxUI::UILabel::MeasureText("...", nullptr, kFont).x + kChevron;
            int first = static_cast<int>(m_Segments.size());
            float used = 0.0f;
            while (first > 0)
            {
                const float add = widths[static_cast<size_t>(first - 1)] + (first < static_cast<int>(m_Segments.size()) ? kChevron : 0.0f);
                const float reserve = first - 1 > 0 ? ellipsis : 0.0f;
                if (used + add + reserve > available && first < static_cast<int>(m_Segments.size()))
                    break;
                used += add;
                --first;
            }
            m_Elided = first > 0;
            float x = b.Min.x + kInset + (m_Elided ? ellipsis : 0.0f);
            for (int i = first; i < static_cast<int>(m_Segments.size()); ++i)
            {
                m_Placed.push_back({ i, x, x + widths[static_cast<size_t>(i)] });
                x += widths[static_cast<size_t>(i)] + kChevron;
            }
        }

        int SegmentAt(const UIVec2& p) const
        {
            const NoxUI::UIRect& b = GetBounds();
            if (!b.Contains(p))
                return -1;
            for (const Placed& placed : m_Placed)
            {
                if (p.x >= placed.X0 && p.x <= placed.X1)
                    return placed.Index;
            }
            return -1;
        }

        std::vector<std::string> m_Segments;
        std::vector<Placed> m_Placed;
        bool m_Elided = false;
        int m_Hovered = -1;
    };

    // ------------------------------------------------------------------------------------------------------------------------------
    // The tiles
    // ------------------------------------------------------------------------------------------------------------------------------

    // The open folder's files as tiles: virtualised (only the rows in sight are drawn), its own scroll bar, selection, drag and
    // drop, the inline rename field.
    class ContentTileView final : public NoxUI::UIElement
    {
    public:
        explicit ContentTileView(ContentBrowserPanel& panel)
            : m_Panel(panel)
        {
            StyleClass = "TextInput";
            DrawBackground = false;
            ClipChildren = true;
            HitTestMode = NoxUI::UIHitTestMode::Self;
            Focusable = true;
            WidthMode = NoxUI::UISizeMode::Fill;
            HeightMode = NoxUI::UISizeMode::Fill;

            m_RenameInput = Ref<NoxUI::UITextInput>(new NoxUI::UITextInput());
            m_RenameInput->Visible = false;
            m_RenameInput->OnSubmit = [this](const std::string& text) { FinishRename(true, text); };
            AddChild(m_RenameInput);

            OnDragStart = [this]() -> std::optional<NoxUI::UIDragPayload>
            {
                if (m_PressedIndex < 0 || m_PressedIndex >= static_cast<int>(m_Panel.m_Entries.size()))
                    return std::nullopt;
                const ContentEntry& entry = m_Panel.m_Entries[static_cast<size_t>(m_PressedIndex)];
                if (entry.Handle == 0)
                    return std::nullopt;
                if (!m_Selected.contains(entry.Handle))
                {
                    m_Selected.clear();
                    m_Selected.insert(entry.Handle);
                }
                NoxUI::UIDragPayload payload;
                payload.UserId = entry.Handle;
                if (m_Selected.size() > 1)
                {
                    payload.Type = "CONTENT_BROWSER_ITEMS";
                    payload.Text = std::to_string(m_Selected.size()) + " assets";
                    payload.Ids.assign(m_Selected.begin(), m_Selected.end());
                }
                else
                {
                    payload.Type = "CONTENT_BROWSER_ITEM";
                    payload.Text = entry.Name;
                    payload.Ids = { static_cast<uint64_t>(entry.Handle) };
                }
                return payload;
            };

            OnDrop = [this](const NoxUI::UIDragPayload& payload) -> bool
            {
                const NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent();
                const int index = ctx ? TileAt(ctx->GetLastMousePos()) : -1;
                const ContentEntry* target = (index >= 0) ? &m_Panel.m_Entries[static_cast<size_t>(index)] : nullptr;
                const std::filesystem::path folder = (target && target->IsDirectory) ? target->Path : m_Panel.m_CurrentDirectory;
                if (payload.Type == "CONTENT_BROWSER_ITEM" || payload.Type == "CONTENT_BROWSER_ITEMS")
                {
                    if (!target || !target->IsDirectory)
                        return false;
                    std::vector<AssetHandle> handles;
                    for (uint64_t id : payload.Ids.empty() ? std::vector<uint64_t>{ payload.UserId } : payload.Ids)
                        handles.push_back(AssetHandle(id));
                    m_Panel.MoveAssets(handles, folder);
                    return true;
                }
                if (payload.Type == "SCENE_HIERARCHY_ENTITY")
                {
                    m_Panel.CreatePrefabFromDrop(payload.UserId, folder);
                    return true;
                }
                return false;
            };
        }

        float ThumbSize = 104.0f;
        std::unordered_set<AssetHandle> m_Selected;
        // The textures shown as tile pictures in the last frame: kept loaded by the editor's unused-asset sweep.
        mutable std::unordered_set<AssetHandle> PaintedTextures;

        // `newListing`: another folder, search or filter (back to the top). A refresh of the same listing keeps the scroll position.
        void ResetForNewEntries(bool newListing)
        {
            if (newListing)
                m_ScrollY = 0.0f;
            m_HoveredIndex = -1;
            m_PressedIndex = -1;
            m_Anchor = -1;
            CancelRename();
            std::erase_if(m_Selected, [this](AssetHandle handle)
            {
                return std::none_of(m_Panel.m_Entries.begin(), m_Panel.m_Entries.end(), [handle](const ContentEntry& e) { return e.Handle == handle; });
            });
        }

        void Select(AssetHandle handle, bool scrollTo)
        {
            m_Selected.clear();
            m_Selected.insert(handle);
            const auto it = std::find_if(m_Panel.m_Entries.begin(), m_Panel.m_Entries.end(), [handle](const ContentEntry& e) { return e.Handle == handle; });
            if (it != m_Panel.m_Entries.end())
            {
                m_Anchor = static_cast<int>(it - m_Panel.m_Entries.begin());
                if (scrollTo)
                    m_ScrollToIndex = m_Anchor;
            }
        }

        void BeginRename(AssetHandle handle)
        {
            const auto it = std::find_if(m_Panel.m_Entries.begin(), m_Panel.m_Entries.end(), [handle](const ContentEntry& e) { return e.Handle == handle; });
            if (it == m_Panel.m_Entries.end())
                return;
            m_RenameIndex = static_cast<int>(it - m_Panel.m_Entries.begin());
            m_RenameHandle = handle;
            m_RenameInput->Text = it->Name;
            m_RenameInput->Visible = true;
            m_RenameFrames = 0;
            m_ScrollToIndex = m_RenameIndex;
            if (NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent())
            {
                ctx->SetFocus(m_RenameInput.get());
                m_RenameInput->SelectAll();
            }
            else
            {
                m_FocusRenameNextFrame = true;
            }
        }

        bool IsRenaming() const { return m_RenameHandle != 0; }

        NoxUI::UICursorShape GetCursorShape(const UIVec2&) const override { return NoxUI::UICursorShape::Default; }

        bool OnPointerDown(const NoxUI::UIPointerEvent& e) override
        {
            m_ScrollBar.SetGeometry(GetBounds(), ContentHeight(), GetBounds().Height());
            if (e.Button == 1 && m_ScrollBar.OnPointerDown(e.Position, m_ScrollY))
            {
                if (NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent())
                    ctx->CancelPendingDrag();
                return true;
            }
            if (IsRenaming() && !m_RenameInput->GetBounds().Contains(e.Position))
                FinishRename(true, m_RenameInput->Text);
            if (NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent(); ctx && !IsRenaming())
                ctx->SetFocus(this); // F2, Delete, Enter, Ctrl+A, Backspace act on the tiles

            const int index = TileAt(e.Position);
            const bool ctrl = (e.Modifiers & NoxUI::UIMod::Ctrl) != 0;
            const bool shift = (e.Modifiers & NoxUI::UIMod::Shift) != 0;

            if (e.Button == 3) // right button: the menu of what is under the pointer
            {
                NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent();
                if (!ctx)
                    return true;
                std::vector<NoxUI::UIMenuItem> items;
                if (index >= 0)
                {
                    const ContentEntry entry = m_Panel.m_Entries[static_cast<size_t>(index)];
                    if (entry.Handle != 0 && !m_Selected.contains(entry.Handle))
                        Select(entry.Handle, false);
                    items = m_Panel.BuildAssetMenu(entry);
                }
                else
                {
                    m_Selected.clear();
                    items = m_Panel.BuildFolderSpaceMenu();
                }
                const UIVec2 p = e.Position;
                NoxUI::UIMenuPopup::ShowPopupMenu(*ctx, { p, p + UIVec2{ 1.0f, 1.0f } }, std::move(items));
                return true;
            }
            if (e.Button != 1)
                return UIElement::OnPointerDown(e);

            m_PressedIndex = index;
            m_DeferredSingleSelect = 0;
            if (index < 0)
            {
                if (!ctrl && !shift)
                    m_Selected.clear();
                return true;
            }

            const ContentEntry& entry = m_Panel.m_Entries[static_cast<size_t>(index)];
            const auto now = std::chrono::steady_clock::now();
            const bool doubleClick = index == m_LastClickIndex && now - m_LastClickTime < std::chrono::milliseconds(400);
            m_LastClickIndex = index;
            m_LastClickTime = now;
            if (doubleClick)
            {
                m_LastClickIndex = -1;
                const ContentEntry copy = entry;
                m_Panel.Activate(copy); // may list another folder: `entry` is gone after it
                return true;
            }

            if (entry.Handle == 0)
            {
                // A folder or a script: one at a time, no multi-selection.
                m_Selected.clear();
                m_SelectedPath = entry.Path;
                return true;
            }
            m_SelectedPath.clear();
            if (ctrl)
            {
                if (!m_Selected.erase(entry.Handle))
                    m_Selected.insert(entry.Handle);
                m_Anchor = index;
            }
            else if (shift && m_Anchor >= 0)
            {
                const int from = std::min(m_Anchor, index), to = std::max(m_Anchor, index);
                for (int i = from; i <= to && i < static_cast<int>(m_Panel.m_Entries.size()); ++i)
                {
                    if (m_Panel.m_Entries[static_cast<size_t>(i)].Handle != 0)
                        m_Selected.insert(m_Panel.m_Entries[static_cast<size_t>(i)].Handle);
                }
            }
            else if (m_Selected.contains(entry.Handle))
            {
                // Pressing a selected tile keeps the selection (it may be the start of a drag of all of them); a plain click
                // selects only it when the button comes up without a drag.
                m_DeferredSingleSelect = entry.Handle;
                m_Anchor = index;
            }
            else
            {
                Select(entry.Handle, false);
            }
            return true;
        }

        bool OnPointerMove(const NoxUI::UIPointerEvent& e) override
        {
            if (m_ScrollBar.OnPointerMove(e.Position, m_ScrollY))
                return true;
            const int index = m_ScrollBar.Contains(e.Position) ? -1 : TileAt(e.Position);
            if (index != m_HoveredIndex)
            {
                m_HoveredIndex = index;
                Tooltip.clear();
                if (index >= 0)
                {
                    const ContentEntry& entry = m_Panel.m_Entries[static_cast<size_t>(index)];
                    std::string relative = entry.Path.lexically_relative(m_Panel.m_BaseDirectory).generic_string();
                    if (entry.IsDirectory)
                        Tooltip = entry.Name + "\nFolder -- double-click to open";
                    else
                        Tooltip = entry.Name + entry.Extension + "\n" + (entry.IsScript ? std::string("C# Script") : std::string(GetAssetTypeVisual(entry.Type).Label)) + "\n" + relative;
                }
            }
            return true;
        }

        bool OnPointerUp(const NoxUI::UIPointerEvent& e) override
        {
            if (e.Button == 1 && m_ScrollBar.OnPointerUp())
                return true;
            if (e.Button == 1 && m_DeferredSingleSelect != 0)
            {
                Select(m_DeferredSingleSelect, false);
                m_DeferredSingleSelect = 0;
            }
            return UIElement::OnPointerUp(e);
        }

        bool OnPointerLeave(const NoxUI::UIPointerEvent&) override
        {
            m_HoveredIndex = -1;
            return false;
        }

        bool OnMouseWheel(const NoxUI::UIPointerEvent& e) override
        {
            if ((e.Modifiers & NoxUI::UIMod::Ctrl) != 0)
            {
                // Ctrl + wheel: tile size, as in Unreal.
                ThumbSize = std::clamp(ThumbSize + e.ScrollDelta.y * 12.0f, 56.0f, 224.0f);
                return true;
            }
            const float maxScroll = std::max(0.0f, ContentHeight() - GetBounds().Height());
            if (maxScroll <= 0.0f)
                return false;
            m_ScrollY = std::clamp(m_ScrollY - e.ScrollDelta.y * 48.0f, 0.0f, maxScroll);
            m_ScrollBar.NotifyScrolled();
            return true;
        }

        bool OnKeyDown(const NoxUI::UIKeyEvent& e) override
        {
            const auto key = static_cast<NoxUI::UIKey>(e.KeyCode);
            if (IsRenaming())
            {
                if (key == NoxUI::UIKey::Escape)
                {
                    CancelRename();
                    return true;
                }
                return false;
            }
            const bool ctrl = (e.Modifiers & NoxUI::UIMod::Ctrl) != 0;
            switch (key)
            {
            case NoxUI::UIKey::Delete:
                if (!m_Selected.empty())
                    m_Panel.RequestDelete(std::vector<AssetHandle>(m_Selected.begin(), m_Selected.end()));
                return true;
            case NoxUI::UIKey::F2:
                if (m_Selected.size() == 1)
                    m_Panel.BeginRename(*m_Selected.begin());
                return true;
            case NoxUI::UIKey::Return:
            case NoxUI::UIKey::KeypadEnter:
                if (m_Selected.size() == 1)
                {
                    const AssetHandle handle = *m_Selected.begin();
                    const auto it = std::find_if(m_Panel.m_Entries.begin(), m_Panel.m_Entries.end(), [handle](const ContentEntry& entry) { return entry.Handle == handle; });
                    if (it != m_Panel.m_Entries.end())
                    {
                        const ContentEntry copy = *it;
                        m_Panel.Activate(copy);
                    }
                }
                return true;
            case NoxUI::UIKey::Backspace:
                m_Panel.GoUp();
                return true;
            case NoxUI::UIKey::A:
                if (ctrl)
                {
                    m_Selected.clear();
                    for (const ContentEntry& entry : m_Panel.m_Entries)
                    {
                        if (entry.Handle != 0)
                            m_Selected.insert(entry.Handle);
                    }
                    return true;
                }
                return false;
            default:
                return false;
            }
        }

    protected:
        UIVec2 ComputeDesiredSize() override
        {
            m_RenameInput->Measure(); // placed over the tile's name by ArrangeChildren; measured so its text layout is current
            return { 200.0f, 160.0f };
        }

        void ArrangeChildren(const NoxUI::UIRect&) override
        {
            m_ScrollBar.SetGeometry(GetBounds(), ContentHeight(), GetBounds().Height());
            m_ScrollY = std::clamp(m_ScrollY, 0.0f, m_ScrollBar.GetMaxScroll());
            if (m_ScrollToIndex >= 0)
            {
                const NoxUI::UIRect tile = TileRect(m_ScrollToIndex);
                const NoxUI::UIRect& b = GetBounds();
                if (tile.Min.y < b.Min.y)
                    m_ScrollY -= b.Min.y - tile.Min.y + kGap;
                else if (tile.Max.y > b.Max.y)
                    m_ScrollY += tile.Max.y - b.Max.y + kGap;
                m_ScrollY = std::clamp(m_ScrollY, 0.0f, m_ScrollBar.GetMaxScroll());
                m_ScrollToIndex = -1;
            }
            if (IsRenaming() && m_RenameIndex >= 0 && m_RenameIndex < static_cast<int>(m_Panel.m_Entries.size()))
            {
                const NoxUI::UIRect tile = TileRect(m_RenameIndex);
                const float top = tile.Min.y + ThumbSize + 5.0f;
                m_RenameInput->Arrange({ { tile.Min.x + 2.0f, top }, { tile.Max.x - 2.0f, top + 26.0f } });
            }
            else
            {
                m_RenameInput->Arrange({});
            }
        }

        void OnUpdate(float dt) override
        {
            m_ScrollBar.SetGeometry(GetBounds(), ContentHeight(), GetBounds().Height());
            bool hovered = false;
            if (NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent())
            {
                hovered = ctx->GetHovered() == this && m_ScrollBar.Contains(ctx->GetLastMousePos());
                if (m_FocusRenameNextFrame && IsRenaming())
                {
                    ctx->SetFocus(m_RenameInput.get());
                    m_RenameInput->SelectAll();
                    m_FocusRenameNextFrame = false;
                }
            }
            m_ScrollBar.Update(dt, hovered);
            // The rename field lost the focus (a click elsewhere, Tab): what was typed is the new name.
            if (IsRenaming() && ++m_RenameFrames > 2 && !NoxUI::HasState(m_RenameInput->GetState(), NoxUI::UIState::Focused))
                FinishRename(true, m_RenameInput->Text);
        }

        void OnPaint(NoxUI::UIDrawList& d, const NoxUI::UITheme& theme) override
        {
            const NoxUI::UIRect& b = GetBounds();
            const UIColor surface = theme.GetToken("Surface1", { 0.125f, 0.145f, 0.20f, 1.0f });
            const UIColor well = theme.GetToken("Background", { 0.063f, 0.075f, 0.106f, 1.0f });
            const UIColor border = theme.GetToken("Border", { 0.20f, 0.235f, 0.32f, 1.0f });
            const UIColor accent = theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f });
            const UIColor text = theme.GetToken("Text", { 0.92f, 0.93f, 0.95f, 1.0f });
            const UIColor muted = theme.GetToken("TextMuted", { 0.58f, 0.62f, 0.69f, 1.0f });

            d.AddRoundedRect(b.Min, b.Max, theme.GetToken("Surface0", { 0.094f, 0.11f, 0.149f, 1.0f }));
            PaintedTextures.clear();
            if (m_Panel.m_Entries.empty())
            {
                const std::string message = m_Panel.m_Search.empty() ? "This folder is empty -- drag files here or use + Add" : "No assets match the search";
                const UIVec2 size = NoxUI::UILabel::MeasureText(message, nullptr, 14.0f);
                d.AddText(message, nullptr, { b.Min.x + std::max(8.0f, (b.Width() - size.x) * 0.5f), b.Min.y + 40.0f }, 14.0f, muted);
                return;
            }

            const NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent();
            const bool dragging = ctx && ctx->IsDragging() && b.Contains(ctx->GetLastMousePos());
            const int dropIndex = dragging ? TileAt(ctx->GetLastMousePos()) : -1;

            const int columns = Columns();
            const float rowH = TileHeight() + kGap;
            const int firstRow = std::max(0, static_cast<int>((m_ScrollY - kGap) / rowH));
            const int lastRow = static_cast<int>((m_ScrollY + b.Height()) / rowH) + 1;
            const int first = firstRow * columns;
            const int last = std::min(static_cast<int>(m_Panel.m_Entries.size()), (lastRow + 1) * columns);

            d.PushClipRect(b.Min, { b.Max.x - m_ScrollBar.GetReservedWidth(), b.Max.y });
            for (int i = first; i < last; ++i)
            {
                const ContentEntry& entry = m_Panel.m_Entries[static_cast<size_t>(i)];
                const NoxUI::UIRect r = TileRect(i);
                const bool hovered = i == m_HoveredIndex;
                const bool selected = entry.Handle != 0 ? m_Selected.contains(entry.Handle) : (entry.Path == m_SelectedPath);
                const bool dropTarget = i == dropIndex && entry.IsDirectory;
                if (entry.IsDirectory)
                    PaintFolder(d, r, entry, hovered, selected, dropTarget, accent, text);
                else
                    PaintAsset(d, r, entry, hovered, selected, surface, well, border, accent, text, muted);
            }
            d.PopClipRect();
        }

        void OnPaintOverlay(NoxUI::UIDrawList& d, const NoxUI::UITheme& theme) override
        {
            m_ScrollBar.Paint(d, theme, m_ScrollY);
        }

        NoxUI::UIRect GetChildClipRect() const override { return GetBounds(); }

    private:
        static constexpr float kGap = 10.0f;
        static constexpr float kNameSize = 13.5f;
        static constexpr float kTypeSize = 12.5f;

        float TileWidth() const { return ThumbSize; }
        float TileHeight() const { return ThumbSize + 3.0f + 6.0f + NoxUI::UILabel::MeasureText("Ag", nullptr, kNameSize).y * 2.0f + 2.0f + 16.0f; }

        int Columns() const
        {
            const float usable = std::max(0.0f, GetBounds().Width() - NoxUI::UIScrollBar::kLaneWidth - kGap);
            return std::max(1, static_cast<int>(usable / (TileWidth() + kGap)));
        }

        float ContentHeight() const
        {
            const int rows = (static_cast<int>(m_Panel.m_Entries.size()) + Columns() - 1) / Columns();
            return kGap + static_cast<float>(rows) * (TileHeight() + kGap);
        }

        NoxUI::UIRect TileRect(int index) const
        {
            const int columns = Columns();
            const int row = index / columns, column = index % columns;
            const NoxUI::UIRect& b = GetBounds();
            const float x = b.Min.x + kGap + static_cast<float>(column) * (TileWidth() + kGap);
            const float y = b.Min.y + kGap + static_cast<float>(row) * (TileHeight() + kGap) - m_ScrollY;
            return { { x, y }, { x + TileWidth(), y + TileHeight() } };
        }

        int TileAt(const UIVec2& p) const
        {
            const NoxUI::UIRect& b = GetBounds();
            if (!b.Contains(p))
                return -1;
            const int columns = Columns();
            const float localX = p.x - b.Min.x - kGap, localY = p.y - b.Min.y - kGap + m_ScrollY;
            if (localX < 0.0f || localY < 0.0f)
                return -1;
            const int column = static_cast<int>(localX / (TileWidth() + kGap));
            const int row = static_cast<int>(localY / (TileHeight() + kGap));
            if (column >= columns)
                return -1;
            // In the gap between tiles: nothing.
            if (localX - static_cast<float>(column) * (TileWidth() + kGap) > TileWidth() || localY - static_cast<float>(row) * (TileHeight() + kGap) > TileHeight())
                return -1;
            const int index = row * columns + column;
            return index < static_cast<int>(m_Panel.m_Entries.size()) ? index : -1;
        }

        void PaintFolder(NoxUI::UIDrawList& d, const NoxUI::UIRect& r, const ContentEntry& entry, bool hovered, bool selected, bool dropTarget,
                         const UIColor& accent, const UIColor& text) const
        {
            if (hovered || selected || dropTarget)
            {
                const UIColor fill = dropTarget ? UIColor{ accent.r, accent.g, accent.b, 0.28f } : selected ? UIColor{ accent.r, accent.g, accent.b, 0.22f } : NoxUI::ThemeColor("ContentTileFill", { 1.0f, 1.0f, 1.0f, 0.05f });
                d.AddRoundedRect(r.Min, r.Max, fill, glm::vec4(6.0f), (selected || dropTarget) ? 1.5f : 0.0f, accent);
            }
            DrawFolderIcon(d, { r.Center().x, r.Min.y + ThumbSize * 0.52f }, ThumbSize * 0.62f, hovered ? NoxUI::ThemeColor("ContentFolderHovered", { 0.95f, 0.80f, 0.48f, 1.0f }) : kFolderColor);
            const float nameTop = r.Min.y + ThumbSize + 6.0f;
            const std::string name = NoxUI::UILabel::ElideText(entry.Name, nullptr, kNameSize, r.Width() - 8.0f);
            const UIVec2 size = NoxUI::UILabel::MeasureText(name, nullptr, kNameSize);
            d.AddText(name, nullptr, { r.Center().x - size.x * 0.5f, nameTop }, kNameSize, text);
        }

        void PaintAsset(NoxUI::UIDrawList& d, const NoxUI::UIRect& r, const ContentEntry& entry, bool hovered, bool selected,
                        const UIColor& surface, const UIColor& well, const UIColor& border, const UIColor& accent,
                        const UIColor& text, const UIColor& muted) const
        {
            const AssetTypeVisual visual = entry.IsScript ? AssetTypeVisual{ "C# Script", kScriptColor } : GetAssetTypeVisual(entry.Type);
            const UIColor cardFill = hovered ? UIColor{ surface.r + 0.035f, surface.g + 0.035f, surface.b + 0.04f, 1.0f } : surface;

            // The card with its shadow; the selected one outlined in the accent colour.
            d.AddRoundedRect(r.Min, r.Max, cardFill, glm::vec4(5.0f), selected ? 2.0f : 1.0f, selected ? accent : (hovered ? UIColor{ border.r + 0.08f, border.g + 0.08f, border.b + 0.1f, 1.0f } : border),
                             { 0.0f, 0.0f, 0.0f, hovered ? 0.55f : 0.40f }, { 0.0f, 3.0f }, hovered ? 10.0f : 7.0f);
            if (selected)
                d.AddRoundedRect(r.Min, r.Max, { accent.r, accent.g, accent.b, 0.12f }, glm::vec4(5.0f));

            // The picture: the asset itself where there is one, else the type's icon on a dark well.
            const NoxUI::UIRect pic{ { r.Min.x + 1.0f, r.Min.y + 1.0f }, { r.Max.x - 1.0f, r.Min.y + ThumbSize } };
            d.AddRoundedRect(pic.Min, pic.Max, well, { 4.0f, 4.0f, 0.0f, 0.0f });
            if (!PaintPreview(d, pic, entry))
                DrawAssetTypeIcon(d, entry.IsScript ? AssetType::MaterialShader : entry.Type, pic.Center(), ThumbSize * 0.46f, visual.Color);

            // The type's line under the picture.
            d.AddRoundedRect({ r.Min.x + 1.0f, pic.Max.y }, { r.Max.x - 1.0f, pic.Max.y + 3.0f }, visual.Color);

            // The name, up to two lines; the second ends in "..." when it does not fit.
            const float nameLeft = r.Min.x + 6.0f, nameWidth = r.Width() - 12.0f;
            const float nameTop = pic.Max.y + 3.0f + 5.0f;
            const std::string wrapped = NoxUI::UILabel::WrapText(entry.Name, nullptr, kNameSize, nameWidth);
            std::string line1 = wrapped, line2;
            if (const size_t newline = wrapped.find('\n'); newline != std::string::npos)
            {
                line1 = wrapped.substr(0, newline);
                line2 = wrapped.substr(newline + 1);
                std::replace(line2.begin(), line2.end(), '\n', ' ');
            }
            const float lineH = NoxUI::UILabel::MeasureText("Ag", nullptr, kNameSize).y;
            if (!(IsRenamingIndex(entry)))
            {
                d.AddText(NoxUI::UILabel::ElideText(line1, nullptr, kNameSize, nameWidth), nullptr, { nameLeft, nameTop }, kNameSize, text);
                if (!line2.empty())
                    d.AddText(NoxUI::UILabel::ElideText(line2, nullptr, kNameSize, nameWidth), nullptr, { nameLeft, nameTop + lineH }, kNameSize, text);
            }

            // The type at the bottom right, small and muted.
            const std::string type = NoxUI::UILabel::ElideText(visual.Label, nullptr, kTypeSize, nameWidth);
            const UIVec2 typeSize = NoxUI::UILabel::MeasureText(type, nullptr, kTypeSize);
            const UIColor typeColor = glm::mix(muted, text, 0.3f);
            d.AddText(type, nullptr, { r.Max.x - 6.0f - typeSize.x, r.Max.y - 5.0f - typeSize.y }, kTypeSize, typeColor);
        }

        bool IsRenamingIndex(const ContentEntry& entry) const { return IsRenaming() && entry.Handle == m_RenameHandle; }

        // The rendered picture of a mesh, prefab or material, or a texture itself; false while there is none yet.
        bool PaintPreview(NoxUI::UIDrawList& d, const NoxUI::UIRect& pic, const ContentEntry& entry) const
        {
            if (entry.Handle == 0)
                return false;
            if (AssetPreviewRenderer::CanRender(entry.Type))
            {
                if (Ref<Texture2D> image = AssetPreviewRenderer::Get().GetThumbnail(entry.Handle))
                {
                    const uint32_t slot = image->GetDescriptorIndexSlot();
                    if (slot != ~0u)
                    {
                        // Drawn previews are upright.
                        d.AddTexturedQuad(pic.Min, pic.Max, slot, { 0.0f, 0.0f }, { 1.0f, 1.0f }, glm::vec4(1.0f), { 4.0f, 4.0f, 0.0f, 0.0f });
                        return true;
                    }
                }
                return false;
            }
            if (entry.Type == AssetType::Texture2D)
            {
                // Asked for every frame it is in sight: loads in the background, drawn once it is there.
                PaintedTextures.insert(entry.Handle);
                if (AssetManager::RequestAsset(entry.Handle) != AssetState::Ready)
                    return false;
                const Texture2D* texture = AssetManager::FindLoadedAsset<Texture2D>(entry.Handle);
                if (!texture || texture->GetDescriptorIndexSlot() == ~0u)
                    return false;
                // Kept in its proportions, centred; imported textures are stored bottom row first.
                const float w = static_cast<float>(std::max(texture->GetWidth(), 1u)), h = static_cast<float>(std::max(texture->GetHeight(), 1u));
                const float scale = std::min(pic.Width() / w, pic.Height() / h);
                const UIVec2 size{ w * scale, h * scale };
                const UIVec2 min = pic.Center() - size * 0.5f;
                d.AddTexturedQuad(min, min + size, texture->GetDescriptorIndexSlot(), { 0.0f, 1.0f }, { 1.0f, 0.0f });
                return true;
            }
            return false;
        }

        void FinishRename(bool apply, const std::string& text)
        {
            if (!IsRenaming())
                return;
            const AssetHandle handle = m_RenameHandle;
            CancelRename();
            if (apply)
                m_Panel.CommitRename(handle, text);
        }

        void CancelRename()
        {
            m_RenameHandle = 0;
            m_RenameIndex = -1;
            m_FocusRenameNextFrame = false;
            if (m_RenameInput)
                m_RenameInput->Visible = false;
        }

        ContentBrowserPanel& m_Panel;
        NoxUI::UIScrollBar m_ScrollBar;
        float m_ScrollY = 0.0f;
        int m_HoveredIndex = -1;
        int m_PressedIndex = -1;
        int m_Anchor = -1;
        int m_ScrollToIndex = -1;
        int m_LastClickIndex = -1;
        std::chrono::steady_clock::time_point m_LastClickTime{};
        AssetHandle m_DeferredSingleSelect = 0;
        std::filesystem::path m_SelectedPath;

        Ref<NoxUI::UITextInput> m_RenameInput;
        AssetHandle m_RenameHandle = 0;
        int m_RenameIndex = -1;
        int m_RenameFrames = 0;
        bool m_FocusRenameNextFrame = false;
    };

    // ------------------------------------------------------------------------------------------------------------------------------
    // The panel
    // ------------------------------------------------------------------------------------------------------------------------------

    ContentBrowserPanel::ContentBrowserPanel() = default;
    ContentBrowserPanel::~ContentBrowserPanel() = default;

    Ref<NoxUI::UIElement> ContentBrowserPanel::BuildUI()
    {
        if (Project::GetActive())
        {
            m_BaseDirectory = Project::GetActiveAssetDirectory();
            if (m_CurrentDirectory.empty() || !IsInside(m_CurrentDirectory, m_BaseDirectory) && m_CurrentDirectory != m_BaseDirectory)
                m_CurrentDirectory = m_BaseDirectory;
        }

        // Every control of the bars is this high, so they line up.
        constexpr float kBarHeight = 26.0f;
        auto compactButton = [](const std::string& text, std::function<void()> onClick, const std::string& tooltip)
        {
            Ref<NoxUI::UIButton> button(new NoxUI::UIButton(text, std::move(onClick)));
            button->Padding = NoxUI::UIThickness::Symmetric(10.0f, 2.0f);
            button->HeightMode = NoxUI::UISizeMode::Fixed;
            button->FixedSize = { 0.0f, kBarHeight };
            button->Tooltip = tooltip;
            button->VAlign = NoxUI::UIAlignment::Center;
            return button;
        };

        // First row (Unreal's): what makes things, then where you are, then the settings.
        Ref<NoxUI::UIStackBox> toolbar(new NoxUI::UIStackBox(NoxUI::UIOrientation::Horizontal, 6.0f));
        toolbar->WidthMode = NoxUI::UISizeMode::Fill;
        toolbar->Padding = NoxUI::UIThickness{ 6.0f, 5.0f, 6.0f, 4.0f };
        toolbar->VAlign = NoxUI::UIAlignment::Center;

        Ref<NoxUI::UIButton> addButton = compactButton("+ Add", {}, "Make a new asset or folder here");
        addButton->StyleVariant = "primary";
        NoxUI::UIButton* addRaw = addButton.get();
        addButton->OnClick = [this, addRaw]()
        {
            if (NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent())
                NoxUI::UIMenuPopup::ShowPopupMenu(*ctx, addRaw->GetBounds(), BuildAddMenu());
        };
        toolbar->AddChild(addButton);
        toolbar->AddChild(compactButton("Import", [this]() { PickAndImport(); }, "Bring a file into the open folder (or drag files from the system onto the editor)"));

        m_BackButton = compactButton("<", [this]() { GoBack(); }, "Back (the folder you came from)");
        m_ForwardButton = compactButton(">", [this]() { GoForward(); }, "Forward");
        m_BackButton->Margin = NoxUI::UIThickness{ 6.0f, 0.0f, 0.0f, 0.0f };
        toolbar->AddChild(m_BackButton);
        toolbar->AddChild(m_ForwardButton);

        m_PathBar = Ref<ContentPathBar>(new ContentPathBar());
        m_PathBar->OnNavigate = [this](int segment)
        {
            std::filesystem::path target = m_BaseDirectory;
            const std::filesystem::path relative = m_CurrentDirectory.lexically_relative(m_BaseDirectory);
            int index = 0;
            for (const auto& part : relative)
            {
                if (index++ >= segment || part == ".")
                    break;
                target /= part;
            }
            NavigateTo(target);
        };
        toolbar->AddChild(m_PathBar);
        if (m_ToolbarExtra)
        {
            m_ToolbarExtra->VAlign = NoxUI::UIAlignment::Center;
            toolbar->AddChild(m_ToolbarExtra);
        }

        Ref<NoxUI::UIButton> viewButton = compactButton("Settings", {}, "Tile size, refresh, show in Explorer");
        NoxUI::UIButton* viewRaw = viewButton.get();
        viewButton->OnClick = [this, viewRaw]()
        {
            NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent();
            if (!ctx || !m_Tiles)
                return;
            const float size = m_Tiles->ThumbSize;
            std::vector<NoxUI::UIMenuItem> items;
            items.push_back({ "Small Tiles", "", [this]() { m_Tiles->ThumbSize = 72.0f; }, false, true, size < 88.0f });
            items.push_back({ "Medium Tiles", "", [this]() { m_Tiles->ThumbSize = 104.0f; }, false, true, size >= 88.0f && size < 136.0f });
            items.push_back({ "Large Tiles", "", [this]() { m_Tiles->ThumbSize = 160.0f; }, false, true, size >= 136.0f });
            items.push_back({ "", "", {}, true });
            items.push_back({ "Refresh", "", [this]()
            {
                if (EditorAssetManager* assets = Assets())
                    assets->ScanAndRegisterNewAssets({});
                Refresh();
            }, false, true, false, false, "Files added or moved outside the editor are picked up (moved ones keep their handle)." });
            items.push_back({ "Show in Explorer", "", [this]() { ShowInExplorer(m_CurrentDirectory); } });
            NoxUI::UIMenuPopup::ShowPopupMenu(*ctx, viewRaw->GetBounds(), std::move(items), true);
        };
        toolbar->AddChild(viewButton);

        // Folder tree | (type filter + search over the tiles).
        m_FolderTree = Ref<NoxUI::UITreeView>(new NoxUI::UITreeView());
        m_FolderTree->WidthMode = NoxUI::UISizeMode::Fixed;
        m_FolderTree->FixedSize = { 210.0f, 0.0f };
        m_FolderTree->HeightMode = NoxUI::UISizeMode::Fill;
        m_FolderTree->ShowTagDots = false;
        m_FolderTree->EmptyText = "No folders";
        m_FolderTree->OnSelect = [this](uint64_t id)
        {
            if (const auto it = m_TreeFolders.find(id); it != m_TreeFolders.end() && it->second != m_CurrentDirectory)
                NavigateTo(it->second);
        };
        m_FolderTree->OnRowClick = [this](uint64_t id, bool, bool)
        {
            if (const auto it = m_TreeFolders.find(id); it != m_TreeFolders.end() && it->second != m_CurrentDirectory)
                NavigateTo(it->second);
        };

        m_FilterDropdown = Ref<NoxUI::UIDropdown>(new NoxUI::UIDropdown(nullptr, std::vector<std::string>(std::begin(kFilterNames), std::end(kFilterNames)), m_TypeFilter,
            [this](int index)
            {
                m_TypeFilter = index;
                m_EntriesDirty = true;
            }));
        m_FilterDropdown->WidthMode = NoxUI::UISizeMode::Fixed;
        m_FilterDropdown->HeightMode = NoxUI::UISizeMode::Fixed;
        m_FilterDropdown->FixedSize = { 128.0f, kBarHeight };
        m_FilterDropdown->MinSize = { 0.0f, 0.0f }; // (its own minimum is 28 px)
        m_FilterDropdown->VAlign = NoxUI::UIAlignment::Center;
        m_FilterDropdown->Tooltip = "Show only one kind of asset";

        m_SearchBar = Ref<NoxUI::UISearchBar>(new NoxUI::UISearchBar("Search this folder and below", [this](const std::string& query)
        {
            m_Search = query;
            m_EntriesDirty = true;
        }));
        m_SearchBar->WidthMode = NoxUI::UISizeMode::Fill;
        m_SearchBar->HeightMode = NoxUI::UISizeMode::Fixed;
        m_SearchBar->FixedSize = { 0.0f, kBarHeight };
        m_SearchBar->VAlign = NoxUI::UIAlignment::Center;

        Ref<NoxUI::UIStackBox> searchRow(new NoxUI::UIStackBox(NoxUI::UIOrientation::Horizontal, 6.0f));
        searchRow->WidthMode = NoxUI::UISizeMode::Fill;
        searchRow->VAlign = NoxUI::UIAlignment::Center;
        searchRow->AddChild(m_FilterDropdown);
        searchRow->AddChild(m_SearchBar);

        m_Tiles = Ref<ContentTileView>(new ContentTileView(*this));

        Ref<NoxUI::UIStackBox> assetsColumn(new NoxUI::UIStackBox(NoxUI::UIOrientation::Vertical, 6.0f));
        assetsColumn->WidthMode = NoxUI::UISizeMode::Fill;
        assetsColumn->HeightMode = NoxUI::UISizeMode::Fill;
        assetsColumn->AddChild(searchRow);
        assetsColumn->AddChild(m_Tiles);

        Ref<NoxUI::UIStackBox> body(new NoxUI::UIStackBox(NoxUI::UIOrientation::Horizontal, 6.0f));
        body->WidthMode = NoxUI::UISizeMode::Fill;
        body->HeightMode = NoxUI::UISizeMode::Fill;
        body->Padding = NoxUI::UIThickness{ 6.0f, 0.0f, 6.0f, 0.0f };
        body->AddChild(m_FolderTree);
        body->AddChild(assetsColumn);

        m_Footer = Ref<NoxUI::UILabel>(new NoxUI::UILabel("", 13.0f));
        m_Footer->UseColorOverride = true;
        m_Footer->ColorOverride = NoxUI::ThemeColor("TextMuted", { 0.60f, 0.64f, 0.72f, 1.0f });
        m_Footer->Margin = NoxUI::UIThickness{ 10.0f, 3.0f, 10.0f, 4.0f };

        Ref<NoxUI::UIStackBox> root(new NoxUI::UIStackBox(NoxUI::UIOrientation::Vertical, 0.0f));
        root->WidthMode = NoxUI::UISizeMode::Fill;
        root->HeightMode = NoxUI::UISizeMode::Fill;
        root->AddChild(toolbar);
        root->AddChild(body);
        root->AddChild(m_Footer);
        m_Root = root;

        m_EntriesDirty = true;
        m_TreeDirty = true;
        Update(0.0f);
        return m_Root;
    }

    void ContentBrowserPanel::CollectAssetReferences(std::unordered_set<AssetHandle>& handles) const
    {
        if (m_Tiles)
            handles.insert(m_Tiles->PaintedTextures.begin(), m_Tiles->PaintedTextures.end());
    }

    void ContentBrowserPanel::Refresh()
    {
        m_EntriesDirty = true;
        m_TreeDirty = true;
    }

    std::filesystem::path ContentBrowserPanel::CurrentFolder() const
    {
        const std::filesystem::path relative = m_CurrentDirectory.lexically_relative(m_BaseDirectory);
        return relative == "." ? std::filesystem::path() : relative;
    }

    void ContentBrowserPanel::ShowNewAsset(AssetHandle handle)
    {
        m_PendingShow = handle;
        m_EntriesDirty = true;
    }

    void ContentBrowserPanel::Notify(NoxUI::UINotificationSeverity severity, std::string title, std::string message)
    {
        if (m_Notify)
            m_Notify({ severity, std::move(title), std::move(message) });
    }

    void ContentBrowserPanel::Update(float dt)
    {
        EditorAssetManager* assets = Assets();
        if (!assets || !m_Root)
            return;

        // A new project: start at its Assets folder.
        if (m_BaseDirectory != Project::GetActiveAssetDirectory())
        {
            m_BaseDirectory = Project::GetActiveAssetDirectory();
            m_CurrentDirectory = m_BaseDirectory;
            m_Back.clear();
            m_Forward.clear();
            Refresh();
        }

        // Files that came in (an import finishing, a tool, a copy in Explorer): the registry grew, or the folder changed on disk
        // (looked at twice a second -- one stat).
        // (Twice a second at most: while a big folder's textures are cooked, files arrive many times a second.)
        m_RescanTimer += dt;
        if (m_RescanTimer > 0.5f)
        {
            m_RescanTimer = 0.0f;
            std::error_code error;
            const auto stamp = std::filesystem::last_write_time(m_CurrentDirectory, error);
            if ((!error && stamp != m_FolderStamp) || assets->GetAssetRegistry().size() != m_RegistrySize)
                Refresh();
        }

        // A model import running in the background finished: one toast for it.
        const bool importing = assets->GetImportProgress().Imports > 0;
        if (m_ImportsWereRunning && !importing)
            Notify(NoxUI::UINotificationSeverity::Success, "Import finished", "The model's meshes, materials and textures are in the Content Browser.");
        m_ImportsWereRunning = importing;

        FinishReferenceCheck();
        if (m_TreeDirty)
            RebuildFolderTree();
        if (m_EntriesDirty)
            RebuildEntries();

        if (m_PendingShow != 0)
        {
            const AssetHandle handle = m_PendingShow;
            m_PendingShow = 0;
            if (assets->IsAssetHandleValid(handle))
            {
                const std::filesystem::path folder = (m_BaseDirectory / assets->GetMetadata(handle).FilePath).parent_path();
                if (folder.lexically_normal() != m_CurrentDirectory.lexically_normal())
                    NavigateTo(folder);
                RebuildEntries();
                m_Tiles->Select(handle, true);
                BeginRename(handle);
            }
        }
        SyncChrome();
    }

    void ContentBrowserPanel::RebuildFolderTree()
    {
        m_TreeDirty = false;
        if (!m_FolderTree)
            return;
        m_TreeFolders.clear();

        // Expanded state survives a rebuild; folders start collapsed by default (except the root Assets node).
        std::unordered_set<uint64_t> expanded;
        std::function<void(const std::vector<NoxUI::UITreeNode>&)> gatherExpanded = [&](const std::vector<NoxUI::UITreeNode>& nodes)
        {
            for (const NoxUI::UITreeNode& node : nodes)
            {
                if (node.Expanded)
                    expanded.insert(node.Id);
                gatherExpanded(node.Children);
            }
        };
        gatherExpanded(m_FolderTree->GetNodes());

        std::function<NoxUI::UITreeNode(const std::filesystem::path&, const std::string&, int)> build =
            [&](const std::filesystem::path& folder, const std::string& label, int depth) -> NoxUI::UITreeNode
        {
            NoxUI::UITreeNode node;
            node.Id = FolderId(folder);
            node.Label = label;
            node.IsFolder = true;
            node.Draggable = false;
            // Start collapsed by default; keep user-expanded folders open and unfold strict ancestors of the active folder.
            const bool isStrictAncestor = (folder != m_CurrentDirectory) && IsInside(m_CurrentDirectory, folder);
            node.Expanded = expanded.contains(node.Id) || isStrictAncestor;
            m_TreeFolders[node.Id] = folder;
            if (depth > 24)
                return node;
            std::vector<std::filesystem::path> children;
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(folder, error))
            {
                if (entry.is_directory(error) && entry.path().filename().string().rfind('.', 0) != 0)
                    children.push_back(entry.path());
            }
            std::sort(children.begin(), children.end(), [](const auto& a, const auto& b) { return ToLower(a.filename().string()) < ToLower(b.filename().string()); });
            for (const auto& child : children)
                node.Children.push_back(build(child, child.filename().string(), depth + 1));
            return node;
        };

        std::vector<NoxUI::UITreeNode> nodes;
        nodes.push_back(build(m_BaseDirectory, "Assets", 0));
        nodes.front().Expanded = true;
        // The same folders, opened the same way: the tree keeps its rows (and its scroll position, its hover).
        size_t signature = 0;
        std::function<void(const NoxUI::UITreeNode&)> hash = [&](const NoxUI::UITreeNode& node)
        {
            signature = signature * 1099511628211ull ^ (node.Id + (node.Expanded ? 1 : 0));
            for (const NoxUI::UITreeNode& child : node.Children)
                hash(child);
        };
        hash(nodes.front());
        if (signature != m_TreeSignature)
        {
            m_TreeSignature = signature;
            m_FolderTree->SetNodes(std::move(nodes));
        }
        m_FolderTree->SetSelectedId(FolderId(m_CurrentDirectory));
    }

    bool ContentBrowserPanel::PassesFilter(AssetType type, bool isScript) const
    {
        switch (m_TypeFilter)
        {
        case 1: return type == AssetType::Mesh || type == AssetType::StaticMesh || type == AssetType::MeshSource || type == AssetType::SkeletalMesh;
        case 2: return type == AssetType::Material || type == AssetType::MaterialGraph || type == AssetType::MaterialShader;
        case 3: return type == AssetType::Texture2D;
        case 4: return type == AssetType::Prefab;
        case 5: return type == AssetType::Scene;
        case 6: return type == AssetType::AnimationSequence || type == AssetType::AnimationGraph || type == AssetType::Skeleton || type == AssetType::Timeline;
        case 7: return type == AssetType::AudioClip || type == AssetType::AudioGraph || type == AssetType::AudioMixer;
        case 8: return isScript;
        case 9: return type == AssetType::Volume;
        case 10: return type == AssetType::Vfx;
        default: return true;
        }
    }

    void ContentBrowserPanel::RebuildEntries()
    {
        m_EntriesDirty = false;
        EditorAssetManager* assets = Assets();
        if (!assets)
            return;
        m_Entries.clear();
        m_RegistrySize = assets->GetAssetRegistry().size();
        const bool newListing = m_ListedDirectory != m_CurrentDirectory || m_ListedSearch != m_Search || m_ListedFilter != m_TypeFilter;
        m_ListedDirectory = m_CurrentDirectory;
        m_ListedSearch = m_Search;
        m_ListedFilter = m_TypeFilter;
        std::error_code error;
        m_FolderStamp = std::filesystem::last_write_time(m_CurrentDirectory, error);

        // Search: every registered asset in the open folder and below whose name has every word typed (case ignored).
        if (!m_Search.empty())
        {
            std::vector<std::string> words;
            {
                std::string word;
                for (const char ch : ToLower(m_Search) + " ")
                {
                    if (ch == ' ')
                    {
                        if (!word.empty())
                            words.push_back(std::move(word));
                        word.clear();
                    }
                    else
                    {
                        word.push_back(ch);
                    }
                }
            }
            for (const auto& [handle, metadata] : assets->GetAssetRegistry())
            {
                const std::filesystem::path absolute = m_BaseDirectory / metadata.FilePath;
                if (!IsInside(absolute, m_CurrentDirectory) || !PassesFilter(metadata.Type, false))
                    continue;
                const std::string name = ToLower(metadata.FilePath.filename().string());
                if (!std::all_of(words.begin(), words.end(), [&name](const std::string& w) { return name.find(w) != std::string::npos; }))
                    continue;
                m_Entries.push_back({ absolute, metadata.FilePath.stem().string(), metadata.FilePath.extension().string(), handle, metadata.Type });
            }
            std::sort(m_Entries.begin(), m_Entries.end(), [](const ContentEntry& a, const ContentEntry& b) { return ToLower(a.Name) < ToLower(b.Name); });
            if (m_Tiles)
                m_Tiles->ResetForNewEntries(newListing);
            return;
        }

        // The folder: its subfolders first, then its assets (source and cooked files of one asset shown once, under the path the
        // registry calls it by: Fox.gltf / Fox.nmesh, Texture.png / Texture.ntex), then its C# scripts.
        std::unordered_map<std::string, AssetHandle> handleOfPath;
        for (const auto& [handle, metadata] : assets->GetAssetRegistry())
        {
            handleOfPath.emplace(metadata.FilePath.lexically_normal().generic_string(), handle);
            if (!metadata.SourceFilePath.empty())
                handleOfPath.emplace(metadata.SourceFilePath.lexically_normal().generic_string(), handle);
        }

        std::vector<ContentEntry> folders, files;
        for (const auto& entry : std::filesystem::directory_iterator(m_CurrentDirectory, error))
        {
            const std::filesystem::path& path = entry.path();
            if (path.filename().string().rfind('.', 0) == 0)
                continue;
            if (entry.is_directory(error))
            {
                if (m_TypeFilter == 0)
                    folders.push_back({ path, path.filename().string(), {}, 0, AssetType::None, true });
                continue;
            }
            const std::filesystem::path relative = path.lexically_relative(m_BaseDirectory).lexically_normal();
            const auto found = handleOfPath.find(relative.generic_string());
            if (found == handleOfPath.end())
            {
                if (path.extension() == ".cs" && PassesFilter(AssetType::None, true))
                    files.push_back({ path, path.stem().string(), ".cs", 0, AssetType::None, false, true });
                continue;
            }
            const AssetMetadata metadata = assets->GetMetadata(found->second);
            if (metadata.FilePath.lexically_normal() != relative || !PassesFilter(metadata.Type, false))
                continue;
            files.push_back({ path, path.stem().string(), path.extension().string(), found->second, metadata.Type });
        }
        auto byName = [](const ContentEntry& a, const ContentEntry& b) { return ToLower(a.Name) < ToLower(b.Name); };
        std::sort(folders.begin(), folders.end(), byName);
        std::sort(files.begin(), files.end(), byName);
        m_Entries = std::move(folders);
        m_Entries.insert(m_Entries.end(), std::make_move_iterator(files.begin()), std::make_move_iterator(files.end()));
        if (m_Tiles)
            m_Tiles->ResetForNewEntries(newListing);
    }

    void ContentBrowserPanel::SyncChrome()
    {
        if (m_PathBar)
        {
            std::vector<std::string> segments = { "Assets" };
            const std::filesystem::path relative = m_CurrentDirectory.lexically_relative(m_BaseDirectory);
            for (const auto& part : relative)
            {
                if (part != "." && !part.empty())
                    segments.push_back(part.string());
            }
            if (segments != m_PathBar->GetSegments())
                m_PathBar->SetSegments(std::move(segments));
        }
        if (m_BackButton)
            m_BackButton->SetEnabled(!m_Back.empty());
        if (m_ForwardButton)
            m_ForwardButton->SetEnabled(!m_Forward.empty());
        if (m_Footer && m_Tiles)
        {
            size_t assets = 0;
            for (const ContentEntry& entry : m_Entries)
                assets += entry.IsDirectory ? 0 : 1;
            std::string text = std::to_string(m_Entries.size()) + (m_Entries.size() == 1 ? " item" : " items");
            if (!m_Search.empty())
                text = std::to_string(assets) + " found";
            if (!m_Tiles->m_Selected.empty())
                text += "  (" + std::to_string(m_Tiles->m_Selected.size()) + " selected)";
            m_Footer->Text = text;
        }
    }

    void ContentBrowserPanel::NavigateTo(const std::filesystem::path& absoluteFolder, bool recordHistory)
    {
        const std::filesystem::path target = absoluteFolder.lexically_normal();
        if (target == m_CurrentDirectory.lexically_normal())
            return;
        if (recordHistory)
        {
            m_Back.push_back(m_CurrentDirectory);
            m_Forward.clear();
        }
        m_CurrentDirectory = target;
        if (m_SearchBar && !m_Search.empty())
        {
            m_Search.clear();
            m_SearchBar->SetQuery("", false);
        }
        m_EntriesDirty = true;
        if (m_FolderTree)
        {
            m_FolderTree->SetSelectedId(FolderId(m_CurrentDirectory));
            m_TreeDirty = true; // unfolds the tree to it
        }
    }

    void ContentBrowserPanel::GoBack()
    {
        if (m_Back.empty())
            return;
        m_Forward.push_back(m_CurrentDirectory);
        const std::filesystem::path target = m_Back.back();
        m_Back.pop_back();
        NavigateTo(target, false);
    }

    void ContentBrowserPanel::GoForward()
    {
        if (m_Forward.empty())
            return;
        m_Back.push_back(m_CurrentDirectory);
        const std::filesystem::path target = m_Forward.back();
        m_Forward.pop_back();
        NavigateTo(target, false);
    }

    void ContentBrowserPanel::GoUp()
    {
        if (m_CurrentDirectory.lexically_normal() != m_BaseDirectory.lexically_normal())
            NavigateTo(m_CurrentDirectory.parent_path());
    }

    void ContentBrowserPanel::Activate(const ContentEntry& entry)
    {
        if (entry.IsDirectory)
        {
            NavigateTo(entry.Path);
            return;
        }
        if (entry.IsScript || entry.Type == AssetType::MaterialShader)
        {
            // Code opens in the system's editor for its kind of file.
            const std::string url = "file:///" + entry.Path.generic_string();
            SDL_OpenURL(url.c_str());
            return;
        }
        if (entry.Type == AssetType::MeshSource)
        {
            OpenModelImportDialog(entry.Path.lexically_relative(m_BaseDirectory), {});
            return;
        }
        if (m_OpenAsset && entry.Handle != 0)
            m_OpenAsset(entry.Handle);
    }

    void ContentBrowserPanel::BeginRename(AssetHandle handle)
    {
        EditorAssetManager* assets = Assets();
        if (!assets || !m_Tiles || handle == 0)
            return;
        if (!assets->CanRename(handle))
        {
            Notify(NoxUI::UINotificationSeverity::Info, "This asset cannot be renamed here", "Scenes, and the skeletons and clips cooked from a model, keep their names.");
            return;
        }
        m_Tiles->BeginRename(handle);
    }

    void ContentBrowserPanel::CommitRename(AssetHandle handle, const std::string& newName)
    {
        EditorAssetManager* assets = Assets();
        if (!assets || !assets->IsAssetHandleValid(handle))
            return;
        const std::string current = assets->GetMetadata(handle).FilePath.stem().string();
        if (newName.empty() || newName == current)
            return;
        if (!assets->RenameAsset(handle, newName))
        {
            Notify(NoxUI::UINotificationSeverity::Error, "Rename failed", "\"" + newName + "\" is not a valid file name, or another file already has it.");
            return;
        }
        Refresh();
        RebuildEntries();
        if (m_Tiles)
            m_Tiles->Select(handle, true);
    }

    void ContentBrowserPanel::AdoptNewFile(const std::filesystem::path& absolutePath)
    {
        EditorAssetManager* assets = Assets();
        if (!assets)
            return;
        std::filesystem::path folder = absolutePath.parent_path().lexically_relative(m_BaseDirectory);
        if (folder == ".")
            folder.clear();
        assets->ScanAndRegisterNewAssets(folder);
        Refresh();
        RebuildEntries();
        const std::filesystem::path relative = absolutePath.lexically_relative(m_BaseDirectory).lexically_normal();
        for (const ContentEntry& entry : m_Entries)
        {
            if (entry.Handle != 0 && entry.Path.lexically_relative(m_BaseDirectory).lexically_normal() == relative)
            {
                m_Tiles->Select(entry.Handle, true);
                BeginRename(entry.Handle);
                return;
            }
        }
    }

    void ContentBrowserPanel::Duplicate(AssetHandle handle)
    {
        EditorAssetManager* assets = Assets();
        if (!assets || !assets->IsAssetHandleValid(handle))
            return;
        const std::filesystem::path source = m_BaseDirectory / assets->GetMetadata(handle).FilePath;
        const std::filesystem::path target = UniquePath(source.parent_path(), source.stem().string() + " Copy", source.extension().string());
        std::error_code error;
        if (!std::filesystem::copy_file(source, target, error))
        {
            Notify(NoxUI::UINotificationSeverity::Error, "Duplicate failed", error.message());
            return;
        }
        AdoptNewFile(target);
    }

    void ContentBrowserPanel::RequestDelete(std::vector<AssetHandle> handles)
    {
        StartReferenceCheck(ReferenceCheckMode::Delete, std::move(handles));
    }

    void ContentBrowserPanel::ShowReferences(AssetHandle handle)
    {
        StartReferenceCheck(ReferenceCheckMode::Show, { handle });
    }

    void ContentBrowserPanel::StartReferenceCheck(ReferenceCheckMode mode, std::vector<AssetHandle> handles)
    {
        EditorAssetManager* assets = Assets();
        NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent();
        std::erase_if(handles, [assets](AssetHandle handle) { return !assets || !assets->IsAssetHandleValid(handle); });
        if (!assets || !ctx || handles.empty())
            return;
        if (m_ReferenceCheck)
        {
            Notify(NoxUI::UINotificationSeverity::Info, "Still looking", "The project is being searched for what uses " + m_ReferenceCheck->Name + ".");
            return;
        }

        auto check = std::make_unique<ReferenceCheck>();
        check->Mode = mode;
        check->Handles = handles;
        check->Context = ctx;
        check->Name = assets->GetMetadata(handles.front()).FilePath.filename().string();
        // The open scene, unsaved changes included: read here, on the main thread.
        if (m_SceneReferences)
            m_SceneReferences(check->InScene);
        // The project's files on a worker thread, from a copy of the registry: the editor keeps drawing meanwhile.
        check->Users = std::async(std::launch::async,
            [registry = assets->GetAssetRegistry(), handles, assetDirectory = Project::GetActiveAssetDirectory(), projectDirectory = Project::GetProjectDirectory(),
             index = assets->GetReferenceIndex()]()
            {
                // Through the project's reference cache: a lookup per file (only files changed since they were indexed are read).
                std::vector<std::vector<std::filesystem::path>> users;
                users.reserve(handles.size());
                for (AssetHandle handle : handles)
                    users.push_back(EditorAssetManager::FindReferencesIn(registry, handle, assetDirectory, projectDirectory, index));
                return users;
            });
        m_ReferenceCheck = std::move(check);
    }

    std::string ContentBrowserPanel::GetBusyText() const
    {
        return m_ReferenceCheck ? "Looking for what uses " + m_ReferenceCheck->Name + "..." : std::string();
    }

    void ContentBrowserPanel::FinishReferenceCheck()
    {
        if (!m_ReferenceCheck || m_ReferenceCheck->Users.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        std::unique_ptr<ReferenceCheck> check = std::move(m_ReferenceCheck);
        const std::vector<std::vector<std::filesystem::path>> users = check->Users.get();
        EditorAssetManager* assets = Assets();
        if (!assets || !check->Context)
            return;
        // Deleted or moved meanwhile: asked again from what is there now.
        std::vector<AssetHandle> handles;
        std::vector<std::vector<std::filesystem::path>> stillUsers;
        for (size_t i = 0; i < check->Handles.size() && i < users.size(); ++i)
        {
            if (assets->IsAssetHandleValid(check->Handles[i]))
            {
                handles.push_back(check->Handles[i]);
                stillUsers.push_back(users[i]);
            }
        }
        if (handles.empty())
            return;

        if (check->Mode == ReferenceCheckMode::Delete)
        {
            ShowDeleteDialog(*check->Context, handles, stillUsers, check->InScene);
            return;
        }
        if (check->Mode == ReferenceCheckMode::Replace)
        {
            ShowReplaceDialog(*check->Context, handles.front(), stillUsers.front(), check->InScene.contains(handles.front()));
            return;
        }
        const AssetHandle handle = handles.front();
        const std::vector<std::filesystem::path>& found = stillUsers.front();
        std::string message;
        if (check->InScene.contains(handle))
            message += "The open scene\n";
        for (size_t i = 0; i < found.size() && i < 14; ++i)
            message += found[i].generic_string() + "\n";
        if (found.size() > 14)
            message += "... and " + std::to_string(found.size() - 14) + " more\n";
        if (message.empty())
            message = "Nothing in the project uses it.";
        NoxUI::UIModal::ShowConfirm(*check->Context, "References to " + assets->GetMetadata(handle).FilePath.filename().string(), message, {}, {}, "OK", "Close");
    }

    void ContentBrowserPanel::ShowDeleteDialog(NoxUI::UIContext& context, const std::vector<AssetHandle>& handles,
                                                 const std::vector<std::vector<std::filesystem::path>>& users, const std::unordered_set<AssetHandle>& inScene)
    {
        EditorAssetManager* assets = Assets();
        if (!assets)
            return;
        std::string message;
        size_t used = 0;
        for (size_t index = 0; index < handles.size(); ++index)
        {
            const AssetHandle handle = handles[index];
            const std::vector<std::filesystem::path>& fileUsers = users[index];
            const bool sceneUses = inScene.contains(handle);
            if (fileUsers.empty() && !sceneUses)
                continue;
            ++used;
            if (used <= 4)
            {
                message += assets->GetMetadata(handle).FilePath.filename().string() + " is used by ";
                message += sceneUses ? "the open scene" : "";
                for (size_t i = 0; i < fileUsers.size() && i < 3; ++i)
                    message += std::string((sceneUses || i > 0) ? ", " : "") + fileUsers[i].filename().string();
                if (fileUsers.size() > 3)
                    message += " and " + std::to_string(fileUsers.size() - 3) + " more";
                message += ".\n";
            }
        }
        const std::string title = handles.size() == 1
            ? "Delete " + assets->GetMetadata(handles.front()).FilePath.filename().string() + "?"
            : "Delete " + std::to_string(handles.size()) + " assets?";
        if (used == 0)
            message = handles.size() == 1 ? "Nothing uses it. The file is deleted from the project folder; this cannot be undone."
                                          : "Nothing uses them. The files are deleted from the project folder; this cannot be undone.";
        else
            message += "What uses it loses it (a mesh draws without its material, a slot becomes empty). This cannot be undone.";

        NoxUI::UIModal::ShowConfirm(context, title, message, [this, handles]()
        {
            EditorAssetManager* manager = Assets();
            if (!manager)
                return;
            size_t deleted = 0;
            for (AssetHandle handle : handles)
            {
                AssetPreviewRenderer::Get().Invalidate(handle);
                deleted += manager->DeleteAsset(handle) ? 1 : 0;
            }
            if (m_Tiles)
                m_Tiles->m_Selected.clear();
            Refresh();
            Notify(deleted == handles.size() ? NoxUI::UINotificationSeverity::Success : NoxUI::UINotificationSeverity::Warning,
                   deleted == 1 ? "Asset deleted" : std::to_string(deleted) + " assets deleted",
                   deleted == handles.size() ? std::string() : "Some files could not be deleted (see the Output Log).");
        }, {}, "Delete", "Cancel");
    }

    void ContentBrowserPanel::CreateVariant(AssetHandle base)
    {
        EditorAssetManager* assets = Assets();
        if (!m_CreateVariant || !assets || !assets->IsAssetHandleValid(base))
            return;
        // Next to its base (the base's folder, not the open one: a variant found beside what it varies).
        const AssetHandle variant = m_CreateVariant(base, assets->GetMetadata(base).FilePath.parent_path());
        if (variant == 0)
        {
            Notify(NoxUI::UINotificationSeverity::Error, "Variant not created", "See the Output Log for why.");
            return;
        }
        Notify(NoxUI::UINotificationSeverity::Success, "Variant created", assets->GetMetadata(variant).FilePath.filename().string());
        ShowNewAsset(variant);
    }

    void ContentBrowserPanel::ShowReplaceDialog(NoxUI::UIContext& context, AssetHandle from, const std::vector<std::filesystem::path>& users, bool sceneUses)
    {
        EditorAssetManager* assets = Assets();
        if (!assets || !assets->IsAssetHandleValid(from))
            return;
        const AssetMetadata fromMetadata = assets->GetMetadata(from);
        const AssetType type = fromMetadata.Type;

        // Which users are rewritten: the text files that name assets by handle (scenes, world cells, prefabs, graphs, timelines, the
        // mixer). Materials and cooked meshes are binary: they keep the old asset, and say so here (re-pick it in them by hand).
        std::unordered_map<std::string, AssetType> typeByPath;
        for (const auto& [handle, metadata] : assets->GetAssetRegistry())
            typeByPath[metadata.FilePath.lexically_normal().generic_string()] = metadata.Type;
        std::vector<std::string> rewritten, kept;
        for (const std::filesystem::path& file : users)
        {
            const auto found = typeByPath.find(file.lexically_normal().generic_string());
            const AssetType userType = found != typeByPath.end() ? found->second : AssetType::None;
            const bool text = file.extension() == ".noxcell" || userType == AssetType::Scene || userType == AssetType::Prefab || userType == AssetType::AnimationGraph ||
                              userType == AssetType::AudioGraph || userType == AssetType::AudioMixer || userType == AssetType::Timeline || userType == AssetType::MaterialGraph;
            (text ? rewritten : kept).push_back(file.generic_string());
        }

        struct State
        {
            AssetHandle Picked = 0;
            std::string Filter;
            std::vector<std::pair<NoxUI::UIButton*, AssetHandle>> Rows; // (the list's buttons: restyled in place when one is picked)
        };
        auto state = std::make_shared<State>();
        auto content = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 6.0f);
        const auto note = [&content](const std::string& text, glm::vec4 color = NoxUI::ThemeColor("ContentNote", { 0.72f, 0.76f, 0.84f, 1.0f }))
        {
            Ref<NoxUI::UILabel> ref(new NoxUI::UILabel(text, 13.0f));
            ref->WordWrap = true;
            ref->WidthMode = NoxUI::UISizeMode::Fill;
            ref->UseColorOverride = true;
            ref->ColorOverride = { color.r, color.g, color.b, color.a };
            content->AddChild(ref);
        };
        note("Everything that uses " + fromMetadata.FilePath.generic_string() + " will use the asset picked below instead. The asset itself stays "
             "(delete it afterwards if nothing needs it).");

        Ref<NoxUI::UITextInput> search(new NoxUI::UITextInput("", "Search"));
        search->WidthMode = NoxUI::UISizeMode::Fill;
        content->AddChild(search);
        auto scroll = NoxUI::MakeUI<NoxUI::UIScrollView>();
        scroll->WidthMode = NoxUI::UISizeMode::Fill;
        scroll->HeightMode = NoxUI::UISizeMode::Fixed;
        scroll->FixedSize = { 0.0f, 200.0f };
        content->AddChild(scroll);
        auto list = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 2.0f);
        list->WidthMode = NoxUI::UISizeMode::Fill;
        scroll->AddChild(list);

        std::string summary;
        if (sceneUses)
            summary += "The open scene (an undo step)\n";
        for (size_t i = 0; i < rewritten.size() && i < 8; ++i)
            summary += rewritten[i] + "\n";
        if (rewritten.size() > 8)
            summary += "... and " + std::to_string(rewritten.size() - 8) + " more\n";
        note(summary.empty() ? std::string("Nothing in the project names it in a file that can be rewritten.")
                             : "Rewritten (" + std::to_string(rewritten.size() + (sceneUses ? 1 : 0)) + "):\n" + summary);
        if (!kept.empty())
        {
            std::string text = "Not rewritten -- materials and meshes are binary files; pick the new asset in them by hand:\n";
            for (size_t i = 0; i < kept.size() && i < 6; ++i)
                text += kept[i] + "\n";
            if (kept.size() > 6)
                text += "... and " + std::to_string(kept.size() - 6) + " more\n";
            note(text, NoxUI::ThemeColor("StatusWarning", { 0.95f, 0.78f, 0.45f, 1.0f }));
        }
        note("Open asset editors with unsaved changes are asked about first; loaded assets that use it are read again.", NoxUI::ThemeColor("TextMuted", { 0.6f, 0.64f, 0.72f, 1.0f }));

        // The candidates: every other asset of its type, filtered by the search; the picked one highlighted.
        NoxUI::UIStackBox* listRaw = list.get();
        auto rebuild = [this, state, from, type, listRaw]()
        {
            EditorAssetManager* manager = Assets();
            listRaw->ClearChildren();
            state->Rows.clear();
            if (!manager)
                return;
            std::vector<std::pair<std::string, AssetHandle>> candidates;
            std::string filter = state->Filter;
            std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (const auto& [candidate, metadata] : manager->GetAssetRegistry())
            {
                if (candidate == from || metadata.Type != type)
                    continue;
                std::string path = metadata.FilePath.generic_string();
                std::string lower = path;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (filter.empty() || lower.find(filter) != std::string::npos)
                    candidates.emplace_back(std::move(path), candidate);
            }
            std::sort(candidates.begin(), candidates.end());
            for (const auto& [path, candidate] : candidates)
            {
                Ref<NoxUI::UIButton> row(new NoxUI::UIButton(path, [state, candidate]()
                {
                    // (restyled, not rebuilt: rebuilding would destroy the button running this)
                    state->Picked = candidate;
                    for (const auto& [button, handle] : state->Rows)
                        button->StyleVariant = handle == candidate ? "primary" : "";
                }));
                row->WidthMode = NoxUI::UISizeMode::Fill;
                row->StyleVariant = candidate == state->Picked ? "primary" : "";
                state->Rows.emplace_back(row.get(), candidate);
                listRaw->AddChild(row);
            }
            if (candidates.empty())
            {
                Ref<NoxUI::UILabel> none(new NoxUI::UILabel(state->Filter.empty() ? "No other asset of this type." : "Nothing matches the search.", 13.0f));
                listRaw->AddChild(none);
            }
        };
        search->OnChanged = [state, rebuild](const std::string& text)
        {
            state->Filter = text;
            rebuild();
        };
        rebuild();
        // Replace: clickable once something is picked (the modal checks it every frame).
        auto replace = [this, state, from]()
        {
            if (state->Picked != 0 && m_ReplaceReferences)
                m_ReplaceReferences(from, state->Picked);
        };
        NoxUI::UIModal::ShowCustom(context, "Replace References to " + fromMetadata.FilePath.filename().string(), content,
                                { { "Replace", replace, true, [state]() { return state->Picked != 0; } }, { "Cancel", {} } }, 560.0f);
    }

    void ContentBrowserPanel::MoveAssets(const std::vector<AssetHandle>& handles, const std::filesystem::path& absoluteFolder)
    {
        EditorAssetManager* assets = Assets();
        if (!assets)
            return;
        std::filesystem::path folder = absoluteFolder.lexically_relative(m_BaseDirectory);
        if (folder == ".")
            folder.clear();
        size_t moved = 0, refused = 0;
        for (AssetHandle handle : handles)
        {
            if (!assets->IsAssetHandleValid(handle))
                continue;
            if (!assets->CanRename(handle) || !assets->MoveAsset(handle, folder))
                ++refused;
            else
                ++moved;
        }
        Refresh();
        if (moved > 0)
            Notify(NoxUI::UINotificationSeverity::Success, moved == 1 ? "Asset moved" : std::to_string(moved) + " assets moved", "To " + (folder.empty() ? std::string("Assets") : folder.generic_string()));
        if (refused > 0)
            Notify(NoxUI::UINotificationSeverity::Warning, std::to_string(refused) + (refused == 1 ? " asset was not moved" : " assets were not moved"),
                   "Scenes, and a model's skeletons and clips, stay where they are.");
    }

    void ContentBrowserPanel::CreatePrefabFromDrop(uint64_t dragId, const std::filesystem::path& absoluteFolder)
    {
        if (!m_CreatePrefab)
            return;
        std::filesystem::path folder = absoluteFolder.lexically_relative(m_BaseDirectory);
        if (folder == ".")
            folder.clear();
        const AssetHandle prefab = m_CreatePrefab(dragId, folder);
        if (prefab == 0)
        {
            Notify(NoxUI::UINotificationSeverity::Error, "Prefab not created", "See the Output Log for why.");
            return;
        }
        Notify(NoxUI::UINotificationSeverity::Success, "Prefab created");
        ShowNewAsset(prefab);
    }

    void ContentBrowserPanel::NewFolder()
    {
        NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent();
        if (!ctx)
            return;
        NoxUI::UIModal::ShowPrompt(*ctx, "New Folder", "Name of the folder, in " + (CurrentFolder().empty() ? std::string("Assets") : CurrentFolder().generic_string()),
            "New Folder", "Folder name", [this](const std::string& name)
            {
                if (name.empty() || name.find_first_of("\\/:*?\"<>|") != std::string::npos)
                {
                    Notify(NoxUI::UINotificationSeverity::Error, "Folder not created", "\"" + name + "\" is not a valid folder name.");
                    return;
                }
                std::error_code error;
                const std::filesystem::path path = m_CurrentDirectory / name;
                if (std::filesystem::exists(path) || !std::filesystem::create_directory(path, error))
                {
                    Notify(NoxUI::UINotificationSeverity::Error, "Folder not created", std::filesystem::exists(path) ? "A folder of that name is already here." : error.message());
                    return;
                }
                Refresh();
            });
    }

    void ContentBrowserPanel::CreateMaterial()
    {
        const std::filesystem::path path = UniquePath(m_CurrentDirectory, "NewMaterial", ".nmat");
        MaterialData data;
        if (!MaterialSerializer::Serialize(path, data))
        {
            Notify(NoxUI::UINotificationSeverity::Error, "Material not created", path.generic_string());
            return;
        }
        AdoptNewFile(path);
    }

    void ContentBrowserPanel::CreateGraph(int kind)
    {
        static constexpr const char* kStems[] = { "NewAnimationGraph", "NewAudioGraph", "NewMaterialGraph" };
        static constexpr const char* kExtensions[] = { ".nanimgraph", ".naudiograph", ".nmatgraph" };
        kind = std::clamp(kind, 0, 2);
        const std::filesystem::path path = UniquePath(m_CurrentDirectory, kStems[kind], kExtensions[kind]);
        const bool written = NodeGraphSerializer::Serialize(path, kind == 1 ? CreateEmptyAudioGraph() : kind == 2 ? CreateEmptyMaterialGraph() : CreateEmptyAnimationGraph());
        if (!written)
        {
            Notify(NoxUI::UINotificationSeverity::Error, "Graph not created", path.generic_string());
            return;
        }
        AdoptNewFile(path);
    }

    void ContentBrowserPanel::CreateMaterialShader()
    {
        const std::filesystem::path path = UniquePath(m_CurrentDirectory, "NewMaterialShader", ".slang");
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        const std::string_view source = CustomLitShaders::TemplateSource();
        if (!file || !file.write(source.data(), static_cast<std::streamsize>(source.size())))
        {
            Notify(NoxUI::UINotificationSeverity::Error, "Material Shader not created", path.generic_string());
            return;
        }
        file.close();
        AdoptNewFile(path);
    }

    void ContentBrowserPanel::CreateTimeline()
    {
        const std::filesystem::path path = UniquePath(m_CurrentDirectory, "NewTimeline", ".ntimeline");
        Timeline timeline;
        if (!TimelineImporter::Serialize(path, timeline))
        {
            Notify(NoxUI::UINotificationSeverity::Error, "Timeline not created", path.generic_string());
            return;
        }
        AdoptNewFile(path);
    }

    void ContentBrowserPanel::CreateVfx()
    {
        const std::filesystem::path path = UniquePath(m_CurrentDirectory, "NewVfx", ".nvfx");
        VfxAsset vfx;
        vfx.LoadPreset("Teleport");
        vfx.Name = path.stem().string();
        if (!vfx.Save(path))
        {
            Notify(NoxUI::UINotificationSeverity::Error, "VFX System not created", path.generic_string());
            return;
        }
        AdoptNewFile(path);
    }

    void ContentBrowserPanel::CreateScript()
    {
        NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent();
        if (!ctx || !Project::GetActive())
            return;
        NoxUI::UIModal::ShowPrompt(*ctx, "New C# Script", "Class name (letters, digits and _). The file goes to Scripts/Source and opens in your code editor.",
            "NewBehaviour", "ClassName", [this](const std::string& className)
            {
                const bool valid = !className.empty() && (std::isalpha(static_cast<unsigned char>(className[0])) || className[0] == '_') &&
                    std::all_of(className.begin() + 1, className.end(), [](unsigned char ch) { return std::isalnum(ch) || ch == '_'; });
                if (!valid)
                {
                    Notify(NoxUI::UINotificationSeverity::Error, "Script not created", "\"" + className + "\" is not a valid C# class name.");
                    return;
                }
                std::string scriptNamespace = Project::GetActive()->GetConfig().Name;
                std::replace_if(scriptNamespace.begin(), scriptNamespace.end(), [](unsigned char ch) { return !std::isalnum(ch) && ch != '_'; }, '_');
                if (scriptNamespace.empty())
                    scriptNamespace = "Game";
                if (std::isdigit(static_cast<unsigned char>(scriptNamespace.front())))
                    scriptNamespace.insert(scriptNamespace.begin(), '_');

                const std::filesystem::path sourceDirectory = m_BaseDirectory / "Scripts/Source";
                const std::filesystem::path scriptPath = sourceDirectory / (className + ".cs");
                if (std::filesystem::exists(scriptPath))
                {
                    Notify(NoxUI::UINotificationSeverity::Error, "Script not created", "Scripts/Source/" + className + ".cs already exists.");
                    return;
                }
                const std::filesystem::path projectTemplate = m_BaseDirectory / "Scripts/Templates/CSharpBehaviour.cs.template";
                const std::filesystem::path editorTemplate = std::filesystem::path(Application::GetExecutableRootPath()) / "assets/Templates/CSharpBehaviour.cs.template";
                const std::filesystem::path templatePath = std::filesystem::exists(projectTemplate) ? projectTemplate
                    : std::filesystem::exists(editorTemplate) ? editorTemplate : std::filesystem::path("assets/Templates/CSharpBehaviour.cs.template");
                std::ifstream templateFile(templatePath, std::ios::binary);
                if (!templateFile)
                {
                    Notify(NoxUI::UINotificationSeverity::Error, "Script not created", "The template is missing: " + templatePath.generic_string());
                    return;
                }
                std::string contents((std::istreambuf_iterator<char>(templateFile)), std::istreambuf_iterator<char>());
                auto replaceAll = [&contents](std::string_view token, std::string_view replacement)
                {
                    for (size_t position = 0; (position = contents.find(token, position)) != std::string::npos; position += replacement.size())
                        contents.replace(position, token.size(), replacement);
                };
                replaceAll("{{NAMESPACE}}", scriptNamespace);
                replaceAll("{{CLASS_NAME}}", className);
                std::error_code error;
                std::filesystem::create_directories(sourceDirectory, error);
                std::ofstream output(scriptPath, std::ios::binary);
                if (!output || !output.write(contents.data(), static_cast<std::streamsize>(contents.size())))
                {
                    Notify(NoxUI::UINotificationSeverity::Error, "Script not created", scriptPath.generic_string());
                    return;
                }
                output.close();
                Notify(NoxUI::UINotificationSeverity::Success, "C# script created", "Scripts/Source/" + className + ".cs");
                NavigateTo(sourceDirectory);
                const std::string url = "file:///" + scriptPath.generic_string();
                SDL_OpenURL(url.c_str());
            });
    }

    void ContentBrowserPanel::PickAndImport()
    {
        // SDL's dialog takes extensions without "*." (it refuses a filter that has them).
        static constexpr char filter[] = "Models, textures, sounds, fonts, scenes\0gltf;glb;vox;png;jpg;jpeg;tga;hdr;wav;ogg;flac;mp3;ttf;otf;nox\0";
        Utility::OpenFileAsync(filter, [this](std::string file) // (the editor keeps running while the dialog is open)
        {
            if (!file.empty())
                ImportFile(file);
        });
    }

    void ContentBrowserPanel::ShowInExplorer(const std::filesystem::path& absolutePath)
    {
        const std::filesystem::path folder = std::filesystem::is_directory(absolutePath) ? absolutePath : absolutePath.parent_path();
        const std::string url = "file:///" + folder.generic_string();
        SDL_OpenURL(url.c_str());
    }

    void ContentBrowserPanel::ImportFile(const std::filesystem::path& path)
    {
        EditorAssetManager* assets = Assets();
        if (!assets || !std::filesystem::exists(path))
            return;
        std::error_code error;
        const std::filesystem::path assetRoot = std::filesystem::weakly_canonical(m_BaseDirectory, error);
        const std::filesystem::path source = std::filesystem::weakly_canonical(path, error);
        if (error)
            return;
        const std::filesystem::path relativeToRoot = source.lexically_relative(assetRoot);
        const bool insideProject = !relativeToRoot.empty() && relativeToRoot != "." && relativeToRoot.generic_string().rfind("..", 0) != 0;
        const std::string extension = ToLower(path.extension().string());

        // A model (glTF, MagicaVoxel .vox): the import dialog (the files are copied only when it is confirmed, so Cancel leaves nothing behind).
        if (extension == ".gltf" || extension == ".glb" || extension == ".vox")
        {
            if (insideProject)
                OpenModelImportDialog(relativeToRoot, {});
            else
                OpenModelImportDialog(CurrentFolder() / path.stem() / path.filename(), source);
            return;
        }

        const AssetType type = EditorAssetManager::GetAssetTypeFromExtension(extension);
        if (type == AssetType::None)
        {
            Notify(NoxUI::UINotificationSeverity::Warning, "Not imported", path.filename().string() + " is not a kind of file the engine reads.");
            return;
        }
        std::filesystem::path relative = relativeToRoot;
        if (!insideProject)
        {
            const std::filesystem::path destination = m_CurrentDirectory / path.filename();
            std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, error);
            if (error)
            {
                Notify(NoxUI::UINotificationSeverity::Error, "Import failed", error.message());
                return;
            }
            relative = destination.lexically_relative(m_BaseDirectory);
        }
        relative = relative.lexically_normal();
        if (type == AssetType::Scene)
            assets->ImportAsset(relative, relative, AssetType::Scene);
        else if (type == AssetType::Texture2D)
            assets->ScanAndRegisterNewAssets(relative.parent_path());
        else if (!assets->FindHandleByPath(relative, type))
            assets->RegisterExistingFile(relative, type);
        Refresh();
        Notify(NoxUI::UINotificationSeverity::Success, "Imported " + path.filename().string(), "Into " + (relative.parent_path().empty() ? std::string("Assets") : relative.parent_path().generic_string()));
    }

    void ContentBrowserPanel::OpenModelImportDialog(const std::filesystem::path& relativeSource, const std::filesystem::path& externalSource)
    {
        // A file dropped from the system or picked in the file dialog arrives between frames, when no UI context is current: the
        // editor's own (the UI layer's) then, as its other dialogs opened from events do. (It returned without a word: dropping a model
        // onto the editor did nothing.)
        NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent();
        if (!ctx)
            if (UILayer* uiLayer = Application::Get().GetUILayer())
                ctx = &uiLayer->GetContext();
        if (!ctx)
        {
            NOX_CORE_WARN("ContentBrowserPanel: no UI to show the import dialog of {} in", relativeSource.generic_string());
            return;
        }

        // Unreal's Interchange options, the ones a glTF needs: what to import, how to combine it, its scale, where it goes.
        struct ImportState
        {
            MeshImportSettings Settings;
            std::string FileName;
        };
        auto state = std::make_shared<ImportState>();
        state->FileName = relativeSource.stem().string() + ".nmesh";
        // A MagicaVoxel file: static meshes only, sized by the voxel size (VoxImporter).
        const bool vox = VoxImporter::IsVoxFile(relativeSource);
        float voxelCentimeters = 10.0f;
        if (vox)
        {
            state->FileName = relativeSource.stem().string() + ".nsmesh";
            state->Settings.ImportSkeletalMeshes = false;
            state->Settings.ImportAnimations = false;
            state->Settings.StaticCombine = MeshCombineMode::CombineAll;
            state->Settings.ImportScale = WorldUnits::FromCentimeters(voxelCentimeters);
        }

        Ref<NoxUI::UIStackBox> content(new NoxUI::UIStackBox(NoxUI::UIOrientation::Vertical, 8.0f));
        auto note = [&content](const std::string& text)
        {
            Ref<NoxUI::UILabel> label(new NoxUI::UILabel(text, 13.5f));
            label->WordWrap = true;
            label->WidthMode = NoxUI::UISizeMode::Fill;
            label->UseColorOverride = true;
            label->ColorOverride = NoxUI::ThemeColor("TextMuted", { 0.62f, 0.66f, 0.74f, 1.0f });
            content->AddChild(label);
        };
        auto row = [&content](const std::string& name, const Ref<NoxUI::UIElement>& editor)
        {
            Ref<NoxUI::UIStackBox> line(new NoxUI::UIStackBox(NoxUI::UIOrientation::Horizontal, 8.0f));
            line->WidthMode = NoxUI::UISizeMode::Fill;
            line->VAlign = NoxUI::UIAlignment::Center;
            Ref<NoxUI::UILabel> label(new NoxUI::UILabel(name, 14.5f));
            label->WidthMode = NoxUI::UISizeMode::Fixed;
            label->FixedSize = { 170.0f, 22.0f };
            label->VAlign = NoxUI::UIAlignment::Center;
            line->AddChild(label);
            editor->WidthMode = NoxUI::UISizeMode::Fill;
            line->AddChild(editor);
            content->AddChild(line);
        };
        auto check = [&row](const std::string& name, bool& value, const std::string& tooltip)
        {
            Ref<NoxUI::UICheckBox> box(new NoxUI::UICheckBox("", value, [&value](bool on) { value = on; }));
            box->Tooltip = tooltip;
            row(name, box);
        };
        auto combine = [&row, ctx](const std::string& name, MeshCombineMode& mode, const std::string& tooltip)
        {
            Ref<NoxUI::UIDropdown> dropdown(new NoxUI::UIDropdown(ctx, { "Do Not Combine", "Combine Visible", "Combine All" }, static_cast<int>(mode),
                [&mode](int index) { mode = static_cast<MeshCombineMode>(index); }));
            dropdown->Tooltip = tooltip;
            row(name, dropdown);
        };

        if (vox)
        {
            // What the file holds (read now; small), so the voxel size can be judged.
            const std::filesystem::path absolute = externalSource.empty() ? m_BaseDirectory / relativeSource : externalSource;
            const MeshImporter::GltfContent voxContent = MeshImporter::InspectGltf(absolute);
            if (!voxContent.HasStaticMeshes)
            {
                Notify(NoxUI::UINotificationSeverity::Error, "Not imported", relativeSource.filename().string() + " has no visible voxels or cannot be read -- see the Output Log.");
                return;
            }
            const glm::vec3 size = voxContent.BoundsMax - voxContent.BoundsMin;
            note(std::format("Importing {} (MagicaVoxel): {} {}, {:.0f} x {:.0f} x {:.0f} voxels (width x height x depth). Colours, roughness, metal, "
                             "glass and emission come from its palette.", relativeSource.filename().string(), voxContent.StaticMeshCount,
                             voxContent.StaticMeshCount == 1 ? "model" : "models", size.x, size.y, size.z));

            auto sizeNote = std::make_shared<Ref<NoxUI::UILabel>>();
            auto describeSize = [size](float centimeters)
            {
                return std::format("Size in the world: {} x {} x {}", WorldUnits::Format(WorldUnits::FromCentimeters(size.x * centimeters)),
                                   WorldUnits::Format(WorldUnits::FromCentimeters(size.y * centimeters)),
                                   WorldUnits::Format(WorldUnits::FromCentimeters(size.z * centimeters)));
            };
            Ref<NoxUI::UIDragScalar> voxelSize(new NoxUI::UIDragScalar(voxelCentimeters, 0.1f, [state, sizeNote, describeSize](float centimeters)
            {
                state->Settings.ImportScale = WorldUnits::FromCentimeters(centimeters);
                if (*sizeNote)
                    (*sizeNote)->Text = describeSize(centimeters);
            }));
            voxelSize->Min = 0.5f;
            voxelSize->Max = 100.0f;
            voxelSize->Format = "{:.1f}";
            voxelSize->TrimZeros = true;
            voxelSize->Tooltip = "The edge of one voxel in the world. 10 cm is the voxel plan's default; 100 cm makes Minecraft-sized blocks.";
            row("Voxel Size (cm)", voxelSize);
            Ref<NoxUI::UILabel> sizeLabel(new NoxUI::UILabel(describeSize(voxelCentimeters), 13.5f));
            sizeLabel->WidthMode = NoxUI::UISizeMode::Fill;
            sizeLabel->UseColorOverride = true;
            sizeLabel->ColorOverride = NoxUI::ThemeColor("TextMuted", { 0.62f, 0.66f, 0.74f, 1.0f });
            content->AddChild(sizeLabel);
            *sizeNote = sizeLabel;

            Ref<NoxUI::UIDropdown> pivot(new NoxUI::UIDropdown(ctx, { "Bottom Center", "Center", "File Origin" }, static_cast<int>(state->Settings.VoxelOrigin),
                [state](int index) { state->Settings.VoxelOrigin = static_cast<VoxelPivot>(index); }));
            pivot->Tooltip = "Where the mesh's origin sits. Bottom Center: it stands on the ground where you drop it. File Origin: MagicaVoxel's own "
                             "coordinates (pieces built to line up keep lining up).";
            row("Pivot", pivot);
            check("Merge Faces", state->Settings.VoxelMergeFaces, "Faces of one colour in one plane merged into rectangles (far fewer triangles). "
                                                              "Off: one quad per voxel face.");
            combine("Combine Models", state->Settings.StaticCombine, "A scene with several models: one mesh with them all, placed as in MagicaVoxel "
                                                                     "(Combine), or a mesh per model (Do Not Combine).");
        }
        else
        {
        note("Importing " + relativeSource.filename().string() + ". Skinned meshes become a skeletal mesh with its skeleton and clips, the rest static meshes.");
        check("Import Static Meshes", state->Settings.ImportStaticMeshes, "Meshes that are not skinned (level geometry, props).");
        combine("Combine Static Meshes", state->Settings.StaticCombine, "Merge static meshes into one per material, node transforms baked in: fewer draw calls.");
        check("Import Skeletal Meshes", state->Settings.ImportSkeletalMeshes, "Skinned meshes and their skeleton.");
        combine("Combine Skeletal Meshes", state->Settings.SkeletalCombine, "Merge skinned meshes that share a skeleton, in bind pose.");
        check("Import Animations", state->Settings.ImportAnimations, "The file's animation clips.");

        Ref<NoxUI::UIDragScalar> scale(new NoxUI::UIDragScalar(state->Settings.ImportScale, 0.01f, [state](float value) { state->Settings.ImportScale = value; }));
        scale->Min = 0.0001f;
        scale->Max = 10000.0f;
        scale->Format = "{:.4f}";
        scale->TrimZeros = true;
        scale->Tooltip = "glTF is in meters; a model authored in centimetres claims meters too and comes in 100x too big: 0.01 fixes it.";
        row("Import Scale", scale);
        }

        Ref<NoxUI::UITextInput> name(new NoxUI::UITextInput(state->FileName, "File name"));
        name->OnChanged = [state](const std::string& text) { state->FileName = text; };
        row("File name", name);
        note("Goes to " + (relativeSource.parent_path() / "Meshes").generic_string() + ". Cooking runs in the background: the status bar shows it.");

        std::vector<NoxUI::UIModal::Button> buttons;
        buttons.push_back({ "Cook & Import", [this, state, relativeSource, externalSource]()
        {
            EditorAssetManager* assets = Assets();
            if (!assets)
                return;
            if (!state->Settings.ImportStaticMeshes && !state->Settings.ImportSkeletalMeshes)
            {
                Notify(NoxUI::UINotificationSeverity::Warning, "Nothing imported", "Turn on static or skeletal meshes.");
                return;
            }
            if (!externalSource.empty())
            {
                // The file and what sits beside it (its .bin, its textures) come into a folder of their own; a .vox holds everything
                // itself, so only it is copied.
                std::error_code error;
                const std::filesystem::path packageDirectory = m_BaseDirectory / relativeSource.parent_path();
                std::filesystem::create_directories(packageDirectory, error);
                if (VoxImporter::IsVoxFile(externalSource))
                    std::filesystem::copy_file(externalSource, packageDirectory / externalSource.filename(), std::filesystem::copy_options::overwrite_existing, error);
                else
                {
                    for (const auto& dependency : std::filesystem::directory_iterator(externalSource.parent_path(), error))
                    {
                        std::filesystem::copy(dependency.path(), packageDirectory / dependency.path().filename(),
                                              std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, error);
                        if (error)
                            break;
                    }
                }
                if (error)
                {
                    Notify(NoxUI::UINotificationSeverity::Error, "Import failed", "Could not copy the files: " + error.message());
                    return;
                }
            }
            const char* defaultExtension = VoxImporter::IsVoxFile(relativeSource) ? ".nsmesh" : ".nmesh";
            std::string fileName = state->FileName.empty() ? relativeSource.stem().string() + defaultExtension : state->FileName;
            if (std::filesystem::path(fileName).extension().empty())
                fileName += defaultExtension;
            const std::filesystem::path destination = relativeSource.parent_path() / "Meshes" / fileName;
            if (!assets->ImportModel(relativeSource, destination, state->Settings))
            {
                Notify(NoxUI::UINotificationSeverity::Error, "Import failed", relativeSource.filename().string() + " -- see the Output Log.");
                return;
            }
            Notify(NoxUI::UINotificationSeverity::Info, "Importing " + relativeSource.filename().string(), "Cooking in the background; the status bar shows its progress.");
            m_ImportsWereRunning = true;
            Refresh();
        }, true });
        buttons.push_back({ "Cancel", {}, false });
        NoxUI::UIModal::ShowCustom(*ctx, vox ? "Import Voxel Model" : "Import Model", content, std::move(buttons), 520.0f);
    }

    std::vector<NoxUI::UIMenuItem> ContentBrowserPanel::BuildAddMenu()
    {
        std::vector<NoxUI::UIMenuItem> items;
        items.push_back({ "New Folder", "", [this]() { NewFolder(); } });
        items.push_back({ "", "", {}, true });
        items.push_back({ "Material", "", [this]() { CreateMaterial(); }, false, true, false, false,
                          "A surface: base color, metallic, roughness, textures. Double-click it to edit it." });
        items.push_back({ "Material Graph", "", [this]() { CreateGraph(2); }, false, true, false, false,
                          "A surface built from nodes (Unreal's Material Editor); materials use it as their Surface Graph." });
        items.push_back({ "Material Shader", "", [this]() { CreateMaterialShader(); }, false, true, false, false,
                          "A Custom Lit material's lighting in Slang: light() for each light, ambient() once." });
        items.push_back({ "", "", {}, true });
        items.push_back({ "Animation Graph", "", [this]() { CreateGraph(0); }, false, true, false, false,
                          "A character's animation logic: state machines, blend spaces, IK." });
        items.push_back({ "Audio Graph", "", [this]() { CreateGraph(1); }, false, true, false, false,
                          "A procedural sound (MetaSounds' Source), played by an Audio Source like a clip." });
        items.push_back({ "Timeline", "", [this]() { CreateTimeline(); }, false, true, false, false,
                          "Keyed positions, lights, camera and events over time, played by a Timeline Player." });
        items.push_back({ "VFX System", "", [this]() { CreateVfx(); }, false, true, false, false,
                          "Multi-layer GPU VFX asset (.nvfx): particles, trails, beams, decals, distortion, volumes, fog, lights." });
        items.push_back({ "", "", {}, true });
        items.push_back({ "C# Script", "", [this]() { CreateScript(); }, false, true, false, false,
                          "A behaviour class from the template, in Scripts/Source." });
        return items;
    }

    std::vector<NoxUI::UIMenuItem> ContentBrowserPanel::BuildFolderSpaceMenu()
    {
        std::vector<NoxUI::UIMenuItem> items = BuildAddMenu();
        items.push_back({ "", "", {}, true });
        items.push_back({ "Import...", "", [this]() { PickAndImport(); } });
        items.push_back({ "Show in Explorer", "", [this]() { ShowInExplorer(m_CurrentDirectory); } });
        return items;
    }

    std::vector<NoxUI::UIMenuItem> ContentBrowserPanel::BuildAssetMenu(const ContentEntry& entry)
    {
        std::vector<NoxUI::UIMenuItem> items;
        if (entry.IsDirectory)
        {
            items.push_back({ "Open", "", [this, path = entry.Path]() { NavigateTo(path); } });
            items.push_back({ "Show in Explorer", "", [this, path = entry.Path]() { ShowInExplorer(path); } });
            return items;
        }
        if (entry.IsScript)
        {
            items.push_back({ "Open in Code Editor", "", [entry]()
            {
                const std::string url = "file:///" + entry.Path.generic_string();
                SDL_OpenURL(url.c_str());
            } });
            items.push_back({ "Show in Explorer", "", [this, path = entry.Path]() { ShowInExplorer(path); } });
            return items;
        }

        EditorAssetManager* assets = Assets();
        const AssetHandle handle = entry.Handle;
        const size_t selected = m_Tiles ? m_Tiles->m_Selected.size() : 1;
        const bool canRename = assets && assets->CanRename(handle);
        const AssetMetadata metadata = assets ? assets->GetMetadata(handle) : AssetMetadata{};
        const bool hasSource = !metadata.SourceFilePath.empty() && metadata.SourceFilePath != metadata.FilePath;

        items.push_back({ entry.Type == AssetType::MeshSource ? "Import..." : "Open", "Enter", [this, entry]() { Activate(entry); } });
        items.push_back({ "", "", {}, true });
        items.push_back({ "Rename", "F2", [this, handle]() { BeginRename(handle); }, false, selected == 1 && canRename });
        items.push_back({ "Duplicate", "", [this, handle]() { Duplicate(handle); }, false, selected == 1 && entry.Type != AssetType::MeshSource });
        items.push_back({ selected > 1 ? "Delete Selected..." : "Delete...", "Del", [this, handle]()
        {
            std::vector<AssetHandle> handles;
            if (m_Tiles && m_Tiles->m_Selected.contains(handle))
                handles.assign(m_Tiles->m_Selected.begin(), m_Tiles->m_Selected.end());
            else
                handles.push_back(handle);
            RequestDelete(std::move(handles));
        } });
        items.push_back({ "", "", {}, true });
        if (entry.Type == AssetType::Prefab)
            items.push_back({ "Create Variant", "", [this, handle]() { CreateVariant(handle); }, false, static_cast<bool>(m_CreateVariant), false, false,
                              "A prefab of its own that is this one plus its changes: what the base gets later, the variant gets too." });
        items.push_back({ "Find References", "", [this, handle]() { ShowReferences(handle); } });
        items.push_back({ "Replace References...", "", [this, handle]() { StartReferenceCheck(ReferenceCheckMode::Replace, { handle }); }, false,
                          selected == 1 && static_cast<bool>(m_ReplaceReferences) && entry.Type != AssetType::Scene, false, false,
                          "Everything that uses this asset uses another of its type instead (the asset itself stays)." });
        items.push_back({ "Reimport", "", [this, handle]()
        {
            if (EditorAssetManager* manager = Assets())
            {
                manager->ReimportAsset(handle, true);
                AssetPreviewRenderer::Get().Invalidate(handle);
                Notify(NoxUI::UINotificationSeverity::Info, "Reimporting", manager->GetMetadata(handle).FilePath.filename().string());
            }
        }, false, hasSource || entry.Type == AssetType::Texture2D });
        items.push_back({ "", "", {}, true });
        items.push_back({ "Copy Path", "", [this, path = entry.Path]()
        {
            const std::string relative = path.lexically_relative(m_BaseDirectory).generic_string();
            SDL_SetClipboardText(relative.c_str());
        }, false, true, false, false, "The asset's path in the project (Assets/...), as scripts name it." });
        items.push_back({ "Show in Explorer", "", [this, path = entry.Path]() { ShowInExplorer(path); } });
        return items;
    }
}
