#include "SceneLoader.h"

#include <chrono>

#include <yaml-cpp/yaml.h>

#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/VirtualFileSystem.h"
#include "Entity.h"
#include "Scene.h"
#include "SceneSerializer.h"

namespace Nox
{
    // Written by the reading job only before it completes (m_Read ready), read by the main thread only after.
    struct SceneLoader::Parsed
    {
        std::string Bytes;      // a runtime scene's bytes (read whole on the main thread)
        bool Runtime = false;
        bool Ok = false;
        std::string Error;
        double ReadMs = 0.0;    // the file read and parsed
        YAML::Node Document;
        YAML::Node Entities;    // the document's entity sequence
        // A world cell built on the worker too (into a scene of its own): its entities, in file order.
        bool Built = false;
        double BuildMs = 0.0;
        std::vector<UUID> Created;
    };

    namespace
    {
        // One entity of a document into a scene: the main thread's slices and a cell's worker alike. A cell's entity already in the
        // scene is skipped (the editor's own copy of one saved into a cell it then loads).
        void CreateDocumentEntity(Scene& scene, const YAML::Node& entity, AssetHandle level, bool cell, const std::filesystem::path& path,
                                  std::vector<UUID>& created)
        {
            if (cell)
            {
                if (Entity existing = scene.GetEntityByUUID(entity["Entity"].as<uint64_t>()))
                {
                    NOX_CORE_WARN("World cell `{}`: entity '{}' ({}) is in the scene already, kept as it is", path.filename().string(),
                                  existing.GetName(), entity["Entity"].as<uint64_t>());
                    return;
                }
            }
            std::string name;
            if (const YAML::Node tag = entity["TagComponent"])
                name = tag["Tag"].as<std::string>();
            Entity made = scene.CreateEntityWithUUID(entity["Entity"].as<uint64_t>(), name);
            SceneSerializer::DeserializeEntityComponents(scene, entity, made, false);
            if (level != 0)
            {
                made.AddComponent<LevelComponent>().Level = level;
                created.push_back(made.GetUUID());
            }
        }
    }

    SceneLoader::SceneLoader(std::filesystem::path path)
        : m_Path(std::move(path)), m_Parsed(std::make_shared<Parsed>()), m_Scene(CreateRef<Scene>())
    {
        m_Into = m_Scene.get();
        StartReading({});
    }

    SceneLoader::SceneLoader(std::filesystem::path path, Scene& into, AssetHandle level, bool cell)
        : m_Path(std::move(path)), m_Parsed(std::make_shared<Parsed>()), m_Into(&into), m_Level(level), m_Cell(cell)
    {
        StartReading({});
    }

    SceneLoader::SceneLoader(std::string name, std::string text, Scene& into, AssetHandle level, bool cell)
        : m_Path(std::move(name)), m_Parsed(std::make_shared<Parsed>()), m_Into(&into), m_Level(level), m_Cell(cell)
    {
        StartReading(std::move(text));
    }

    SceneLoader::SceneLoader(std::filesystem::path path, const Ref<Scene>& cellScene, AssetHandle cell)
        : m_Path(std::move(path)), m_Parsed(std::make_shared<Parsed>()), m_Into(cellScene.get()), m_Level(cell), m_Cell(true)
    {
        StartReading({}, cellScene);
    }

    SceneLoader::SceneLoader(std::string name, std::string text, const Ref<Scene>& cellScene, AssetHandle cell)
        : m_Path(std::move(name)), m_Parsed(std::make_shared<Parsed>()), m_Into(cellScene.get()), m_Level(cell), m_Cell(true)
    {
        StartReading(std::move(text), cellScene);
    }

    void SceneLoader::StartReading(std::string text, Ref<Scene> buildInto)
    {
        const bool additive = m_Level != 0;
        const bool inMemory = !text.empty();
        m_Read = JobSystem::Get().Async("Read Scene", JobPriority::Normal,
                                        [shared = m_Parsed, path = m_Path, additive, inMemory, text = std::move(text), buildInto = std::move(buildInto),
                                         level = m_Level](const CancellationToken& token) mutable
        {
            if (token.IsCancelled()) // (the loader is gone)
                return;
            Parsed& parsed = *shared;
            const auto readStart = std::chrono::steady_clock::now();
            struct ReadTimer
            {
                Parsed& Result;
                std::chrono::steady_clock::time_point Start;
                ~ReadTimer() { Result.ReadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count(); }
            } timer{ parsed, readStart };
            std::string bytes = std::move(text);
            if (!inMemory && !VFS::ReadText(path, bytes))
                parsed.Error = "could not open it";
            else if (SceneSerializer::IsRuntimeScene(bytes) && !additive)
            {
                parsed.Bytes = std::move(bytes);
                parsed.Runtime = true;
                parsed.Ok = true;
            }
            else if (SceneSerializer::IsRuntimeScene(bytes) && !SceneSerializer::RuntimeSceneText(bytes, path.string(), bytes))
                parsed.Error = "not a runtime scene this build reads";
            else
            {
                try
                {
                    parsed.Document = YAML::Load(bytes);
                    if (!parsed.Document["Scene"])
                        parsed.Error = "not a scene file";
                    else
                    {
                        parsed.Entities = parsed.Document["Entities"];
                        parsed.Ok = true;
                    }
                }
                catch (const YAML::Exception& exception)
                {
                    parsed.Error = exception.what();
                }
            }

            // A world cell into a scene of its own (Unreal's async loading thread): nothing else sees that scene until the job is
            // done, and creating an entity touches only its scene. The main thread is left with copying the cell into the world.
            if (!buildInto || !parsed.Ok)
                return;
            const auto buildStart = std::chrono::steady_clock::now();
            try
            {
                Scene& scene = *buildInto;
                if (const YAML::Node folders = parsed.Document["Folders"])
                {
                    for (const YAML::Node& folder : folders)
                        scene.AddFolder(folder.as<std::string>());
                }
                const size_t count = parsed.Entities && parsed.Entities.IsSequence() ? parsed.Entities.size() : 0;
                parsed.Created.reserve(count);
                for (size_t index = 0; index < count; ++index)
                {
                    if ((index & 63) == 0 && token.IsCancelled()) // (unloaded meanwhile: its scene goes with the job)
                        return;
                    CreateDocumentEntity(scene, parsed.Entities[index], level, true, path, parsed.Created);
                }
                parsed.Built = true;
            }
            catch (const YAML::Exception& exception)
            {
                parsed.Ok = false;
                parsed.Error = exception.what();
            }
            parsed.BuildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count();
            parsed.Document = YAML::Node();
            parsed.Entities = YAML::Node();
        });
    }

    SceneLoader::~SceneLoader()
    {
        m_Read.Cancel();
    }

    bool SceneLoader::Step(double budgetMs)
    {
        if (m_Failed)
            return true;
        if (!m_Read.IsReady())
            return false;

        Parsed& parsed = *m_Parsed;
        if (!parsed.Ok)
        {
            NOX_CORE_ERROR("Failed to load the scene `{}`: {}", m_Path.string(), parsed.Error);
            m_Failed = true;
            return true;
        }
        if (parsed.Runtime) // (a whole scene only: a level's is parsed on the thread like text)
        {
            SceneSerializer serializer(m_Scene);
            m_Failed = !serializer.DeserializeRuntimeBytes(parsed.Bytes, m_Path.string());
            parsed.Bytes.clear();
            return true;
        }
        if (parsed.Built) // (a cell: built on the worker)
        {
            m_Created = std::move(parsed.Created);
            m_Total = m_Next = m_Created.size();
            m_Started = true;
            return true;
        }

        // (Timed from here: what the main thread spends creating, and in how many steps.)
        const auto stepStart = std::chrono::steady_clock::now();
        struct StepTimer
        {
            SceneLoader& Loader;
            std::chrono::steady_clock::time_point Start;
            ~StepTimer()
            {
                Loader.m_CreateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
                ++Loader.m_CreateSteps;
            }
        } stepTimer{ *this, stepStart };

        Scene& scene = *m_Into;
        if (!m_Started)
        {
            m_Started = true;
            NOX_CORE_TRACE("Deserializing scene '{}'", parsed.Document["Scene"].as<std::string>());
            m_Total = parsed.Entities && parsed.Entities.IsSequence() ? parsed.Entities.size() : 0;
            if (m_Level == 0)
                SceneSerializer::ReadWorldPartition(scene, parsed.Document);
            // A level: none of its entities may be in the scene already (the same scene file, or one saved as a copy of it, shares the
            // IDs its entities, parents and scripts refer to each other by). Nothing is created then. A cell skips them one by one.
            if (m_Level != 0 && !m_Cell)
            {
                for (size_t index = 0; index < m_Total; ++index)
                {
                    const YAML::Node entity = parsed.Entities[index];
                    if (Entity existing = scene.GetEntityByUUID(entity["Entity"].as<uint64_t>()))
                    {
                        NOX_CORE_ERROR("Level `{}` not loaded: its entity '{}' ({}) is in the scene already (a scene saved as a copy of "
                                       "another keeps its entity IDs; make the level a new scene, or load it once)",
                                       m_Path.filename().string(), existing.GetName(), entity["Entity"].as<uint64_t>());
                        m_Failed = true;
                        return true;
                    }
                }
            }
            if (m_Level != 0)
                m_Created.reserve(m_Total);
            if (const YAML::Node folders = parsed.Document["Folders"])
            {
                for (const YAML::Node& folder : folders)
                    scene.AddFolder(folder.as<std::string>());
            }
        }

        // The entities in file order (what ReadScene does), until the frame's slice is used up.
        const auto start = std::chrono::steady_clock::now();
        while (m_Next < m_Total)
        {
            CreateDocumentEntity(scene, parsed.Entities[m_Next++], m_Level, m_Cell, m_Path, m_Created);

            // The clock is read every few entities (an entity takes microseconds).
            if ((m_Next & 15) == 0 &&
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() >= budgetMs)
                break;
        }
        if (m_Next < m_Total)
            return false;

        parsed.Document = YAML::Node();
        parsed.Entities = YAML::Node();
        return true;
    }

    double SceneLoader::GetReadMs() const
    {
        return m_Read.IsReady() ? m_Parsed->ReadMs : 0.0;
    }

    double SceneLoader::GetWorkerBuildMs() const
    {
        return m_Read.IsReady() ? m_Parsed->BuildMs : 0.0;
    }

    float SceneLoader::GetProgress() const
    {
        if (!m_Started)
            return 0.0f;
        return m_Total > 0 ? static_cast<float>(m_Next) / static_cast<float>(m_Total) : 1.0f;
    }

    std::string SceneLoader::GetStage() const
    {
        if (!m_Read.IsReady())
            return "Reading " + m_Path.filename().string();
        if (!m_Started)
            return "Preparing " + m_Path.filename().string();
        return "Creating entities " + std::to_string(m_Next) + " / " + std::to_string(m_Total);
    }
}
