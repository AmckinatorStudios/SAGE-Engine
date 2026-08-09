#include "sage/assets/AssetDatabase.h"
#include "sage/assets/Pack.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include "sage/core/Log.h"

namespace fs = std::filesystem;

namespace sage {

// ---------------------------------------------------------------------------
//  GUID
// ---------------------------------------------------------------------------

std::string AssetGuid::ToString() const {
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx", (unsigned long long)High,
                  (unsigned long long)Low);
    return buf;
}

AssetGuid AssetGuid::FromString(const std::string& text) {
    AssetGuid g;
    if (text.size() != 32) return g;   // битая строка -> невалидный, а не исключение
    auto hex = [](const std::string& s) -> uint64_t {
        uint64_t v = 0;
        for (char c : s) {
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint64_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint64_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint64_t)(c - 'A' + 10);
            else return 0;
        }
        return v;
    };
    g.High = hex(text.substr(0, 16));
    g.Low = hex(text.substr(16, 16));
    return g;
}

AssetGuid AssetGuid::Generate() {
    // Случайность плюс время: два проекта, созданные на разных машинах и позже
    // слитые вместе, не должны получить одинаковые номера. Полноценный UUIDv4
    // здесь избыточен — сталкиваться этим номерам негде, кроме одного проекта.
    static std::mt19937_64 rng([] {
        std::random_device rd;
        const uint64_t seed =
            ((uint64_t)rd() << 32) ^ (uint64_t)rd() ^
            (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
        return seed;
    }());
    AssetGuid g;
    do {
        g.High = rng();
        g.Low = rng();
    } while (!g.Valid());   // нулевой GUID означает «нет ссылки» и выдан быть не может
    return g;
}

// ---------------------------------------------------------------------------
//  Сайдкары
// ---------------------------------------------------------------------------

namespace {

std::string MetaPath(const std::string& assetPath) { return assetPath + ".meta"; }

// Пути в базе — относительные к проекту и через '/'. Иначе один и тот же файл
// на Windows и Linux выглядел бы как два разных, и сцена, сохранённая на одной
// машине, не находила бы ассеты на другой.
std::string Normalize(const std::string& path) {
    std::string out = path;
    std::replace(out.begin(), out.end(), '\\', '/');
    while (out.size() > 2 && out.compare(0, 2, "./") == 0) out.erase(0, 2);
    return out;
}

} // namespace

std::string AssetDatabase::TypeFromExtension(const std::string& path) {
    const std::string p = Normalize(path);
    const size_t dot = p.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = p.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    if (ext == "sagemat") return "material";
    if (ext == "sage") return "scene";
    if (ext == "lua") return "script";
    if (ext == "obj" || ext == "gltf" || ext == "glb") return "model";
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "tga" || ext == "bmp" ||
        ext == "hdr")
        return "texture";
    if (ext == "wav" || ext == "ogg" || ext == "mp3" || ext == "flac") return "audio";
    if (ext == "vert" || ext == "frag") return "shader";
    if (ext == "ttf" || ext == "otf") return "font";
    return "";
}

AssetGuid AssetDatabase::ReadMeta(const std::string& assetPath, std::string* outType) {
    std::ifstream f(MetaPath(assetPath));
    if (!f) return {};
    // Формат нарочно строчный и без библиотеки: сайдкар должен читаться глазами
    // в diff'е и не зависеть от парсера, который однажды поменяется.
    std::string line;
    AssetGuid guid;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
        if (key == "guid") guid = AssetGuid::FromString(value);
        else if (key == "type" && outType) *outType = value;
    }
    return guid;
}

bool AssetDatabase::WriteMeta(const std::string& assetPath, const AssetGuid& guid,
                              const std::string& type) {
    std::ofstream f(MetaPath(assetPath), std::ios::trunc);
    if (!f) return false;
    f << "sage_meta=1\n";
    f << "guid=" << guid.ToString() << "\n";
    if (!type.empty()) f << "type=" << type << "\n";
    return true;
}

// ---------------------------------------------------------------------------
//  База
// ---------------------------------------------------------------------------

AssetDatabase& AssetDatabase::Instance() {
    static AssetDatabase db;
    return db;
}

void AssetDatabase::Clear() {
    m_projectDir.clear();
    m_records.clear();
    m_byGuid.clear();
    m_byPath.clear();
    ClearDependencies();
    ClearBroken();
}

int AssetDatabase::ScanProject(const std::string& projectDir) {
    m_projectDir = Normalize(projectDir);
    std::error_code ec;
    if (!fs::is_directory(projectDir, ec)) {
        LOG_WARN("Assets") << "Каталог проекта недоступен: " << projectDir;
        return 0;
    }

    int found = 0;
    int created = 0;
    int moved = 0;
    for (const fs::directory_entry& entry :
         fs::recursive_directory_iterator(projectDir, fs::directory_options::skip_permission_denied,
                                          ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string full = Normalize(entry.path().string());
        if (full.size() > 5 && full.compare(full.size() - 5, 5, ".meta") == 0) continue;

        const std::string type = TypeFromExtension(full);
        if (type.empty()) continue;   // не ассет — исходники, README, .gitignore

        const std::string rel = Normalize(fs::relative(entry.path(), projectDir, ec).string());
        AssetGuid guid = ReadMeta(full);
        if (!guid.Valid()) {
            guid = AssetGuid::Generate();
            WriteMeta(full, guid, type);
            ++created;
        }

        auto it = m_byGuid.find(guid);
        if (it != m_byGuid.end()) {
            // GUID уже знаком — значит файл ПЕРЕЕХАЛ. Ровно ради этого случая
            // всё и затевалось: обновляем путь, и все ссылки остаются целы.
            AssetRecord& rec = m_records[it->second];
            if (rec.Path != rel) {
                m_byPath.erase(rec.Path);
                LOG_INFO("Assets") << "Ассет переехал: " << rec.Path << " -> " << rel;
                rec.Path = rel;
                m_byPath[rel] = it->second;
                ++moved;
            }
        } else {
            m_byGuid[guid] = m_records.size();
            m_byPath[rel] = m_records.size();
            m_records.push_back({guid, rel, type});
        }
        ++found;
    }

    LOG_INFO("Assets") << "База ассетов: " << found << " файлов (новых " << created
                       << ", переехавших " << moved << ") в " << m_projectDir;
    return found;
}

std::string AssetDatabase::PathOf(const AssetGuid& guid) const {
    auto it = m_byGuid.find(guid);
    return it == m_byGuid.end() ? std::string() : m_records[it->second].Path;
}

AssetGuid AssetDatabase::GuidOf(const std::string& path) const {
    auto it = m_byPath.find(Normalize(path));
    return it == m_byPath.end() ? AssetGuid{} : m_records[it->second].Guid;
}

AssetGuid AssetDatabase::Register(const std::string& path, const std::string& type) {
    const std::string rel = Normalize(path);
    auto it = m_byPath.find(rel);
    if (it != m_byPath.end()) return m_records[it->second].Guid;

    const std::string full = m_projectDir.empty() ? rel : m_projectDir + "/" + rel;
    const std::string kind = type.empty() ? TypeFromExtension(rel) : type;

    // Уже существующий сайдкар главнее: повторная регистрация не должна менять
    // личность файла — иначе ссылки на него разом протухли бы.
    AssetGuid guid = ReadMeta(full);
    if (!guid.Valid()) {
        guid = AssetGuid::Generate();
        WriteMeta(full, guid, kind);
    }
    m_byGuid[guid] = m_records.size();
    m_byPath[rel] = m_records.size();
    m_records.push_back({guid, rel, kind});
    return guid;
}

std::string AssetDatabase::LocatePath(const std::string& path) const {
    if (path.empty() || m_projectDir.empty()) return path;
    std::error_code ec;
    if (fs::exists(path, ec)) return path;
    // Абсолютный путь мимо проекта достраивать нечем и незачем.
    if (fs::path(path).is_absolute()) return path;
    const std::string full = m_projectDir + "/" + Normalize(path);
    if (fs::exists(full, ec)) return full;
    return path;
}

void AssetDatabase::ClearDependencies() {
    m_dependsOn.clear();
    m_dependedBy.clear();
}

void AssetDatabase::AddDependency(const AssetGuid& from, const AssetGuid& to) {
    if (!from.Valid() || !to.Valid() || from == to) return;
    std::vector<AssetGuid>& deps = m_dependsOn[from];
    if (std::find(deps.begin(), deps.end(), to) != deps.end()) return;  // без дубликатов
    deps.push_back(to);
    m_dependedBy[to].push_back(from);
}

std::vector<AssetGuid> AssetDatabase::Dependents(const AssetGuid& guid) const {
    auto it = m_dependedBy.find(guid);
    return it == m_dependedBy.end() ? std::vector<AssetGuid>{} : it->second;
}

std::vector<AssetGuid> AssetDatabase::Dependencies(const AssetGuid& guid) const {
    auto it = m_dependsOn.find(guid);
    return it == m_dependsOn.end() ? std::vector<AssetGuid>{} : it->second;
}

void AssetDatabase::NoteBroken(const AssetGuid& from, const AssetGuid& missing,
                               const std::string& hint) {
    for (const BrokenRef& b : m_broken)
        if (b.Missing == missing && b.From == from) return;   // не копим повторы
    m_broken.push_back({from, missing, hint});
    LOG_ERROR("Assets") << "Ссылка ведёт в никуда: " << (missing.Valid() ? missing.ToString() : "-")
                        << (hint.empty() ? "" : " (последний известный путь: " + hint + ")");
}

std::string AssetDatabase::Resolve(const AssetGuid& guid, const std::string& pathHint,
                                   const AssetGuid& referencedBy) {
    // 1. GUID знаком — только он знает, куда файл переехал.
    if (guid.Valid()) {
        const std::string byGuid = PathOf(guid);
        if (!byGuid.empty()) {
            if (referencedBy.Valid()) AddDependency(referencedBy, guid);
            return byGuid;
        }
    }

    // 2. GUID неизвестен, но путь есть — проект сделан до появления базы (или
    //    ассет добавили мимо неё). Заводим GUID и продолжаем работать: отказ
    //    здесь означал бы, что старые проекты просто не открываются.
    if (!pathHint.empty()) {
        const std::string rel = Normalize(pathHint);
        const std::string full = m_projectDir.empty() ? rel : m_projectDir + "/" + rel;
        std::error_code ec;
        // Через vfs — тоже: в СОБРАННОЙ игре проект лежит одним файлом
        // game.sagepak, файлов на диске нет, и проверка «есть ли такой путь»
        // отвечала «нет» вообще на всё. Сцена при этом работала (скрипт и меш
        // читаются через vfs), но каждая ссылка объявлялась битой и печатала
        // ОШИБКУ в лог — то есть собранная игра ругалась на саму себя.
        if (fs::exists(full, ec) || fs::exists(rel, ec) || assets::vfs::Exists(rel) ||
            assets::vfs::Exists(pathHint)) {
            const AssetGuid fresh = Register(rel);
            if (referencedBy.Valid()) AddDependency(referencedBy, fresh);
            // Отдаём путь РОВНО ТАКИМ, каким его дали. Нормализация нужна
            // ключам базы, а не вызывающему: вернув «причёсанный» путь, мы
            // молча меняем то, что записано в сцене, и сохранение перестаёт
            // совпадать с загрузкой. На этом сразу и попался self-test
            // редактора — путь "./x.sagemat" возвращался как "x.sagemat".
            return pathHint;
        }
    }

    // 3. Ничего. Молчать нельзя — молчание и было исходной болезнью: сцена
    //    загружалась, объект стоял на месте, просто без модели, и в логе не
    //    было ни строчки.
    if (guid.Valid() || !pathHint.empty()) NoteBroken(referencedBy, guid, pathHint);
    return {};
}

} // namespace sage
