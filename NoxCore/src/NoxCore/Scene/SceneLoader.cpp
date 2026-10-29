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
    };

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

    void SceneLoader::StartReading(std::string text)
    {
        const bool additive = m_Level != 0;
        const bool inMemory = !text.empty();
        m_Read = JobSystem::Get().Async("Read Scene", JobPriority::Normal,
                                        [shared = m_Parsed, path = m_Path, additive, inMemory, text = std::move(text)](const CancellationToken& token) mutable
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
            const YAML::Node& entities = parsed.Entities;
            const YAML::Node entity = entities[m_Next++];
            if (m_Cell)
            {
                if (Entity existing = scene.GetEntityByUUID(entity["Entity"].as<uint64_t>()))
                {
                    NOX_CORE_WARN("World cell `{}`: entity '{}' ({}) is in the scene already, kept as it is", m_Path.filename().string(),
                                  existing.GetName(), entity["Entity"].as<uint64_t>());
                    continue;
                }
            }
            std::string name;
            if (const YAML::Node tag = entity["TagComponent"])
                name = tag["Tag"].as<std::string>();
            Entity created = scene.CreateEntityWithUUID(entity["Entity"].as<uint64_t>(), name);
            SceneSerializer::DeserializeEntityComponents(scene, entity, created, false);
            if (m_Level != 0)
            {
                created.AddComponent<LevelComponent>().Level = m_Level;
                m_Created.push_back(created.GetUUID());
            }

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
