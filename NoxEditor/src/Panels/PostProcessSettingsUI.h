#pragma once
#include "NoxCore/Renderer/PostProcessSettings.h"

namespace Nox
{
    // The post-process settings' controls (PostProcessSettings): the Renderer Settings panel and a Post Process Volume's Inspector draw
    // the same ones. True when something changed.
    bool DrawPostProcessSettings(PostProcessSettings& settings);
}
