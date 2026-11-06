namespace Nox;

/// <summary>
/// The entity's Audio Source: plays its sound -- an audio clip or an audio graph -- at the entity's position (3D) or as it is (2D). Its
/// sound, bus and distances are set in the editor; scripts play and stop it (<c>GetComponent&lt;AudioSourceComponent&gt;().Play()</c>
/// when a door opens) and drive an audio graph's parameters (<c>engine.SetFloat("RPM", rpm)</c>, <c>gun.Trigger("Fire")</c>).
/// </summary>
public sealed class AudioSourceComponent : Component
{
    internal override ComponentType Type => ComponentType.AudioSource;

    /// <summary>Plays the sound from the start (also when it is playing). In Play only; once the sound is loaded.</summary>
    public void Play() => InternalCalls.PlayAudioSource(EntityID);

    /// <summary>Stops it, fading out over <paramref name="fadeOut"/> seconds (a few milliseconds at least, so it never clicks).</summary>
    public void Stop(float fadeOut = 0.0f) => InternalCalls.StopAudioSource(EntityID, fadeOut);

    public bool IsPlaying => InternalCalls.IsAudioSourcePlaying(EntityID);

    /// <summary>Linear volume (1 as the file is). A playing sound follows from the next frame, smoothly.</summary>
    public float Volume
    {
        get => InternalCalls.GetAudioSourceFloat(EntityID, 0);
        set => InternalCalls.SetAudioSourceFloat(EntityID, 0, value);
    }

    /// <summary>Playback rate: 2 is an octave up and twice as fast, 0.5 an octave down (an audio graph's Wave Players and oscillators follow it).</summary>
    public float Pitch
    {
        get => InternalCalls.GetAudioSourceFloat(EntityID, 1);
        set => InternalCalls.SetAudioSourceFloat(EntityID, 1, value);
    }

    /// <summary>Whether a clip starts over at its end (an audio graph loops by itself). Turning it off lets a playing loop end at its end.</summary>
    public bool Loop
    {
        get => InternalCalls.GetAudioSourceFloat(EntityID, 2) != 0.0f;
        set => InternalCalls.SetAudioSourceFloat(EntityID, 2, value ? 1.0f : 0.0f);
    }

    /// <summary>
    /// The sound the next <see cref="Play"/> plays: an audio clip or an audio graph, by its path (extension optional). False when there is
    /// no such sound.
    /// </summary>
    public bool SetSound(string soundPath) => InternalCalls.SetAudioSourceSound(EntityID, soundPath);

    /// <summary>
    /// An audio graph parameter (its Get Float node reads it): sent to the playing sound at once, smoothly, and kept for the next
    /// <see cref="Play"/> this session.
    /// </summary>
    public void SetFloat(string name, float value) => InternalCalls.SetAudioSourceParameter(EntityID, name, value, 0);

    public void SetInt(string name, int value) => InternalCalls.SetAudioSourceParameter(EntityID, name, value, 1);

    public void SetBool(string name, bool value) => InternalCalls.SetAudioSourceParameter(EntityID, name, value ? 1.0f : 0.0f, 2);

    /// <summary>Fires an audio graph's trigger parameter on the playing sound (its Get Trigger node fires): "Fire", "Footstep".</summary>
    public void Trigger(string name) => InternalCalls.FireAudioSourceParameter(EntityID, name);
}
