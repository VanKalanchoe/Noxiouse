namespace Nox;

/// <summary>
/// The entity's Particle Emitter (fire, smoke, sparks, dust). Its look and motion are set in the editor; scripts start and stop it
/// and fire bursts: <c>GetComponent&lt;ParticleEmitterComponent&gt;().Emit(40)</c> where a bullet hits.
/// </summary>
public sealed class ParticleEmitterComponent : Component
{
    internal override ComponentType Type => ComponentType.ParticleEmitter;

    /// <summary>Starts spawning; <paramref name="restart"/> also clears the particles and starts its timeline (and burst) over.</summary>
    public void Play(bool restart = false) => InternalCalls.PlayParticles(EntityID, restart);

    /// <summary>Stops spawning; the particles already out live on until their lifetime ends.</summary>
    public void Stop() => InternalCalls.StopParticles(EntityID);

    /// <summary>Spawns this many particles at once (on its next update), whether it is emitting or not.</summary>
    public void Emit(int count) => InternalCalls.EmitParticles(EntityID, count);

    public bool IsEmitting => InternalCalls.IsEmittingParticles(EntityID);

    /// <summary>How many particles are alive.</summary>
    public int ParticleCount => InternalCalls.GetParticleCount(EntityID);
}
