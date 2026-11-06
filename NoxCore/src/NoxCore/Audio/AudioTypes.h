#pragma once

#include <cstdint>
#include <string_view>

namespace Nox
{
    // The mixer's buses (docs/Audio_Architecture_Plan_2026.md §7). Every sound plays into one; each top bus has a volume (a game's
    // Music / SFX / Voice options) and they all sum into Master. The bus tree, effects, sends and snapshots are phase A3's Mixer asset.
    enum class AudioBus : uint8_t
    {
        Master = 0,
        Music,
        SFX,
        Voice,
        UI,
        Ambience,
        Count
    };

    std::string_view AudioBusToString(AudioBus bus);
    // Master when the name is not a bus.
    AudioBus AudioBusFromString(std::string_view name);

    // How the volume falls with distance between Min Distance (full volume) and Max Distance (silent) -- Godot's and Unity's
    // rolloff curves. The inverse curves are brought down to exactly zero at Max Distance (as Unreal's attenuation always is), so a
    // sound never cuts off audibly there.
    enum class AudioAttenuation : uint8_t
    {
        Inverse = 0,   // Min / distance: how a point source falls off in the open (-6 dB per doubling), the default
        InverseSquare, // (Min / distance)^2: faster, for small sounds that should stay local
        Linear,        // straight from 1 at Min to 0 at Max: easy to reason about for gameplay sounds
        None           // no falloff (panned only), up to Max Distance
    };

    std::string_view AudioAttenuationToString(AudioAttenuation attenuation);
    AudioAttenuation AudioAttenuationFromString(std::string_view name);

    // How a clip is held while it plays (Unity's Load Type, Unreal's decompression threshold).
    enum class AudioLoadType : uint8_t
    {
        Auto = 0,           // Decompress On Load up to 5 seconds (Unreal's decompression threshold), Streaming above
        DecompressOnLoad,   // decoded to samples when loaded: no decoding while playing, most memory (short sound effects)
        CompressedInMemory, // the file's bytes stay in memory, decoded while playing (many medium sounds, dialogue)
        Streaming           // read from disk / the pack in chunks while playing, decoded as it goes (music, ambience)
    };

    std::string_view AudioLoadTypeToString(AudioLoadType type);
    AudioLoadType AudioLoadTypeFromString(std::string_view name);

    // Who owns a voice: the playing game (Play / Simulate; stopped and paused with it) or the editor (Content Browser preview).
    enum class AudioVoiceGroup : uint8_t
    {
        Game = 0,
        Editor,
        Count
    };

    // A playing voice: 0 is none. Slot and generation, so a handle of a voice that ended never reaches the voice now in its slot.
    using AudioVoiceHandle = uint64_t;
}
