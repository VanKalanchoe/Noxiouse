namespace Nox;

/// <summary>
/// The entity's Timeline Player: plays a sequencer timeline (keyed positions, rotations, scales, light colors and intensities,
/// camera FOV and events) on this entity and the entities below it. Event keys reach this entity's scripts as
/// <see cref="EntityBehaviour.OnAnimationEvent"/>. A door: <c>GetComponent&lt;TimelinePlayerComponent&gt;().Play()</c> when
/// the player comes near.
/// </summary>
public sealed class TimelinePlayerComponent : Component
{
    internal override ComponentType Type => ComponentType.TimelinePlayer;

    /// <summary>Plays from the start, or on from where it is (<paramref name="fromStart"/> false).</summary>
    public void Play(bool fromStart = true) => InternalCalls.PlayTimeline(EntityID, fromStart);

    /// <summary>Stops where it is (the targets keep that pose).</summary>
    public void Stop() => InternalCalls.StopTimeline(EntityID);

    public bool IsPlaying => InternalCalls.IsTimelinePlaying(EntityID);

    /// <summary>The playhead in seconds; setting it jumps there (the next update poses the targets).</summary>
    public float Time
    {
        get => InternalCalls.GetTimelineTime(EntityID);
        set => InternalCalls.SetTimelineTime(EntityID, value);
    }

    /// <summary>Playback rate (negative plays backwards: a door closing along the same keys).</summary>
    public float Speed
    {
        set => InternalCalls.SetTimelineSpeed(EntityID, value);
    }
}
