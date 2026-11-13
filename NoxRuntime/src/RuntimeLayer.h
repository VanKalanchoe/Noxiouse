#pragma once
#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "NoxCore/Core/Layer.h"
#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/Network/NetProtocol.h"
#include "NoxCore/Network/NetSession.h"

namespace Nox
{
    class Renderer;
    class Scene;
    namespace Net
    {
        class NetServer;
        class NetClient;
        class NetReplication;
    }

    // Plays a project's start scene the way the editor's Play button does (a copy of the scene asset, OnRuntimeStart,
    // OnUpdateRuntime every frame), without the editor around it: no panels, no editor camera, no picking, no undo snapshots,
    // no file watching. For testing a game at its real cost and as the base of the packaged player (R7). Headless (--server): the same
    // game as a dedicated server -- no rendering, a status line in the log every 10 s and a console on stdin.
    //
    // Networking (docs/Networking_Plan_2026-11.md, NET1; Unreal's net modes): Standalone; DedicatedServer (--server) and
    // ListenServer (--listen) accept clients on --port (7777); Client (--connect=<address>) starts on the project's start scene,
    // joins the server and plays the scene the server plays, and goes back to the start scene when the session ends. The frame:
    // receive (UpdateNetworking) -> the scene's update -> send (FlushNetworking). NET2 replicates the game through these sessions.
    class RuntimeLayer : public Layer
    {
    public:
        RuntimeLayer();
        ~RuntimeLayer() override;

        void OnUpdate(Timestep ts) override;
        void OnEvent(Event& event) override;

    private:
        enum class NetRole : uint8_t { Standalone, DedicatedServer, ListenServer, Client };

        // The project to play: the first command line argument, else the first .nproj in the working directory, next to the
        // executable, in Game/ next to it (a packaged game), or in ../Facerun (the sample project, from the editor's working
        // directory).
        std::filesystem::path FindProjectFile() const;
        void StartScene(AssetHandle sceneHandle);
        void UnloadUnusedAssets();
        // Headless: the console's lines (a thread reads stdin) and the periodic status line.
        void UpdateServerConsole(float ts);
        void RunConsoleCommand(const std::string& line);
        void LogServerStatus();
        void LogClients() const;

        // Networking: the role and sessions from the command line (before the scene starts: its scripts see their role in OnCreate);
        // receive / send around the frame; the client's session events.
        void DetermineNetRole();
        void StartNetworking();
        void UpdateNetworking(float ts);
        void FlushNetworking();
        bool OnJoinedServer(AssetHandle scene);
        void OnLeftServer(Net::DisconnectReason reason, const std::string& text, bool wasJoined);
        Net::NetServer* Server() const { return m_Session ? m_Session->GetServer() : nullptr; }
        Net::NetReplication* Replication() const { return m_Session ? m_Session->GetReplication() : nullptr; }
        // The window title shows the session (the project's name, then the state).
        void UpdateWindowTitle();

    private:
        Renderer* m_Renderer = nullptr; // null while headless
        Ref<Scene> m_Scene;
        AssetHandle m_SceneHandle = 0;      // the scene asset m_Scene is a copy of
        AssetHandle m_StartSceneHandle = 0; // the project's start scene (a client's scene outside a session)
        bool m_UnloadUnusedAssetsRequested = false;
        std::chrono::steady_clock::time_point m_LastUnloadSweep{}; // the sweep runs at most once a second

        // Headless server (the console's lines are read on a thread of their own; shared with it, so it may outlive the layer).
        struct ConsoleInput
        {
            std::mutex Mutex;
            std::deque<std::string> Lines;
        };
        bool m_Headless = false;
        std::shared_ptr<ConsoleInput> m_Console;
        float m_StatusTimer = 0.0f;
        uint32_t m_FramesSinceStatus = 0;
        double m_WorkSinceStatus = 0.0;      // seconds worked by the updates since the last status line
        double m_WorstWorkSinceStatus = 0.0;
        uint64_t m_TickAtStatus = 0;

        // Networking.
        NetRole m_NetRole = NetRole::Standalone;
        // The session (Net::NetSession, NET6): its server or client and the scene's replication -- a server's from the start, a
        // client's while it is joined (its scene is the server's).
        std::unique_ptr<Net::NetSession> m_Session;
        std::string m_ProjectName;
        float m_TitleTimer = 0.0f;
    };
}
