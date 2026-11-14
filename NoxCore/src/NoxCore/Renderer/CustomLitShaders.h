#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Custom Lit (MaterialShadingModel::CustomLit; docs/Stylized_Rendering_And_Material_Graph_Plan_2026-11.md step 3): the project's Material
// Shaders -- .slang files in its Assets, each with a light() and an ambient() function (NoxEditor/assets/shaders/CustomLit.slang) -- as the
// renderer compiles them. Every Material Shader goes into one generated file, each in a namespace of its own, with two functions that call
// the one a pixel's material names by its slot (custom_lit_light / custom_lit_ambient). Two small generated files then compile the
// deferred lighting and the forward shader with them (NOX_CUSTOM_LIT): the Custom Lit pass and the forward passes' Custom Lit variant.
//
// The generated files live next to the executable, in shaders/Generated (beside the precompiled binaries, so a packaged game ships them
// and finds their binaries by the same keys); a manifest there lists the Material Shaders in slot order, which is how a packaged game --
// that cannot read the sources or compile -- learns each one's slot.
namespace Nox
{
    // A Material Shader: its path in the project's Assets (generic separators; what MaterialData::CustomShader names) and its code.
    struct CustomLitSource
    {
        std::string Path;
        std::string Code;
        // The file on disk (absolute; empty: Path): what the #line before its code names, so a compile error names the real file and
        // the compiler can show its line (a path it cannot open made Slang quote the generated file's line instead).
        std::string File;
    };

    namespace CustomLitShaders
    {
        // The code a new Material Shader starts with (Content Browser > + Add > Material Shader): a two-tone toon with a rim.
        std::string_view TemplateSource();

        // <executable folder>/shaders/Generated, and the two pipelines' sources in it.
        std::filesystem::path GeneratedFolder();
        std::filesystem::path DeferredSourcePath(); // DeferredLightingCustomLit.slang
        std::filesystem::path ForwardSourcePath();  // TransparentLitCustomLit.slang
        // A small file of the Material Shaders alone (with an entry point that calls them): compiled first, in a fraction of the
        // pipelines' time, so an error is reported as one -- a hot reload that failed would otherwise keep the last working binary.
        std::filesystem::path CheckSourcePath();    // CustomLitCheck.slang

        // The generated file's text for these Material Shaders, `paths[slot]` naming each one's slot (a path with no source here: its
        // case left out, it lights nothing).
        std::string GenerateDispatcher(const std::vector<std::string>& paths, const std::vector<CustomLitSource>& sources);

        // Writes the generated files and the manifest for `paths` (slot order) where their text changed. Returns whether anything was
        // written (the pipelines must be built again); throws nothing (a write that fails is logged and returns false).
        bool WriteGenerated(const std::vector<std::string>& paths, const std::vector<CustomLitSource>& sources);

        // <project>/slangdconfig.json for the Slang language server (VS Code's and Visual Studio's Slang extensions): the engine's shader
        // folder as a search path, so a Material Shader's `#include "CustomLit.slang"` resolves there (the generated file blanks that
        // line: it has the include already). Absolute, so per machine; written when the editor opens a project. False if not written.
        bool WriteCodeEditorConfig(const std::filesystem::path& projectDirectory, const std::filesystem::path& engineShaderDirectory);

        // The manifest's paths in slot order (empty: none written).
        std::vector<std::string> ReadManifest();

        // The active project's Material Shaders read from its Assets folder, sorted by path. `complete` says whether every one registered
        // was read: false in a packaged game (its assets are in the pack), which then uses the manifest and the binaries shipped with it.
        std::vector<CustomLitSource> GatherProjectSources(bool& complete);

        // The slot order for `sources` that keeps the slots `previous` gave (a material's packed slot stays valid while the editor runs):
        // previous paths still present keep their place, new ones are appended, removed ones leave an empty slot.
        std::vector<std::string> AssignSlots(const std::vector<std::string>& previous, const std::vector<CustomLitSource>& sources);
    }
}
