#pragma once

// Precompiled header of NoxCore, NoxEditor and NoxRuntime (target_precompile_headers in their CMakeLists): the standard library and
// the third-party headers most of the engine includes, parsed once per target instead of in every source file. Only headers that do
// not change and whose configuration macros are not set by a source file before including them. Not glm's clip-space settings: no
// GLM_FORCE_* here (Renderer2D.h's GLM_FORCE_DEPTH_ZERO_TO_ONE never reached glm::perspective / glm::ortho's callers, which keep
// glm's default). Not vulkan.hpp (only the NRI Vulkan files use it, and it would make every file's precompiled header far larger),
// not windows.h. The vendors' own sources compiled into NoxCore do not use it (SKIP_PRECOMPILE_HEADERS).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

// The gtx headers refuse to compile without it; the engine's headers define it the same way before including them.
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/string_cast.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>
#include <spdlog/fmt/std.h>

#include <entt/entt.hpp>

#include <imgui.h>
