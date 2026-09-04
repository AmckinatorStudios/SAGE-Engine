#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#include "sage/assets/import/Importer.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"

// ---------------------------------------------------------------------------
// Blender (.blend).
//
// ЧТО ЭТО ЗА ФОРМАТ. .blend — не формат обмена, а ДАМП ПАМЯТИ Blender'а. Файл
// состоит из блоков, каждый из которых — кусок кучи вместе с его адресом на
// момент сохранения; ссылки между объектами хранятся как указатели тех времён.
// Схему структур файл несёт в себе, в блоке DNA1: имена типов, поля, размеры.
// Именно поэтому Blender открывает файлы, сделанные версиями, о которых он не
// знал, — он читает описание структур из самого файла.
//
// Из этого следуют две вещи. Первая: разобрать контейнер и схему МОЖНО, и это
// делается надёжно — здесь оно и сделано. Вторая: смысл полей меняется от
// версии к версии, и «прочитать меш» — это не одна раскладка, а их история. В
// Blender 3.x позиции вершин переехали из MVert.co в обобщённые атрибуты, в 4.0
// MVert исчез. Обещать поддержку «любого .blend» значит обещать сопровождать
// чужую внутреннюю схему вечно.
//
// ПОЭТОМУ ДВА ПУТИ, в таком порядке:
//
//   1. Прямое чтение. Работает для несжатых файлов с классической раскладкой
//      (MVert/MPoly/MLoop, Blender до 4.0). Не требует ничего, кроме файла.
//   2. Мост через Blender. Если на машине есть сам Blender, импорт просит его
//      выгрузить сцену в .glb во временный каталог и читает уже её. Так делают
//      промышленные пайплайны, и это единственный способ, который переживает
//      смену внутренней схемы: конвертирует её тот, кто её и придумал.
//
// Если не сработало ничего — сообщение объясняет, ЧТО сделать: пересохранить
// без сжатия, поставить Blender или экспортировать в glTF руками. Ответ «не
// удалось загрузить» на формат такой сложности бесполезен.
// ---------------------------------------------------------------------------

namespace sage::assets {

namespace {

namespace fs = std::filesystem;

struct BlendHeader {
    bool Valid = false;
    int PointerSize = 8;
    bool LittleEndian = true;
    std::string Version;   // "403" -> 4.03
};

// Блок файла: код, размер, старый адрес, индекс структуры в SDNA, число экземпляров.
struct BlendBlock {
    char Code[5] = {0, 0, 0, 0, 0};
    uint64_t OldAddress = 0;
    uint32_t SdnaIndex = 0;
    uint32_t Count = 0;
    size_t DataOffset = 0;
    size_t DataSize = 0;
};

// Схема структур из блока DNA1.
struct Sdna {
    std::vector<std::string> Names;
    std::vector<std::string> Types;
    std::vector<uint16_t> TypeLengths;
    struct Field {
        uint16_t TypeIndex;
        uint16_t NameIndex;
    };
    struct Struct {
        uint16_t TypeIndex;
        std::vector<Field> Fields;
    };
    std::vector<Struct> Structs;

    int StructIndexByName(const std::string& name) const {
        for (size_t i = 0; i < Structs.size(); ++i)
            if (Structs[i].TypeIndex < Types.size() && Types[Structs[i].TypeIndex] == name)
                return (int)i;
        return -1;
    }
};

class Reader {
public:
    Reader(const std::vector<uint8_t>& b, bool little) : m_b(b), m_little(little) {}
    size_t Pos = 0;
    bool Bad = false;

    bool Has(size_t n) const { return Pos + n <= m_b.size(); }
    uint8_t U8() {
        if (!Has(1)) { Bad = true; return 0; }
        return m_b[Pos++];
    }
    uint16_t U16() {
        if (!Has(2)) { Bad = true; return 0; }
        const uint8_t a = m_b[Pos], b = m_b[Pos + 1];
        Pos += 2;
        return m_little ? (uint16_t)(a | (b << 8)) : (uint16_t)(b | (a << 8));
    }
    uint32_t U32() {
        if (!Has(4)) { Bad = true; return 0; }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const uint8_t byte = m_b[Pos + (m_little ? i : 3 - i)];
            v |= (uint32_t)byte << (i * 8);
        }
        Pos += 4;
        return v;
    }
    uint64_t U64() {
        if (!Has(8)) { Bad = true; return 0; }
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            const uint8_t byte = m_b[Pos + (m_little ? i : 7 - i)];
            v |= (uint64_t)byte << (i * 8);
        }
        Pos += 8;
        return v;
    }
    float F32() {
        const uint32_t bits = U32();
        float f = 0.0f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    uint64_t Ptr(int size) { return size == 8 ? U64() : (uint64_t)U32(); }
    std::string CStr() {
        std::string s;
        while (Has(1)) {
            const char c = (char)m_b[Pos++];
            if (c == 0) return s;
            s.push_back(c);
        }
        Bad = true;
        return s;
    }
    void Align4() { Pos = (Pos + 3) & ~size_t(3); }

private:
    const std::vector<uint8_t>& m_b;
    bool m_little;
};

BlendHeader ParseHeader(const std::vector<uint8_t>& bytes) {
    BlendHeader h;
    if (bytes.size() < 12) return h;
    if (std::memcmp(bytes.data(), "BLENDER", 7) != 0) return h;
    const char ptr = (char)bytes[7];
    const char endian = (char)bytes[8];
    if (ptr != '_' && ptr != '-') return h;
    if (endian != 'v' && endian != 'V') return h;
    h.PointerSize = (ptr == '-') ? 8 : 4;
    h.LittleEndian = (endian == 'v');
    h.Version.assign((const char*)bytes.data() + 9, 3);
    h.Valid = true;
    return h;
}

bool ParseSdna(const std::vector<uint8_t>& bytes, const BlendBlock& block, bool little,
               Sdna& out, std::string& err) {
    Reader r(bytes, little);
    r.Pos = block.DataOffset;
    char tag[5] = {0};
    auto readTag = [&](const char* expect) {
        for (int i = 0; i < 4; ++i) tag[i] = (char)r.U8();
        if (std::memcmp(tag, expect, 4) != 0) {
            err = std::string("в блоке DNA1 ожидался тег ") + expect;
            return false;
        }
        return true;
    };
    if (!readTag("SDNA")) return false;
    if (!readTag("NAME")) return false;
    const uint32_t nameCount = r.U32();
    out.Names.reserve(nameCount);
    for (uint32_t i = 0; i < nameCount && !r.Bad; ++i) out.Names.push_back(r.CStr());
    r.Align4();
    if (!readTag("TYPE")) return false;
    const uint32_t typeCount = r.U32();
    out.Types.reserve(typeCount);
    for (uint32_t i = 0; i < typeCount && !r.Bad; ++i) out.Types.push_back(r.CStr());
    r.Align4();
    if (!readTag("TLEN")) return false;
    out.TypeLengths.resize(typeCount);
    for (uint32_t i = 0; i < typeCount && !r.Bad; ++i) out.TypeLengths[i] = r.U16();
    r.Align4();
    if (!readTag("STRC")) return false;
    const uint32_t structCount = r.U32();
    out.Structs.reserve(structCount);
    for (uint32_t i = 0; i < structCount && !r.Bad; ++i) {
        Sdna::Struct s;
        s.TypeIndex = r.U16();
        const uint16_t fieldCount = r.U16();
        s.Fields.reserve(fieldCount);
        for (uint16_t fi = 0; fi < fieldCount && !r.Bad; ++fi) {
            Sdna::Field f;
            f.TypeIndex = r.U16();
            f.NameIndex = r.U16();
            s.Fields.push_back(f);
        }
        out.Structs.push_back(std::move(s));
    }
    if (r.Bad) {
        err = "блок DNA1 обрывается — файл повреждён";
        return false;
    }
    return true;
}

// Размер поля по его имени: имя в SDNA несёт указатели и массивы прямо в тексте
// («*next», «co[3]», «(*func)()»), потому что так их и объявляли в исходниках.
size_t FieldSize(const Sdna& dna, const Sdna::Field& f, int pointerSize) {
    const std::string& name = dna.Names[f.NameIndex];
    size_t base;
    if (!name.empty() && (name[0] == '*' || name.find("(*") == 0)) {
        base = (size_t)pointerSize;
    } else {
        base = f.TypeIndex < dna.TypeLengths.size() ? dna.TypeLengths[f.TypeIndex] : 0;
    }
    // Массивы: перемножаем все размерности из имени.
    size_t count = 1;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] != '[') continue;
        size_t j = i + 1;
        size_t n = 0;
        while (j < name.size() && name[j] >= '0' && name[j] <= '9') n = n * 10 + (name[j++] - '0');
        if (n > 0) count *= n;
        i = j;
    }
    return base * count;
}

// Смещение поля внутри структуры. -1, если поля нет — так и проверяется, знает
// ли эта версия Blender'а нужную нам раскладку.
long FieldOffset(const Sdna& dna, const Sdna::Struct& s, const std::string& fieldName,
                 int pointerSize, size_t* outSize = nullptr) {
    size_t offset = 0;
    for (const Sdna::Field& f : s.Fields) {
        const size_t size = FieldSize(dna, f, pointerSize);
        if (f.NameIndex < dna.Names.size()) {
            std::string n = dna.Names[f.NameIndex];
            // Обрезаем украшения имени, чтобы сравнивать с «co», а не с «co[3]».
            const size_t br = n.find('[');
            if (br != std::string::npos) n = n.substr(0, br);
            while (!n.empty() && (n[0] == '*' || n[0] == '(')) n.erase(n.begin());
            if (n == fieldName) {
                if (outSize) *outSize = size;
                return (long)offset;
            }
        }
        offset += size;
    }
    return -1;
}

bool LooksCompressed(const std::vector<uint8_t>& b) {
    if (b.size() < 4) return false;
    // Zstd (Blender 3.0+ по умолчанию) и gzip (старое сжатие).
    const bool zstd = b[0] == 0x28 && b[1] == 0xB5 && b[2] == 0x2F && b[3] == 0xFD;
    const bool gzip = b[0] == 0x1F && b[1] == 0x8B;
    return zstd || gzip;
}

// --- Мост через сам Blender ---------------------------------------------------

std::string FindBlenderExecutable() {
    // Явное указание побеждает поиск: на машине может стоять несколько версий, и
    // выбирать за человека мы не вправе.
    // EnvPath, а не getenv: путь к Blender вполне может лежать в папке с
    // кириллицей, а узкое окружение Windows отдаёт её байтами ANSI — на них
    // конструктор fs::path бросает исключение (см. Paths.h).
    if (const fs::path env = sage::EnvPath("SAGE_BLENDER"); !env.empty()) {
        std::error_code ec;
        if (fs::exists(env, ec)) return sage::PathToUtf8(env);
    }
#if defined(_WIN32)
    const char* probe = "where blender >nul 2>&1";
    const char* name = "blender";
#else
    const char* probe = "command -v blender >/dev/null 2>&1";
    const char* name = "blender";
#endif
    if (std::system(probe) == 0) return name;
    return {};
}

bool ConvertViaBlender(const std::string& blender, const std::string& blendPath,
                       const std::string& outGlb, std::string& err) {
    // --factory-startup: чужие настройки и аддоны пользователя не должны влиять
    // на результат конвертации. --background: без окна.
    std::string cmd = "\"" + blender + "\" --background --factory-startup \"" + blendPath +
                      "\" --python-expr \"import bpy; bpy.ops.export_scene.gltf(filepath=r'" +
                      outGlb + "', export_format='GLB')\"";
#if defined(_WIN32)
    cmd += " >nul 2>&1";
    cmd = "\"" + cmd + "\"";   // cmd.exe съедает кавычки без внешней пары
#else
    cmd += " >/dev/null 2>&1";
#endif
    const int rc = std::system(cmd.c_str());
    if (rc != 0 || !fs::exists(outGlb)) {
        err = "Blender не смог выгрузить сцену (код " + std::to_string(rc) + ")";
        return false;
    }
    return true;
}

} // namespace

bool ImportBlend(const std::string& path, ImportedScene& out, std::string& err) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        err = "не открыть файл: " + path;
        return false;
    }
    const std::streamsize size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> bytes((size_t)std::max<std::streamsize>(size, 0));
    if (size > 0) f.read((char*)bytes.data(), size);

    const bool compressed = LooksCompressed(bytes);
    BlendHeader header;
    if (!compressed) header = ParseHeader(bytes);

    // --- Путь 1: прямое чтение --------------------------------------------
    std::string directErr;
    if (header.Valid) {
        std::vector<BlendBlock> blocks;
        Sdna dna;
        bool haveDna = false;

        Reader r(bytes, header.LittleEndian);
        r.Pos = 12;
        while (r.Has(4)) {
            BlendBlock b;
            for (int i = 0; i < 4; ++i) b.Code[i] = (char)r.U8();
            if (std::memcmp(b.Code, "ENDB", 4) == 0) break;
            b.DataSize = r.U32();
            b.OldAddress = r.Ptr(header.PointerSize);
            b.SdnaIndex = r.U32();
            b.Count = r.U32();
            b.DataOffset = r.Pos;
            if (r.Bad || b.DataOffset + b.DataSize > bytes.size()) {
                directErr = "файл обрывается на блоке " + std::string(b.Code);
                blocks.clear();
                break;
            }
            r.Pos += b.DataSize;
            if (std::memcmp(b.Code, "DNA1", 4) == 0) {
                std::string dnaErr;
                haveDna = ParseSdna(bytes, b, header.LittleEndian, dna, dnaErr);
                if (!haveDna) directErr = dnaErr;
            }
            blocks.push_back(b);
        }

        if (haveDna && !blocks.empty()) {
            // Блоки адресуются по СТАРОМУ адресу: ссылки внутри файла — это
            // указатели времён сохранения, и других имён у данных там нет.
            std::unordered_map<uint64_t, const BlendBlock*> byAddress;
            for (const BlendBlock& b : blocks) byAddress[b.OldAddress] = &b;

            const int meshStruct = dna.StructIndexByName("Mesh");
            const int mvertStruct = dna.StructIndexByName("MVert");
            const int mpolyStruct = dna.StructIndexByName("MPoly");
            const int mloopStruct = dna.StructIndexByName("MLoop");

            if (meshStruct < 0 || mvertStruct < 0 || mpolyStruct < 0 || mloopStruct < 0) {
                // Это ровно тот случай, ради которого существует путь 2: схема
                // новее той, что мы умеем читать напрямую.
                directErr = "в файле нет классических структур MVert/MPoly/MLoop "
                            "(Blender 4.0+ хранит геометрию иначе)";
            } else {
                const Sdna::Struct& ms = dna.Structs[meshStruct];
                size_t idSize = 0;
                const long offTotvert = FieldOffset(dna, ms, "totvert", header.PointerSize);
                const long offTotpoly = FieldOffset(dna, ms, "totpoly", header.PointerSize);
                const long offTotloop = FieldOffset(dna, ms, "totloop", header.PointerSize);
                const long offMvert = FieldOffset(dna, ms, "mvert", header.PointerSize);
                const long offMpoly = FieldOffset(dna, ms, "mpoly", header.PointerSize);
                const long offMloop = FieldOffset(dna, ms, "mloop", header.PointerSize);
                const long offId = FieldOffset(dna, ms, "id", header.PointerSize, &idSize);

                if (offMvert < 0 || offMpoly < 0 || offMloop < 0) {
                    directErr = "структура Mesh в этом файле не содержит полей mvert/mpoly/mloop "
                                "(геометрия хранится в атрибутах — нужен путь через Blender)";
                } else {
                    const Sdna::Struct& vs = dna.Structs[mvertStruct];
                    const long offCo = FieldOffset(dna, vs, "co", header.PointerSize);
                    const Sdna::Struct& ps = dna.Structs[mpolyStruct];
                    const long offLoopstart = FieldOffset(dna, ps, "loopstart", header.PointerSize);
                    const long offTotloopP = FieldOffset(dna, ps, "totloop", header.PointerSize);
                    const Sdna::Struct& lsq = dna.Structs[mloopStruct];
                    const long offV = FieldOffset(dna, lsq, "v", header.PointerSize);

                    const size_t vertStride = dna.TypeLengths[vs.TypeIndex];
                    const size_t polyStride = dna.TypeLengths[ps.TypeIndex];
                    const size_t loopStride = dna.TypeLengths[lsq.TypeIndex];

                    if (offCo < 0 || offLoopstart < 0 || offTotloopP < 0 || offV < 0) {
                        directErr = "не найдены поля геометрии внутри MVert/MPoly/MLoop";
                    } else {
                        for (const BlendBlock& b : blocks) {
                            if (std::memcmp(b.Code, "ME\0\0", 4) != 0) continue;
                            Reader mr(bytes, header.LittleEndian);

                            auto readI32At = [&](size_t off) {
                                mr.Pos = off;
                                return (int32_t)mr.U32();
                            };
                            auto readPtrAt = [&](size_t off) {
                                mr.Pos = off;
                                return mr.Ptr(header.PointerSize);
                            };

                            const int32_t totvert =
                                offTotvert >= 0 ? readI32At(b.DataOffset + offTotvert) : 0;
                            const int32_t totpoly =
                                offTotpoly >= 0 ? readI32At(b.DataOffset + offTotpoly) : 0;
                            const int32_t totloop =
                                offTotloop >= 0 ? readI32At(b.DataOffset + offTotloop) : 0;
                            if (totvert <= 0 || totpoly <= 0 || totloop <= 0) continue;

                            const uint64_t pVert = readPtrAt(b.DataOffset + offMvert);
                            const uint64_t pPoly = readPtrAt(b.DataOffset + offMpoly);
                            const uint64_t pLoop = readPtrAt(b.DataOffset + offMloop);
                            auto vIt = byAddress.find(pVert);
                            auto pIt = byAddress.find(pPoly);
                            auto lIt = byAddress.find(pLoop);
                            if (vIt == byAddress.end() || pIt == byAddress.end() ||
                                lIt == byAddress.end())
                                continue;

                            ImportedNode node;
                            node.Name = "Mesh";
                            if (offId >= 0) {
                                // ID.name — двухбуквенный префикс типа плюс имя.
                                const long offName =
                                    FieldOffset(dna, dna.Structs[dna.StructIndexByName("ID")],
                                                "name", header.PointerSize);
                                if (offName >= 0) {
                                    Reader nr(bytes, header.LittleEndian);
                                    nr.Pos = b.DataOffset + offId + offName;
                                    std::string raw = nr.CStr();
                                    node.Name = raw.size() > 2 ? raw.substr(2) : raw;
                                }
                            }

                            node.Mesh.Vertices.resize((size_t)totvert);
                            for (int32_t i = 0; i < totvert; ++i) {
                                Reader vr(bytes, header.LittleEndian);
                                vr.Pos = vIt->second->DataOffset + (size_t)i * vertStride + offCo;
                                const float x = vr.F32(), y = vr.F32(), z = vr.F32();
                                // Blender: Z вверх, правая тройка. Движок: Y вверх.
                                node.Mesh.Vertices[i].Position = glm::vec3(x, z, -y);
                                node.Mesh.Vertices[i].Normal = glm::vec3(0.0f, 1.0f, 0.0f);
                            }

                            for (int32_t p = 0; p < totpoly; ++p) {
                                Reader pr(bytes, header.LittleEndian);
                                pr.Pos = pIt->second->DataOffset + (size_t)p * polyStride;
                                pr.Pos += offLoopstart;
                                const int32_t loopstart = (int32_t)pr.U32();
                                pr.Pos = pIt->second->DataOffset + (size_t)p * polyStride +
                                         offTotloopP;
                                const int32_t nloops = (int32_t)pr.U32();
                                if (nloops < 3 || loopstart < 0 || loopstart + nloops > totloop)
                                    continue;
                                // Веер треугольников: грани Blender'а выпуклые,
                                // и для них веер даёт правильную триангуляцию.
                                std::vector<uint32_t> vidx((size_t)nloops);
                                for (int32_t k = 0; k < nloops; ++k) {
                                    Reader lr(bytes, header.LittleEndian);
                                    lr.Pos = lIt->second->DataOffset +
                                             (size_t)(loopstart + k) * loopStride + offV;
                                    vidx[(size_t)k] = lr.U32();
                                }
                                for (int32_t k = 1; k + 1 < nloops; ++k) {
                                    if (vidx[0] >= (uint32_t)totvert ||
                                        vidx[(size_t)k] >= (uint32_t)totvert ||
                                        vidx[(size_t)k + 1] >= (uint32_t)totvert)
                                        continue;
                                    node.Mesh.Indices.push_back(vidx[0]);
                                    node.Mesh.Indices.push_back(vidx[(size_t)k + 1]);
                                    node.Mesh.Indices.push_back(vidx[(size_t)k]);
                                }
                            }

                            if (node.Mesh.Empty()) continue;
                            // Нормали Blender хранит отдельно и по-разному в
                            // разных версиях — считаем свои по граням: это
                            // дешевле, чем ещё одна зависимость от чужой схемы.
                            for (size_t i = 0; i + 2 < node.Mesh.Indices.size(); i += 3) {
                                const unsigned a = node.Mesh.Indices[i];
                                const unsigned b2 = node.Mesh.Indices[i + 1];
                                const unsigned c = node.Mesh.Indices[i + 2];
                                const glm::vec3 n = glm::cross(
                                    node.Mesh.Vertices[b2].Position - node.Mesh.Vertices[a].Position,
                                    node.Mesh.Vertices[c].Position - node.Mesh.Vertices[a].Position);
                                node.Mesh.Vertices[a].Normal += n;
                                node.Mesh.Vertices[b2].Normal += n;
                                node.Mesh.Vertices[c].Normal += n;
                            }
                            for (Vertex& v : node.Mesh.Vertices) {
                                const float l = glm::length(v.Normal);
                                v.Normal = l > 1e-12f ? v.Normal / l : glm::vec3(0, 1, 0);
                            }
                            out.Nodes.push_back(std::move(node));
                        }
                    }
                }
            }
        }

        if (!out.Nodes.empty()) {
            out.Warnings.push_back("прочитано напрямую из .blend " + header.Version +
                                   " (UV и материалы из .blend не читаются — "
                                   "для полного переноса используйте glTF или Blender)");
            return true;
        }
    }

    // --- Путь 2: попросить сам Blender ------------------------------------
    const std::string blender = FindBlenderExecutable();
    if (!blender.empty()) {
        std::error_code ec;
        const fs::path tmp = fs::temp_directory_path(ec) /
                             ("sage_blend_" + std::to_string((unsigned long long)std::rand()) + ".glb");
        std::string convErr;
        if (ConvertViaBlender(blender, path, tmp.string(), convErr)) {
            std::string glbErr;
            const bool ok = ImportGltf(tmp.string(), out, glbErr);
            fs::remove(tmp, ec);
            if (ok) {
                out.Warnings.push_back("файл сконвертирован установленным Blender'ом через glTF");
                return true;
            }
            err = "Blender выгрузил сцену, но она не читается: " + glbErr;
            return false;
        }
        err = convErr;
        return false;
    }

    // --- Ничего не вышло: объясняем, что делать ----------------------------
    if (compressed) {
        err = "файл .blend сжат (Blender 3.0+ жмёт по умолчанию). Варианты: пересохранить "
              "с выключенной галочкой Compress, установить Blender (движок тогда "
              "сконвертирует сам) или экспортировать в .glb из Blender.";
    } else if (!header.Valid) {
        err = "это не файл Blender (нет сигнатуры BLENDER в начале)";
    } else {
        err = "не удалось прочитать геометрию напрямую" +
              (directErr.empty() ? std::string() : (": " + directErr)) +
              ". Установите Blender (движок сконвертирует сам, путь можно задать "
              "переменной SAGE_BLENDER) или экспортируйте модель в .glb.";
    }
    return false;
}

} // namespace sage::assets
