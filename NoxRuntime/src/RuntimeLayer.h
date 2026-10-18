#pragma once
#include <filesystem>

#include "NoxCore/Core/Layer.h"
#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/Ref.h"

namespace Nox
{
    class Renderer;
    class Scene;

    // Plays a project's start scene the way the editor's Play button does (a copy of the scene asset, OnRuntimeStart,
    // OnUpdateRuntime every frame), without the editor around it: no panels, no editor camera, no picking, no undo snapshots,
    // no file watching. For testing a game at its real cost and as the base of the packaged player (R7).
    class RuntimeLayer : public Layer
    {
    public:
        RuntimeLayer();
        ~RuntimeLayer() override;

        void OnUpdate(Timestep ts) override;

    private:
        // The project to play: the first command line argument, else the first .nproj in the working directory, next to the
        // executable, or in ../Facerun (the sample project, from the editor's working directory).
        std::filesystem::path FindProjectFile() const;
        void StartScene(AssetHandle sceneHandle);
        void UnloadUnusedAssets();

    private:
        Renderer* m_Renderer = nullptr;
        Ref<Scene> m_Scene;
        bool m_UnloadUnusedAssetsRequested = false;
    };
}
