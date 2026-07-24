#include "NOXWatcher.h"

#include "NoxCore/Core/Log.h"

namespace Utils
{
    void NOXWatcher::watch(std::filesystem::path path, std::function<void()> onModified)
    {
        if (!std::filesystem::exists(path))
        {
            NOX_CORE_ERROR("NOXWatcher::watch Cannot watch path because it does not exist: {}", path.string());
            
            return;
        }
        
        std::string absolutePathStr = std::filesystem::absolute(path).string();
        
        m_fileTimestamps[absolutePathStr] = std::chrono::steady_clock::now();
        
        m_activeWatches.push_back(std::make_unique<filewatch::FileWatch<std::string>>(
            absolutePathStr,
            [this, onModified](const std::string& pathStr, const filewatch::Event change_type)
            {
                if (change_type == filewatch::Event::modified)
                {
                    auto currentTime = std::chrono::steady_clock::now();
                    
                    auto& lastTime = m_fileTimestamps[pathStr];
                    auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastTime).count();
                    
                    if (elapsedTime < 100) return; 
                    
                    lastTime = currentTime;
                    
                    if (onModified) 
                    {
                        onModified();
                    }
                    
                    std::filesystem::path modifiedPath(pathStr);
                    NOX_CORE_INFO("NoxWatcher: {}", modifiedPath.filename().string());;
                }
            }
        ));
    }
}
