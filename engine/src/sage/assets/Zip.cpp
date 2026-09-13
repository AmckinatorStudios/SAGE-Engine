#include "sage/assets/Zip.h"

#include <cstdio>
#include <fstream>

#include <miniz.h>

#include "sage/core/Log.h"
#include "sage/core/Paths.h"

namespace fs = std::filesystem;

namespace sage::assets {

namespace {

// Безопасно ли распаковывать запись с таким именем. Путь обязан остаться
// ВНУТРИ папки назначения; всё остальное — отказ.
bool SafeEntryPath(const std::string& name, fs::path& out) {
    if (name.empty()) return false;
    // Абсолютный путь и корень диска («/etc/passwd», «C:\Windows\...»)
    // распаковались бы мимо папки назначения целиком.
    if (name.front() == '/' || name.front() == '\\') return false;
    if (name.size() > 1 && name[1] == ':') return false;

    fs::path rel;
    std::string part;
    auto flush = [&]() {
        if (part.empty() || part == ".") { part.clear(); return true; }
        // «..» — ровно тот приём, которым выходят наружу.
        if (part == "..") return false;
        rel /= sage::PathFromUtf8(part);
        part.clear();
        return true;
    };
    for (char c : name) {
        if (c == '/' || c == '\\') {
            if (!flush()) return false;
        } else {
            part.push_back(c);
        }
    }
    if (!flush()) return false;
    if (rel.empty()) return false;
    out = rel;
    return true;
}

} // namespace

bool IsZip(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    char sig[4] = {0, 0, 0, 0};
    in.read(sig, 4);
    if (in.gcount() < 4) return false;
    // "PK\3\4" — обычный архив, "PK\5\6" — пустой, "PK\7\8" — составной.
    return sig[0] == 'P' && sig[1] == 'K' && sig[2] >= 3 && sig[2] <= 7;
}

int ExtractZip(const fs::path& archive, const fs::path& destDir, std::string& err,
               std::vector<std::string>* skipped) {
    mz_zip_archive zip{};
    // Путь к архиву отдаётся miniz УЗКОЙ строкой, и на русской Windows это не
    // мелочь: сборка несёт манифест с activeCodePage=UTF-8 (см.
    // cmake/windows/sage.manifest), поэтому узкие функции системы понимают
    // UTF-8, и PathToUtf8 — именно то, что им нужно.
    const std::string archiveUtf8 = sage::PathToUtf8(archive);
    if (!mz_zip_reader_init_file(&zip, archiveUtf8.c_str(), 0)) {
        err = "архив не читается";
        return -1;
    }

    std::error_code ec;
    fs::create_directories(destDir, ec);
    if (ec) {
        mz_zip_reader_end(&zip);
        err = "папка назначения недоступна: " + ec.message();
        return -1;
    }

    int written = 0;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;

        fs::path rel;
        if (!SafeEntryPath(st.m_filename, rel)) {
            if (skipped) skipped->push_back(st.m_filename);
            LOG_WARN("Assets") << "Архив: запись с опасным путём пропущена — " << st.m_filename;
            continue;
        }
        const fs::path target = destDir / rel;

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            fs::create_directories(target, ec);
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        // Существующий файл НЕ перезаписываем — по той же причине, что и при
        // обычном импорте: повторная распаковка не должна затирать текстуру,
        // которую после первого раза поправили.
        if (fs::exists(target, ec)) continue;
        if (!mz_zip_reader_extract_to_file(&zip, i, sage::PathToUtf8(target).c_str(), 0)) {
            LOG_WARN("Assets") << "Архив: запись не распаковалась — " << st.m_filename;
            if (skipped) skipped->push_back(st.m_filename);
            continue;
        }
        ++written;
    }
    mz_zip_reader_end(&zip);

    if (written == 0 && count > 0 && skipped && !skipped->empty()) {
        err = "ни одна запись архива не распаковалась";
        return -1;
    }
    return written;
}

} // namespace sage::assets
