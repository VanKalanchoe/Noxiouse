# Audio and the Audio Graph -- architecture plan (2026-11-06)

> Status: **A1 Core, A2 Audio graph, A3 Mixer and A4a Steam Audio (HRTF, occlusion) built and tested; A4b real-time reflections built
> (2026-11-07)**, waiting for the user's test before A4c (baked reverb probes, pathing) (see §11 "... as built"). Backlog C4. This plan covers
> the whole audio system the graph needs, not just the graph. Phase gates as in the other plans: each phase is built, then tested by
> the user, before the next one starts.

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

**As built in A1:** the Audio Clip is the sound file itself (`.wav` / `.ogg` / `.flac` / `.mp3`), as Godot keeps Ogg and MP3 -- no
`.naudio` cook. Its **Load Type** (Auto / Decompress On Load / Compressed In Memory / Streaming, Unity's three plus Auto at Unreal's 5 s
decompression threshold) is an import setting in the asset registry, like a texture's compression (Content Browser > right-click > Load
Type). Packages ship the file as it is (Ogg / MP3 / FLAC are already compressed, WAV is the uncompressed choice for short sounds); a
streamed clip is read in 64 KB chunks through the VFS, from the loose file or from `Content.npak`. Loop points, cue markers and the
Impulse Response flag come with the assets that use them (A2 Wave Player, A3 convolution reverb), in a `.naudio` sidecar then.

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
  - The work without a deadline stays off the mixer thread: Steam Audio's ray casts and bakes (JobSystem workers), loading (the asset
    loader's jobs). They talk to the mixer thread through lock-free queues. **Decoding streams** runs on a second, normal-priority thread
    of the audio engine, the **stream thread** (FMOD's and Wwise's stream threads): it refills each streamed voice's ring buffer within
    10 ms of it draining, whatever the frame is doing -- JobSystem IO jobs are scheduled from the game thread, so a 2-second hitch
    there (a level load) would starve every stream. (As built in A1.)
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
Audio.PlayOneShot("Sounds/Explosion", position);          // fire and forget, 3D at a point            (A1)
Audio.PlayOneShot2D("Sounds/UI_Click");                   // UI bus, no position                       (A1)

AudioSourceComponent engine = GetComponent<AudioSourceComponent>();
engine.Play();  engine.Stop(fadeOut: 0.5f);               //                                           (A1)
engine.Volume = 0.8f;  engine.Pitch = 1.1f;  engine.Loop = true;  engine.SetClip("Sounds/Idle");    // (A1)
bool playing = engine.IsPlaying;                          //                                           (A1)
engine.SetFloat("RPM", rpm);  engine.Trigger("Backfire"); // graph parameters                          (A2)

Audio.SetBusVolume("Music", 0.5f);                        //                                           (A1)
Audio.SetSnapshot("Underwater", blendSeconds: 0.3f);      //                                           (A3)

protected override void OnAudioEvent(string name) { }      // On Finished, cue markers                 (A2)
```

Clip paths are under the asset folder with the extension optional; a bus name that is not a bus is reported, not taken as Master.

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

### A1 as built (2026-11-06)

- **Library:** miniaudio 0.11.26 (submodule `NoxCore/vendors/miniaudio`), compiled once as C (`Audio/MiniaudioImpl.c`) with its bundled
  stb_vorbis for Ogg; its engine / node graph / resource manager are off -- voices, buses and spatialization are Nox's own.
- **Threads:** the device callback only copies out of a ring buffer; the **mixer thread** (MMCSS "Pro Audio", `AvSetMmThreadPriority`
  high) mixes 256-frame blocks to keep one device period + 2 blocks ahead (its first fill covers the device's whole buffer: devices
  ask for all their periods at once when they start), sleeping on an event the callback signals; the **stream thread** decodes. The
  game thread talks to the mixer through a lock-free SPSC command queue (8192) and hears voices end through a lock-free event queue;
  the mixer never locks or allocates. Checked under ThreadSanitizer.
- **Voices:** 128, handles with a generation (a stale handle finds nothing); linear-interpolation resampling for pitch, Doppler and the
  clip-to-device rate (Unreal's mixer source); every gain ramped across its block; stops fade (5 ms minimum, no clicks); fade-in.
- **Spatialization (game thread, once a frame):** listener-relative positions in meters (large worlds: double on the game side),
  equal-power panning (-3 dB centre), Inverse / Inverse Square / Linear / None attenuation reaching zero at Max Distance, OpenAL's
  Doppler (343 m/s, speeds clamped to half of it) from smoothed velocities with teleports ignored; stereo clips summed to mono in 3D.
- **Buses:** Master <- Music, SFX, Voice, UI, Ambience; volume and mute, ramped; peak meters; pausing the game pauses its voices except
  the UI bus. Master is clamped to [-1, 1] (its limiter is an A3 bus effect).
- **Scene:** `AudioSourceComponent` / `AudioListenerComponent` (Add Component, Inspector, scene files, duplicate, world-partition cells);
  Play / Simulate start the Play On Start sources and stop every game voice at the end; sources follow their entity; switching an entity
  off stops it, on again replays a Play On Start one; destroying it or removing the component stops it; clips are loaded with the scene
  and a source asked to play before its clip is in plays once it is. Selected sources show their Min / Max spheres.
- **Editor:** Content Browser play button / double-click preview (its own voice group), tooltip with format and Load Type, Load Type
  menu, drag-in from Explorer (copied and registered), files put in the project folder registered when their folder is shown.
- **Scripts (API 16):** `Audio.PlayOneShot / PlayOneShot2D / SetBusVolume / GetBusVolume`, `AudioSourceComponent.Play / Stop / IsPlaying /
  Volume / Pitch / Loop / SetClip`.
- **Tests (harness on miniaudio's null device):** WAV / FLAC / MP3 / Ogg load with the right format and length; decoded, streamed and
  in-memory loops at rate 0.75 / 1.37 match a reference resampler with 0 error; one-shots end on their last block; a 50 ms stop fade
  ends in 10 blocks; an Ogg stream ends within 5 ms of its length; pans, attenuation curves and Doppler match their formulas; bus gains
  and group pause; 128-voice limit; no underruns or stream starvation in a live run.
- **Not in A1 (by phase):** the graph and graph parameters (A2); the bus tree asset, effects, sends, ducking, snapshots, Mixer panel
  (A3); HRTF, occlusion, reverb (A4); concurrency groups, virtual voices and priorities, stats overlay lines (the profiler already has the mixer's lane, "Audio Mixer"
  with its "Audio Mix Block" scopes, and a warning when a block takes longer than its 5.3 ms), notifies,
  sequencer track, impact sounds (A5).

### A2 as built (2026-11-06)

- **Authoring on the shared core:** domain `AudioGraph`, `.naudiograph` (`AssetType::AudioGraph`, the same `NodeGraphAsset` and
  serializer as animation graphs), edited in the node graph window: Content Browser > + Add > Audio Graph. The core gained what the audio
  graph needed and any domain can use: variable pin *groups* (a Mixer's "In n" + "Gain n", their literals seeded), node readers typed by
  parameter (`NodeTypeDesc::ParameterType`: the editor offers each parameter with its reader -- Get Float / Int / Bool / Trigger),
  **Trigger parameters** (`GraphParameter::IsTrigger`), and option properties drawn as a labelled slider (`PropertyEnums`; combos do not
  work inside imgui-node-editor nodes). Pin colours / shapes: Audio blue squares, Trigger white arrows, Int cyan.
- **Runtime (`Audio/AudioGraph.h`):** `GraphCompiler` validates and orders the graph, then `AudioGraphProgram::Build` lays its pins out as
  slots -- audio (256 floats), values (Float / Int / Bool as one float), triggers (up to 16 frame offsets a block) -- unconnected inputs
  as constant slots with their literals, and every node's state at an offset of one block of memory. An `AudioGraphInstance` (one voice)
  allocates once on the game thread; `Process` runs the nodes in order on the mixer thread, allocation- and lock-free (the generic
  `GraphEvalContext`, `std::any` / `std::function` based, is not used here). The Output node's On Finished ends the voice at that frame;
  a graph without it plays until stopped. Programs are built per asset version once its clips are loaded (`GetAudioGraphProgram`).
- **Engine:** a voice is a clip or a graph instance; graph voices take parameter set / fire commands and live literal edits through the
  same lock-free queue, report a per-voice peak meter, and their Wave Players' streams are serviced by the stream thread. The source's
  Pitch and Doppler reach the graph's Wave Players and oscillators (unlike a MetaSound's, which Unreal leaves to the graph).
- **Streamed clips in graphs:** Compressed In Memory / Streaming clips keep their first second decoded (the prime, Unreal's stream
  priming); a Wave Player starts from it at once while an `AudioClipStream` positions itself after it, and a looping one keeps a second
  stream ready at the loop's start, so starts are instant and loops seamless. Requests are generation-counted: the stream thread resets
  and seeks a ring only between the mixer's reads, neither ever waits.
- **Nodes (~60):** sources (Wave / Random Wave / Sequence Player, band-limited Oscillator, Noise), triggers and logic (On Play, Repeat,
  Delay, Counter, Random, Pick Random, Sequence, Any, All, Once, Gate, Branch, Compare, Select, Switch), math (arithmetic, Clamp, Abs, Map
  Range, Lerp, Random Float / Int, Interpolate, unit conversions, audio Add / Multiply), envelopes (AD, ADSR -- exponential segments end
  60 dB down in their time --, LFO with sample & hold, Envelope Follower), mixing (Mixer, Stereo Mixer, Crossfade, Gain, Gain dB, Stereo
  Panner, Mono / Stereo), filters (RBJ Biquad in seven types, Simper SVF, Zavalishin ZDF Ladder, One-Pole LP / HP, 3-band Parametric EQ).
  Gains, pans and filter frequencies are ramped across each block (frequencies in octaves); triggers are sample-accurate.
- **Editor:** the graph window's Preview (Play / Stop, parameters as sliders / checkboxes / Fire buttons, L / R meters); a literal edit goes
  into the playing preview (`AudioGraphProgram::LiteralChanges`), other edits restart it with a 20 ms crossfade; Content Browser play
  button on graph tiles.
- **Audio Source:** `Clip` became **`Sound`** (a clip or a graph); a graph's parameters are overridden per entity in the Inspector (saved,
  Reset), set live in Play, and by scripts. The scene keeps a graph's clips loaded with it.
- **Scripts (API 17):** `AudioSourceComponent.SetSound / SetFloat / SetInt / SetBool / Trigger`; `Audio.PlayOneShot` plays graphs too.
- **Tests (harness, real compiler and nodes):** oscillator frequency and level, PolyBLEP aliasing (strongest alias -25 dB for saw and square
  at 3.5 kHz, -42 dB triangle); every filter's gain at 100 Hz / 1 kHz / 10 kHz equal to the bilinear-transform theory to 0.3 %; ladder 4-pole
  response; Wave Player output equal to a reference resampler (decoded 4e-6; streamed and in-memory loops at 1.37x 4e-6; a 2-second loop
  region past the prime of a streamed Ogg over 10 s: 0 frames off); Random Wave Player never repeating with No Repeats 1 and Trigger
  Repeat starting it every 4800 frames exactly; ADSR attack / sustain / release times; parameters ramped; live literal edits applied in
  place and structural ones refused; the footstep, car engine (0.013 ms a block for 4 Wave Players, crossfade and filter) and wind examples
  rendered; graph voices in the live engine ending by themselves, stopping, streaming without starvation; ThreadSanitizer clean.
- **Not in A2:** graphs inside graphs (composition) and the Curve node (needs a curve editor in nodes) -- with A3; the delay / reverb /
  dynamics nodes (A3, with the bus effects); notifies playing graphs (A5).


### A3 as built (2026-11-07)

- **Effects (`Audio/AudioEffects.h`), one implementation for bus chains and graph nodes:** EQ (low shelf, two peaks, high shelf), Filter
  (low / high / band pass, frequency in octaves every 32 frames), Compressor (Giannoulis-Massberg-Reiss soft-knee gain computer, smooth
  branching attack / release in dB, peak or RMS detector, makeup, mix, sidechain key), Limiter (look-ahead brickwall: sliding minimum of
  the needed gain, release, box filter of the look-ahead's length; the audio delayed by look-ahead - 1 frames), Gate (hysteresis, hold),
  Delay (Hermite-interpolated lines whose time slews like tape; normal / ping pong / cross; damped feedback), Reverb (Jot FDN: 8 lines
  with slow modulation, Householder feedback, per-line one-pole absorption giving Decay Time at DC and Decay Time x HF Ratio at 5 kHz
  -- I3DL2's HF Reference --, input fed by sqrt(1 - g^2) so long and short decays sound as loud, four Schroeder all-passes per side, 8-tap
  early reflections), Convolution Reverb (uniformly partitioned overlap-save, 256-frame partitions: no latency beyond the block; both
  inputs in one complex FFT and both outputs in one inverse; impulse responses up to 10 s, resampled by windowed sinc, normalized to unit
  energy), Chorus (3 voices a side) / Flanger, Phaser (2-12 first-order all-pass stages), Distortion (soft / hard clip, sine foldback,
  tube; 2x oversampled through a 31-tap Kaiser half-band, DC blocked, tone filter), Bitcrusher. Parameters are raw values the effects
  smooth themselves (gains ramped per block, frequencies in octaves, times slewed). `Audio/AudioDSP.h` holds the shared biquad (moved
  out of the graph nodes), one-pole, delay line, ramps and FFT. Every effect is also an audio graph node (category Effects).
- **Engine:** the fixed bus enum is gone; buses are the mixer's (up to 32), referenced by id. The game thread builds a layout (buses,
  effect instances, sends, ducking, the processing order: every bus after its children, send sources, sidechain and ducking sources;
  loops dropped with a warning) and swaps it in by command; the replaced layout comes back through a queue to be freed on the game
  thread. An effect whose bus id, effect id, type and impulse response are unchanged keeps its instance (a reverb's tail runs through
  any edit); its values reach it as commands after the swap. Per block: voices into their bus (and their environment send), then each bus:
  effects, pre-fader sends, ducking (the source's output peak against a threshold, smoothed in dB), the fader ramp, meters (peak falling
  ~20 dB/s, RMS over 300 ms, gain reduction), post-fader sends, into the parent. Per voice: a low pass (12 dB/oct, cutoff moved in
  octaves) and one send, for Audio Volumes. Pausing honours each bus's Plays While Paused. The mixer thread flushes denormals.
- **Mixer asset (`Audio/AudioMixer.h`):** `.nmixer` (`AssetType::AudioMixer`), YAML with effect parameters by name; the project's at
  `ProjectConfig::AudioMixer` (default `Audio/Mixer.nmixer`, created with the default buses when the editor opens a project without one;
  the runtime reads it through the VFS; the packager adds it as a root, with the impulse responses it names). Default: Master (Limiter
  -0.3 dB), Music (ducked 10 dB by Voice), SFX, Voice, UI (plays while paused), Ambience, Reverb (a Reverb, all wet) at fixed ids 1-7.
- **AudioMixer (game thread):** builds the layout, sends only changed values, mute / solo (soloed buses plus what feeds them, their way
  to Master and their send targets), game volumes (`Audio.SetBusVolume`, on top of the mix, reset when Play stops), snapshots as overrides
  of the Default mix (faders, send levels, number parameters) blended with a smoothstep, frequencies in octaves; `Audio.SetSnapshot`.
- **Mixer panel:** channel strips (fader, L/R peak + RMS meter, GR, M / S / B, effect list), the selected bus's Output To, effect chain
  (add, reorder, bypass, remove, every parameter, sidechain, impulse response), sends (level, pre-fader) and ducking; snapshot bar
  (add, rename, duplicate, delete; editing one stores changes in it, shown orange; right-click: remove from snapshot); its own undo /
  redo; Ctrl+S saves it with the scene. Only routings that make no loop are offered.
- **Audio Volume component:** box, priority, reverb bus + send, interior / exterior volume and low pass (Unreal's interior settings),
  fade time; worked out per 3D voice (sources and one-shots) each frame from the listener's and the sound's volumes; blue box in the
  viewport.
- **Tests (harness):** delay echoes exactly on their frame; reverb T30 within 6 % of Decay Time at 0.8 / 2 / 5 s, HF / LF decay ratio
  0.35 for HF Ratio 0.3, wet loudness within 3 dB of the input for every decay; compressor static curve exact (knee included) and
  sidechain ducking 10.48 dB for 10.5; limiter peak -1.0000 dB under +12 dB bursts; gate; EQ and filter equal to theory; convolution
  equal to direct convolution (3e-6); flanger and phaser notches; distortion aliases 150 dB under the harmonics; routing, pre / post-fader
  sends, ducking -12.00 dB with the meter agreeing, a reverb tail identical through a layout swap, voice low pass -37.8 dB at 8 kHz for a
  1 kHz cutoff, sidechain compressor -17.98 dB for -18; snapshot blend -10 dB half way, solo, game volume, a bus added while voices
  play, save / load round trip, the default project mixer created; ThreadSanitizer clean; A1 and A2 suites still pass.
- **Not in A3:** graphs inside graphs and the Curve node (moved to A5 with the notifies); HRTF / occlusion / Steam Audio reverb (A4).

### A4a as built (2026-11-07): Steam Audio -- HRTF, air absorption, occlusion and transmission

A4 is built in two halves, each tested by the user: **A4a** (this) and **A4b** (real-time reflections with a budget, baked reverb
probes and pathing with a Bake button).

- **Library:** Steam Audio 4.8.1 (Valve, Apache 2.0), the prebuilt SDK that `scripts/install_windows_dependencies.bat` downloads into
  `NoxCore/vendors/steamaudio`; NoxCore links `phonon.lib`, the editor's build copies `phonon.dll` next to it (the runtime and packaged
  games take it from there).
- **`Audio/SpatialAudio.h`:** the context (AVX2 at most), the default HRTF, the acoustic scene and the simulator. Without Steam Audio
  (it failed to start) the engine pans 3D voices itself, as before.
- **Per voice on the mixer thread:** every voice slot has a direct, binaural and panning effect made at startup. A 3D voice is rendered
  summed to one channel with its gain (volume x the engine's distance curve x Audio Volumes), then Steam Audio's **direct effect** applies
  air absorption (3-band EQ) and occlusion / frequency-dependent transmission, then the **binaural effect** (HRTF, bilinear interpolation)
  or the **panning effect** turns it into stereo in the direction the listener hears it from. Then its low pass and sends, as in A3.
- **Spatialization:** per source Default / Panning / Binaural; Default follows the mixer's **Headphones** setting (Mixer panel toolbar),
  which a game's options menu overrides (`Audio.Headphones`, reset when Play stops).
- **Occlusion (game thread, every frame, as Steam Audio's docs advise -- no lag):** an `IPLSource` per audible 3D voice with Occlusion or
  Air Absorption (released beyond Max Distance), volumetric occlusion (16 rays to points over the Source Radius sphere: half hidden is half
  occluded), up to 3 surfaces passed through for transmission, the simulator's air absorption model; the results go to the mixer with the
  voice's update.
- **Acoustic scene:** the running scene's static meshes (skinned / animated ones left out, as HLODs leave them). Each mesh asset + its
  submeshes' acoustic materials is one sub-scene: its cooked file read on a worker (the LOD 0 cluster triangles, welded, as physics' mesh
  colliders read them), each submesh simplified to about a quarter (meshoptimizer, at most 1 % of its size moved, borders locked), then built
  on the game thread. Each entity places an instanced mesh with its transform: moving one is a transform update; switched off, destroyed or
  its world-partition cell unloaded removes it. Positions are meters around an origin that follows the listener (rebased past 1 km).
  Cleared when Play / Simulate stops.
- **Acoustic Material:** a material asset's (Material editor > Acoustic Material; kept in its asset registry entry, so `.nmat` files and
  cooked meshes are unchanged): Steam Audio's presets Generic, Brick, Concrete, Ceramic, Gravel, Carpet, Glass, Plaster, Wood, Metal, Rock
  (absorption, scattering, transmission at low / mid / high); **Auto** picks Glass for transmissive / blended materials, Metal for metallic
  ones, else Generic. Changing it re-reads a playing scene's walls.
- **Audio Source:** Spatialization, Air Absorption, Occlusion, Source Radius (Inspector, scene files).
- **Scripts (API 19):** `Audio.Headphones` (get / set).
- **Tests (harness, the Linux build of the same SDK, null device):** HRTF: a source 5 m to the left 15.7 dB louder in the left ear, to the
  right 17.1 dB in the right one, in front within 2.1 dB (the measured head's own asymmetry), a turned listener hears the front source on
  its right; panning 44 dB left / right; a concrete wall: occlusion 0, transmission 0.015 / 0.002 / 0.001 exactly the preset, 37.6 dB under
  the open path for white noise; glass 0.060 / 0.044 / 0.011; a source half behind a wall's edge occlusion 0.44; wall removed: 1.0; air
  absorption at 150 m 0.97 / 0.78 / 0.07; 64 binaural voices with occlusion against 2000 triangles: 0.79 ms a mixer block (of 5.33),
  1.1 ms a frame on the game thread; A1-A3 suites still pass; ThreadSanitizer clean.
- **Next, A4b:** real-time reflections (per-source convolution through a reflection mixer, a budget of sources and rays), baked reverb
  probes per level and pathing (a Bake button, as HLODs), the reflections' output bus.

### A4b as built (2026-11-07): real-time reflections

- **Two acoustic scenes:** the occlusion scene (changed at once, traced on the game thread) and a reflection scene with the same
  placements (the sub-scenes are shared) that is brought up to date and committed only between reflection runs -- so occlusion never
  waits for the reflection thread and no commit races a run. The origin rebases only between runs too.
- **Reflection thread** ("Audio Reflections"): Steam Audio's own simulator for reflections (up to 16384 rays, 32 diffuse samples, order-1
  ambisonics, up to 1.5 s, its own worker threads), woken by the game thread; a run takes ~12 ms for a closed room.
- **Budget:** a pool of 16 reflection sources; each run (at most every 0.1 s, when the previous one finished) gives the nearest audible
  3D voices with Reflections on (Mixer > Acoustics > Voices, 4 by default) one each. A voice keeps its slot while it stays among them.
  A new voice takes the free slot last simulated nearest to it (within 8 m) and uses that slot's response at once -- a footstep is over
  before a new run could finish, and consecutive steps reuse the previous step's response.
- **Mixer thread:** per pool slot a convolution reflection effect (1.5 s, 4 channels) summing into Steam Audio's reflection mixer (one
  inverse FFT for all), then one ambisonics decoder (HRTF with Headphones, else panning; turned with the listener) into the reflections'
  bus (Mixer > Acoustics > Output Bus, Master by default; Level). The input is the voice without the engine's distance curve (the simulated
  response carries the distance), faded out over the last fifth of Max Distance. A slot passed to another voice is not reset (the last
  voice's tail rings out under the new input); a voice that ends keeps feeding silence until its response has passed (its tail).
- **Why convolution, not hybrid:** Steam Audio 4.8.1's real-time simulation here returned no parametric reverb estimate (reverb times at
  their 0.1 s placeholder, EQ 0), so the hybrid tail would have been silent; convolution through the mixer is what Steam Audio documents as
  its fast path. Cost ~0.15-0.3 ms of mixing per reflecting voice.
- **Settings (Mixer > Acoustics..., saved in the .nmixer):** Reflections on / off, Voices, Rays, Bounces, Duration, Output Bus, Level, and
  the last run's time. Audio Source: **Reflections** (per sound).
- **Tests:** a closed 10 x 4 x 8 m concrete room: 120-440 ms after a 100 ms burst ended the output peaks at 0.51 with reflections and 0.0
  without (the burst reusing a nearby slot's response from its first block); 64 binaural voices with occlusion and 4 with reflections:
  1.86 ms a mixer block in the harness's container; A1-A4a suites pass; ThreadSanitizer: no engine race.
- **Next, A4c:** baked reverb probes per level and pathing (sound around corners), a Bake button like HLODs.

