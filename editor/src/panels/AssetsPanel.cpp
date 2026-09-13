#include "../PanelWindows.h"
#include "AssetsPanel.h"
#include "../FolderColors.h"
#include "ui/UI.h"
#include "EditorTheme.h"

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
#include "../AssetSlot.h"
#include "../EditorIcons.h"

#include "sage/assets/import/Convert.h"
#include "sage/render/ResourceManager.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/assets/Zip.h"
#include "Project.h"
#include "sage/core/Log.h"
#include "../Localization.h"

namespace fs = std::filesystem;

namespace {

// Как выглядит карточка ассета: значок типа, цвет типа и короткий тег.
//
// РАНЬШЕ ТИП ПОКАЗЫВАЛСЯ ЗАЛИВКОЙ ВО ВСЮ КАРТОЧКУ. Папка с двумя десятками
// файлов превращалась в мозаику из насыщенных оранжевых, зелёных, бирюзовых и
// малиновых прямоугольников — глазу не за что зацепиться, а разница между
// «выделено» и «просто жёлтое» терялась совсем. Цвет остался как признак типа,
// но теперь он живёт в маленькой метке с расширением и в оттенке значка; поле
// обложки у всех карточек одинаково нейтральное — на нём видно САМО превью, а
// не краску вокруг него.
struct AssetStyle {
    ImVec4 Color;
    std::string Tag;
    const char* Icon;
};

std::string ToUpper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

std::string ToLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ЧТО ЭТО ЗА ФАЙЛ — одним словом, одинаково для всех расширений одного предмета.
// Расширение отвечает на вопрос «чем открыть», а человек в дереве проекта ищет
// предмет: модель, текстуру, звук. Для .obj, .gltf, .glb, .fbx и .sagemesh ответ
// один и тот же.
std::string KindLabel(const fs::path& path, bool isDir) {
    if (isDir) return T("Folder");
    const std::string ext = ToLower(path.extension().string());
    if (ext == ".sage") return T("Scene");
    if (ext == ".sageprefab") return T("Prefab");
    if (ext == ".sagemat") return T("Material");
    if (ext == ".lua") return T("Script");
    if (ext == ".obj" || ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".blend" ||
        ext == ".bbmodel" || ext == ".sagemesh")
        return T("Model");
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" ||
        ext == ".hdr" || ext == ".sagetex")
        return T("Texture");
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3") return T("Sound");
    if (ext == ".vert" || ext == ".frag" || ext == ".glsl") return T("Shader");
    if (ext == ".sageanim") return T("Animation clip");
    if (ext.empty()) return T("File");
    return ToUpper(ext.substr(1));
}

AssetStyle StyleForPath(const fs::path& path, bool isDir) {
    if (isDir) return { ImVec4(0.85f, 0.68f, 0.32f, 1.0f), "", AssetsPanel::FolderIcon(path) };
    std::string ext = ToLower(path.extension().string());
    if (ext == ".sage") return { ImVec4(0.45f, 0.62f, 0.95f, 1.0f), "scene", "scene" };
    if (ext == ".sageprefab") return { ImVec4(0.55f, 0.70f, 1.00f, 1.0f), "prefab", "prefab" };
    if (ext == ".sagemat") return { ImVec4(0.90f, 0.62f, 0.35f, 1.0f), "mat", "material" };
    if (ext == ".sageimport") return { ImVec4(0.62f, 0.58f, 0.48f, 1.0f), "import", "file" };
    if (ext == ".lua") return { ImVec4(0.45f, 0.82f, 0.50f, 1.0f), "lua", "script" };
    if (ext == ".obj" || ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".blend" ||
        ext == ".bbmodel" || ext == ".sagemesh")
        return { ImVec4(0.70f, 0.55f, 0.92f, 1.0f), ext.substr(1), "model" };
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" ||
        ext == ".hdr" || ext == ".sagetex")
        return { ImVec4(0.40f, 0.80f, 0.80f, 1.0f), ext.substr(1), "texture" };
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3")
        return { ImVec4(0.92f, 0.52f, 0.70f, 1.0f), ext.substr(1), "audio" };
    if (ext == ".vert" || ext == ".frag" || ext == ".glsl")
        return { ImVec4(0.68f, 0.68f, 0.74f, 1.0f), ext.substr(1), "shader" };
    std::string tag = ext.empty() ? "" : ToLower(ext.substr(1));
    return { ImVec4(0.66f, 0.66f, 0.70f, 1.0f), tag, "file" };
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
// Подпись — РОВНО ОДНА строка. Раньше под неё отводилось 34 пикселя «на одну-две
// строки», но рисовалась всегда одна: каждая карточка несла полторы строки
// пустоты, и сетка выглядела рыхлой.
constexpr float kLabelH = 22.0f;
// Под подписью ДВЕ строки: имя и тип. Тип — это второй вопрос к файлу («что это
// вообще такое»), и раньше на него отвечали только трёхбуквенная метка в углу
// обложки и значок. Для .obj, .gltf и .sagemesh метка разная, а предмет один —
// модель; словом это сказано один раз и одинаково.
constexpr float kTileH = kSwatchH + kLabelH * 2.0f + 8.0f;
constexpr float kTileSpacing = 12.0f;

} // namespace

// КОРЕНЬ ПАНЕЛИ — assets/ ПРОЕКТА, и выше него панель не поднимается.
//
// Всё, с чем панель умеет работать, живёт в assets/: только оттуда ассет
// попадает в сцену ссылкой, которая переживёт сборку игры, и только там его
// найдут импорт, превью и слоты. Выше лежат служебные файлы проекта
// (project.sageproj, sage.cfg, папка scenes/) — трогать их отсюда нечем, а
// показывать значит предлагать работу, которой панель не делает: человек
// заходил в папку проекта, видел «пусто» и решал, что ассеты потерялись.
//
// Возврат пути наружу тоже закрыт этим: путь за пределами assets/ в сцене не
// работает (Project::AssetRef), и запрещать его в слотах, продолжая водить туда
// панелью, — это два правила об одном и том же, из которых верно только одно.
fs::path AssetsPanel::AssetsRoot(EditorHost& host) {
    Project& project = host.CurrentProject();
    return project.Loaded() ? project.AssetsDir() : host.AssetsCwd();
}

// Загоняет текущую папку обратно в корень, если она оказалась снаружи. Так
// бывает после открытия другого проекта и после удаления папки, в которой
// стояли: молча остаться «нигде» хуже, чем вернуться в корень.
void AssetsPanel::ClampCwd(EditorHost& host) {
    const fs::path root = AssetsRoot(host);
    fs::path& cwd = host.AssetsCwd();
    std::error_code ec;
    if (!fs::exists(cwd, ec)) { cwd = root; return; }
    const fs::path c = fs::weakly_canonical(cwd, ec);
    const fs::path r = fs::weakly_canonical(root, ec);
    for (fs::path p = c; ; p = p.parent_path()) {
        if (p == r) return;                  // внутри корня — всё в порядке
        if (!p.has_parent_path() || p.parent_path() == p) break;
    }
    cwd = root;
}

void AssetsPanel::DrawBreadcrumb(EditorHost& host) {
    fs::path& cwd = host.AssetsCwd();
    // Цепочка обрывается на КОРНЕ ПАНЕЛИ: показывать дорогу туда, куда всё
    // равно не пустят, — значит обещать несуществующий ход.
    const fs::path root = AssetsRoot(host);

    std::vector<fs::path> chain;
    std::error_code cec;
    const fs::path rootCanon = fs::weakly_canonical(root, cec);
    for (fs::path p = cwd; p.has_parent_path(); p = p.parent_path()) {
        chain.push_back(p);
        if (fs::weakly_canonical(p, cec) == rootCanon) break;
        if (p.parent_path() == p) break; // достигли корня файловой системы
    }
    std::reverse(chain.begin(), chain.end());

    // Хвост, а не вся цепочка, и хвост ПО ШИРИНЕ ОКНА. Без проекта cwd — это
    // абсолютный путь вроде /home/user/work/<длинный-uuid>/assets: полная
    // цепочка съедала строку целиком и выталкивала за край окна всё, что стояло
    // правее. Начало пути всё равно никому не нужно — нужно то место, где
    // стоишь, и дорога на пару шагов назад. Сколько шагов поместится, столько и
    // показываем: панель Assets сужают до трети экрана, и фиксированное число
    // сегментов при такой ширине снова полезло бы за край.
    const float avail = ImGui::GetContentRegionAvail().x - 24.0f; // запас под «…/»
    const float sepW = ImGui::CalcTextSize(" / ").x;
    float used = 0.0f;
    size_t first = chain.size();
    while (first > 0) {
        std::string seg = chain[first - 1].filename().string();
        if (seg.empty()) seg = chain[first - 1].string();
        const float w = ImGui::CalcTextSize(seg.c_str()).x + 8.0f + sepW;
        if (first < chain.size() && used + w > avail) break;
        used += w;
        --first;
    }

    // Кнопки хлебных крошек — без рамки: это путь, а не ряд кнопок. Рамка
    // появляется под курсором, чтобы было видно, что по сегменту можно кликнуть.
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, ImGui::GetStyle().FramePadding.y));
    if (first > 0) {
        ImGui::TextDisabled("...");
        ImGui::SameLine(0, 2);
        ImGui::TextDisabled("/");
        ImGui::SameLine(0, 2);
    }
    for (size_t i = first; i < chain.size(); ++i) {
        std::string seg = chain[i].filename().string();
        if (seg.empty()) seg = chain[i].string();
        ImGui::PushID(static_cast<int>(i));
        // Текущая папка — не кнопка: щёлкать по ней некуда, и подсветка при
        // наведении обещала бы переход, которого не будет.
        if (i + 1 == chain.size()) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(seg.c_str());
        } else if (ImGui::Button(seg.c_str())) {
            cwd = chain[i];
        }
        ImGui::PopID();
        if (i + 1 < chain.size()) { ImGui::SameLine(0, 2); ImGui::TextDisabled("/"); ImGui::SameLine(0, 2); }
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}


// Превью для карточки. 0 — превью нет (папка, скрипт, сцена): для них тег
// честнее пустого квадрата.
//
// Картинки показываются сами собой, материал — шариком с этим материалом.
// Материал рендерится НЕ БОЛЬШЕ ОДНОГО ЗА КАДР и запоминается: это полный
// проход сцены со светом, и делать двадцать таких на открытие папки значит
// уронить редактор . Остальные карточки получат своё превью в
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
        if (std::shared_ptr<Mesh> mesh = ResourceManager::Instance().GetModel(key)) {
            // Обложка модели — с ЕЁ материалом, а не серым пластиком: иначе
            // текстурированный персонаж и болванка в сетке неотличимы.
            id = m_preview.RenderMesh(mesh, 96, key, AssetPreview::MaterialsForModel(key));
        }
    }
    // Ноль тоже запоминаем — иначе битый или пустой ассет пытался бы
    // отрисоваться каждый кадр, съедая всю очередь превью и не давая остальным
    // карточкам получить свои обложки.
    m_thumbs[key] = Thumb{id, stamp};
    if (id) m_thumbRenderedThisFrame = true;
    return id;
}

void AssetsPanel::DrawFolderNode(EditorHost& host, const fs::path& dir, int depth) {
    std::error_code ec;
    std::vector<fs::path> subdirs;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_directory(ec)) subdirs.push_back(entry.path());
    }
    std::sort(subdirs.begin(), subdirs.end());

    fs::path& cwd = host.AssetsCwd();
    const bool current = fs::weakly_canonical(cwd, ec) == fs::weakly_canonical(dir, ec);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DrawLinesNone;
    if (subdirs.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (current) flags |= ImGuiTreeNodeFlags_Selected;
    // Корень раскрыт сразу: свёрнутое дерево из одной строки не отвечает ни на
    // один вопрос и требует лишнего щелчка каждый раз.
    if (depth == 0) ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);

    ImGui::PushID(dir.string().c_str());
    const ImVec2 rowPos = ImGui::GetCursorScreenPos();
    const float indent = ImGui::GetTreeNodeToLabelSpacing();
    m_treeRows.push_back({rowPos.y, rowPos.x + indent, depth});
    const bool open = ImGui::TreeNodeEx("##folder", flags, "%s", "");
    // Значок папки — её цветом (тем же, что в сетке): метка обязана означать
    // одно и то же в обоих местах. Значок и подпись — одной парой с общим
    // зазором (EditorIcons::DrawLabeled), а не двумя пробелами в формате.
    {
        glm::vec3 tint(0.85f, 0.68f, 0.32f);
        sage::editor::foldercolors::Get(dir, tint);
        const ImVec4 c(tint.x, tint.y, tint.z, 1.0f);
        EditorIcons::DrawLabeled(ImGui::GetWindowDrawList(), ImVec2(rowPos.x + indent, rowPos.y),
                                 ImGui::GetTextLineHeight(),
                                 subdirs.empty() ? "folder" : "folder-full", ImGui::GetColorU32(c),
                                 dir.filename().string().c_str(),
                                 ImGui::GetColorU32(ImGuiCol_Text));
    }
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) cwd = dir;
    // Бросок файла на папку дерева — перенос в неё: то же, что и в сетке.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ASSET_PATH")) {
            std::string dropped((const char*)p->Data, (size_t)p->DataSize);
            if (!dropped.empty() && dropped.back() == '\0') dropped.pop_back();
            MoveIntoFolder(host, dropped, dir);
        }
        ImGui::EndDragDropTarget();
    }
    if (open && !subdirs.empty()) {
        for (const fs::path& sub : subdirs) DrawFolderNode(host, sub, depth + 1);
        // Линии связи — свои, по той же причине, что и в иерархии: встроенная в
        // ImGui горизонталь обрывается далеко от значка папки.
        {
            const float line = ImGui::GetTextLineHeight();
            const float spineX = std::floor(rowPos.x + indent * 0.5f);
            const ImU32 col = ImGui::GetColorU32(ImGuiCol_TreeLines);
            const float thickness = std::max(1.0f, ImGui::GetStyle().TreeLinesSize);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float lastMid = 0.0f;
            for (const TreeRow& r : m_treeRows) {
                if (r.Depth != depth + 1) continue;
                const float mid = std::floor(r.Y + line * 0.5f);
                lastMid = mid;
                dl->AddLine(ImVec2(spineX, mid),
                            ImVec2(r.IconX - EditorIcons::TextGap() * 0.5f, mid), col, thickness);
            }
            if (lastMid > 0.0f)
                dl->AddLine(ImVec2(spineX, std::floor(rowPos.y + line)), ImVec2(spineX, lastMid),
                            col, thickness);
            m_treeRows.erase(std::remove_if(m_treeRows.begin(), m_treeRows.end(),
                                            [depth](const TreeRow& r) { return r.Depth > depth; }),
                             m_treeRows.end());
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void AssetsPanel::DrawFolderTree(EditorHost& host) {
    ImGui::BeginChild("##assets_tree", ImVec2(m_treeWidth, 0), ImGuiChildFlags_Borders);
    m_treeRows.clear();
    // Корень дерева — assets/, а не папка проекта: см. AssetsRoot().
    DrawFolderNode(host, AssetsRoot(host), 0);
    ImGui::EndChild();

    // Разделитель: ширину дерева правят перетаскиванием. Не настройка в меню —
    // её меняют на глаз, пока смотрят на имена, которые не помещаются.
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::InvisibleButton("##assets_split", ImVec2(6.0f, ImGui::GetContentRegionAvail().y));
    if (ImGui::IsItemActive()) m_treeWidth += ImGui::GetIO().MouseDelta.x;
    m_treeWidth = std::clamp(m_treeWidth, 120.0f, 420.0f);
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2((a.x + b.x) * 0.5f - 1.0f, a.y), ImVec2((a.x + b.x) * 0.5f + 1.0f, b.y),
            ImGui::GetColorU32(ImGuiCol_NavHighlight));
    }
    ImGui::SameLine(0.0f, 0.0f);
}

void AssetsPanel::DrawTile(EditorHost& host, const fs::path& path, bool isDir) {
    AssetStyle style = StyleForPath(path, isDir);
    // Метка папки перекрывает общий жёлтый: две сотни одинаковых значков —
    // ровно то, из-за чего папку и приходится искать чтением имён.
    if (isDir) {
        glm::vec3 tint;
        if (sage::editor::foldercolors::Get(path, tint))
            style.Color = ImVec4(tint.r, tint.g, tint.b, 1.0f);
    }
    std::string filename = path.filename().string();

    ImGui::PushID(filename.c_str());

    // Весь тайл — ОДИН item (InvisibleButton на полную высоту карточки): подпись
    // теперь внутри его границ, строки грида больше не налезают друг на друга.
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##tile", ImVec2(kTileW, kTileH));
    bool hovered = ImGui::IsItemHovered();
    bool doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    bool isSelected = std::find(m_multi.begin(), m_multi.end(), path) != m_multi.end();

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

    // Поле обложки — нейтральное у ВСЕХ типов (см. AssetStyle). Тип читается по
    // значку и метке, а не по цвету всей карточки.
    constexpr float kInset = kTileInset;
    ImVec2 sw0(cursor.x + kInset, cursor.y + kInset);
    ImVec2 sw1(tileMax.x - kInset, cursor.y + kInset + kSwatchH - kInset);
    dl->AddRectFilled(sw0, sw1, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.28f)), 6.0f);

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
        // Значок типа по центру поля — тот же набор, что в иерархии, тулбаре и
        // инспекторе. Три буквы «MAT»/«LUA» приходилось читать, значок узнаётся.
        const float glyph = std::floor((sw1.y - sw0.y) * 0.52f);
        const ImVec4 tint = hovered || isSelected
                                ? style.Color
                                : ImVec4(style.Color.x * 0.82f, style.Color.y * 0.82f,
                                         style.Color.z * 0.82f, 1.0f);
        EditorIcons::Overlay(std::floor(sw0.x + (sw1.x - sw0.x - glyph) * 0.5f),
                             std::floor(sw0.y + (sw1.y - sw0.y - glyph) * 0.5f), glyph,
                             style.Icon, glm::vec3(tint.x, tint.y, tint.z));
    }

    // Метка расширения в углу поля: тип отличим и когда вместо значка стоит
    // превью (у .png и .sagetex обложка одинаковая — сама картинка).
    if (!style.Tag.empty()) {
        const std::string tag = style.Tag;
        const ImVec2 size = ImGui::CalcTextSize(tag.c_str());
        const ImVec2 p1(sw1.x - 4.0f, sw1.y - 4.0f);
        const ImVec2 p0(p1.x - size.x - 8.0f, p1.y - size.y - 2.0f);
        dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 150), 4.0f);
        dl->AddText(ImVec2(p0.x + 4.0f, p0.y + 1.0f),
                    ImGui::ColorConvertFloat4ToU32(style.Color), tag.c_str());
    }

    // Имя файла по центру области подписи (внутри границ тайла, с усечением).
    std::string label = TruncateToWidth(filename, kTileW - 8.0f);
    ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
    ImVec2 labelPos(std::floor(cursor.x + (kTileW - labelSize.x) * 0.5f), sw1.y + 5.0f);
    // Имя — главное, что читают в этой панели, поэтому оно нормального цвета
    // всегда. Приглушённой была ВСЯ сетка, и найти файл глазами по бледным
    // подписям было тяжелее, чем по цветной мозаике плашек.
    ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text, isSelected || hovered ? 1.0f : 0.85f);
    dl->AddText(labelPos, textCol, label.c_str());

    // Вторая строка — ЧТО ЭТО. Одним словом и одинаково для всех расширений
    // одного предмета: .obj, .gltf и .sagemesh — «модель».
    {
        const std::string kind = KindLabel(path, isDir);
        const std::string kindShown = TruncateToWidth(kind, kTileW - 24.0f);
        const ImVec2 kindSize = ImGui::CalcTextSize(kindShown.c_str());
        dl->AddText(ImVec2(std::floor(cursor.x + (kTileW - kindSize.x) * 0.5f),
                           labelPos.y + kLabelH),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), kindShown.c_str());
    }

    // «Ещё» — та же правая кнопка, но нажимаемая мышью без правой кнопки: на
    // ноутбуке и планшете правого клика может не быть вовсе, а действия над
    // файлом (переименовать, удалить, цвет папки) больше нигде не живут.
    {
        const float dots = ImGui::GetTextLineHeight();
        const ImVec2 at(tileMax.x - dots - 4.0f, tileMax.y - dots - 3.0f);
        const ImVec2 mouse = ImGui::GetMousePos();
        const bool overDots = mouse.x >= at.x && mouse.x <= at.x + dots && mouse.y >= at.y &&
                              mouse.y <= at.y + dots && hovered;
        if (hovered || isSelected) {
            EditorIcons::Overlay(at.x, at.y, dots, "list",
                                 overDots ? glm::vec3(0.95f, 0.78f, 0.30f)
                                          : glm::vec3(0.55f, 0.57f, 0.62f));
        }
        if (overDots && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_selected = path;
            if (std::find(m_multi.begin(), m_multi.end(), path) == m_multi.end()) m_multi = {path};
            ImGui::OpenPopup("##tile_ctx");
        }
    }

    // Источник перетаскивания: файл можно бросить в слот текстуры инспектора.
    // Путь передаётся строкой с завершающим нулём — принимающая сторона получает
    // ровно то, что открыла бы сама.
    // Папка перетаскивается тоже: в слот типа «папка» (небо-кубмап) её иначе
    // было бы нечем назначить, кроме как набрать путь руками, — а это ровно то,
    // от чего слоты и избавляют (см. AssetSlot.h).
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        // Полезная нагрузка и карточка под курсором — общие для всего
        // редактора (см. AssetSlot.h): и панель, и слоты компонентов начинают
        // перетаскивание одинаково, поэтому и принимающая сторона у них одна.
        assetslot::BeginDrag(path, &m_preview);
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

    // Рамка выделения: карточка засчитывается, если её прямоугольник задет.
    if (m_rectActive && sage::editor::rectselect::Hits(m_rect, cursor, tileMax)) {
        m_rectHits.push_back(path);
    }

    if (clicked && !m_rectActive) {
        // Ctrl/Shift — добавить или убрать из набора; обычный клик — один файл.
        // Без этого набор, собранный рамкой, разрушался бы первым же кликом по
        // соседнему файлу, и «обвести, потом добавить ещё один» не работало.
        if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift) {
            auto it = std::find(m_multi.begin(), m_multi.end(), path);
            if (it != m_multi.end()) {
                m_multi.erase(it);
                m_selected = m_multi.empty() ? fs::path{} : m_multi.back();
            } else {
                m_multi.push_back(path);
                m_selected = path;
            }
        } else {
            m_selected = path;
            m_multi = {path};
        }
    }
    if (doubleClicked) {
        if (isDir) host.AssetsCwd() = path;
        else if (path.extension() == ".sage") host.LoadSceneFromFile(path);
        else if (path.extension() == ".sageprefab") host.InstantiatePrefab(path); // инстанс в сцену
        else {
            // Текст открывается ТЕМ, ЧЕМ ЕГО ОТКРЫВАЕТ СИСТЕМА. Своего редактора
            // кода у SAGE больше нет: он всегда проигрывал бы тому, что у
            // человека уже стоит, а горячая перезагрузка от этого не страдает —
            // движок следит за файлом, а не за тем, кто его сохранил.
            const std::string ext = path.extension().string();
            if (ext == ".lua" || ext == ".vert" || ext == ".frag" || ext == ".glsl" ||
                ext == ".txt" || ext == ".md" || ext == ".json") {
                // Отказ системы виден: молчащий двойной клик неотличим от
                // сломанного редактора.
                if (!host.OpenFileInSystemEditor(path)) {
                    host.SetStatusMessage(T("The system has no program for this file: ") +
                                          path.filename().string());
                }
            }
        }
    }
    if (ImGui::BeginPopupContextItem("##tile_ctx")) {
        // ПКМ по файлу ВНЕ набора выбирает его одного: меню всегда про то, на
        // что нажали. ПКМ по файлу ИЗ набора набор сохраняет — иначе обвести
        // рамкой двадцать файлов и удалить их разом было бы нельзя.
        if (std::find(m_multi.begin(), m_multi.end(), path) == m_multi.end()) {
            m_selected = path;
            m_multi = {path};
        } else {
            m_selected = path;
        }
        // Переименование — всегда про ОДИН файл: у двадцати файлов общего имени
        // нет, и придумывать правило вроде «имя + номер» здесь не за чем.
        if (isDir) {
            namespace foldercolors = sage::editor::foldercolors;
            if (ImGui::BeginMenu(T("Folder Colour"))) {
                for (const foldercolors::Tint& tint : foldercolors::Palette()) {
                    // Образец рядом с названием: цвет выбирают глазами, а
                    // список из восьми слов цвета не показывает.
                    const ImVec2 at = ImGui::GetCursorScreenPos();
                    const float box = ImGui::GetTextLineHeight();
                    // Место под образец — пробелами ровно по его ширине с общим
                    // зазором, а не «три пробела на глаз»: ширина пробела
                    // зависит от шрифта, и на другом масштабе подпись налезала
                    // на квадратик.
                    const float gap = EditorIcons::TextGap();
                    const float spaceW = ImGui::CalcTextSize(" ").x;
                    const int count = spaceW > 0.0f ? (int)std::ceil((box + gap) / spaceW) : 2;
                    const bool picked =
                        ImGui::MenuItem((std::string((size_t)count, ' ') + tint.Label).c_str());
                    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
                    const float x = at.x + count * spaceW - gap - box;
                    const float y = std::floor(r0.y + ((r1.y - r0.y) - box) * 0.5f);
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        ImVec2(x + 2.0f, y + 2.0f), ImVec2(x + box - 2.0f, y + box - 2.0f),
                        ImGui::GetColorU32(ImVec4(tint.Color.r, tint.Color.g, tint.Color.b, 1.0f)),
                        3.0f);
                    if (picked) foldercolors::Set(path, tint.Color);
                }
                ImGui::Separator();
                if (EditorIcons::MenuItem("folder", T("No colour"))) foldercolors::Clear(path);
                ImGui::EndMenu();
            }
        }
        if (EditorIcons::MenuItem("pencil", T("Rename"))) { m_renameTarget = path; m_error.clear(); }
        if (EditorIcons::MenuItem("trash", T("Delete"))) { m_deleteTargets = m_multi; }

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
const char* AssetsPanel::FolderIcon(const fs::path& dir) {
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) return "folder"; // нечитаемая папка — пусть выглядит пустой, а не полной
    // Сайдкары .meta не в счёт: это служебная запись движка на каждый файл, и
    // без своего файла она не остаётся. Сам файл здесь же и папку заполняет.
    for (const fs::directory_entry& e : it) {
        if (!e.is_directory(ec) && e.path().extension() == ".meta") continue;
        return "folder-full";
    }
    return "folder";
}

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
    for (fs::path& p : m_multi) if (p == source) p = target;
    // Метка едет с папкой: иначе раскладка рассыпалась бы от первого переезда.
    sage::editor::foldercolors::Rename(source, target);
    // Пересканировать проект: база ассетов помнит пути, и после переезда её
    // ответ на «где этот файл» обязан измениться.
    sage::AssetDatabase::Instance().ScanProject(host.CurrentProject().Dir().string());
    host.SetStatusMessage(T("Moved into ") + folder.filename().string() + ": " +
                          source.filename().string());
}

// Переименование ассета вместе с сайдкарами. Сайдкар .meta — это личность
// файла: в нём лежит GUID, по которому сцены его и находят. Оставить .meta под
// старым именем значит превратить переименование в «удалили один ассет,
// создали другой»: все ссылки на него в сценах становятся битыми, а рядом
// навсегда поселяется запись про файл, которого нет. В панели такая запись
// теперь даже не видна — сайдкары в сетке скрыты.
bool AssetsPanel::RenameAsset(const fs::path& path, const std::string& newName,
                              fs::path& outRenamed, std::string& err) {
    if (newName.empty()) {
        err = T("The name cannot be empty");
        return false;
    }
    const fs::path target = path.parent_path() / newName;
    std::error_code ec;
    if (fs::exists(target, ec)) {
        err = T("A file with this name already exists");
        return false;
    }
    fs::rename(path, target, ec);
    if (ec) {
        err = "Rename failed: " + ec.message();
        return false;
    }
    for (const char* sidecar : {".meta", ".sageimport"}) {
        const fs::path from = path.string() + sidecar;
        std::error_code sec;
        if (fs::exists(from, sec)) fs::rename(from, target.string() + sidecar, sec);
    }
    outRenamed = target;
    return true;
}

// Удаление ассета вместе с сайдкарами: иначе от него остаётся невидимая запись
// .meta, которую база ассетов честно считает существующим ассетом.
void AssetsPanel::DeleteAsset(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
    if (ec) LOG_ERROR("Editor") << "Asset delete failed: " << ec.message();
    for (const char* sidecar : {".meta", ".sageimport"}) {
        std::error_code sec;
        fs::remove(path.string() + sidecar, sec);
    }
}

AssetsPanel::ImportReport AssetsPanel::ImportAsset(const fs::path& source, const fs::path& destDir) {
    ImportReport report;
    std::error_code ec;

    if (!fs::exists(source, ec)) {
        report.Error = T("File not found: ") + source.string();
        return report;
    }
    fs::create_directories(destDir, ec);
    if (ec) {
        report.Error = T("The folder is not accessible: ") + ec.message();
        return report;
    }

    // --- ПАПКА ЦЕЛИКОМ -------------------------------------------------------
    //
    // Скачанный ассет — это чаще всего папка: модель, набор карт, лицензия,
    // превью. Вносить её по файлу значит десяток раз открыть диалог и один раз
    // ошибиться, а разбор спутников (.mtl, .bin, текстуры) тут и не нужен —
    // внутри и так уже всё, что нужно модели.
    if (fs::is_directory(source, ec)) {
        const fs::path target = destDir / source.filename();
        fs::copy(source, target,
                 fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
        if (ec) {
            report.Error = T("Could not copy: ") + source.filename().string();
            return report;
        }
        for (const auto& entry : fs::recursive_directory_iterator(target, ec)) {
            if (entry.is_directory(ec)) continue;
            RegisterInDatabase(entry.path());
            report.Extra.push_back(entry.path());
        }
        report.Ok = true;
        report.Created = target;
        return report;
    }

    // --- АРХИВ ---------------------------------------------------------------
    //
    // Определяется ПО СОДЕРЖИМОМУ, а не по расширению: тот же zip приезжает и
    // как .zip, и без расширения вовсе. Распаковывается в свою папку, а не
    // вперемешку с тем, что уже лежит рядом: в архиве часто десятки файлов, и
    // высыпать их в открытую папку проекта значит превратить её в свалку,
    // которую потом разбирать руками.
    if (sage::assets::IsZip(source)) {
        const fs::path target = destDir / source.stem();
        std::string zipErr;
        std::vector<std::string> skipped;
        const int written = sage::assets::ExtractZip(source, target, zipErr, &skipped);
        if (written < 0) {
            report.Error = T("The archive could not be unpacked: ") + zipErr;
            return report;
        }
        for (const auto& entry : fs::recursive_directory_iterator(target, ec)) {
            if (entry.is_directory(ec)) continue;
            RegisterInDatabase(entry.path());
            report.Extra.push_back(entry.path());
        }
        report.Missing = skipped;
        report.Ok = true;
        report.Created = target;
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

    if (EditorIcons::Button("open", T("Import..."))) {
        FileBrowser::Config c;
        c.Title = T("Bring into the project");
        // ФАЙЛ, ПАПКА ИЛИ АРХИВ — одной кнопкой. Скачивают по-разному: модель
        // одним файлом, набор текстур папкой, ассет с маркетплейса архивом.
        // Кнопка, которая берёт только файл, на двух из трёх случаев отвечает
        // «выберите файл» — и человек идёт распаковывать и перетаскивать руками
        // ровно то, что программа сделала бы за секунду.
        c.Mode = FileBrowser::PickMode::OpenAny;
        // Пусто — показывать всё: в проект вносят и модели, и картинки, и звук,
        // и чужие скрипты, а перечислять их фильтром значит однажды забыть
        // формат, который движок уже понимает.
        c.FilterLabel = T("All files");
        m_importBrowser.Open(c);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Copy an outside file, folder or .zip into the current project folder.\n"
                                  "A model moves together with its .mtl/.bin files and textures;\n"
                                  "an archive is unpacked into a folder of its own."));
    }

    if (!m_importBrowser.Draw()) return;

    const ImportReport r = ImportAsset(m_importBrowser.Result(), host.AssetsCwd());
    if (!r.Ok) {
        host.SetStatusMessage(T("Import failed: ") + r.Error);
        LOG_ERROR("Editor") << "Импорт не удался: " << r.Error;
        return;
    }
    m_selected = r.Created;
    m_multi = {r.Created};

    std::string message = T("Brought in: ") + r.Created.filename().string();
    if (!r.Extra.empty()) message += " (+" + std::to_string(r.Extra.size()) + T(" file(s) inside)");
    if (!r.Missing.empty()) {
        // Недостающие спутники (и пропущенные записи архива) — это будущее
        // «модель без текстуры», и узнать о них надо здесь. Список уходит в
        // консоль целиком: в статусной строке ему не поместиться, а первое имя
        // уже подсказывает, что искать.
        message += T("; not brought in: ") + r.Missing.front();
        if (r.Missing.size() > 1) message += T(" and ") + std::to_string(r.Missing.size() - 1);
        for (const std::string& m : r.Missing)
            LOG_WARN("Editor") << "Импорт: не внесено — " << m;
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
                "    \"metallic\": 0.0,\n"
                "    \"roughness\": 0.5,\n"
                "    \"cull\": \"back\",\n"
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
                m_multi = {created};
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
            fs::path target;
            if (!RenameAsset(m_renameTarget, m_renameBuf, target, m_error)) {
                LOG_ERROR("Editor") << "Asset rename failed: " << m_error;
            } else {
                sage::editor::foldercolors::Rename(m_renameTarget, target);
                if (m_selected == m_renameTarget) m_selected = target;
                for (fs::path& p : m_multi) if (p == m_renameTarget) p = target;
                sage::AssetDatabase::Instance().ScanProject(
                    host.CurrentProject().Dir().string());
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

    if (!m_deleteTargets.empty() && !ImGui::IsPopupOpen("Delete Asset")) {
        ImGui::OpenPopup("Delete Asset");
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(T("Delete Asset" "###Delete Asset"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        // Один файл называется по имени, набор — числом. «Удалить 17 файлов?»
        // отвечает на тот же вопрос, что и имя: понимаю ли я, что сейчас
        // исчезнет.
        if (m_deleteTargets.size() == 1) {
            ImGui::Text(T("Delete \"%s\"?"), m_deleteTargets.front().filename().string().c_str());
        } else {
            ImGui::Text(T("Delete selected files: %zu?"), m_deleteTargets.size());
        }
        ImGui::TextDisabled("%s", T("This cannot be undone."));
        ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(EditorTheme::Role::Danger));
        if (ImGui::Button(T("Delete"), ImVec2(120, 0))) {
            for (const fs::path& victim : m_deleteTargets) {
                DeleteAsset(victim);
                if (m_selected == victim) m_selected.clear();
                auto it = std::find(m_multi.begin(), m_multi.end(), victim);
                if (it != m_multi.end()) m_multi.erase(it);
            }
            m_deleteTargets.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(120, 0))) {
            m_deleteTargets.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void AssetsPanel::Draw(EditorHost& host, bool* open) {
    // Бюджет превью на кадр: см. ThumbnailFor.
    m_thumbRenderedThisFrame = false;
    Project& project = host.CurrentProject();
    ClampCwd(host);
    fs::path& cwd = host.AssetsCwd();

    ImGui::Begin(T("Assets" "###Assets"), open, panelwindows::WindowFlags("Assets"));

    // --- Шапка панели: две строки с ЯСНЫМ разделением обязанностей ---
    //
    // Раньше здесь в одну строку выкладывались SmallButton «Вверх», хлебные
    // крошки, обычная кнопка «Импорт…» и следом поле поиска во всю ширину.
    // Виджеты разной высоты в одном ряду и строка поиска, ничем не отделённая
    // от содержимого, и читались как «интерфейс кривой и смешанный»: глазу не
    // за что зацепиться, потому что группы не выделены. Теперь первая строка —
    // ТОЛЬКО навигация (где я нахожусь), вторая — ТОЛЬКО действия над этой
    // папкой (что я здесь ищу и что приношу), и все виджеты в ряду одной высоты.
    // Выше корня проекта панель не поднимается: снаружи проекта её файлы
    // редактору не принадлежат, а ссылка на них не переживёт сборку игры.
    const fs::path root = AssetsRoot(host);
    std::error_code upec;
    const bool atRoot = fs::weakly_canonical(cwd, upec) == fs::weakly_canonical(root, upec);
    const bool canGoUp = cwd.has_parent_path() && !atRoot;
    ImGui::BeginDisabled(!canGoUp);
    if (EditorIcons::IconOnlyButton("up", T("Up"))) cwd = cwd.parent_path();
    ImGui::EndDisabled();
    ImGui::SameLine(0, 6);
    DrawBreadcrumb(host);

    // Внесение файлов со стороны. До сих пор этого не было вовсе: панель умела
    // ходить по папкам проекта и создавать пустые ассеты, а положить в проект
    // СВОЮ модель или картинку можно было только мимо редактора — проводником.
    // Человек при этом обычно шёл другим путём: выбирал файл прямо из «Загрузок»
    // через «Обзор…», получал в сцене абсолютный путь и рабочий вид ровно до
    // первой сборки игры (см. Project::AssetRef).
    const float importW = ImGui::CalcTextSize(T("Import...")).x +
                          ImGui::GetFrameHeight() * 0.68f + ImGui::GetStyle().FramePadding.x * 3.0f;
    // Через Sage::UI: одно поле поиска на весь редактор — с иконкой внутри,
    // тем же отступом и той же высотой, что в консоли и в палитре команд. Три
    // самодельных поля выглядели тремя разными полями.
    const float searchW = std::max(120.0f, ImGui::GetContentRegionAvail().x - importW -
                                               ImGui::GetStyle().ItemSpacing.x);
    Sage::UI::SearchField("assets_search", m_search, sizeof(m_search), T("Search..."), searchW);
    ImGui::SameLine();
    DrawImportButton(host);

    // Сломанные ссылки — В ПАНЕЛИ, а не только в логе. Именно молчание и было
    // исходной болезнью: сцена грузилась, объект стоял на месте, просто без
    // модели, и заметить это можно было лишь случайно.
    const auto& broken = sage::AssetDatabase::Instance().Broken();
    if (!broken.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Danger));
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
                sage::AssetDatabase::Instance().ScanProject(project.Dir().string());
            }
        }
    }
    ImGui::Separator();

    // Какой проект обслуживаем (метки папок лежат в нём). Дёшево: перечитывает
    // файл только при СМЕНЕ проекта.
    sage::editor::foldercolors::SetProject(host.CurrentProject().Dir());

    DrawFolderTree(host);

    ImGui::BeginChild("##assets_scroll");
    namespace rectselect = sage::editor::rectselect;
    m_rectActive = rectselect::Begin(m_rect);
    m_rectHits.clear();
    std::error_code ec;
    std::vector<fs::directory_entry> dirs, files;
    for (const auto& entry : fs::directory_iterator(cwd, ec)) {
        // Сайдкары .meta в сетке не показываем. Это служебная запись движка
        // (GUID ассета, см. AssetDatabase) — по одной НА КАЖДЫЙ файл: папка с
        // двадцатью ассетами показывала сорок карточек, половина из которых
        // одинаковые серые «meta», которые нельзя ни открыть, ни осмысленно
        // править. Файл при этом никуда не девается и переезжает вместе с
        // ассетом при переносе и переименовании.
        if (!entry.is_directory(ec) && entry.path().extension() == ".meta") continue;
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
        ImGui::TextDisabled("%s", m_search[0] ? T("Nothing matches the search.")
                                             : T("This folder is empty."));
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
        if (EditorIcons::MenuItem("folder-plus", T("New Folder"))) startCreate(CreateKind::Folder, "NewFolder");
        if (EditorIcons::MenuItem("script", T("New Script (.lua)"))) startCreate(CreateKind::Script, "new_script");
        if (EditorIcons::MenuItem("file", T("New Text File (.txt)"))) startCreate(CreateKind::TextFile, "notes");
        if (EditorIcons::MenuItem("material", T("New Material (.sagemat)"))) startCreate(CreateKind::Material, "NewMaterial");
        ImGui::Separator();
        if (EditorIcons::MenuItem("refresh", T("Convert the whole folder to engine formats"))) ConvertFolderHere(host);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("Models and images in this folder (including nested) -> .sagemesh/.sagetex.\n"
              "Already converted files are skipped, sources stay in place."));
        }
        ImGui::EndPopup();
    }

    // Рамка выделения. Начинается только в пустом месте сетки: над карточкой
    // живёт перетаскивание файла, и рамка отняла бы его.
    if (m_rectActive && m_rect.Finished && rectselect::Meaningful(m_rect)) {
        if (!m_rect.Additive) m_multi.clear();
        for (const fs::path& hit : m_rectHits) {
            if (std::find(m_multi.begin(), m_multi.end(), hit) == m_multi.end())
                m_multi.push_back(hit);
        }
        m_selected = m_multi.empty() ? fs::path{} : m_multi.back();
    }
    rectselect::End(m_rect, ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                                !ImGui::IsAnyItemHovered() &&
                                !ImGui::IsPopupOpen("##assets_create_ctx"));
    rectselect::Draw(m_rect);
    ImGui::EndChild();

    DrawModals(host);

    ImGui::End();
}
