#include "sage/scripting/ScriptFields.h"

#include <algorithm>
#include <cstring>
#include <cctype>

#include <functional>
#include <mutex>
#include <unordered_map>

#include "sage/core/Problems.h"
#include "sage/scripting/Languages.h"

namespace sage::scripting {

// Поля и методы спрашиваются у языка файла (sage/scripting/Languages.h):
// выбор по расширению живёт в реестре языков, а не здесь.
sage::vars::Table ParseFields(const std::string& path, const std::string& source) {
    Language lang;
    if (!LanguageForPath(path, lang) || !lang.Fields) return {};
    return lang.Fields(path, source);
}

std::vector<FieldDiagnostic> ParseFieldProblems(const std::string& path, const std::string& source) {
    Language lang;
    if (!LanguageForPath(path, lang) || !lang.FieldProblems) return {};
    return lang.FieldProblems(path, source);
}

void ReportFieldProblems(const std::string& path, const std::string& source, bool force) {
    // Хеш текста на файл: тот же текст — те же находки, повторять их каждый
    // кадр показа инспектора значило бы забить консоль.
    static std::mutex mutex;
    static std::unordered_map<std::string, size_t> seen;
    const size_t hash = std::hash<std::string>()(source);
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = seen.find(path);
        if (!force && it != seen.end() && it->second == hash) return;
        seen[path] = hash;
    }
    sage::Problems::Resolve(path, "script.annotation");
    for (const FieldDiagnostic& d : ParseFieldProblems(path, source)) {
        sage::Problem p;
        p.Level = d.Error ? sage::Severity::Error : sage::Severity::Warning;
        p.Category = "Script";
        p.Code = "script.annotation";
        // Сообщение — в заголовке: в консоль идёт именно он, и «файл:строка:
        // что не так» читается без открытия вкладки проблем.
        p.Title = d.Message;
        p.Detail = "Аннотации `---@` над переменной скрипта задают её вид в Inspector (docs/scripting.md).";
        p.File = path;
        p.Resource = path;
        p.Line = d.Line;
        p.Excerpt = sage::Problems::Excerpt(source, d.Line);
        p.Technical = path + ":" + std::to_string(d.Line) + ": " + d.Message;
        sage::Problems::Report(std::move(p));
    }
}

namespace {

bool IsHook(const std::string& name) {
    static const char* kHooks[] = {
        "Awake", "Start", "Update", "FixedUpdate", "LateUpdate", "OnEnable", "OnDisable",
        "OnDestroy", "OnCollisionEnter", "OnCollisionExit", "OnTriggerEnter", "OnTriggerExit",
        "OnTriggerStay", "OnAnimationEvent", "OnMessage", "OnQuit", "OnStart", "OnUpdate",
        "OnFixedUpdate", "OnLateUpdate", "start", "update", "fixed_update", "late_update",
        "on_enable", "on_disable", "on_destroy"};
    for (const char* h : kHooks)
        if (name == h) return true;
    return false;
}

bool IdentChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

} // namespace

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

std::vector<std::string> ParseMethods(const std::string& path, const std::string& source) {
    Language lang;
    if (!LanguageForPath(path, lang) || !lang.Methods) return {};
    return lang.Methods(path, source);
}

// Общий разбор вызова `x:<call>("имя")` — для emit и define_event.
static std::vector<std::string> ParseCallNames(const std::string& source, const char* call) {
    const size_t callLen = std::strlen(call);
    std::vector<std::string> out;
    const size_t n = source.size();
    for (size_t i = source.find(":"); i != std::string::npos && i < n; i = source.find(":", i + 1)) {
        size_t j = i + 1;
        while (j < n && (source[j] == ' ' || source[j] == '\t')) ++j;
        if (source.compare(j, callLen, call) != 0) continue;
        j += callLen;
        // «emitter» и подобные — не вызов emit.
        if (j < n && (std::isalnum((unsigned char)source[j]) || source[j] == '_')) continue;
        while (j < n && (source[j] == ' ' || source[j] == '\t')) ++j;
        if (j >= n || source[j] != '(') continue;
        ++j;
        while (j < n && (source[j] == ' ' || source[j] == '\t')) ++j;
        if (j >= n || (source[j] != '"' && source[j] != '\'')) continue; // имя переменной — не узнать
        const char quote = source[j++];
        const size_t start = j;
        while (j < n && source[j] != quote && source[j] != '\n') ++j;
        if (j >= n || source[j] != quote) continue;
        std::string name = source.substr(start, j - start);
        const bool valid = !name.empty() && std::none_of(name.begin(), name.end(), [](unsigned char c) {
            return std::isspace(c) || c < 32;
        });
        if (valid && std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
    }
    return out;
}



// `events.create(object, "name")` — первый аргумент любой (`object`,
// `scene.find("Door")`), имя — строка. Глобальная форма `events.create("name")`
// сигналом объекта не является и в список объекта не попадает.
static std::vector<std::string> ParseEventsCall(const std::string& source, const char* lowerHead,
                                                const char* upperHead) {
    std::vector<std::string> out;
    const size_t n = source.size();
    for (const char* head : {lowerHead, upperHead}) {
        const size_t headLen = std::strlen(head);
        for (size_t at = source.find(head); at != std::string::npos; at = source.find(head, at + 1)) {
            if (at > 0 && IdentChar(source[at - 1])) continue;
            size_t j = at + headLen;
            while (j < n && (source[j] == ' ' || source[j] == '\t')) ++j;
            if (j >= n || source[j] != '(') continue;
            ++j;
            while (j < n && (source[j] == ' ' || source[j] == '\t')) ++j;
            // Первый аргумент — строка: это глобальная форма (имя события, затем
            // данные), сигналом объекта она не является.
            if (j < n && (source[j] == '"' || source[j] == '\'')) continue;
            // Первый аргумент — до запятой верхнего уровня.
            int depth = 0;
            bool comma = false;
            for (; j < n && source[j] != '\n'; ++j) {
                const char c = source[j];
                if (c == '(' || c == '{' || c == '[') ++depth;
                else if (c == ')' || c == '}' || c == ']') {
                    if (depth == 0) break;
                    --depth;
                } else if (c == '"' || c == '\'') {
                    const char q = c;
                    for (++j; j < n && source[j] != q && source[j] != '\n'; ++j) {}
                } else if (c == ',' && depth == 0) { comma = true; ++j; break; }
            }
            if (!comma) continue;
            while (j < n && (source[j] == ' ' || source[j] == '\t')) ++j;
            if (j >= n || (source[j] != '"' && source[j] != '\'')) continue;
            const char quote = source[j++];
            const size_t start = j;
            while (j < n && source[j] != quote && source[j] != '\n') ++j;
            if (j >= n || source[j] != quote) continue;
            std::string name = source.substr(start, j - start);
            const bool valid = !name.empty() && std::none_of(name.begin(), name.end(), [](unsigned char c) {
                return std::isspace(c) || c < 32;
            });
            if (valid && std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
        }
    }
    return out;
}

// Сигналы, которые скрипт шлёт: `obj:emit("hit")` (прежнее) и
// `events.trigger(obj, "hit")` (стандарт).
std::vector<std::string> ParseEmittedSignals(const std::string& source) {
    std::vector<std::string> out = ParseEventsCall(source, "events.trigger", "Events.trigger");
    for (std::string& name : ParseCallNames(source, "emit"))
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
    return out;
}

std::vector<std::string> ParseDefinedEvents(const std::string& source) {
    // Стандарт: events.create(object, "имя"); прежнее: obj:define_event("имя").
    std::vector<std::string> out = ParseEventsCall(source, "events.create", "Events.create");
    for (std::string& name : ParseCallNames(source, "define_event"))
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
    return out;
}

} // namespace sage::scripting
