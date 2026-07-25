#include "Utils.h"

#include <atomic>
#include <fstream>
#include <iostream>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>

#include "NoxCore/Core/Application.h"

namespace Nox
{
     /*static const SDL_DialogFileFilter filters[] = {
        { "Nox Scene *.nox", "nox" },  // Your custom filter
        { "All files",   "*" },
        { "PNG images",  "png" },
        { "JPEG images", "jpg;jpeg" },
        { "All images",  "png;jpg;jpeg" }
    };*/

    // Atomic flag to indicate when the callback is done
    std::atomic<bool> callbackDone(false);
    std::string selectedFile;
    
    static void SDLCALL callback(void* userdata, const char* const* filelist, int filter)
    {
        // Cast userdata back to SDL_DialogFileFilter array
        SDL_DialogFileFilter* filters = (SDL_DialogFileFilter*)userdata;
        
        // Set the callbackDone flag to true when the callback is called
        callbackDone = true;
        
        if (!filelist) {
            SDL_Log("An error occured: %s", SDL_GetError());
            return;
        } else if (!*filelist) {
            SDL_Log("The user did not select any file.");
            SDL_Log("Most likely, the dialog was canceled.");
            return;
        }

        while (*filelist) {
            SDL_Log("Full path to selected file: '%s'", *filelist);
            selectedFile = filelist[0];
            filelist++;
        }

        if (filter < 0) {
            SDL_Log("The current platform does not support fetching "
                    "the selected filter, or the user did not select"
                    " any filter.");
        } else if (filter < SDL_arraysize(filters)) {
            SDL_Log("The filter selected by the user is '%s' (%s).",
                    filters[filter].pattern, filters[filter].name);
        }
    }
    
    std::string Utility::OpenFile(const char* filter)
    {
        callbackDone = false;  // Reset the flag before opening the dialog
        selectedFile.clear();  // Clear any previous file path

        // Split the filter string by '\0'
        const char* filterName = filter;
        const char* filterPattern = filter + strlen(filter) + 1;  // Move the pointer to the pattern part

        // Define the filter array
        static const SDL_DialogFileFilter filters[] = {
            { filterName, filterPattern },  // Use the parts from the split filter string
            { "All files", "*" }
        };
        
        // Show the open file dialog
        SDL_ShowOpenFileDialog(callback, (void*)filters, Application::Get().getWindow()->getHandle(), filters, SDL_arraysize(filters), SDL_GetBasePath(), false);
        
        // Loop until the callback is done (the user has made a selection)
        while (!callbackDone) {
            // Pump events to ensure the callback gets called
            SDL_PumpEvents();
            SDL_Delay(10);  // Small delay to avoid busy-waiting
        }

        // Return the selected file
        return selectedFile;
    }

    std::string Utility::SaveFile(const char* filter)
    {
        callbackDone = false;  // Reset the flag before opening the dialog
        selectedFile.clear();  // Clear any previous file path

        // Split the filter string by '\0'
        const char* filterName = filter;
        const char* filterPattern = filter + strlen(filter) + 1;  // Move the pointer to the pattern part

        // Define the filter array
        static const SDL_DialogFileFilter filters[] = {
            { filterName, filterPattern },  // Use the parts from the split filter string
            { "All files", "*" }
        };
        
        // Show the open file dialog
        
        SDL_ShowSaveFileDialog(callback, nullptr, Application::Get().getWindow()->getHandle(), filters, SDL_arraysize(filters), nullptr);
        
        // Loop until the callback is done (the user has made a selection)
        while (!callbackDone) {
            // Pump events to ensure the callback gets called
            SDL_PumpEvents();
            SDL_Delay(10);  // Small delay to avoid busy-waiting
        }

        // Return the selected file
        return selectedFile;
    }

    // load file without dialog

    std::string Utility::LoadFileFromPath(const std::string& path)
    {
        size_t dataSize;
        void* data = SDL_LoadFile(path.c_str(), &dataSize);
        if (data == nullptr)
        {
            /*VK_ERROR("Failed to load file: {}", SDL_GetError());*/
            return "";
        }

        std::string result(static_cast<char*>(data), dataSize);
        SDL_free(data);
        return result;
    }

    std::vector<uint32_t> Utility::LoadSpvFromPath(const std::string& path)
    {
        size_t dataSize;
        void* data = SDL_LoadFile(path.c_str(), &dataSize);
        if (!data || dataSize % sizeof(uint32_t) != 0)
        {
            SDL_Log("Failed to load SPIR-V file '%s': %s", path.c_str(), SDL_GetError());
            return {};
        }

        std::vector<uint32_t> result(static_cast<uint32_t*>(data), static_cast<uint32_t*>(data) + dataSize / sizeof(uint32_t));
        
        SDL_free(data);
        return result;
    }

    void Utility::SaveToFile(const char* filename, const void* data, size_t size)
    {
        if (!SDL_SaveFile(filename, data, size))
        {
            /*VK_ERROR("Failed to save file: {}", SDL_GetError());  */
        }
    }

    std::string Utility::GetCachePath()
    {
        const char* basePath  = SDL_GetBasePath();
        if (!basePath)
        {
            SDL_Log("Failed to get base path: %s", SDL_GetError());
                
        }
        std::string cachePath = std::string(basePath) + "cache/"; // no slash at start
        if (!SDL_CreateDirectory(cachePath.c_str()))
        {
            SDL_Log("Failed to create directory: %s", SDL_GetError());
        }

        return cachePath;
    }

    std::string Utility::GetCachePath(std::string name)
    {
        const char* basePath  = SDL_GetBasePath();
        if (!basePath)
        {
            SDL_Log("Failed to get base path: %s", SDL_GetError());
                
        }
        std::string cachePath = std::string(basePath) + "cache/" + name; // no slash at start
        if (!SDL_CreateDirectory(cachePath.c_str()))
        {
            SDL_Log("Failed to create directory: %s", SDL_GetError());
        }

        return cachePath;
    }

    // xxhash functions ----------------------------------
    XXH128_hash_t Utility::calcul_hash_streaming(const std::string& path)
    {
        constexpr size_t bufferSize = 1 * 1024 * 1024; // 1MB
        std::vector<char> buffer(bufferSize); // RAII-safe buffer

        // Open file
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Failed to open file: " << path << '\n';
            return {};
        }

        // Initialize XXH3 state
        auto state = std::unique_ptr<XXH3_state_t, decltype(&XXH3_freeState)>(
            XXH3_createState(), &XXH3_freeState
        );

        if (!state) {
            std::cerr << "Failed to create XXH3 state\n";
            return {};
        }

        if (XXH3_128bits_reset(state.get()) == XXH_ERROR) {
            std::cerr << "Failed to reset XXH3 state\n";
            return {};
        }

        // Read and hash file in chunks
        while (file) {
            file.read(buffer.data(), buffer.size());
            std::streamsize readBytes = file.gcount();

            if (readBytes > 0) {
                if (XXH3_128bits_update(state.get(), buffer.data(), static_cast<size_t>(readBytes)) == XXH_ERROR) {
                    std::cerr << "Failed to update hash for file: " << path << '\n';
                    return {};
                }
            }
        }

        return XXH3_128bits_digest(state.get());
    }

    // Save hash to file
    void Utility::saveHashToFile(const std::string& hashFile, const XXH128_hash_t& hash) {
        std::ofstream out(hashFile, std::ios::binary);
        out.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
    }

    // Load hash from file
    bool Utility::loadHashFromFile(const std::string& hashFile, XXH128_hash_t& hash) {
        std::ifstream in(hashFile, std::ios::binary);
        if (!in) return false;
        in.read(reinterpret_cast<char*>(&hash), sizeof(hash));
        return in.good();
    }
}
