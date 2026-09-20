#include "sage/assets/import/TextureResolve.h"

#include <algorithm>
#include <cctype>

#include "sage/core/Paths.h"

namespace fs = std::filesystem;

namespace sage::assets {

namespace {

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Соседние папки, куда экспортёры кладут набор карт. Список короткий и
// намеренно не «любая подпапка»: обход всего дерева проекта ради одной
// ненайденной картинки стоит секунд на больших наборах, а ошибиться именем в
// чужой папке проще, чем кажется.
const char* const kTextureDirs[] = {"textures", "texture", "tex", "maps", "materials",
                                    "images",   "img",     "source"};

bool ExistsFile(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec) && fs::is_regular_file(p, ec);
}

// Файл с таким именем в этой папке, БЕЗ учёта регистра. Обход папки, а не
// подбор вариантов написания: вариантов у «Body_Normal.PNG» слишком много, а
// записей в папке — десятки.
fs::path FindIgnoringCase(const fs::path& dir, const std::string& fileName) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};
    const std::string want = Lower(sage::PathToUtf8(fs::path(fileName).filename()));
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (Lower(sage::PathToUtf8(entry.path().filename())) == want) return entry.path();
    }
    return {};
}

} // namespace

std::string DecodeUri(const std::string& uri) {
    std::string out;
    out.reserve(uri.size());
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            const int hi = HexDigit(uri[i + 1]), lo = HexDigit(uri[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(uri[i]);
    }
    return out;
}

std::string ResolveTexturePath(const fs::path& modelDir, const std::string& reference,
                               std::vector<std::string>* warnings) {
    if (reference.empty()) return {};
    // data:... — картинка лежит В САМОМ файле модели, искать её на диске нечего.
    // Это не потеря: встроенные картинки распаковывает тот, кто их читает.
    if (reference.rfind("data:", 0) == 0) return {};

    std::string ref = DecodeUri(reference);
    // Обратные слэши — от экспортёров под Windows. Прямой понимают обе системы.
    std::replace(ref.begin(), ref.end(), '\\', '/');

    const fs::path refPath = sage::PathFromUtf8(ref);
    const std::string fileName = sage::PathToUtf8(refPath.filename());
    if (fileName.empty()) return {};

    // 1. КАК СКАЗАНО. Сначала относительно модели, потом как есть (абсолютный
    // путь тоже бывает верным — у модели, которую не переносили).
    if (ExistsFile(modelDir / refPath)) return sage::PathToUtf8(modelDir / refPath);
    if (refPath.is_absolute() && ExistsFile(refPath)) return sage::PathToUtf8(refPath);

    // 2. ПО ИМЕНИ РЯДОМ С МОДЕЛЬЮ. Абсолютный путь чужой машины и ссылка в
    // несуществующую папку лечатся одинаково: имя файла в ссылке верное, а
    // дорога к нему — нет.
    std::vector<fs::path> roots;
    roots.push_back(modelDir);
    // Папка, названная в самой ссылке («../textures/body.png» -> textures рядом
    // с моделью): экспортёр знал, как называется набор, просто положил его
    // относительно другого корня.
    const fs::path refDir = refPath.parent_path().filename();
    if (!refDir.empty() && refDir != "." && refDir != "..") roots.push_back(modelDir / refDir);
    for (const char* dir : kTextureDirs) roots.push_back(modelDir / dir);
    // Уровень выше: модель нередко лежит в своей папке, а текстуры — общие для
    // набора. Выше одного уровня не поднимаемся: там уже чужие ассеты проекта.
    const fs::path up = modelDir.parent_path();
    if (!up.empty()) {
        roots.push_back(up);
        for (const char* dir : kTextureDirs) roots.push_back(up / dir);
    }

    for (const fs::path& root : roots) {
        const fs::path direct = root / sage::PathFromUtf8(fileName);
        if (ExistsFile(direct)) return sage::PathToUtf8(direct);
    }
    // 3. БЕЗ УЧЁТА РЕГИСТРА — последним: он дороже (обход папки) и рискованнее
    // (в одной папке могут лежать Body.png и body.png), поэтому сперва честный
    // поиск по точному имени.
    for (const fs::path& root : roots) {
        const fs::path found = FindIgnoringCase(root, fileName);
        if (!found.empty()) return sage::PathToUtf8(found);
    }

    if (warnings) warnings->push_back("текстура не найдена: " + reference);
    return {};
}

} // namespace sage::assets
