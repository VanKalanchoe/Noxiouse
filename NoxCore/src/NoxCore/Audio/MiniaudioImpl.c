/* miniaudio's implementation (docs/Audio_Architecture_Plan_2026.md): the device, the decoders (WAV, FLAC, MP3 built in) and the
   lock-free PCM ring buffer. Ogg Vorbis comes from the stb_vorbis copy miniaudio ships in extras/: its header first, so miniaudio
   compiles its Vorbis decoder against it, then its implementation (the documented way of enabling Vorbis in miniaudio 0.11).
   Compiled as C, apart from the engine's precompiled header (NoxCore/CMakeLists.txt). */

#define STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"

/* MA_NO_ENCODING, MA_NO_GENERATION, MA_NO_ENGINE, MA_NO_NODE_GRAPH and MA_NO_RESOURCE_MANAGER are set for all of NoxCore
   (CMakeLists.txt): the voices, buses and spatialization are the engine's own (AudioEngine.cpp), so every file sees the same API. */
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"
