#pragma once

#include <cstdint>
#include <string_view>

namespace Nox
{
    // The mixer's buses (docs/Audio_Architecture_Plan_2026.md §7) are the .nmixer asset's (AudioMixer.h), named by id. A new mixer
    // starts with these, at fixed ids every project shares; buses added in the Mixer panel get random ones.
    inline constexpr uint64_t kAudioBusMaster = 1;
    inline constexpr uint64_t kAudioBusMusic = 2;
    inline constexpr uint64_t kAudioBusSFX = 3;
    inline constexpr uint64_t kAudioBusVoice = 4;
    inline constexpr uint64_t kAudioBusUI = 5;
    inline constexpr uint64_t kAudioBusAmbience = 6;
    inline constexpr uint64_t kAudioBusReverb = 7;

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

    // How a 3D sound reaches the ears (docs/Audio_Architecture_Plan_2026.md §6; Steam Audio): Panning for speakers, Binaural (HRTF)
    // for headphones -- sounds above, below and behind are heard there. Default follows the game's output setting (the mixer's
    // Headphones, a game's options menu: Audio.SetHeadphones).
    enum class AudioSpatialization : uint8_t
    {
        Default = 0,
        Panning,
        Binaural
    };

    std::string_view AudioSpatializationToString(AudioSpatialization mode);
    AudioSpatialization AudioSpatializationFromString(std::string_view name);

    // What a surface does to sound (Steam Audio's material presets, as its Unity / Unreal plugins ship them): how much it absorbs and
    // lets through at low / mid / high frequencies, how much it scatters. A material asset's acoustic material (its asset registry
    // entry); Auto picks one from how the material looks (metal, glass, the rest Generic).
    enum class AcousticMaterial : uint8_t
    {
        Auto = 0,
        Generic,
        Brick,
        Concrete,
        Ceramic,
        Gravel,
        Carpet,
        Glass,
        Plaster,
        Wood,
        Metal,
        Rock,
        Count
    };

    struct AcousticMaterialProperties
    {
        float Absorption[3];   // low (< 800 Hz), mid (800 Hz - 8 kHz), high (> 8 kHz): the energy taken by a bounce
        float Scattering;      // the share reflected diffusely
        float Transmission[3]; // the share passing through (a wall's thickness included)
    };

    std::string_view AcousticMaterialToString(AcousticMaterial material);
    AcousticMaterial AcousticMaterialFromString(std::string_view name);
    // Auto is Generic here (the caller resolves Auto from the material first).
    const AcousticMaterialProperties& GetAcousticMaterialProperties(AcousticMaterial material);

    // A playing voice: 0 is none. Slot and generation, so a handle of a voice that ended never reaches the voice now in its slot.
    using AudioVoiceHandle = uint64_t;
}
