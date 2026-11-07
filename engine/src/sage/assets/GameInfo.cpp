#include "sage/assets/GameInfo.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>
#include <stb_image.h>        // реализация — в render/Texture.cpp
#include <stb_image_write.h>  // реализация — в render/Screenshot.cpp

#include "sage/assets/AssetDatabase.h"
#include "sage/core/EngineContext.h"
#include "sage/assets/Pack.h"
#include "sage/core/Paths.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace sage::assets {

GameInfo GameInfo::FromProject(const nlohmann::json& project) {
    GameInfo g;
    if (!project.is_object()) return g;
    g.Name = project.value("name", std::string());
    auto it = project.find("game");
    if (it == project.end() || !it->is_object()) return g;
    const nlohmann::json& j = *it;
    auto str = [&](const char* key, std::string& field) {
        auto v = j.find(key);
        if (v != j.end() && v->is_string()) field = v->get<std::string>();
    };
    str("name", g.Name);
    str("description", g.Description);
    str("author", g.Author);
    str("company", g.Company);
    str("version", g.Version);
    str("copyright", g.Copyright);
    str("icon", g.Icon);
    if (g.Version.empty()) g.Version = "1.0.0";
    return g;
}

void GameInfo::WriteTo(nlohmann::json& project) const {
    nlohmann::json& j = project["game"];
    j["name"] = Name;
    j["description"] = Description;
    j["author"] = Author;
    j["company"] = Company;
    j["version"] = Version.empty() ? std::string("1.0.0") : Version;
    j["copyright"] = Copyright;
    j["icon"] = Icon;
}

std::string GameInfo::FileName() const {
    std::string out;
    for (char c : Name) {
        if (std::strchr("\\/:*?\"<>|", c) || (unsigned char)c < 32) continue;
        out += c;
    }
    // Windows не любит имя, кончающееся точкой или пробелом.
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out.empty() ? std::string("Game") : out;
}

void GameInfo::VersionNumbers(uint16_t out[4]) const {
    for (int i = 0; i < 4; ++i) out[i] = 0;
    std::stringstream ss(Version);
    std::string part;
    int i = 0;
    while (i < 4 && std::getline(ss, part, '.')) {
        int v = 0;
        for (char c : part) {
            if (c < '0' || c > '9') break;
            v = v * 10 + (c - '0');
            if (v > 65535) { v = 65535; break; }
        }
        out[i++] = (uint16_t)v;
    }
}

bool LoadGameInfo(const fs::path& projectDir, GameInfo& out) {
    std::string text;
    // Собранная игра: файл проекта — в пакете; разработка — на диске.
    if (!vfs::ReadText(sage::PathToUtf8(projectDir / "project.sageproj"), text) &&
        !vfs::ReadText("project.sageproj", text))
        return false;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) return false;
    out = GameInfo::FromProject(j);
    if (out.Name.empty()) out.Name = sage::PathToUtf8(fs::absolute(projectDir).filename());
    return true;
}

bool SaveGameInfo(const fs::path& projectDir, const GameInfo& info, std::string* error) {
    const fs::path file = projectDir / "project.sageproj";
    nlohmann::json j = nlohmann::json::object();
    {
        std::ifstream in(file);
        if (in) {
            j = nlohmann::json::parse(in, nullptr, false);
            if (j.is_discarded() || !j.is_object()) {
                if (error) *error = "project.sageproj не разбирается — не перезаписываю";
                return false;
            }
        }
    }
    info.WriteTo(j);
    std::ofstream outFile(file);
    if (!outFile) {
        if (error) *error = "не записать " + sage::PathToUtf8(file);
        return false;
    }
    outFile << j.dump(2) << "\n";
    return (bool)outFile;
}

bool LoadIconImage(const std::string& path, IconImage& out) {
    std::vector<uint8_t> bytes;
    // Базу ассетов спрашиваем, только если движок запущен: сборщик игры из
    // командной строки (sage_build) работает без контекста, и обращение к ней
    // роняло сборку на Windows — единственной системе, где иконка вшивается.
    const std::string located =
        EngineContext::CurrentOrNull() ? sage::AssetDatabase::Instance().LocatePath(path) : path;
    if (!vfs::ReadFile(located, bytes) && !vfs::ReadFile(path, bytes)) return false;
    int w = 0, h = 0, n = 0;
    // Флаг переворота у stb — на поток, и загрузчики текстур ставят его в
    // «перевернуть» (так нужно GPU). Иконка нужна как картинка — сверху вниз.
    stbi_set_flip_vertically_on_load_thread(false);
    stbi_uc* px = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
    if (!px) return false;
    out.Width = w;
    out.Height = h;
    out.Rgba.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    return w > 0 && h > 0;
}

IconImage ResizeIcon(const IconImage& src, int size) {
    IconImage dst;
    dst.Width = dst.Height = size;
    dst.Rgba.assign((size_t)size * size * 4, 0);
    if (src.Width <= 0 || src.Height <= 0) return dst;
    // Неквадратная картинка вписывается по центру, не растягиваясь.
    const int side = std::max(src.Width, src.Height);
    const double scale = (double)side / size;
    const int ox = (side - src.Width) / 2, oy = (side - src.Height) / 2;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            double acc[4] = {0, 0, 0, 0};
            double area = 0;
            const double x0 = x * scale, x1 = (x + 1) * scale, y0 = y * scale, y1 = (y + 1) * scale;
            for (int sy = (int)y0; sy < (int)std::ceil(y1); ++sy) {
                for (int sx = (int)x0; sx < (int)std::ceil(x1); ++sx) {
                    const double w = (std::min<double>(sx + 1, x1) - std::max<double>(sx, x0)) *
                                     (std::min<double>(sy + 1, y1) - std::max<double>(sy, y0));
                    const int ix = sx - ox, iy = sy - oy;
                    area += w;
                    if (w <= 0 || ix < 0 || iy < 0 || ix >= src.Width || iy >= src.Height) continue;
                    const uint8_t* p = &src.Rgba[((size_t)iy * src.Width + ix) * 4];
                    // Цвет — с весом прозрачности: иначе у краёв проступает
                    // цвет невидимых пикселей (тёмная кайма).
                    const double a = p[3] / 255.0;
                    acc[0] += p[0] * a * w;
                    acc[1] += p[1] * a * w;
                    acc[2] += p[2] * a * w;
                    acc[3] += a * w;
                }
            }
            uint8_t* d = &dst.Rgba[((size_t)y * size + x) * 4];
            if (acc[3] > 0) {
                for (int c = 0; c < 3; ++c) d[c] = (uint8_t)std::clamp(acc[c] / acc[3] + 0.5, 0.0, 255.0);
                d[3] = (uint8_t)std::clamp(acc[3] / std::max(area, 1e-9) * 255.0 + 0.5, 0.0, 255.0);
            }
        }
    }
    return dst;
}

namespace {

void PngWrite(void* ctx, void* data, int size) {
    auto* v = (std::vector<uint8_t>*)ctx;
    v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
}

void Put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); }
void Put32(std::vector<uint8_t>& b, uint32_t v) { Put16(b, v & 0xFFFF); Put16(b, v >> 16); }
void Align4(std::vector<uint8_t>& b) { while (b.size() % 4) b.push_back(0); }

void PutWide(std::vector<uint8_t>& b, const std::string& utf8) {
    // UTF-8 -> UTF-16 (с суррогатными парами), с завершающим нулём.
    size_t i = 0;
    while (i < utf8.size()) {
        uint32_t c = (unsigned char)utf8[i];
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= (0x3F >> extra);
        ++i;
        for (int k = 0; k < extra && i < utf8.size(); ++k, ++i) c = (c << 6) | ((unsigned char)utf8[i] & 0x3F);
        if (c >= 0x10000) {
            c -= 0x10000;
            Put16(b, (uint16_t)(0xD800 + (c >> 10)));
            Put16(b, (uint16_t)(0xDC00 + (c & 0x3FF)));
        } else {
            Put16(b, (uint16_t)c);
        }
    }
    Put16(b, 0);
}

size_t WideUnits(const std::string& utf8) {
    std::vector<uint8_t> tmp;
    PutWide(tmp, utf8);
    return tmp.size() / 2;
}

// Узел дерева VERSIONINFO: длина, длина значения, тип, ключ, значение, дети.
std::vector<uint8_t> Node(const std::string& key, const std::vector<uint8_t>& value, uint16_t valueLen,
                          uint16_t type, const std::vector<std::vector<uint8_t>>& children) {
    std::vector<uint8_t> b;
    Put16(b, 0);           // длина — ниже, когда известна
    Put16(b, valueLen);
    Put16(b, type);
    PutWide(b, key);
    Align4(b);
    b.insert(b.end(), value.begin(), value.end());
    for (const auto& c : children) {
        Align4(b);
        b.insert(b.end(), c.begin(), c.end());
    }
    const uint16_t len = (uint16_t)b.size();
    b[0] = len & 0xFF;
    b[1] = len >> 8;
    return b;
}

std::vector<uint8_t> StringNode(const std::string& key, const std::string& value) {
    std::vector<uint8_t> v;
    PutWide(v, value);
    return Node(key, v, (uint16_t)WideUnits(value), 1, {});
}

} // namespace

IconResources BuildIconResources(const IconImage& icon, uint16_t firstId) {
    IconResources r;
    static const int kSizes[] = {16, 24, 32, 48, 64, 128, 256};
    std::vector<uint8_t>& g = r.Group;
    Put16(g, 0);                                   // reserved
    Put16(g, 1);                                   // type: icon
    Put16(g, (uint16_t)(sizeof(kSizes) / sizeof(kSizes[0])));
    uint16_t id = firstId;
    for (int size : kSizes) {
        const IconImage img = ResizeIcon(icon, size);
        std::vector<uint8_t> png;
        stbi_write_png_to_func(PngWrite, &png, size, size, 4, img.Rgba.data(), size * 4);
        // GRPICONDIRENTRY: 0 вместо 256 — так формат пишет «256».
        g.push_back((uint8_t)(size >= 256 ? 0 : size));
        g.push_back((uint8_t)(size >= 256 ? 0 : size));
        g.push_back(0);                            // цветов в палитре
        g.push_back(0);                            // reserved
        Put16(g, 1);                               // planes
        Put16(g, 32);                              // бит на пиксель
        Put32(g, (uint32_t)png.size());
        Put16(g, id++);
        r.Images.push_back(std::move(png));
    }
    return r;
}

std::vector<uint8_t> BuildVersionResource(const GameInfo& info, const std::string& exeFileName) {
    uint16_t v[4];
    info.VersionNumbers(v);
    std::vector<uint8_t> fixed;
    Put32(fixed, 0xFEEF04BD);                      // подпись VS_FIXEDFILEINFO
    Put32(fixed, 0x00010000);                      // версия структуры
    Put32(fixed, ((uint32_t)v[0] << 16) | v[1]);   // FileVersionMS
    Put32(fixed, ((uint32_t)v[2] << 16) | v[3]);   // FileVersionLS
    Put32(fixed, ((uint32_t)v[0] << 16) | v[1]);   // ProductVersionMS
    Put32(fixed, ((uint32_t)v[2] << 16) | v[3]);   // ProductVersionLS
    Put32(fixed, 0x3F);                            // FileFlagsMask
    Put32(fixed, 0);                               // FileFlags
    Put32(fixed, 0x00040004);                      // VOS_NT_WINDOWS32
    Put32(fixed, 1);                               // VFT_APP
    Put32(fixed, 0);                               // подтип
    Put32(fixed, 0);                               // дата
    Put32(fixed, 0);

    const std::string company = info.Company.empty() ? info.Author : info.Company;
    std::vector<std::vector<uint8_t>> strings;
    auto add = [&](const char* key, const std::string& value) {
        if (!value.empty()) strings.push_back(StringNode(key, value));
    };
    add("CompanyName", company);
    add("FileDescription", info.Description.empty() ? info.Name : info.Description);
    add("FileVersion", info.Version);
    add("InternalName", info.Name);
    add("LegalCopyright", info.Copyright);
    add("OriginalFilename", exeFileName);
    add("ProductName", info.Name);
    add("ProductVersion", info.Version);
    add("Comments", info.Author.empty() ? std::string() : "Author: " + info.Author);

    // 0409 — английский (США), 04B0 — Юникод: строки читаются при любом языке
    // системы.
    const auto table = Node("040904B0", {}, 0, 1, strings);
    const auto sfi = Node("StringFileInfo", {}, 0, 1, {table});
    std::vector<uint8_t> trans;
    Put16(trans, 0x0409);
    Put16(trans, 0x04B0);
    const auto var = Node("Translation", trans, 4, 0, {});
    const auto vfi = Node("VarFileInfo", {}, 0, 1, {var});
    return Node("VS_VERSION_INFO", fixed, (uint16_t)fixed.size(), 0, {sfi, vfi});
}

bool EmbedWindowsResources(const fs::path& exe, const GameInfo& info, const IconImage* icon, std::string& error) {
#ifdef _WIN32
    HANDLE h = BeginUpdateResourceW(exe.wstring().c_str(), FALSE);
    if (!h) {
        error = "BeginUpdateResource: ошибка " + std::to_string(GetLastError());
        return false;
    }
    const WORD lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
    // Типы ресурсов — явно широкими: RT_ICON и соседи без UNICODE — узкие строки.
    LPCWSTR kIcon = MAKEINTRESOURCEW(3), kGroupIcon = MAKEINTRESOURCEW(14), kVersion = MAKEINTRESOURCEW(16);
    bool ok = true;
    if (icon && icon->Width > 0) {
        const IconResources res = BuildIconResources(*icon, 1);
        for (size_t i = 0; i < res.Images.size() && ok; ++i) {
            ok = UpdateResourceW(h, kIcon, MAKEINTRESOURCEW((WORD)(1 + i)), lang,
                                 (LPVOID)res.Images[i].data(), (DWORD)res.Images[i].size()) != 0;
        }
        // Группа номер 1 — первая в каталоге ресурсов: её Проводник и панель
        // задач и берут иконкой файла.
        ok = ok && UpdateResourceW(h, kGroupIcon, MAKEINTRESOURCEW(1), lang, (LPVOID)res.Group.data(),
                                   (DWORD)res.Group.size()) != 0;
    }
    const std::vector<uint8_t> ver = BuildVersionResource(info, sage::PathToUtf8(exe.filename()));
    ok = ok && UpdateResourceW(h, kVersion, MAKEINTRESOURCEW(1), MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                               (LPVOID)ver.data(), (DWORD)ver.size()) != 0;
    if (!ok) {
        error = "UpdateResource: ошибка " + std::to_string(GetLastError());
        EndUpdateResourceW(h, TRUE);
        return false;
    }
    if (!EndUpdateResourceW(h, FALSE)) {
        error = "EndUpdateResource: ошибка " + std::to_string(GetLastError());
        return false;
    }
    return true;
#else
    (void)exe;
    (void)info;
    (void)icon;
    error = "иконка и сведения о файле вшиваются в .exe только сборкой на Windows";
    return false;
#endif
}

} // namespace sage::assets
