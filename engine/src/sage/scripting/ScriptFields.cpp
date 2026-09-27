#include "sage/scripting/ScriptFields.h"

#include <algorithm>
#include <cctype>

#include "sage/scripting/lua/LuaInternal.h"

namespace sage::scripting {

sage::vars::Table ParseFields(const std::string& path, const std::string& source) {
    std::string ext;
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    if (ext == ".lua") return lua::ParsePublicFields(source);
    return {};
}

namespace {

bool IsHook(const std::string& name) {
    static const char* kHooks[] = {
        "Awake", "Start", "Update", "FixedUpdate", "LateUpdate", "OnEnable", "OnDisable",
        "OnDestroy", "OnCollisionEnter", "OnCollisionExit", "OnTriggerEnter", "OnTriggerExit",
        "OnTriggerStay", "OnAnimationEvent", "OnMessage", "OnQuit", "OnStart", "OnUpdate",
        "OnFixedUpdate", "OnLateUpdate"};
    for (const char* h : kHooks)
        if (name == h) return true;
    return false;
}

bool IdentChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

// `function Menu:start_game(` / `function Menu.start_game(` / `function start_game(`
// (старый стиль — глобальная функция файла). Комментарии и строки не
// разбираются: объявление метода в комментарии — редкость, а ложная строка в
// списке безобиднее пропущенной.
std::vector<std::string> ParseLuaMethods(const std::string& src) {
    std::vector<std::string> out;
    size_t pos = 0;
    while ((pos = src.find("function", pos)) != std::string::npos) {
        const bool wordStart = pos == 0 || !IdentChar(src[pos - 1]);
        // `local function helper()` — вспомогательная функция файла, а не
        // метод: у экземпляра её нет, и связь на неё никогда не сработает.
        size_t back = pos;
        while (back > 0 && (src[back - 1] == ' ' || src[back - 1] == '\t')) --back;
        const bool isLocal = back >= 5 && src.compare(back - 5, 5, "local") == 0 &&
                             (back == 5 || !IdentChar(src[back - 6]));
        pos += 8;
        if (!wordStart || isLocal || pos >= src.size() || !std::isspace((unsigned char)src[pos])) continue;
        size_t p = pos;
        while (p < src.size() && std::isspace((unsigned char)src[p])) ++p;
        const size_t start = p;
        while (p < src.size() && (IdentChar(src[p]) || src[p] == '.' || src[p] == ':')) ++p;
        if (p == start || p >= src.size()) continue;
        std::string full = src.substr(start, p - start);
        while (p < src.size() && std::isspace((unsigned char)src[p])) ++p;
        if (p >= src.size() || src[p] != '(') continue;
        const size_t sep = full.find_last_of(".:");
        const std::string name = sep == std::string::npos ? full : full.substr(sep + 1);
        if (name.empty() || name[0] == '_' || IsHook(name)) continue;
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    }
    return out;
}

} // namespace

std::vector<std::string> ParseMethods(const std::string& path, const std::string& source) {
    std::string ext;
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (ext == ".lua") return ParseLuaMethods(source);
    return {};
}

} // namespace sage::scripting
