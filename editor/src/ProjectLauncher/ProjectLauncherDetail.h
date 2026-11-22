#pragma once
// Общее для файлов ProjectLauncher*.cpp. Только для них.
#include "ProjectLauncher.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <system_error>
#include "imgui_internal.h"
#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorPrefs.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "../Project.h"
#include "../ui/UI.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/core/Version.h"

namespace fs = std::filesystem;

namespace Sage::Launcher {
namespace launcher_detail {

using EditorTheme::Role;

// Кегль из токенов — в единицах ДО масштаба интерфейса: ImGui умножает на
// style.FontScaleMain сам, а токены приходят уже умноженными (см. UIStyle.h).
// Без деления крупные надписи росли бы вдвое на экране со 150 %.
inline void PushFontSize(float scaledToken) {
    ImGui::PushFont(nullptr, scaledToken / std::max(0.01f, Sage::UI::Scale()));
}

} // namespace launcher_detail
using namespace launcher_detail;
} // namespace Sage::Launcher
