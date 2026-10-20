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
    // Written by the reading thread only before m_ReadDone is set, read by the main thread only after.
    struct SceneLoader::Parsed
    {
        std::string Bytes;      // a runtime scene's bytes (read whole on the main thread)
        bool Runtime = false;
        bool Ok = false;
        std::string Error;
        YAML::Node Document;
        YAML::Node Entities;    // the document's entity sequence
    };

    SceneLoader::SceneLoader(std::filesystem::path path)
        : m_Path(std::move(path)), m_Parsed(std::make_unique<Parsed>()), m_Scene(CreateRef<Scene>())
    {
        m_Thread = std::thread([this]()
        {
            Parsed& parsed = *m_Parsed;
            std::string bytes;
            if (!VFS::ReadText(m_Path, bytes))
                parsed.Error = "could not open it";
            else if (SceneSerializer::IsRuntimeScene(bytes))
            {
                parsed.Bytes = std::move(bytes);
                parsed.Runtime = true;
                parsed.Ok = true;
            }
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
            m_ReadDone.store(true, std::memory_order_release);
        });
    }

    SceneLoader::~SceneLoader()
    {
        if (m_Thread.joinable())
            m_Thread.join();
    }

    bool SceneLoader::Step(double budgetMs)
    {
        if (m_Failed)
            return true;
        if (!m_ReadDone.load(std::memory_order_acquire))
            return false;
        if (m_Thread.joinable())
            m_Thread.join();

        Parsed& parsed = *m_Parsed;
        if (!parsed.Ok)
        {
            NOX_CORE_ERROR("Failed to load the scene `{}`: {}", m_Path.string(), parsed.Error);
            m_Failed = true;
            return true;
        }
        if (parsed.Runtime)
        {
            SceneSerializer serializer(m_Scene);
            m_Failed = !serializer.DeserializeRuntimeBytes(parsed.Bytes, m_Path.string());
            parsed.Bytes.clear();
            return true;
        }

        if (!m_Started)
        {
            m_Started = true;
            NOX_CORE_TRACE("Deserializing scene '{}'", parsed.Document["Scene"].as<std::string>());
            if (const YAML::Node folders = parsed.Document["Folders"])
            {
                for (const YAML::Node& folder : folders)
                    m_Scene->AddFolder(folder.as<std::string>());
            }
            m_Total = parsed.Entities && parsed.Entities.IsSequence() ? parsed.Entities.size() : 0;
        }

        // The entities in file order (what ReadScene does), until the frame's slice is used up.
        const auto start = std::chrono::steady_clock::now();
        while (m_Next < m_Total)
        {
            const YAML::Node& entities = parsed.Entities;
            const YAML::Node entity = entities[m_Next++];
            std::string name;
            if (const YAML::Node tag = entity["TagComponent"])
                name = tag["Tag"].as<std::string>();
            Entity created = m_Scene->CreateEntityWithUUID(entity["Entity"].as<uint64_t>(), name);
            SceneSerializer::DeserializeEntityComponents(*m_Scene, entity, created, false);

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

    float SceneLoader::GetProgress() const
    {
        if (!m_Started)
            return 0.0f;
        return m_Total > 0 ? static_cast<float>(m_Next) / static_cast<float>(m_Total) : 1.0f;
    }

    std::string SceneLoader::GetStage() const
    {
        if (!m_ReadDone.load(std::memory_order_acquire))
            return "Reading " + m_Path.filename().string();
        if (!m_Started)
            return "Preparing " + m_Path.filename().string();
        return "Creating entities " + std::to_string(m_Next) + " / " + std::to_string(m_Total);
    }
}
