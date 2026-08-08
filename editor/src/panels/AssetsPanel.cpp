#include "AssetsPanel.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "imgui.h"

#include "EditorHost.h"

#include "sage/assets/import/Convert.h"
#include "sage/render/ResourceManager.h"
#include "sage/assets/AssetDatabase.h"
#include "Project.h"
#include "sage/core/Log.h"
#include "../Localization.h"

namespace fs = std::filesystem;

namespace {

// Цветная плашка + короткий тег по типу файла — свой минимализм панели Assets
// вместо иконочного шрифта: ImDrawList::AddRectFilled с закруглением плюс
// 2-4 символа расширения по центру.
struct AssetStyle { ImVec4 Color; std::string Tag; };

std::string ToLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

AssetStyle StyleForPath(const fs::path& path, bool isDir) {
    if (isDir) return { ImVec4(0.80f, 0.60f, 0.20f, 1.0f), "DIR" };
    std::string ext = ToLower(path.extension().string());
    if (ext == ".sage") return { ImVec4(0.25f, 0.45f, 0.85f, 1.0f), "SCENE" };
    if (ext == ".sageprefab") return { ImVec4(0.35f, 0.55f, 0.95f, 1.0f), "PREFAB" };
    if (ext == ".sagemat") return { ImVec4(0.75f, 0.45f, 0.20f, 1.0f), "MAT" };
    if (ext == ".sageimport") return { ImVec4(0.45f, 0.40f, 0.30f, 1.0f), "IMPORT" };
    if (ext == ".lua") return { ImVec4(0.25f, 0.70f, 0.30f, 1.0f), "LUA" };
    if (ext == ".obj" || ext == ".gltf" || ext == ".glb") return { ImVec4(0.55f, 0.35f, 0.80f, 1.0f), "MESH" };
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp")
        return { ImVec4(0.20f, 0.65f, 0.65f, 1.0f), ext.substr(1) };
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3") return { ImVec4(0.80f, 0.30f, 0.55f, 1.0f), "AUDIO" };
    if (ext == ".vert" || ext == ".frag" || ext == ".glsl") return { ImVec4(0.50f, 0.50f, 0.55f, 1.0f), "GLSL" };
    std::string tag = ext.empty() ? "FILE" : ToLower(ext.substr(1));
    return { ImVec4(0.45f, 0.45f, 0.48f, 1.0f), tag };
}

// Обрезает строку многоточием справа, чтобы уместиться в maxWidth.
std::string TruncateToWidth(const std::string& s, float maxWidth) {
    if (ImGui::CalcTextSize(s.c_str()).x <= maxWidth) return s;
    const char* ellipsis = "...";
    float ellipsisW = ImGui::CalcTextSize(ellipsis).x;
    std::string out;
    for (size_t n = 1; n <= s.size(); ++n) {
        std::string candidate = s.substr(0, n);
        if (ImGui::CalcTextSize(candidate.c_str()).x + ellipsisW > maxWidth) {
            out = s.substr(0, n > 1 ? n - 1 : 1);
            break;
        }
        out = candidate;
    }
    return out + ellipsis;
}

// Габариты карточки-тайла. ВАЖНО: высота тайла включает и плашку, и подпись —
// раньше подпись рисовалась DrawList'ом НИЖЕ InvisibleButton'а (высотой лишь в
// плашку), выходила за границы item'а, и строки грида налезали друг на друга
// («неровность»). Теперь весь тайл — единый item, всё внутри его границ.
constexpr float kTileW = 104.0f;
constexpr float kTileInset = 8.0f; // внутренний отступ карточки
// Обложка КВАДРАТНАЯ у всех типов ассетов.
//
// Раньше она была 88x54 — вытянутый прямоугольник. Для трёхбуквенного тега это
// было неважно, но превью в него вписывается по меньшей стороне, то есть по
// высоте: от карточки шириной 88 под картинку работали 54 пикселя, а остальное
// оставалось пустым. Квадрат отдаёт превью всю ширину и заодно выравнивает
// сетку — карточки разных типов перестают отличаться пропорциями.
//
// Высота плашки подобрана так, чтобы её ВИДИМАЯ область (kTileW - 2*inset по
// ширине, kSwatchH - inset по высоте) была квадратом.
constexpr float kSwatchH = kTileW - kTileInset;
constexpr float kLabelH = 34.0f;   // область подписи (1-2 строки)
constexpr float kTileH = kSwatchH + kLabelH + 6.0f; // + внутренний отступ
constexpr float kTileSpacing = 12.0f;

} // namespace

void AssetsPanel::DrawBreadcrumb(EditorHost& host) {
    Project& project = host.CurrentProject();
    fs::path& cwd = host.AssetsCwd();
    fs::path root = project.Loaded() ? project.Dir().parent_path() : cwd.root_path();

    // Собираем цепочку сегментов от текущей папки вверх до корня.
    std::vector<fs::path> chain;
    for (fs::path p = cwd; p != root && p.has_parent_path(); p = p.parent_path()) {
        chain.push_back(p);
        if (p.parent_path() == p) break; // достигли корня файловой системы
    }
    std::reverse(chain.begin(), chain.end());

    for (size_t i = 0; i < chain.size(); ++i) {
        std::string seg = chain[i].filename().string();
        if (seg.empty()) seg = chain[i].string();
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton(seg.c_str())) cwd = chain[i];
        ImGui::PopID();
        if (i + 1 < chain.size()) { ImGui::SameLine(0, 2); ImGui::TextDisabled("/"); ImGui::SameLine(0, 2); }
    }
}


// Превью для карточки. 0 — превью нет (папка, скрипт, сцена): для них тег
// честнее пустого квадрата.
//
// Картинки показываются сами собой, материал — шариком с этим материалом.
// Материал рендерится НЕ БОЛЬШЕ ОДНОГО ЗА КАДР и запоминается: это полный
// проход сцены со светом, и делать двадцать таких на открытие папки значит
// уронить редактор на ровном месте. Остальные карточки получат своё превью в
// следующих кадрах — за десяток кадров это незаметно глазу.
uint64_t AssetsPanel::ThumbnailFor(const fs::path& path, bool isDir) {
    if (isDir) return 0;
    const std::string key = path.string();

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" ||
        ext == ".hdr" || ext == ".sagetex") {
        // Текстуры и так кэшируются менеджером ресурсов — просто спрашиваем.
        // Негативный кэш там же: битый файл не будет перечитываться каждый кадр.
        std::shared_ptr<Texture> tex = ResourceManager::Instance().GetTexture(key);
        return tex ? tex->NativeHandle() : 0;
    }

    // Материал — шариком, префаб — собой, модель — своей геометрией. Все трое
    // идут через один кэш и одну очередь «не больше одного рендера за кадр»:
    // каждый такой рендер это полный проход сцены со светом, и папка с двумя
    // десятками префабов иначе уронила бы кадр при первом же открытии.
    const bool renderable = ext == ".sagemat" || ext == ".sageprefab" ||
                            sage::assets::IsConvertibleModel(key) || ext == ".sagemesh";
    if (!renderable) return 0;

    std::error_code ec;
    const auto write = fs::last_write_time(path, ec);
    const long long stamp = ec ? 0 : (long long)write.time_since_epoch().count();

    auto it = m_thumbs.find(key);
    if (it != m_thumbs.end() && it->second.Stamp == stamp) return it->second.Id;
    if (m_thumbRenderedThisFrame) {
        // Очередь занята — отдаём прошлую обложку, если она была. Мигание
        // «пусто -> картинка» на каждой правке файла заметнее, чем кадр
        // устаревшего превью.
        return it != m_thumbs.end() ? it->second.Id : 0;
    }

    // Ключ = путь: у каждой обложки СВОЙ буфер. С общим буфером все запомненные
    // хендлы указывали бы на одну текстуру, и вся папка показывала бы то, что
    // нарисовали последним (см. комментарий про key в AssetPreview).
    uint64_t id = 0;
    if (ext == ".sagemat") {
        if (std::shared_ptr<Material> mat = ResourceManager::Instance().GetMaterial(key))
            id = m_preview.RenderMaterial(mat, 96, key);
    } else if (ext == ".sageprefab") {
        id = m_preview.RenderPrefab(key, 96, key);
    } else {
        // Модель: копия геометрии на стороне процессора нужна, чтобы вписать её
        // в кадр по габаритам (см. AssetPreview::RenderMesh).
        if (std::shared_ptr<Mesh> mesh = ResourceManager::Instance().GetModel(key))
            id = m_preview.RenderMesh(mesh, 96, key);
    }
    // Ноль тоже запоминаем — иначе битый или пустой ассет пытался бы
    // отрисоваться каждый кадр, съедая всю очередь превью и не давая остальным
    // карточкам получить свои обложки.
    m_thumbs[key] = Thumb{id, stamp};
    if (id) m_thumbRenderedThisFrame = true;
    return id;
}

void AssetsPanel::DrawTile(EditorHost& host, const fs::path& path, bool isDir) {
    AssetStyle style = StyleForPath(path, isDir);
    std::string filename = path.filename().string();

    ImGui::PushID(filename.c_str());

    // Весь тайл — ОДИН item (InvisibleButton на полную высоту карточки): подпись
    // теперь внутри его границ, строки грида больше не налезают друг на друга.
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##tile", ImVec2(kTileW, kTileH));
    bool hovered = ImGui::IsItemHovered();
    bool doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    bool isSelected = m_selected == path;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 tileMax(cursor.x + kTileW, cursor.y + kTileH);

    // Подложка карточки — по теме редактора; подсвечивается при наведении/выборе.
    ImU32 cardBg = 0;
    if (isSelected)      cardBg = ImGui::GetColorU32(ImGuiCol_Header);
    else if (hovered)    cardBg = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
    else                 cardBg = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.035f));
    dl->AddRectFilled(cursor, tileMax, cardBg, 8.0f);
    if (isSelected) {
        dl->AddRect(cursor, tileMax, ImGui::GetColorU32(ImGuiCol_NavHighlight), 8.0f, 0, 1.6f);
    }

    // Цветная плашка типа с внутренним отступом (верх карточки).
    constexpr float kInset = kTileInset;
    ImVec2 sw0(cursor.x + kInset, cursor.y + kInset);
    ImVec2 sw1(tileMax.x - kInset, cursor.y + kInset + kSwatchH - kInset);
    ImVec4 fill = style.Color;
    if (!hovered && !isSelected) { fill.x *= 0.9f; fill.y *= 0.9f; fill.z *= 0.9f; }
    dl->AddRectFilled(sw0, sw1, ImGui::ColorConvertFloat4ToU32(fill), 6.0f);

    // Настоящее превью вместо трёхбуквенного тега — там, где его есть из чего
    // сделать. Тег отвечает на вопрос «какого типа этот файл», а человек в
    // панели ассетов ищет КОНКРЕТНУЮ картинку или материал среди двух десятков
    // одинаковых оранжевых прямоугольников с надписью MAT. Имя файла помогает
    // только если его помнят.
    const uint64_t thumb = ThumbnailFor(path, isDir);
    if (thumb) {
        // Шахматка под картинкой: прозрачные места иначе неотличимы от фона
        // карточки, и текстура с альфой выглядит просто дырявой.
        const float checker = 8.0f;
        dl->PushClipRect(sw0, sw1, true);
        for (float y = sw0.y; y < sw1.y; y += checker) {
            for (float x = sw0.x; x < sw1.x; x += checker) {
                const bool odd = ((int)((x - sw0.x) / checker) + (int)((y - sw0.y) / checker)) % 2;
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + checker, y + checker),
                                  odd ? IM_COL32(70, 70, 76, 255) : IM_COL32(52, 52, 58, 255));
            }
        }
        // Вписываем по меньшей стороне, сохраняя пропорции: растянутое превью
        // врёт о содержимом.
        const float availW = sw1.x - sw0.x, availH = sw1.y - sw0.y;
        const float side = std::min(availW, availH);
        const ImVec2 c0(sw0.x + (availW - side) * 0.5f, sw0.y + (availH - side) * 0.5f);
        dl->AddImage((ImTextureID)(std::intptr_t)thumb, c0, ImVec2(c0.x + side, c0.y + side),
                     ImVec2(0, 1), ImVec2(1, 0));
        dl->PopClipRect();
        dl->AddRect(sw0, sw1, IM_COL32(255, 255, 255, 30), 6.0f);
    } else {
        // Тег типа по центру плашки.
        std::string tagUpper = style.Tag;
        for (char& c : tagUpper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        ImVec2 tagSize = ImGui::CalcTextSize(tagUpper.c_str());
        ImVec2 tagPos(sw0.x + (sw1.x - sw0.x - tagSize.x) * 0.5f,
                      sw0.y + (sw1.y - sw0.y - tagSize.y) * 0.5f);
        dl->AddText(tagPos, ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.96f)), tagUpper.c_str());
    }

    // Имя файла по центру области подписи (внутри границ тайла, с усечением).
    std::string label = TruncateToWidth(filename, kTileW - 8.0f);
    ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
    ImVec2 labelPos(cursor.x + (kTileW - labelSize.x) * 0.5f, sw1.y + 7.0f);
    ImU32 textCol = ImGui::GetColorU32(isSelected ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    if (hovered) textCol = ImGui::GetColorU32(ImGuiCol_Text);
    dl->AddText(labelPos, textCol, label.c_str());

    // Источник перетаскивания: файл можно бросить в слот текстуры инспектора.
    // Путь передаётся строкой с завершающим нулём — принимающая сторона получает
    // ровно то, что открыла бы сама.
    if (!isDir && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        const std::string p = path.string();
        ImGui::SetDragDropPayload("SAGE_ASSET_PATH", p.c_str(), p.size() + 1);
        // Под курсором — обложка и имя, а не одно имя: тащат обычно из десятка
        // похожих файлов, и по картинке видно, что именно ухватили.
        const uint64_t dragThumb = ThumbnailFor(path, false);
        if (dragThumb) {
            ImGui::Image((ImTextureID)(std::intptr_t)dragThumb, ImVec2(48, 48), ImVec2(0, 1),
                         ImVec2(1, 0));
            ImGui::SameLine();
        }
        ImGui::TextUnformatted(filename.c_str());
        ImGui::EndDragDropSource();
    }

    // Папка принимает файлы: бросок ПЕРЕМЕЩАЕТ, а не копирует — это раскладка
    // уже своих ассетов по местам, и вторая копия тут никому не нужна. Ссылки
    // в сценах переживают переезд: они держатся за GUID из сайдкара .meta,
    // который едет вместе с файлом (см. sage/assets/AssetDatabase.h).
    if (isDir && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ASSET_PATH")) {
            std::string dropped((const char*)p->Data, (size_t)p->DataSize);
            if (!dropped.empty() && dropped.back() == '\0') dropped.pop_back();
            MoveIntoFolder(host, dropped, path);
        }
        ImGui::EndDragDropTarget();
    }

    if (clicked) m_selected = path;
    if (doubleClicked) {
        if (isDir) host.AssetsCwd() = path;
        else if (path.extension() == ".sage") host.LoadSceneFromFile(path);
        else if (path.extension() == ".sageprefab") host.InstantiatePrefab(path); // инстанс в сцену
        else {
            // Код открывается во встроенном редакторе: скрипты и шейдеры правят
            // постоянно, и уводить человека во внешний редактор на каждую
            // строчку значит терять горячую перезагрузку, ради которой она и
            // сделана.
            const std::string ext = path.extension().string();
            if (ext == ".lua" || ext == ".vert" || ext == ".frag" || ext == ".glsl" ||
                ext == ".txt" || ext == ".md" || ext == ".json") {
                host.OpenCodeFile(path);
            }
        }
    }
    if (ImGui::BeginPopupContextItem("##tile_ctx")) {
        m_selected = path;
        if (ImGui::MenuItem(T("Rename"))) { m_renameTarget = path; m_error.clear(); }
        if (ImGui::MenuItem(T("Delete"))) { m_deleteTarget = path; }

        // Конвертация в свой формат — там же, где всё остальное про файл.
        // Отдельной кнопки в меню нет намеренно: конвертируют КОНКРЕТНЫЙ файл,
        // и меню файла — единственное место, где не надо объяснять, какой.
        const std::string p = path.string();
        const bool model = sage::assets::IsConvertibleModel(p);
        const bool texture = sage::assets::IsConvertibleTexture(p);
        if (model || texture) {
            ImGui::Separator();
            const char* label = model ? T("Convert to .sagemesh")
                                      : T("Convert to .sagetex");
            if (ImGui::MenuItem(label)) ConvertOne(host, path);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", T("The engine's own format: loads without parsing and weighs less.\n"
                  "The source file stays where it is."));
            }
        }
        ImGui::EndPopup();
    }
    if (hovered && !filename.empty() && label != filename) ImGui::SetTooltip("%s", filename.c_str());

    ImGui::PopID();
}

// Конвертация одного файла. Отчёт уходит в статусную строку: человек нажал
// пункт меню и обязан увидеть, что получилось, не открывая консоль.
void AssetsPanel::ConvertOne(EditorHost& host, const fs::path& path) {
    sage::assets::ConvertOptions opts;
    opts.Overwrite = true;   // явное действие по одному файлу — перезапись ожидаема
    const sage::assets::ConvertResult r = sage::assets::ConvertAnyToNative(path.string(), {}, opts);
    if (!r.Ok) {
        host.SetStatusMessage(T("Conversion failed: ") + r.Error);
        return;
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf), T("%s -> %s (%.1fx smaller)"),
                  path.filename().string().c_str(),
                  fs::path(r.OutputPath).filename().string().c_str(), (double)r.Ratio());
    host.SetStatusMessage(buf);
    for (const std::string& w : r.Warnings) host.SetStatusMessage(T("Import: ") + w);
}

// Вся текущая папка. Отчёт — одной строкой со сводкой: перечислять полсотни
// файлов в статусной строке бессмысленно, а подробности уже в консоли.
void AssetsPanel::ConvertFolderHere(EditorHost& host) {
    sage::assets::ConvertOptions opts;
    opts.Overwrite = false;   // пакетная операция не должна затирать правки руками
    const std::vector<sage::assets::ConvertResult> results =
        sage::assets::ConvertFolder(host.AssetsCwd().string(), opts);

    size_t ok = 0, failed = 0, srcBytes = 0, outBytes = 0;
    for (const sage::assets::ConvertResult& r : results) {
        if (r.Ok) {
            ++ok;
            srcBytes += r.SourceBytes;
            outBytes += r.OutputBytes;
        } else {
            ++failed;
            LOG_WARN("Convert") << r.SourcePath << ": " << r.Error;
        }
    }
    char buf[256];
    if (ok == 0 && failed == 0) {
        std::snprintf(buf, sizeof(buf), "%s", T("Nothing to convert: no models or images in the folder"));
    } else {
        std::snprintf(buf, sizeof(buf), T("Converted %zu, skipped %zu; %.1f -> %.1f KB"),
                      ok, failed, srcBytes / 1024.0, outBytes / 1024.0);
    }
    host.SetStatusMessage(buf);
}

// ============================================================================
//  Внесение чужих файлов в проект
// ============================================================================
namespace {

// Файлы, на которые ссылается .mtl (map_Kd, norm, bump, …). Разбираем сами, а
// не через tinyobj: нужен СПИСОК ИМЁН, а не разобранный материал, и делать ради
// него полный разбор геометрии — платить кратно больше, чем стоит задача.
std::vector<std::string> MtlTextureNames(const fs::path& mtl) {
    std::vector<std::string> names;
    std::ifstream in(mtl);
    if (!in) return names;
    std::string line;
    while (std::getline(in, line)) {
        // Ключевые слова карт: всё, что начинается на map_, плюс bump/norm/disp/refl.
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        const bool isMap = key.rfind("map_", 0) == 0 || key == "bump" || key == "norm" ||
                           key == "disp" || key == "refl";
        if (!isMap) continue;
        // У карты бывают опции («map_Kd -bm 0.2 stone.png») — имя файла это
        // ПОСЛЕДНИЙ токен строки, а не первый после ключевого слова.
        std::string token, last;
        while (ls >> token) last = token;
        if (!last.empty() && last[0] != '-') names.push_back(last);
    }
    return names;
}

// Внешние файлы, на которые ссылается .gltf: uri у buffers[] и images[].
// Разбираем как JSON — .gltf это он и есть. Разбор может не удаться (битый
// файл): тогда спутников просто не будет, а сам файл всё равно переедет.
std::vector<std::string> GltfExternalUris(const fs::path& gltf) {
    std::vector<std::string> uris;
    std::ifstream in(gltf);
    if (!in) return uris;
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception&) {
        return uris;
    }
    for (const char* section : {"buffers", "images"}) {
        if (!j.contains(section) || !j[section].is_array()) continue;
        for (const auto& entry : j[section]) {
            const std::string uri = entry.value("uri", std::string{});
            // data:… — содержимое лежит внутри самого файла, копировать нечего.
            if (uri.empty() || uri.rfind("data:", 0) == 0) continue;
            uris.push_back(uri);
        }
    }
    return uris;
}

// Регистрация в базе ассетов ждёт путь ОТНОСИТЕЛЬНО корня проекта: по нему она
// кладёт сайдкар .meta и по нему же потом отвечает на «где этот файл». Отдать
// ей абсолютный путь значит записать ассет под ключом, которого в проекте нет.
void RegisterInDatabase(const fs::path& file) {
    sage::AssetDatabase& db = sage::AssetDatabase::Instance();
    const std::string root = db.ProjectDir();
    if (root.empty()) return;   // проект не открыт — регистрировать некуда
    std::error_code ec;
    const fs::path rel = fs::relative(file, fs::path(root), ec);
    if (ec || rel.empty()) return;
    db.Register(rel.generic_string());
}

} // namespace

// Перенос файла в папку броском. Вместе с ним едет сайдкар .meta (личность
// ассета — по нему ссылки в сценах остаются целыми) и .sageimport (настройки
// импорта модели): оставить их на старом месте значило бы, что переехавшая
// модель потеряла и свой GUID, и свой масштаб.
void AssetsPanel::MoveIntoFolder(EditorHost& host, const fs::path& source, const fs::path& folder) {
    std::error_code ec;
    if (!fs::exists(source, ec) || fs::is_directory(source, ec)) return;
    const fs::path target = folder / source.filename();
    if (fs::weakly_canonical(source, ec) == fs::weakly_canonical(target, ec)) return;
    if (fs::exists(target, ec)) {
        host.SetStatusMessage(T("The folder already contains ") + source.filename().string());
        return;
    }

    fs::rename(source, target, ec);
    if (ec) {
        host.SetStatusMessage(T("Moving failed: ") + ec.message());
        LOG_ERROR("Editor") << "Перенос ассета не удался: " << ec.message();
        return;
    }
    for (const char* sidecar : {".meta", ".sageimport"}) {
        const fs::path from = source.string() + sidecar;
        std::error_code sec;
        if (fs::exists(from, sec)) fs::rename(from, target.string() + sidecar, sec);
    }

    if (m_selected == source) m_selected = target;
    // Пересканировать проект: база ассетов помнит пути, и после переезда её
    // ответ на «где этот файл» обязан измениться.
    Project& project = host.CurrentProject();
    if (project.Loaded()) sage::AssetDatabase::Instance().ScanProject(project.Dir().string());
    host.SetStatusMessage(T("Moved into ") + folder.filename().string() + ": " +
                          source.filename().string());
}

AssetsPanel::ImportReport AssetsPanel::ImportAsset(const fs::path& source, const fs::path& destDir) {
    ImportReport report;
    std::error_code ec;

    if (!fs::exists(source, ec) || fs::is_directory(source, ec)) {
        report.Error = T("File not found: ") + source.string();
        return report;
    }
    fs::create_directories(destDir, ec);
    if (ec) {
        report.Error = T("The folder is not accessible: ") + ec.message();
        return report;
    }

    // Уже в проекте — вносить нечего. Это не ошибка: человек мог выбрать файл в
    // папке проекта просто потому, что диалог там и открылся.
    const fs::path canonicalSrc = fs::weakly_canonical(source, ec);
    const fs::path canonicalDst = fs::weakly_canonical(destDir / source.filename(), ec);
    if (!ec && canonicalSrc == canonicalDst) {
        report.Ok = true;
        report.Created = canonicalDst;
        return report;
    }

    // Спутники ищем ДО копирования: если модель ссылается на файлы, которых нет,
    // сказать об этом надо про исходную папку, а не про проект.
    const fs::path srcDir = source.parent_path();
    std::string ext = ToLower(source.extension().string());
    std::vector<std::string> companions;
    if (ext == ".obj") {
        // .mtl обычно называется как модель, но в файле может стоять и другое
        // имя — читаем mtllib, а одноимённый добавляем на всякий случай.
        std::ifstream obj(source);
        std::string line;
        while (std::getline(obj, line)) {
            std::istringstream ls(line);
            std::string key, name;
            ls >> key;
            if (key != "mtllib") continue;
            while (ls >> name) companions.push_back(name);
        }
        companions.push_back(source.stem().string() + ".mtl");
    } else if (ext == ".gltf") {
        companions = GltfExternalUris(source);
    }

    // Картинки, на которые ссылаются найденные .mtl — второй уровень.
    std::vector<std::string> nested;
    for (const std::string& c : companions) {
        if (ToLower(fs::path(c).extension().string()) != ".mtl") continue;
        for (const std::string& tex : MtlTextureNames(srcDir / c)) nested.push_back(tex);
    }
    companions.insert(companions.end(), nested.begin(), nested.end());

    auto copyOne = [&](const fs::path& from, const fs::path& to) -> bool {
        std::error_code cec;
        fs::create_directories(to.parent_path(), cec);
        // Существующий файл НЕ перезаписываем: повторный импорт не должен
        // затирать текстуру, которую после первого раза поправили.
        if (fs::exists(to, cec)) return true;
        fs::copy_file(from, to, cec);
        return !cec;
    };

    const fs::path mainDst = destDir / source.filename();
    if (!copyOne(source, mainDst)) {
        report.Error = T("Could not copy: ") + source.filename().string();
        return report;
    }
    report.Created = mainDst;
    RegisterInDatabase(mainDst);

    for (const std::string& rel : companions) {
        // Путь спутника относительный — сохраняем его форму, иначе .gltf,
        // ссылающийся на «textures/wood.png», после импорта не нашёл бы файл.
        const fs::path from = srcDir / rel;
        if (!fs::exists(from, ec) || fs::is_directory(from, ec)) {
            report.Missing.push_back(rel);
            continue;
        }
        const fs::path to = destDir / rel;
        if (copyOne(from, to)) {
            report.Extra.push_back(to);
            RegisterInDatabase(to);
        } else {
            report.Missing.push_back(rel);
        }
    }

    report.Ok = true;
    return report;
}

void AssetsPanel::DrawImportButton(EditorHost& host) {
    Project& project = host.CurrentProject();
    ImGui::BeginDisabled(!project.Loaded());
    if (ImGui::SmallButton(T("Import..."))) {
        FileBrowser::Config c;
        c.Title = T("Bring a file into the project");
        // Пусто — показывать всё: в проект вносят и модели, и картинки, и звук,
        // и чужие скрипты, а перечислять их фильтром значит однажды забыть
        // формат, который движок уже понимает.
        c.FilterLabel = T("All files");
        m_importBrowser.Open(c);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "%s", project.Loaded()
                      ? T("Copy an outside file into the current project folder.\n"
                          "A model moves together with its .mtl/.bin files and textures.")
                      : T("Open a project first (File > New Project...)"));
    }

    if (!m_importBrowser.Draw()) return;

    const ImportReport r = ImportAsset(m_importBrowser.Result(), host.AssetsCwd());
    if (!r.Ok) {
        host.SetStatusMessage(T("Import failed: ") + r.Error);
        LOG_ERROR("Editor") << "Импорт не удался: " << r.Error;
        return;
    }
    m_selected = r.Created;

    std::string message = T("Brought in: ") + r.Created.filename().string();
    if (!r.Extra.empty()) message += " (+" + std::to_string(r.Extra.size()) + T(" companion file(s))");
    if (!r.Missing.empty()) {
        // Недостающие спутники — это будущее «модель без текстуры», и узнать о
        // них надо здесь. Список уходит в консоль целиком: в статусной строке
        // ему не поместиться, а первое имя уже подсказывает, что искать.
        message += T("; not found: ") + r.Missing.front();
        if (r.Missing.size() > 1) message += T(" and ") + std::to_string(r.Missing.size() - 1);
        for (const std::string& m : r.Missing)
            LOG_WARN("Editor") << "Импорт: спутник не найден — " << m;
    }
    host.SetStatusMessage(message);
    LOG_INFO("Editor") << "Импорт: " << r.Created.string();
}

bool AssetsPanel::CreateAsset(CreateKind kind, const std::string& rawName, const fs::path& dir,
                              fs::path& outCreated, std::string& err) {
    std::string name = rawName;
    if (name.empty()) {
        err = "Name must not be empty";
        return false;
    }

    const char* wantExt = nullptr;
    switch (kind) {
        case CreateKind::Script:   wantExt = ".lua"; break;
        case CreateKind::TextFile: wantExt = ".txt"; break;
        case CreateKind::Material: wantExt = ".sagemat"; break;
        default: break;
    }
    if (wantExt && fs::path(name).extension() != wantExt) name += wantExt;

    fs::path target = dir / name;
    std::error_code ec;
    if (fs::exists(target, ec)) {
        err = "Already exists: " + name;
        return false;
    }

    if (kind == CreateKind::Folder) {
        if (!fs::create_directory(target, ec) || ec) {
            err = "Create folder failed: " + ec.message();
            return false;
        }
    } else {
        std::string content;
        if (kind == CreateKind::Script) {
            content =
                "-- " + name + " — скрипт сущности SAGE.\n"
                "-- OnStart(entity) вызывается один раз при привязке скрипта,\n"
                "-- OnUpdate(entity, dt) — каждый кадр Play-режима/игры.\n"
                "\n"
                "function OnStart(entity)\n"
                "end\n"
                "\n"
                "function OnUpdate(entity, dt)\n"
                "    -- entity.Transform.Rotation.y = entity.Transform.Rotation.y + dt * 45.0\n"
                "end\n";
        } else if (kind == CreateKind::Material) {
            content =
                "{\n"
                "    \"albedo\": [1.0, 1.0, 1.0],\n"
                "    \"emissive\": [0.0, 0.0, 0.0],\n"
                "    \"shininess\": 32.0,\n"
                "    \"texture\": \"\"\n"
                "}\n";
        }
        std::ofstream out(target);
        if (!out) {
            err = "Create file failed: " + target.string();
            return false;
        }
        out << content;
    }

    LOG_INFO("Editor") << "Asset created: " << target.string();
    outCreated = target;
    return true;
}

// Модалки панели. Деферред-паттерн: контекстные меню только выставляют
// m_createKind/m_renameTarget/m_deleteTarget, OpenPopup зовётся здесь, на
// уровне окна панели (OpenPopup изнутри BeginMenu не находит модалку —
// другой ID-стек, документированная ловушка ImGui).
void AssetsPanel::DrawModals(EditorHost& host) {
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();

    if (m_createKind != CreateKind::None && !ImGui::IsPopupOpen("Create Asset")) {
        ImGui::OpenPopup("Create Asset");
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(T("Create Asset" "###Create Asset"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const char* kindLabel = "";
        switch (m_createKind) {
            case CreateKind::Folder:   kindLabel = "Folder"; break;
            case CreateKind::Script:   kindLabel = "Lua script"; break;
            case CreateKind::TextFile: kindLabel = "Text file"; break;
            case CreateKind::Material: kindLabel = "Material"; break;
            default: break;
        }
        ImGui::TextDisabled("%s in %s", kindLabel, host.AssetsCwd().filename().string().c_str());
        bool enterPressed = ImGui::InputText(T("Name"), m_createName, sizeof(m_createName),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
        if (enterPressed || ImGui::Button(T("Create"), ImVec2(120, 0))) {
            fs::path created;
            if (CreateAsset(m_createKind, m_createName, host.AssetsCwd(), created, m_error)) {
                m_selected = created;
                m_createKind = CreateKind::None;
                m_error.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(120, 0))) {
            m_createKind = CreateKind::None;
            m_error.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!m_renameTarget.empty() && !ImGui::IsPopupOpen("Rename Asset")) {
        ImGui::OpenPopup("Rename Asset");
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(T("Rename Asset" "###Rename Asset"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("%s", m_renameTarget.filename().string().c_str());
        if (ImGui::IsWindowAppearing()) {
            std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s",
                          m_renameTarget.filename().string().c_str());
        }
        bool enterPressed = ImGui::InputText(T("New name"), m_renameBuf, sizeof(m_renameBuf),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
        if (enterPressed || ImGui::Button(T("Rename"), ImVec2(120, 0))) {
            fs::path target = m_renameTarget.parent_path() / m_renameBuf;
            std::error_code ec;
            fs::rename(m_renameTarget, target, ec);
            if (ec) {
                m_error = "Rename failed: " + ec.message();
                LOG_ERROR("Editor") << "Asset rename failed: " << ec.message();
            } else {
                m_renameTarget.clear();
                m_error.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(120, 0))) {
            m_renameTarget.clear();
            m_error.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!m_deleteTarget.empty() && !ImGui::IsPopupOpen("Delete Asset")) {
        ImGui::OpenPopup("Delete Asset");
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(T("Delete Asset" "###Delete Asset"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(T("Delete \"%s\"?"), m_deleteTarget.filename().string().c_str());
        ImGui::TextDisabled("%s", T("This cannot be undone."));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
        if (ImGui::Button(T("Delete"), ImVec2(120, 0))) {
            std::error_code ec;
            fs::remove_all(m_deleteTarget, ec);
            if (ec) LOG_ERROR("Editor") << "Asset delete failed: " << ec.message();
            if (m_selected == m_deleteTarget) m_selected.clear();
            m_deleteTarget.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(120, 0))) {
            m_deleteTarget.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void AssetsPanel::Draw(EditorHost& host, bool* open) {
    // Бюджет превью на кадр: см. ThumbnailFor.
    m_thumbRenderedThisFrame = false;
    Project& project = host.CurrentProject();
    fs::path& cwd = host.AssetsCwd();

    ImGui::Begin(T("Assets" "###Assets"), open);

    if (!project.Loaded()) {
        ImGui::TextDisabled("%s", T("No project open."));
        ImGui::TextDisabled("%s", T("File > New Project... to create one; browsing current dir:"));
    }

    fs::path root = project.Loaded() ? project.Dir() : fs::path("/");
    bool canGoUp = cwd.has_parent_path() && cwd != root;
    if (canGoUp && ImGui::SmallButton(T("Up"))) cwd = cwd.parent_path();
    ImGui::SameLine();
    DrawBreadcrumb(host);

    // Внесение файлов со стороны. До сих пор этого не было вовсе: панель умела
    // ходить по папкам проекта и создавать пустые ассеты, а положить в проект
    // СВОЮ модель или картинку можно было только мимо редактора — проводником.
    // Человек при этом обычно шёл другим путём: выбирал файл прямо из «Загрузок»
    // через «Обзор…», получал в сцене абсолютный путь и рабочий вид ровно до
    // первой сборки игры (см. Project::AssetRef).
    ImGui::SameLine();
    DrawImportButton(host);

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##assets_search", T("Search..."), m_search, sizeof(m_search));

    // Сломанные ссылки — В ПАНЕЛИ, а не только в логе. Именно молчание и было
    // исходной болезнью: сцена грузилась, объект стоял на месте, просто без
    // модели, и заметить это можно было лишь случайно.
    const auto& broken = sage::AssetDatabase::Instance().Broken();
    if (!broken.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.40f, 0.40f, 1.0f));
        const bool open = ImGui::CollapsingHeader(
            (T("Broken references: ") + std::to_string(broken.size()) + "###brokenrefs").c_str());
        ImGui::PopStyleColor();
        if (open) {
            for (const auto& b : broken) {
                ImGui::BulletText("%s", b.Hint.empty() ? b.Missing.ToString().c_str()
                                                       : b.Hint.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "%s", T("The file is missing. Put it back or reassign the asset —\n"
                          "only the .meta sidecar could have caught a rename made outside the editor."));
                }
            }
            if (ImGui::SmallButton(T("Rescan project"))) {
                sage::AssetDatabase::Instance().ClearBroken();
                if (project.Loaded())
                    sage::AssetDatabase::Instance().ScanProject(project.Dir().string());
            }
        }
    }
    ImGui::Separator();

    ImGui::BeginChild("##assets_scroll");
    std::error_code ec;
    std::vector<fs::directory_entry> dirs, files;
    for (const auto& entry : fs::directory_iterator(cwd, ec)) {
        (entry.is_directory(ec) ? dirs : files).push_back(entry);
    }
    auto byName = [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return a.path().filename() < b.path().filename();
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    std::string filter = ToLower(m_search);
    auto matches = [&](const fs::path& p) {
        if (filter.empty()) return true;
        return ToLower(p.filename().string()).find(filter) != std::string::npos;
    };

    // Единый ритм по вертикали между строками грида — как горизонтальный зазор.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kTileSpacing, kTileSpacing));
    float availWidth = ImGui::GetContentRegionAvail().x;
    int columns = std::max(1, static_cast<int>((availWidth + kTileSpacing) / (kTileW + kTileSpacing)));
    int col = 0;
    bool any = false;
    auto placeTile = [&](const fs::path& p, bool isDir) {
        any = true;
        if (col > 0) ImGui::SameLine(0, kTileSpacing);
        DrawTile(host, p, isDir);
        col = (col + 1) % columns;
    };
    for (const auto& d : dirs) if (matches(d.path())) placeTile(d.path(), true);
    for (const auto& f : files) if (matches(f.path())) placeTile(f.path(), false);
    ImGui::PopStyleVar();

    // Обложки файлов, которых в этой папке нет, больше не нужны — отпускаем и
    // их буферы. Иначе за сеанс блуждания по проекту накопился бы буфер на
    // КАЖДЫЙ ассет, который когда-либо показали, и все они висели бы в
    // видеопамяти до выхода из редактора.
    for (auto it = m_thumbs.begin(); it != m_thumbs.end();) {
        std::error_code exists;
        if (fs::path(it->first).parent_path() == cwd && fs::exists(it->first, exists)) {
            ++it;
            continue;
        }
        m_preview.ReleaseTarget(it->first);
        it = m_thumbs.erase(it);
    }
    if (!any) {
        ImGui::Spacing();
        ImGui::TextDisabled(m_search[0] ? "Nothing matches the search." : "This folder is empty.");
        ImGui::TextDisabled("%s", T("Right-click to create a folder, script, material or text file."));
    }

    // Создание ассетов — ПКМ по пустому месту (не по тайлу: у тайлов своё меню).
    if (ImGui::BeginPopupContextWindow("##assets_create_ctx",
                                       ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        auto startCreate = [this](CreateKind kind, const char* defaultName) {
            m_createKind = kind;
            std::snprintf(m_createName, sizeof(m_createName), "%s", defaultName);
            m_error.clear();
        };
        if (ImGui::MenuItem(T("New Folder"))) startCreate(CreateKind::Folder, "NewFolder");
        if (ImGui::MenuItem(T("New Script (.lua)"))) startCreate(CreateKind::Script, "new_script");
        if (ImGui::MenuItem(T("New Text File (.txt)"))) startCreate(CreateKind::TextFile, "notes");
        if (ImGui::MenuItem(T("New Material (.sagemat)"))) startCreate(CreateKind::Material, "NewMaterial");
        ImGui::Separator();
        if (ImGui::MenuItem(T("Convert the whole folder to engine formats"))) ConvertFolderHere(host);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("Models and images in this folder (including nested) -> .sagemesh/.sagetex.\n"
              "Already converted files are skipped, sources stay in place."));
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    DrawModals(host);

    ImGui::End();
}
