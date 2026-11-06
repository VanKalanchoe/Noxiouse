namespace Nox;

/// <summary>
/// The entity's Audio Source: plays its clip at the entity's position (3D) or as the file is (2D). Its clip, bus and distances are set
/// in the editor; scripts play and stop it: <c>GetComponent&lt;AudioSourceComponent&gt;().Play()</c> when a door opens.
/// </summary>
public sealed class AudioSourceComponent : Component
{
    internal override ComponentType Type => ComponentType.AudioSource;

    /// <summary>Plays the clip from the start (also when it is playing). In Play only; once the clip is loaded.</summary>
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

    /// <summary>Playback rate: 2 is an octave up and twice as fast (an engine revving), 0.5 an octave down.</summary>
    public float Pitch
    {
        get => InternalCalls.GetAudioSourceFloat(EntityID, 1);
        set => InternalCalls.SetAudioSourceFloat(EntityID, 1, value);
    }

    /// <summary>Whether it starts over at its end. Turning it off lets a playing loop end at its end.</summary>
    public bool Loop
    {
        get => InternalCalls.GetAudioSourceFloat(EntityID, 2) != 0.0f;
        set => InternalCalls.SetAudioSourceFloat(EntityID, 2, value ? 1.0f : 0.0f);
    }

    /// <summary>The clip the next <see cref="Play"/> plays, by its path (extension optional). False when there is no such sound.</summary>
    public bool SetClip(string clipPath) => InternalCalls.SetAudioSourceClip(EntityID, clipPath);
}
