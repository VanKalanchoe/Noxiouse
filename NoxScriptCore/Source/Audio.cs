namespace Nox;

/// <summary>
/// The game's sound (docs/Audio_Architecture_Plan_2026.md): fire-and-forget sounds and the mixer's bus volumes. Sounds on entities
/// are <see cref="AudioSourceComponent"/>s. Clips are named by their path under the asset directory, the extension optional
/// (<c>"Sounds/Explosion"</c> finds <c>Sounds/Explosion.wav</c>, <c>.ogg</c>, <c>.flac</c> or <c>.mp3</c>).
/// </summary>
public static class Audio
{
    /// <summary>
    /// Plays a clip once at a point in the world (3D: panned and attenuated around the listener, full volume within 1 m, silent
    /// from 50 m) -- an explosion, an impact. Plays as soon as the clip is loaded.
    /// </summary>
    public static void PlayOneShot(string clipPath, Vector3d position, float volume = 1.0f, string bus = "SFX") =>
        InternalCalls.PlayOneShot(clipPath, position, volume, true, bus);

    /// <summary>Plays a clip once as the file is (2D: no position) -- a UI click on the <c>"UI"</c> bus, a stinger.</summary>
    public static void PlayOneShot2D(string clipPath, float volume = 1.0f, string bus = "UI") =>
        InternalCalls.PlayOneShot(clipPath, default, volume, false, bus);

    /// <summary>
    /// A bus's volume, linear (0 silent, 1 as mixed): <c>"Master"</c>, <c>"Music"</c>, <c>"SFX"</c>, <c>"Voice"</c>, <c>"UI"</c>,
    /// <c>"Ambience"</c> -- what a game's options menu sets. Every sound on the bus follows it, smoothly.
    /// </summary>
    public static void SetBusVolume(string bus, float volume) => InternalCalls.SetBusVolume(bus, volume);

    public static float GetBusVolume(string bus) => InternalCalls.GetBusVolume(bus);
}
