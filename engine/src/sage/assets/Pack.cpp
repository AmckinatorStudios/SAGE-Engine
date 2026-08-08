#include "sage/assets/Pack.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>

#include "sage/assets/format/Blob.h"
#include "sage/core/Log.h"

namespace fs = std::filesystem;

namespace sage::assets {

namespace {

constexpr uint32_t kMagic = FourCC("SPAK");
constexpr uint32_t kVersion = 1;

// Числа пишутся ПОБАЙТНО, little-endian, как и в Blob. Не memcpy структуры:
// раскладка зависит от компилятора, а пакет переезжает между машинами всегда —
// собран он на машине разработчика, а читается у игрока.
void PutU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
}
void PutU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
}
void PutString(std::vector<uint8_t>& out, const std::string& s) {
    PutU32(out, (uint32_t)s.size());
    out.insert(out.end(), s.begin(), s.end());
}

uint32_t GetU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t GetU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (i * 8);
    return v;
}

// Путь внутри пакета: всегда прямой слэш, без ведущего "./".
std::string Normalize(const std::string& path) {
    std::string out;
    out.reserve(path.size());
    for (char c : path) out += (c == '\\') ? '/' : c;
    while (out.rfind("./", 0) == 0) out.erase(0, 2);
    while (!out.empty() && out.front() == '/') out.erase(0, 1);
    return out;
}

bool ReadWholeFile(const fs::path& file, std::vector<uint8_t>& out) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    if (size < 0) return false;
    in.seekg(0);
    out.resize((size_t)size);
    if (size > 0 && !in.read((char*)out.data(), size)) return false;
    return true;
}

} // namespace

// ============================================================================
//  PackWriter
// ============================================================================

void PackWriter::Add(const std::string& virtualPath, const std::vector<uint8_t>& data) {
    const std::string path = Normalize(virtualPath);
    if (path.empty()) return;
    // Повторное добавление того же пути ЗАМЕНЯЕТ прежнее содержимое. Два файла
    // с одним именем в пакете означали бы, что какой из них прочитают —
    // вопрос порядка обхода каталога, то есть случайность.
    for (Entry& e : m_entries) {
        if (e.Path == path) { e.Data = data; return; }
    }
    m_entries.push_back({path, data});
}

size_t PackWriter::AddDirectory(const fs::path& root, const std::vector<std::string>& skipSuffixes) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return 0;

    size_t added = 0;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string rel = Normalize(fs::relative(entry.path(), root, ec).generic_string());
        if (rel.empty()) continue;

        bool skip = false;
        for (const std::string& suffix : skipSuffixes) {
            if (rel.size() >= suffix.size() &&
                rel.compare(rel.size() - suffix.size(), suffix.size(), suffix) == 0) {
                skip = true;
                break;
            }
        }
        if (skip) continue;

        std::vector<uint8_t> data;
        if (!ReadWholeFile(entry.path(), data)) {
            LOG_WARN("Pack") << "не прочитался файл, пропускаю: " << entry.path().string();
            continue;
        }
        Add(rel, data);
        ++added;
    }
    return added;
}

bool PackWriter::Save(const fs::path& file) const {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        LOG_ERROR("Pack") << "не удалось создать пакет: " << file.string();
        return false;
    }

    // Заголовок пишется дважды: сейчас — заглушкой, чтобы занять место, и в
    // конце — по-настоящему, когда станет известно смещение оглавления.
    // Оглавление в конце потому, что размеры сжатых кусков заранее неизвестны.
    std::vector<uint8_t> header;
    PutU32(header, kMagic);
    PutU32(header, kVersion);
    PutU32(header, (uint32_t)m_entries.size());
    PutU32(header, 0);                 // выравнивание
    PutU64(header, 0);                 // смещение оглавления — заполним потом
    out.write((const char*)header.data(), (std::streamsize)header.size());

    struct Written {
        uint64_t Offset = 0, Stored = 0, Original = 0;
        bool Compressed = false;
    };
    std::vector<Written> written(m_entries.size());

    for (size_t i = 0; i < m_entries.size(); ++i) {
        const Entry& e = m_entries[i];
        Written& w = written[i];
        w.Offset = (uint64_t)out.tellp();
        w.Original = e.Data.size();

        // Жмём, только если СТАЛО МЕНЬШЕ. Уже сжатые файлы (png, ogg) от
        // deflate обычно растут, и записать их «сжатыми» значило бы платить
        // временем распаковки за увеличенный размер.
        std::vector<uint8_t> packed;
        if (e.Data.size() >= 64 && DeflateBytes(e.Data, packed) && packed.size() < e.Data.size()) {
            w.Compressed = true;
            w.Stored = packed.size();
            out.write((const char*)packed.data(), (std::streamsize)packed.size());
        } else {
            w.Stored = e.Data.size();
            if (!e.Data.empty()) {
                out.write((const char*)e.Data.data(), (std::streamsize)e.Data.size());
            }
        }
    }

    const uint64_t indexOffset = (uint64_t)out.tellp();
    std::vector<uint8_t> index;
    for (size_t i = 0; i < m_entries.size(); ++i) {
        PutString(index, m_entries[i].Path);
        PutU64(index, written[i].Offset);
        PutU64(index, written[i].Stored);
        PutU64(index, written[i].Original);
        PutU32(index, written[i].Compressed ? 1u : 0u);
    }
    out.write((const char*)index.data(), (std::streamsize)index.size());

    // Дописываем настоящее смещение оглавления в заголовок.
    out.seekp(16);
    std::vector<uint8_t> tail;
    PutU64(tail, indexOffset);
    out.write((const char*)tail.data(), (std::streamsize)tail.size());
    out.close();

    if (!out) {
        LOG_ERROR("Pack") << "запись пакета не завершилась: " << file.string();
        return false;
    }
    return true;
}

// ============================================================================
//  PackReader
// ============================================================================

bool PackReader::Open(const fs::path& file) {
    m_open = false;
    m_index.clear();

    std::ifstream in(file, std::ios::binary);
    if (!in) return false;

    uint8_t header[24]{};
    if (!in.read((char*)header, sizeof(header))) return false;
    if (GetU32(header) != kMagic) {
        LOG_ERROR("Pack") << "не пакет SAGE: " << file.string();
        return false;
    }
    const uint32_t version = GetU32(header + 4);
    if (version != kVersion) {
        // Версия формата — не «предупреждение». Прочитать пакет чужой версии по
        // старым правилам означает получить мусор, который выглядит как данные.
        LOG_ERROR("Pack") << "версия пакета " << version << " не поддерживается (ожидалась "
                          << kVersion << "): " << file.string();
        return false;
    }
    const uint32_t count = GetU32(header + 8);
    const uint64_t indexOffset = GetU64(header + 16);

    in.seekg((std::streamoff)indexOffset);
    if (!in) return false;

    m_index.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t lenBuf[4]{};
        if (!in.read((char*)lenBuf, 4)) return false;
        const uint32_t len = GetU32(lenBuf);
        std::string path(len, '\0');
        if (len && !in.read(path.data(), len)) return false;

        uint8_t rec[28]{};
        if (!in.read((char*)rec, sizeof(rec))) return false;
        IndexEntry e;
        e.Offset = GetU64(rec);
        e.Stored = GetU64(rec + 8);
        e.Original = GetU64(rec + 16);
        e.Compressed = GetU32(rec + 24) != 0;
        m_index.emplace_back(std::move(path), e);
    }

    // Оглавление сортируется по пути: поиск двоичный, а не перебором. На пакете
    // в тысячи файлов перебор на каждое чтение текстуры заметен.
    std::sort(m_index.begin(), m_index.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    m_file = file;
    m_open = true;
    return true;
}

bool PackReader::Contains(const std::string& virtualPath) const {
    if (!m_open) return false;
    const std::string path = Normalize(virtualPath);
    auto it = std::lower_bound(m_index.begin(), m_index.end(), path,
                               [](const auto& entry, const std::string& key) {
                                   return entry.first < key;
                               });
    return it != m_index.end() && it->first == path;
}

bool PackReader::Read(const std::string& virtualPath, std::vector<uint8_t>& out) const {
    out.clear();
    if (!m_open) return false;
    const std::string path = Normalize(virtualPath);

    auto it = std::lower_bound(m_index.begin(), m_index.end(), path,
                               [](const auto& entry, const std::string& key) {
                                   return entry.first < key;
                               });
    if (it == m_index.end() || it->first != path) return false;

    const IndexEntry& e = it->second;
    std::ifstream in(m_file, std::ios::binary);
    if (!in) return false;
    in.seekg((std::streamoff)e.Offset);

    std::vector<uint8_t> stored((size_t)e.Stored);
    if (e.Stored && !in.read((char*)stored.data(), (std::streamsize)e.Stored)) return false;

    if (!e.Compressed) {
        out = std::move(stored);
        return true;
    }
    return InflateBytes(stored, (size_t)e.Original, out);
}

std::vector<std::string> PackReader::Paths() const {
    std::vector<std::string> paths;
    paths.reserve(m_index.size());
    for (const auto& [path, entry] : m_index) paths.push_back(path);
    return paths;
}

// ============================================================================
//  vfs
// ============================================================================

namespace vfs {

namespace {
std::unique_ptr<PackReader> g_pack;
} // namespace

bool Mount(const fs::path& packFile) {
    Unmount();
    if (packFile.empty()) return false;
    std::error_code ec;
    if (!fs::exists(packFile, ec)) return false;   // не ошибка: так работает редактор

    auto reader = std::make_unique<PackReader>();
    if (!reader->Open(packFile)) return false;
    LOG_INFO("Pack") << "пакет подключён: " << packFile.filename().string() << " ("
                     << reader->Count() << " файлов)";
    g_pack = std::move(reader);
    return true;
}

void Unmount() { g_pack.reset(); }
bool Mounted() { return g_pack != nullptr; }

bool Exists(const std::string& path) {
    if (g_pack && g_pack->Contains(path)) return true;
    std::error_code ec;
    return fs::exists(path, ec);
}

std::vector<std::string> ListFiles(const std::string& directory, const std::string& extension) {
    std::vector<std::string> found;
    const std::string prefix = Normalize(directory).empty() ? "" : Normalize(directory) + "/";

    if (g_pack) {
        for (const std::string& path : g_pack->Paths()) {
            if (path.rfind(prefix, 0) != 0) continue;
            // Только НЕПОСРЕДСТВЕННО в каталоге: вложенные подпапки — не то, что
            // спрашивали, и класть их в один список значило бы, что «главной
            // сценой» может стать что-то из scenes/old/.
            if (path.find('/', prefix.size()) != std::string::npos) continue;
            if (extension.empty() ||
                (path.size() >= extension.size() &&
                 path.compare(path.size() - extension.size(), extension.size(), extension) == 0)) {
                found.push_back(path);
            }
        }
    } else {
        std::error_code ec;
        for (const fs::directory_entry& e : fs::directory_iterator(directory, ec)) {
            if (!e.is_regular_file(ec)) continue;
            const std::string path = Normalize(e.path().generic_string());
            if (extension.empty() || e.path().extension().string() == extension) {
                found.push_back(path);
            }
        }
    }
    // Порядок обхода каталога не определён, а «первая сцена по алфавиту» —
    // обещание движка. Сортируем здесь, чтобы вызывающие не помнили об этом.
    std::sort(found.begin(), found.end());
    return found;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    // Пакет ПЕРВЫМ. Рядом с собранной игрой могут остаться файлы от прошлой
    // сборки, и прочитать их вместо упакованных значило бы запустить вчерашнюю
    // логику с сегодняшними данными — дефект, который ищут очень долго.
    if (g_pack && g_pack->Read(path, out)) return true;
    return ReadWholeFile(path, out);
}

bool ReadText(const std::string& path, std::string& out) {
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, bytes)) return false;
    out.assign((const char*)bytes.data(), bytes.size());
    return true;
}

} // namespace vfs

} // namespace sage::assets
