#include "Paths.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#  include <vector>
#else
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace sage {

fs::path ExecutableDir() {
    // Считается ОДИН раз: путь к бинарнику за время работы процесса не
    // меняется, а системный вызов внутри — не бесплатный, и звать его из
    // загрузчика ресурсов на каждый файл было бы расточительно.
    static const fs::path cached = [] {
        std::error_code ec;
#if defined(_WIN32)
        wchar_t buf[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return fs::path();
        return fs::path(buf, buf + n).parent_path();
#elif defined(__APPLE__)
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size); // первый вызов сообщает нужный размер
        std::vector<char> buf(size + 1, '\0');
        if (_NSGetExecutablePath(buf.data(), &size) != 0) return fs::path();
        return fs::weakly_canonical(fs::path(buf.data()), ec).parent_path();
#else
        const fs::path self = fs::read_symlink("/proc/self/exe", ec);
        if (ec) return fs::path();
        return self.parent_path();
#endif
    }();
    return cached;
}

std::string EngineAssetPath(const std::string& relative) {
    const fs::path dir = ExecutableDir();
    if (dir.empty()) return relative;
    std::error_code ec;
    const fs::path candidate = dir / relative;
    if (fs::exists(candidate, ec)) return candidate.string();
    return relative;
}

// ---------------------------------------------------------------------------
//  Папки пользователя
// ---------------------------------------------------------------------------
namespace {

bool IsUsableDir(const fs::path& p) {
    if (p.empty()) return false;
    std::error_code ec;
    return fs::is_directory(p, ec);
}

#if !defined(_WIN32)
// XDG-папки лежат в ~/.config/user-dirs.dirs строками вида
//     XDG_DOWNLOAD_DIR="$HOME/Загрузки"
// Имена ЛОКАЛИЗОВАНЫ, поэтому угадать их по-английски нельзя: у русской
// системы это «Загрузки», у немецкой «Downloads» рядом с «Schreibtisch».
// Читаем то, что записано.
fs::path XdgUserDir(const char* key, const fs::path& home) {
    if (home.empty()) return {};
    std::error_code ec;
    const fs::path cfg = home / ".config" / "user-dirs.dirs";
    if (!fs::exists(cfg, ec)) return {};
    std::ifstream in(cfg);
    std::string line;
    const std::string prefix = std::string(key) + "=";
    while (std::getline(in, line)) {
        const size_t at = line.find(prefix);
        if (at == std::string::npos || line.find('#') < at) continue;
        std::string value = line.substr(at + prefix.size());
        // Значение в кавычках; $HOME разворачивается вручную.
        if (!value.empty() && value.front() == '"') value.erase(0, 1);
        if (!value.empty() && value.back() == '"') value.pop_back();
        if (value.rfind("$HOME", 0) == 0) return home / value.substr(6);
        if (!value.empty()) return fs::path(value);
    }
    return {};
}
#endif

// Папка пользователя по стандартному имени. Сначала спрашиваем систему, потом
// пробуем привычное имя рядом с домом — и в обоих случаях отдаём путь, только
// если такая папка ДЕЙСТВИТЕЛЬНО существует.
//
// На Windows берём USERPROFILE, а не SHGetKnownFolderPath: перенесённые
// «Документы» (на D:) так не находятся, но эта ветка собирается только
// кросс-компилятором в CI и здесь не проверяется ничем — тащить ради неё
// COM-зависимость (ole32/uuid) вслепую хуже, чем честно откатиться к
// USERPROFILE. Ошибиться не страшно: несуществующий путь отбрасывается, и
// кнопки в быстром доступе просто не будет.
fs::path KnownFolder(const char* xdgKey, const char* fallbackName, const fs::path& home) {
#if defined(_WIN32)
    (void)xdgKey;
    if (const char* env = std::getenv("USERPROFILE")) {
        const fs::path guess = fs::path(env) / fallbackName;
        if (IsUsableDir(guess)) return guess;
    }
#else
    if (const fs::path xdg = XdgUserDir(xdgKey, home); IsUsableDir(xdg)) return xdg;
#endif
    if (!home.empty()) {
        const fs::path guess = home / fallbackName;
        if (IsUsableDir(guess)) return guess;
    }
    return {};
}

} // namespace

// Не кэшируется, в отличие от ExecutableDir: это один getenv, зато результат
// остаётся честным, если переменную окружения подменили (так её проверяют
// тесты, и так же ведут себя запуски из-под другого пользователя).
fs::path HomeDir() {
#if defined(_WIN32)
    if (const char* p = std::getenv("USERPROFILE")) return fs::path(p);
    const char* drive = std::getenv("HOMEDRIVE");
    const char* rest = std::getenv("HOMEPATH");
    if (drive && rest) return fs::path(std::string(drive) + rest);
#else
    if (const char* p = std::getenv("HOME")) return fs::path(p);
#endif
    return fs::path();
}

std::vector<UserFolder> UserFolders() {
    const fs::path home = HomeDir();
    std::vector<UserFolder> out;
    auto add = [&](const char* label, const fs::path& p) {
        if (IsUsableDir(p)) out.push_back({label, p});
    };

    add("Домой", home);
    add("Рабочий стол", KnownFolder("XDG_DESKTOP_DIR", "Desktop", home));
    add("Документы", KnownFolder("XDG_DOCUMENTS_DIR", "Documents", home));
    add("Загрузки", KnownFolder("XDG_DOWNLOAD_DIR", "Downloads", home));
    add("Изображения", KnownFolder("XDG_PICTURES_DIR", "Pictures", home));
    add("Музыка", KnownFolder("XDG_MUSIC_DIR", "Music", home));
    add("Видео", KnownFolder("XDG_VIDEOS_DIR", "Videos", home));
    return out;
}

fs::path DefaultProjectsDir() {
    const fs::path home = HomeDir();
    const fs::path docs = KnownFolder("XDG_DOCUMENTS_DIR", "Documents", home);
    if (!docs.empty()) return docs / "SAGE Projects";
    if (!home.empty()) return home / "SAGE Projects";
    std::error_code ec;
    return fs::current_path(ec);
}

} // namespace sage
