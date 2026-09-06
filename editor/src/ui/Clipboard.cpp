#include "ui/Clipboard.h"

#include "imgui.h"

namespace Clipboard {

void SetText(const std::string& text) { ImGui::SetClipboardText(text.c_str()); }

std::string GetText() {
    const char* text = ImGui::GetClipboardText();
    return text ? std::string(text) : std::string();
}

} // namespace Clipboard
