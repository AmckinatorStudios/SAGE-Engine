#include "sage/core/AtomicFile.h"

#include <filesystem>
#include <fstream>

#include "sage/core/Paths.h"

namespace sage {

bool WriteFileAtomic(const std::string& utf8Path, const std::string& bytes, std::string& err) {
    const std::filesystem::path path = PathFromUtf8(utf8Path);
    const std::filesystem::path tmp = PathFromUtf8(utf8Path + ".tmp");
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) {
            err = "не удалось открыть на запись: " + utf8Path;
            return false;
        }
        file.write(bytes.data(), (std::streamsize)bytes.size());
        file.flush();
        if (!file) {
            err = "не удалось записать: " + utf8Path;
            std::error_code ignore;
            std::filesystem::remove(tmp, ignore);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // Windows не подменяет существующий файл переименованием — удалить и
        // переименовать.
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp, path, ec);
    }
    if (ec) {
        err = "не удалось заменить файл: " + utf8Path + " (" + ec.message() + ")";
        return false;
    }
    return true;
}

} // namespace sage
