#include "Paths.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
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

// ---------------------------------------------------------------------------
//  Кодировки: система <-> UTF-8 (подробности — в Paths.h)
// ---------------------------------------------------------------------------
#if defined(_WIN32)
namespace {

// Широкая строка -> UTF-8. WideCharToMultiByte, а не codecvt: он не бросает
// и не зависит от текущей локали процесса.
std::string WideToUtf8(const wchar_t* w, int len) {
    if (!w || len == 0) return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out((size_t)need, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), need, nullptr, nullptr);
    return out;
}

// Узкая строка в заданной кодировке -> широкая. Пусто, если байты этой
// кодировке не соответствуют (флаг MB_ERR_INVALID_CHARS: без него функция
// молча подставляет «?» и отличить успех от порчи нельзя).
std::wstring NarrowToWide(std::string_view s, UINT codepage, bool strict) {
    if (s.empty()) return {};
    const DWORD flags = strict ? MB_ERR_INVALID_CHARS : 0;
    const int need = ::MultiByteToWideChar(codepage, flags, s.data(), (int)s.size(), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out((size_t)need, L'\0');
    ::MultiByteToWideChar(codepage, flags, s.data(), (int)s.size(), out.data(), need);
    return out;
}

} // namespace
#endif

std::string SystemToUtf8(const char* bytes) {
    if (!bytes || !*bytes) return {};
#if defined(_WIN32)
    // Если ANSI-кодировка процесса и так UTF-8 (манифест приложения, см.
    // cmake/windows/sage.manifest), обе перекодировки — тождественные, и
    // строка проходит насквозь.
    const std::wstring wide = NarrowToWide(bytes, CP_ACP, /*strict=*/false);
    if (wide.empty()) return bytes;
    return WideToUtf8(wide.c_str(), (int)wide.size());
#else
    return bytes;
#endif
}

std::string PathToUtf8(const fs::path& p) {
#if defined(_WIN32)
    const std::wstring& native = p.native();
    return WideToUtf8(native.c_str(), (int)native.size());
#else
    return p.native();
#endif
}

fs::path PathFromUtf8(std::string_view utf8) {
    if (utf8.empty()) return {};
#if defined(_WIN32)
    // Порядок попыток — от самого вероятного к самому терпимому:
    //   1) честный UTF-8 (так выглядит всё, что программа писала сама);
    //   2) ANSI — сюда попадает то, что пришло от системы мимо EnvPath;
    //   3) UTF-8 «как получится», с U+FFFD вместо мусора.
    // Третий шаг существует ради одного: НИ ПРИ КАКИХ байтах не бросить.
    // Испорченное имя файла — это ненайденный файл, и это переживаемо;
    // исключение отсюда убивало всю программу.
    if (std::wstring w = NarrowToWide(utf8, CP_UTF8, /*strict=*/true); !w.empty())
        return fs::path(std::move(w));
    if (std::wstring w = NarrowToWide(utf8, CP_ACP, /*strict=*/true); !w.empty())
        return fs::path(std::move(w));
    if (std::wstring w = NarrowToWide(utf8, CP_UTF8, /*strict=*/false); !w.empty())
        return fs::path(std::move(w));
    return {};
#else
    // На Unix путь — просто байты, перекодировать нечего и падать не на чем.
    return fs::path(std::string(utf8));
#endif
}

fs::path EnvPath(const char* name) {
    if (!name) return {};
#if defined(_WIN32)
    // _wgetenv, а НЕ getenv: узкое окружение приходит в ANSI, и путь с
    // кириллицей в нём — не UTF-8, то есть верная смерть конструктора path.
    if (const wchar_t* w = _wgetenv(NarrowToWide(name, CP_UTF8, false).c_str()))
        if (*w) return fs::path(w);
    return {};
#else
    if (const char* v = std::getenv(name)) {
        if (*v) return fs::path(v);
    }
    return {};
#endif
}

std::string EnvString(const char* name) {
#if defined(_WIN32)
    const fs::path p = EnvPath(name);
    return p.empty() ? std::string() : PathToUtf8(p);
#else
    if (const char* v = std::getenv(name)) return v;
    return {};
#endif
}

std::string EngineAssetPath(const std::string& relative) {
    const fs::path dir = ExecutableDir();
    if (dir.empty()) return relative;
    std::error_code ec;
    const fs::path candidate = dir / relative;
    if (fs::exists(candidate, ec)) return PathToUtf8(candidate);
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
    if (const fs::path env = EnvPath("USERPROFILE"); !env.empty()) {
        const fs::path guess = env / fallbackName;
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
    if (const fs::path p = EnvPath("USERPROFILE"); !p.empty()) return p;
    // HOMEDRIVE = «C:», HOMEPATH = «\\Users\\Вова» — их СКЛЕИВАЮТ, а не
    // соединяют через «/»: «C:» — это диск без корня, и operator/ дал бы
    // «C:Users/Вова», то есть путь относительно текущей папки на диске C.
    const fs::path drive = EnvPath("HOMEDRIVE");
    const fs::path rest = EnvPath("HOMEPATH");
    if (!drive.empty() && !rest.empty()) return fs::path(drive.native() + rest.native());
#else
    if (const fs::path p = EnvPath("HOME"); !p.empty()) return p;
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
