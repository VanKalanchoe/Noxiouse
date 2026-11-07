namespace Nox;

/// <summary>
/// The game's sound (docs/Audio_Architecture_Plan_2026.md): fire-and-forget sounds and the mixer's bus volumes. Sounds on entities
/// are <see cref="AudioSourceComponent"/>s. Sounds -- audio clips and audio graphs -- are named by their path under the asset directory,
/// the extension optional (<c>"Sounds/Explosion"</c> finds <c>Sounds/Explosion.wav</c>, <c>.ogg</c>, <c>.flac</c>, <c>.mp3</c> or
/// <c>.naudiograph</c>).
/// </summary>
public static class Audio
{
    /// <summary>
    /// Plays a sound once at a point in the world (3D: panned and attenuated around the listener, full volume within 1 m, silent
    /// from 50 m) -- an explosion, an impact. Plays as soon as it is loaded. An audio graph played this way should end by itself
    /// (its Output's On Finished).
    /// </summary>
    public static void PlayOneShot(string clipPath, Vector3d position, float volume = 1.0f, string bus = "SFX") =>
        InternalCalls.PlayOneShot(clipPath, position, volume, true, bus);

    /// <summary>Plays a sound once as it is (2D: no position) -- a UI click on the <c>"UI"</c> bus, a stinger.</summary>
    public static void PlayOneShot2D(string clipPath, float volume = 1.0f, string bus = "UI") =>
        InternalCalls.PlayOneShot(clipPath, default, volume, false, bus);

    /// <summary>
    /// A bus's volume, linear (0 silent, 1 as mixed), on top of the mix the Mixer panel sets -- what a game's options menu sets. Buses
    /// are the project mixer's, by name (any letter case): a new mixer has <c>"Master"</c>, <c>"Music"</c>, <c>"SFX"</c>,
    /// <c>"Voice"</c>, <c>"UI"</c>, <c>"Ambience"</c> and <c>"Reverb"</c>. Every sound on the bus follows it, smoothly. Back to 1 when
    /// Play stops.
    /// </summary>
    public static void SetBusVolume(string bus, float volume) => InternalCalls.SetBusVolume(bus, volume);

    public static float GetBusVolume(string bus) => InternalCalls.GetBusVolume(bus);

    /// <summary>
    /// Blends the mix to one of the mixer's snapshots over <paramref name="blendSeconds"/> (0: at once) -- <c>"Pause"</c> when the
    /// pause menu opens, <c>"Underwater"</c> when the camera dives; <c>"Default"</c> goes back to the mix itself. False (and an error in
    /// the console listing the snapshots) when the mixer has no such snapshot. Back to Default when Play stops.
    /// </summary>
    public static bool SetSnapshot(string name, float blendSeconds = 0.5f) => InternalCalls.SetSnapshot(name, blendSeconds);

    /// <summary>
    /// Headphones (true: 3D sounds through Steam Audio's HRTF -- above, below and behind are heard) or speakers (false: panned left /
    /// right) -- a game's options menu. Sounds whose Spatialization is Default follow it; the mixer's Headphones setting is used until
    /// a script sets it, and again when Play stops.
    /// </summary>
    public static bool Headphones
    {
        get => InternalCalls.GetHeadphones();
        set => InternalCalls.SetHeadphones(value);
    }
}
