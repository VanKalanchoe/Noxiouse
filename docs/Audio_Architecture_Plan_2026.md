# Audio and the Audio Graph -- architecture plan (2026-11-06)

> Status: plan only, nothing built. Backlog C4 ("audio graph", on hold). The engine has no audio at all today; this plan covers the
> whole audio system the graph needs, not just the graph. Phase gates as in the other plans: each phase is built, then tested by the
> user, before the next one starts.

---

## 1. Which library

### 1.1 The candidates

| Library | What it is | Licence / cost | Fits an in-engine audio graph? |
|---|---|---|---|
| **FMOD** (Studio + Core) | Commercial middleware: runtime + its own authoring tool (FMOD Studio: events, timelines, parameters, mixer, live update) | Proprietary. Free indie licence below a revenue / budget limit, paid above (check the current terms per title) | No: the sound design lives in FMOD Studio, not in Nox. Nox would only post events. Great tool, but the graph would be theirs, not ours |
| **Wwise** | Commercial middleware, the AAA standard (Actor-Mixer hierarchy, RTPCs, states / switches, interactive music, spatial audio) | Proprietary. Free for small budgets with a sound-count limit, paid above (check the current terms) | No, for the same reason: authoring in Wwise's tool. Heavy integration (SoundBanks, generated headers, per-platform SDKs) |
| **SDL3_mixer** | Simple mixer on SDL3: play sounds / music, a few effects | zlib | No: no DSP graph, no real 3D, no custom nodes. Fine for a small 2D game, not for this engine |
| **OpenAL Soft** | OpenAL 1.1 implementation: 3D sources, HRTF, EFX reverb / filters | LGPL (dynamic linking) | Partly: good 3D and HRTF, but its fixed source / effect model does not run a custom per-voice DSP graph; no decoders, no streaming |
| **miniaudio** | Single-file C audio library: devices on every platform, decoders (WAV, FLAC, MP3; Vorbis via stb_vorbis), resource manager with async loading and streaming, a **node graph API** (`ma_node_graph`) with built-in nodes (biquad, low / high / band pass, shelf, peak EQ, delay, splitter) and **custom nodes**, sound groups (buses), basic 3D spatializer, Doppler | Public domain / MIT-0 | **Yes**: the node graph is exactly the substrate an engine graph compiles onto, and everything else (devices, decode, streaming, mixing) comes with it |
| **Steam Audio** | Not an audio engine: a **spatial audio / acoustics** library (Valve): HRTF binaural rendering, distance and air absorption, **occlusion and transmission by ray casts**, **reflections and reverb** (real-time or baked probes), **pathing** (sound around corners, baked). Plugs into an engine's mixer as a per-source effect | Apache 2.0, free, prebuilt libraries | **Yes, as the spatial layer on top of miniaudio** (it is what Unity / Unreal / FMOD / Wwise users add for physically based acoustics). It accepts a custom ray tracer callback, so it can use the engine's own scene |

### 1.2 Recommendation: miniaudio + Steam Audio, graph and mixer in Nox

- **miniaudio** as the core: device output, decoding, streaming from the VFS / `.npak`, voices, buses, and the DSP graph the audio graph
  compiles to. One file, no licence cost or terms, every platform the engine targets.
- **Steam Audio** for spatialization: HRTF (headphones), occlusion / transmission (rays through walls), reflections and reverb from the
  actual geometry, baked per level like the irradiance probes. Apache 2.0.
- **The authoring stays in Nox:** the audio graph is a second domain on the existing node-graph core and editor (what was planned for
  the core from the start), exactly like **Unreal 5's MetaSounds** (in-engine DSP graphs per sound) and its **Submix / Audio Modulation**
  mixer. Godot does the same with its own audio server and buses.
- **FMOD / Wwise stay possible later** behind the same interface (Unreal ships its own MetaSounds and also accepts FMOD / Wwise plugins):
  the engine talks to an `AudioBackend`, so a project that wants Wwise could swap it in. Not needed to start.

Why not FMOD / Wwise as the default: the whole point of an *audio graph in the engine* is that sound is authored in the engine's own
node editor, saved as engine assets, previewed live, packaged with the game. Middleware moves that into a separate application with its
own project, banks and licence terms. Excellent for big audio teams; the wrong base for an engine that wants its own graph.

---

## 2. How it fits together (the layers)

```
Game thread (scripts, components, animation notifies, physics impacts)
   |  commands (play, stop, set parameter, trigger) -- lock-free queue, no locks on the audio thread
   v
Audio thread (one block every 256 frames at 48 kHz = 5.3 ms)
   Voice manager: priorities, concurrency limits, virtual voices, distance culling
      Voice = one playing Sound: a clip, or an instance of an Audio Graph (compiled DSP)
         -> Spatializer (Steam Audio: HRTF / panning, attenuation, air absorption, occlusion, reflections send)
            -> Bus (submix): Music / SFX / Voice / UI / Ambience ... each with effects, sends, ducking
               -> Master bus -> limiter -> miniaudio device
```

- **Clips** are plain sounds (a file). **Audio Graphs** are procedural sounds: a graph of nodes run per voice, per block, on the
  audio thread (MetaSounds' model). A graph can play clips, mix them, filter them, react to parameters and triggers.
- **Buses (submixes)** form the mixer: Unreal's Submixes, Godot's audio buses, Unity's Audio Mixer groups.
- **Spatialization** happens per voice between the graph and its bus, from the voice's 3D position and the listener's.

---

## 3. Assets

| Asset | File | What it holds |
|---|---|---|
| **Audio Clip** | `.naudio` (cooked from `.wav` / `.flac` / `.mp3` / `.ogg`) | Samples (compressed Ogg Vorbis / Opus for long sounds, PCM / ADPCM for short ones), sample rate, channels, loop points, **Load Type**: Decompress on Load (short SFX) / Stream (music, ambience, dialogue), cue markers (for subtitles / beat sync) |
| **Audio Graph** | `.naudiograph` | A node graph in domain `AudioGraph`: inputs (parameters), nodes, one Output. Compiled on load like animation graphs |
| **Attenuation** | `.nattenuation` | Distance falloff curve (inverse / linear / log / custom curve), min / max distance, spatialization mode (none / panning / binaural), air absorption, occlusion on / off, reverb send by distance. Shared by many sounds (Unreal's Sound Attenuation) |
| **Concurrency** | `.nconcurrency` | Max voices of a group (e.g. 8 footsteps at once), resolution: stop oldest / quietest / farthest / refuse new, a minimum retrigger time |
| **Mixer** | `.nmixer` (one per project) | The bus tree, each bus's effect chain and sends, ducking rules, **snapshots** (named mix states: Pause Menu, Underwater, Cutscene) |
| **Impulse Response** | `.naudio` with an IR flag | For the convolution reverb node / bus effect |

Import: drag a `.wav` / `.ogg` into the Content Browser like a texture; the import dialog picks Load Type, compression and quality.
Cooking for packages: compressed to Vorbis / Opus, streamed ones split for reading in chunks through the IO workers.

---

## 4. Components (what you put on entities)

| Component | Unreal / Unity equivalent | Fields |
|---|---|---|
| **Audio Source** | Audio Component / AudioSource | Sound (a clip or an audio graph), Play On Start, Loop, Volume, Pitch, Bus, Attenuation, Concurrency, Priority, 2D / 3D, graph parameters (overrides of the graph's defaults, the same model as the Animator's) |
| **Audio Listener** | the player camera's listener | Usually on the camera; the primary camera's is used when none exists. Several (split screen) later |
| **Audio Volume** | Audio Volume / Reverb Zone | A box: reverb settings or a reverb bus send inside it, interior / exterior filtering (muffle sounds outside a room), priority for overlaps |
| **Ambient Sound** | Ambient Sound actor | An Audio Source preset: looping, 3D, no script needed |

Editor: a speaker icon in the viewport for sources (with the attenuation spheres when selected), an Audio Volume box, a play button on
clips and graphs in the Content Browser, and the graph editor's own live preview (below).

---

## 5. The audio graph

### 5.1 How it works

- **Same core as the animation graph:** `NodeGraph` / `GraphCompiler` / the node editor panel, domain `AudioGraph`, its own node types
  registered like `RegisterAnimationGraphNodeTypes`. Saved as `.naudiograph`, edited in the same window (state machines are an
  animation-only node; the audio graph does not need them).
- **Two kinds of pins**, as in MetaSounds:
  - **Audio** pins carry a block of samples (256 frames, mono or stereo) -- the signal.
  - **Control** pins carry values once per block: Float, Int, Bool, and **Trigger** (an event that fires at a sample-accurate offset
    inside the block: "start", "play next", "finished").
- **Per voice, per block:** every playing instance of a graph has its own node state (oscillator phase, filter memory, playheads), like an
  animation graph instance. The compiled graph is run node by node in dependency order once per 256-frame block on the audio thread --
  no allocations, no locks; node state lives in one block of memory sized at compile time.
- **Inputs / parameters:** a graph declares inputs (Float, Int, Bool, Trigger) with defaults. Set per source in the Inspector (overrides,
  like the Animator), and from scripts at run time (`AudioSource.SetFloat("RPM", 3500)`, `Trigger("Fire")`), smoothed so a jump in a
  value does not click.
- **Outputs:** Audio Out (mono / stereo), and an **On Finished** trigger that ends the voice (a one-shot graph decides itself when it
  is done; a looping one runs until stopped).
- **Live preview:** the editor plays the graph while you edit it (Play button in the graph window), with the parameters as sliders;
  edits apply to the playing preview (MetaSounds does this). Meters on the output.
- **Graphs inside graphs:** a graph can be used as a node in another (MetaSounds' "Composition"): a gun graph built from a "mechanical
  click" graph and a "tail" graph.

### 5.2 Nodes

**Sources (generators)**

| Node | What it does |
|---|---|
| Wave Player | Plays a clip: Play / Stop triggers, Start Time, Pitch (semitones or ratio), Loop + loop region; outputs audio, On Finished, On Looped, playback position |
| Random Wave Player | Picks a clip from a list: weights, no-repeat count (footstep / impact variations) |
| Sequence Player | Plays its clips in order, one per trigger (dialogue lines, combo sounds) |
| Oscillator | Sine / Saw / Square (pulse width) / Triangle, frequency, phase reset; band-limited (no aliasing) |
| Noise | White / Pink / Brown, seed |
| Granular Player (later) | Plays tiny grains of a clip: time stretching, textures (rain, engines) |

**Triggers and logic** (control rate, sample-accurate)

| Node | What it does |
|---|---|
| On Play | Fires when the voice starts |
| Trigger Repeat | Fires every N seconds (or BPM) while enabled |
| Trigger Delay | Fires N seconds after its input |
| Trigger Counter | Counts triggers, fires at a count, resets |
| Trigger Random | Passes a trigger with a probability, or picks one of several outputs |
| Trigger Sequence | Each input trigger goes to the next output in turn |
| Trigger Any / All / Once / Gate | Combine and filter triggers |
| Compare / Select / Switch | Branch on values |

**Values and math**

| Node | What it does |
|---|---|
| Get Parameter | Reads a graph input (as in the animation graph) |
| Add / Subtract / Multiply / Divide / Min / Max / Clamp / Abs | Arithmetic on control values (and on audio, sample by sample) |
| Map Range | e.g. RPM 800..7000 -> pitch 0.8..1.6 |
| Lerp / Crossfade Value | Blends two values |
| Random Float / Int | Seeded, re-rolled on a trigger (random pitch per footstep) |
| Interpolate (slew) | Moves a value towards its target at a rate: no clicks when a parameter jumps |
| dB <-> Linear, Frequency <-> MIDI note, Semitones <-> Ratio | Unit conversions |
| Curve | Looks a value up in a curve (an asset or drawn in the node) |

**Envelopes and modulation**

| Node | What it does |
|---|---|
| AD / ADSR Envelope | Attack-decay(-sustain-release) on a trigger, as a value or applied to audio |
| LFO | Low-frequency oscillator for modulation (vibrato, tremolo, wobble) |
| Envelope Follower | The loudness of an audio input as a value (sidechain: duck music under dialogue inside a graph) |

**Mixing and routing**

| Node | What it does |
|---|---|
| Mixer | N audio inputs, each with a gain |
| Crossfade (N-way) | Blends N inputs by a value: an engine sound from idle / low / mid / high recordings by RPM (the classic car-engine graph) |
| Gain / Volume (dB) | |
| Stereo Panner | Pan law |
| Mono <-> Stereo, Channel Split / Merge | |

**Filters and EQ**

| Node | What it does |
|---|---|
| Biquad Filter | Low pass / high pass / band pass / notch / peak / low shelf / high shelf; frequency, Q, gain (miniaudio has these) |
| State Variable Filter | LP / HP / BP at once, smooth to modulate |
| Ladder Filter | Synth-style resonant low pass |
| Parametric EQ | A few bands in one node |
| One-pole Low / High Pass | Cheap muffling (behind a wall, underwater) |

**Delays, reverbs and modulation effects**

| Node | What it does |
|---|---|
| Delay / Stereo Delay | Time, feedback, wet / dry (echoes) |
| Chorus / Flanger / Phaser | |
| Reverb (algorithmic) | Feedback delay network or Freeverb: size, decay, damping, pre-delay |
| Convolution Reverb | With an Impulse Response asset (a real room's response) |

**Dynamics and distortion**

| Node | What it does |
|---|---|
| Compressor / Limiter | Threshold, ratio, attack, release, knee; sidechain input |
| Gate | |
| Waveshaper / Distortion | Soft / hard clip, drive |
| Bitcrusher | Sample rate / bit depth reduction (retro, radio) |

**Pitch and time (later)**

| Node | What it does |
|---|---|
| Pitch Shift | Without changing length |
| Time Stretch | Without changing pitch |

**Output**

| Node | What it does |
|---|---|
| Output | Audio (mono / stereo) and On Finished |

### 5.3 Examples it should make easy

- **Footsteps:** On Play -> Random Wave Player (6 clips, no repeat 2) -> Random pitch +-1 semitone -> Gain -> Output. Played by an
  animation notify ("Footstep") on each foot contact; the surface (from the physics material under the foot) picks the clip set.
- **Car engine:** parameter RPM -> Map Range -> Crossfade between four looping recordings, each pitched by RPM -> Low Pass (Load) ->
  Output.
- **Gun:** Trigger Fire -> Wave Player (shot) + Random Wave Player (mechanical) + delayed tail by distance -> Mixer -> Compressor ->
  Output.
- **Music:** layers (drums / bass / strings) as Wave Players looping in sync, a parameter Intensity crossfading layers in and out.
- **Wind:** Noise -> Band Pass whose frequency follows an LFO and a Wind Strength parameter -> Output (no clips at all).

---

## 6. Spatial audio (Steam Audio)

| Feature | What it does | Cost / when |
|---|---|---|
| Distance attenuation | From the source's Attenuation asset | Per voice, cheap |
| Panning / Binaural HRTF | Panning for speakers; HRTF for headphones (sounds above / behind) | Per voice; HRTF a little more |
| Air absorption | High frequencies fade with distance | Cheap |
| Doppler | Pitch from relative velocity | Cheap |
| **Occlusion and transmission** | Rays from source to listener (volumetric: several rays around the source) through the scene; walls muffle by their **acoustic material** (absorption / transmission per frequency band: wood, concrete, glass) | Ray casts per source a few times a second, on a worker |
| **Reflections / reverb** | Real-time: rays bounced off the scene give each source's early reflections and reverb tail. **Baked**: reverb probes per level (like the irradiance probes), looked up at run time | Real-time on a worker with a budget; baked costs nothing at run time |
| **Pathing** | Sound finds its way around corners and through doors (baked probe graph per level) | Baked |

- **Geometry:** Steam Audio takes the scene as triangle meshes plus acoustic materials, or a custom ray tracer callback. Nox gives it
  the static meshes' physics / collision triangles (the same data `MeshColliderGeometry` already reads), with an **Acoustic Material**
  on a mesh's material (defaults from the material's look: metal, wood, stone, fabric). World partition cells add and remove their
  geometry as they stream.
- **Settings per source** (in its Attenuation asset): spatialization None / Panning / Binaural, occlusion on / off, reflections on / off,
  pathing on / off -- distant or unimportant sounds keep the cheap path.

---

## 7. The mixer (buses)

- **Bus tree:** Master -> Music, SFX (-> Weapons, Footsteps, Impacts), Voice, UI, Ambience; any sound plays into one bus.
- **Effects per bus:** the same DSP as graph nodes (EQ, compressor, reverb, delay, limiter), a chain per bus.
- **Sends:** a bus or a source sends a part of its signal to another bus (a shared Reverb bus fed by everything indoors).
- **Ducking (sidechain):** Voice playing lowers Music by N dB with attack / release (Unreal's submix sidechain, Wwise's ducking).
- **Snapshots:** named mix states that blend in over time: Pause Menu (everything but UI muffled), Underwater (low pass on SFX),
  Cutscene. Set from scripts or timelines.
- **Volume settings:** each top bus's volume is a player setting (Music / SFX / Voice sliders in a game's options menu), saved with the
  project's runtime settings.
- **Metering:** peak / RMS meters per bus in the Mixer panel, live in Play.

### 7.1 The Mixer panel (decided 2026-11-06)

The mixer is **its own panel, not the audio graph**: the graph is *what one sound is* (Unreal's MetaSounds), the mixer is *where every
sound goes* (Unity's Audio Mixer, Godot's bus panel, FMOD's and Wwise's mixers). Every engine keeps the two apart. The panel shows **all
buses side by side as channel strips**, like a mixing desk:

- per strip: name, **volume fader** (dB), live **level meter** (peak / RMS, moving in Play), **mute / solo / bypass**, its **effect slots**
  (add, reorder, edit, bypass each), **send** amounts to other buses, and **Output To** (the parent bus: the routing);
- **snapshots** across the top (Default / Pause / Underwater ...): pick one to edit its faders and effect values; scripts and timelines
  blend between them in game;
- **ducking** set on a bus: lower by N dB while another bus plays, with attack / release;
- the faders are the project's mix (saved in the `.nmixer` asset); a game's options menu changes the top buses at run time
  (`Audio.SetBusVolume`).

---

## 8. Engine side

- **Threads (decided 2026-11-06):** a **dedicated mixer thread**, not JobSystem tasks -- the one exception to "the JobSystem owns every
  thread" (§5.2 of the engine plan), as in Unreal (its audio render thread), FMOD and Wwise (their mixer threads):
  - JobSystem workers are not preemptive: even a High-priority job waits until a worker finishes its current task (a frame graph
    pass, physics, a cook step), while audio has a hard deadline every block (5.3 ms at 256 frames, 48 kHz); a missed block is a click.
    A frame may be 2 ms late, audio may not.
  - The mixer thread runs at high OS priority (MMCSS "Pro Audio" on Windows) and mixes **1-2 blocks ahead into a ring buffer**; the
    device callback (miniaudio's) only copies out of it and never mixes, so an occasionally expensive block does not glitch (~5 ms of
    added latency, inaudible for games; Unreal does the same).
  - The work without a deadline stays on the JobSystem: decoding streams (IO workers, into ring buffers the mixer reads), Steam Audio's
    ray casts and bakes (workers), loading. They talk to the mixer thread through lock-free queues.
  - Should 64 voices ever not fit one thread, the mixer thread gets helpers of its own, still apart from the frame's workers.
- **Game -> audio:** a lock-free command queue (play, stop, parameter, trigger, position updates once per frame per moving source);
  **audio -> game:** events back (On Finished, cue markers) delivered to scripts on the main thread like animation notifies.
- **Voices:** a hard voice limit (e.g. 64 real voices), **virtual voices** for the rest (they keep their playhead but are not mixed, and
  become real when they matter again: Unreal / Wwise behaviour), priority by importance x loudness at the listener, concurrency groups
  (section 3), distance culling (beyond max distance: virtual).
- **Play / Simulate:** the runtime scene's sounds stop when Play stops; the editor keeps its own preview voices (Content Browser, graph
  preview). Pausing the game pauses its voices (UI bus keeps playing).
- **Large worlds:** positions relative to the listener (the engine's camera-relative rendering, in double precision on the game side).
- **Packaging:** clips cooked and compressed into the `.npak`, streamed ones read in chunks; graphs and mixer as assets; Steam Audio's
  baked data per level beside the level.
- **Profiling:** the audio thread's block time as its own lane in the frame profiler, voices real / virtual and streams in the stats
  overlay, a budget warning when a block takes too long.

---

## 9. Scripting (C#)

```csharp
Audio.PlayOneShot("Sounds/Explosion", position);          // fire and forget, 3D at a point
Audio.PlayOneShot2D("Sounds/UI_Click");                   // UI, no position

AudioSource engine = GetComponent<AudioSourceComponent>();
engine.Play();  engine.Stop(fadeOut: 0.5f);
engine.SetFloat("RPM", rpm);  engine.Trigger("Backfire");
engine.Volume = 0.8f;  engine.Pitch = 1.1f;
bool playing = engine.IsPlaying;

Audio.SetBusVolume("Music", 0.5f);
Audio.SetSnapshot("Underwater", blendSeconds: 0.3f);

protected override void OnAudioEvent(string name) { }      // On Finished, cue markers
```

---

## 10. Integrations

- **Animation notifies -> sounds:** a notify can name a sound directly ("Play Sound" notify, Unreal's), played at the bone
  (footsteps at the foot, swooshes at the hand), with the surface under the foot choosing the variation (physics material ->
  Footstep Surface).
- **Sequencer:** an Audio track on timelines (clips with fades, sync to the timeline's time), for cutscenes.
- **Physics impacts:** an Impact Sound on a rigid body: plays when a contact's impulse passes a threshold, volume by impulse, with a
  cooldown (Jolt's contact listener already exists for rolling resistance).
- **Particles:** a sound per emitter burst (sparks, debris).

---

## 11. Phases

| Phase | Contents | Test |
|---|---|---|
| **A1 Core** | miniaudio backend behind `AudioBackend`, audio thread, command queue; Audio Clip import / cook (WAV, FLAC, MP3, Vorbis), stream vs decompress; Audio Source / Listener components with 2D / 3D panning, distance attenuation and Doppler; Play / Stop / volume / pitch; buses with volumes; `Audio.PlayOneShot` and `AudioSource` script API; preview in the Content Browser; packaging | A looping ambient sound and footsteps play and pan correctly around the listener; a streamed music track loops; works in a packaged build |
| **A2 Audio graph** | Domain `AudioGraph` on the node-graph core; audio / control / trigger pins; compiler to per-voice block processing; the source, trigger, math, envelope, mixing and filter nodes (5.2) first; parameters with Inspector overrides and script setters; live preview in the graph window | The footstep, car engine and wind examples (5.3) built in the editor and played in Play |
| **A3 Mixer** | Bus tree asset, effect chains, sends, ducking, snapshots, Mixer panel with meters; Audio Volumes (reverb zones, interior / exterior); the delay / reverb / dynamics nodes | Dialogue ducks music; entering a room switches reverb; a Pause snapshot |
| **A4 Steam Audio** | HRTF binaural, air absorption; occlusion / transmission with acoustic materials (geometry from the static meshes, streamed with world cells); real-time reflections with a budget; baked reverb probes and pathing per level (a Bake button like HLODs) | A sound behind a wall is muffled and comes through the door; Bistro's streets sound different from its interiors |
| **A5 Integrations** | Play Sound notifies with surface types, a sequencer audio track, impact sounds, profiler lane and stats, concurrency and virtual voices tuned | A character's footsteps change on grass / stone; a cutscene with music and dialogue |

Each phase lands with the docs-site pages updated and the manual's walkthrough, like the other systems.
