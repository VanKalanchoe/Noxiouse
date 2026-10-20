#pragma once

#include <cstdint>
#include <filesystem>
#include <istream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Nox
{
    // How the engine reads asset files (docs/Runtime_And_Packaging_Plan_2026-10.md R4): a loose file on disk when there is one,
    // else the entry of a mounted pack file (.npak) that holds it. The editor mounts nothing, so every read is the loose file;
    // a packaged game mounts Game/*.npak over its asset directory and reads assets out of them, only the table of contents
    // staying in memory.
    //
    // .npak (little endian): the 8-byte magic "NOXPAK01", uint32 version (1), uint32 entry count, uint64 offset of the table of
    // contents; the files' bytes, each 16-byte aligned; the table of contents: per entry its path (uint32 length + UTF-8, '/'
    // separated, relative to the directory the pack was made from), uint64 offset, uint64 size.
    namespace VFS
    {
        // Mounts a pack: its entries appear as files under `root`. False when it is not a readable pack.
        bool MountPack(const std::filesystem::path& pack, const std::filesystem::path& root);
        // Every *.npak in `directory`, over `root`; how many mounted.
        size_t MountPacks(const std::filesystem::path& directory, const std::filesystem::path& root);
        void UnmountAll();
        bool HasMounts();

        bool Exists(const std::filesystem::path& path);
        std::optional<uint64_t> FileSize(const std::filesystem::path& path);
        bool ReadBytes(const std::filesystem::path& path, std::vector<char>& out);
        bool ReadText(const std::filesystem::path& path, std::string& out);
        // The text, or empty when the file is in neither place (a YAML document that then parses as empty).
        std::string ReadTextOrEmpty(const std::filesystem::path& path);
        // `size` bytes from `offset` into `destination` (a streamed texture's mips): only that range, from a pack as from a file.
        bool ReadRange(const std::filesystem::path& path, uint64_t offset, uint64_t size, void* destination);
        // A stream over the file: the loose file itself, or an in-memory stream of the pack entry's bytes. Null if neither exists.
        std::unique_ptr<std::istream> Open(const std::filesystem::path& path);

        // Writes a pack of `files` (paths relative to `root`); false and `error` when it could not be written.
        bool WritePack(const std::filesystem::path& pack, const std::filesystem::path& root, const std::vector<std::filesystem::path>& files,
                       std::string& error, uint64_t* bytesWritten = nullptr);
    }
}
