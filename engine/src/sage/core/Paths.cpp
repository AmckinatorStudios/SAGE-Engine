#include "Paths.h"

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

} // namespace sage
