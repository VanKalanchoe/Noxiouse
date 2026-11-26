namespace Nox;

/// <summary>
/// Unified multi-layer GPU VFX component (.nvfx). Drives any combination of GPU particles,
/// ribbons/trails, lightning beams, decals, screen-space distortion, heterogeneous volumes,
/// local volumetric fog, and dynamic point lights from a single asset.
/// </summary>
public sealed class VfxComponent : Component
{
    internal override ComponentType Type => ComponentType.Vfx;

    /// <summary>Starts or resumes playback; pass <c>restart: true</c> to reset the timeline and particles.</summary>
    public void Play(bool restart = false) => InternalCalls.PlayVfx(EntityID, restart);

    /// <summary>Pauses playback.</summary>
    public void Stop() => InternalCalls.StopVfx(EntityID);

    /// <summary>Restarts all layers from t = 0.</summary>
    public void Restart() => InternalCalls.PlayVfx(EntityID, true);

    public bool IsPlaying => InternalCalls.IsVfxPlaying(EntityID);

    public float Scale
    {
        get => InternalCalls.GetVfxScale(EntityID);
        set => InternalCalls.SetVfxScale(EntityID, value);
    }

    public void SetColorTint(float r, float g, float b, float a = 1.0f) =>
        InternalCalls.SetVfxColorTint(EntityID, r, g, b, a);

    /// <summary>Overrides a named float parameter (e.g. "Intensity") on this VFX instance.</summary>
    public void SetFloat(string name, float value) => InternalCalls.SetVfxFloat(EntityID, name, value);

    /// <summary>Overrides a named Vector3 parameter on this VFX instance.</summary>
    public void SetVector3(string name, Vector3 value) => InternalCalls.SetVfxVec3(EntityID, name, value);

    /// <summary>Overrides a named RGBA color parameter (e.g. "Color") on this VFX instance.</summary>
    public void SetColor(string name, float r, float g, float b, float a = 1.0f) =>
        InternalCalls.SetVfxColor(EntityID, name, r, g, b, a);
}
