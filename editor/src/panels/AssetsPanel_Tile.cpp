#include "../AssetOpen.h"
#include "../CreateAssetDialog.h"
#include "../DragSet.h"
#include "../PanelWindowId.h"
#include "../PanelWindows.h"
#include "AssetsPanel.h"
#include "FsCache.h"
#include "ModelImportDialog.h"
#include "../Progress.h"

#include <chrono>
#include <future>
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
#include "imgui_internal.h" // BeginDragDropTargetCustom: объект на всё окно сетки

#include "EditorHost.h"
#include "../AssetSlot.h"
#include "../EditorIcons.h"
#include "../HotkeyScope.h"
#include "../SceneCover.h"
#include "../Thumbnails.h"
#include "../AssetCovers.h"

#include "sage/assets/import/Convert.h"
#include "sage/render/ResourceManager.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/assets/Zip.h"
#include "Project.h"
#include "sage/core/Log.h"
#include "sage/ecs/LightSystem.h"
#include "sage/scene/Components.h"
#include "sage/scene/SceneSerializer.h"
#include "../Localization.h"

#include "AssetsPanelDetail.h"

namespace fs = std::filesystem;



namespace covers = sage::editor::covers;
using namespace sage::editor::assetspanel_detail;

// Плитка ассета: обложка, значок типа, подпись и всё, что с плиткой делают мышью.
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
    if (ext == ".slua") return T("SAGE Lua script");
    if (ext == ".obj" || ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".blend" ||
        ext == ".bbmodel" || ext == ".sagemesh")
        return T("Model");
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" ||
        ext == ".hdr" || ext == ".sagetex")
        return T("Texture");
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3") return T("Sound");
    if (ext == ".vert" || ext == ".frag" || ext == ".glsl") return T("Shader");
    if (ext == ".sageanim") return T("Animation clip");
    if (ext == ".sageclip") return T("Property clip");
    if (ext.empty()) return T("File");
    return ToUpper(ext.substr(1));
}

AssetStyle StyleForPath(const fs::path& path, bool isDir) {
    if (isDir) return { ImVec4(0.85f, 0.68f, 0.32f, 1.0f), "", AssetsPanel::FolderIcon(path) };
    std::string ext = ToLower(path.extension().string());
    if (ext == ".sage") return { ImVec4(0.45f, 0.62f, 0.95f, 1.0f), "scene", "scene" };
    if (ext == ".sageprefab") return { ImVec4(0.55f, 0.70f, 1.00f, 1.0f), "prefab", "prefab" };
    if (ext == ".sagemat") return { ImVec4(0.90f, 0.62f, 0.35f, 1.0f), "mat", "material" };
    if (ext == ".sageclip" || ext == ".sageanim")
        return { ImVec4(0.55f, 0.72f, 0.95f, 1.0f), "clip", "anim" };
    if (ext == ".sageimport") return { ImVec4(0.62f, 0.58f, 0.48f, 1.0f), "import", "file" };
    if (ext == ".lua") return { ImVec4(0.45f, 0.82f, 0.50f, 1.0f), "lua", "script" };
    if (ext == ".slua") return { ImVec4(0.40f, 0.85f, 0.75f, 1.0f), "slua", "script" };
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
} // namespace

// Превью для карточки. 0 — превью нет (папка, скрипт, сцена): для них тег
// честнее пустого квадрата.
//
// Картинки показываются сами собой, материал — шариком с этим материалом.
// Материал рендерится НЕ БОЛЬШЕ ОДНОГО ЗА КАДР и запоминается: это полный
// проход сцены со светом, и делать двадцать таких на открытие папки значит
// уронить редактор . Остальные карточки получат своё превью в
// следующих кадрах — за десяток кадров это незаметно глазу.
AssetsPanel::Cover AssetsPanel::ThumbnailFor(EditorHost& host, const fs::path& path, bool isDir) {
    if (isDir) return {};
    const std::string key = path.string();

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    // СЦЕНА — СВОЕЙ ОБЛОЖКОЙ (см. SceneCover.h). Файл .sage показывался значком,
    // одинаковым у всех сцен проекта, и выбрать нужную из трёх можно было
    // только открыв каждую. Обложка снимается при сохранении сцены, поэтому
    // здесь её только показывают: рисовать сцену в панели значило бы грузить
    // её целиком ради картинки размером с ноготь.
    if (ext == ".sage") {
        const fs::path cover = scenecover::For(host.CurrentProject().Dir(), path);
        if (cover.empty()) return {};
        // Через общий кэш обложек-картинок: он асинхронный и с мипмапами, то
        // есть снимок 1920x1080 не разбирается в кадре и не рябит в плитке.
        const thumbs::Thumb t = thumbs::Get(cover, thumbs::Size::Tile);
        return {t.Id, t.W, t.H};
    }

    if (thumbs::IsImage(path)) {
        // ЧЕРЕЗ КЭШ ОБЛОЖЕК, а не через ResourceManager. Прежний путь просил у
        // менеджера ресурсов САМУ текстуру: полный синхронный разбор файла и
        // заливка его в видеопамять прямо в кадре — папка с сотней фотографий
        // по 4000x3000 вставала колом и съедала гигабайты ради картинок
        // размером с ноготь (см. Thumbnails.h). И главное: оттуда не узнать
        // пропорций исходника, а без них обложка рисовалась квадратом.
        const thumbs::Thumb t = thumbs::Get(path, thumbs::Size::Tile);
        return {t.Id, t.W, t.H};
    }

    // Материал — шариком, префаб — собой, модель — своей геометрией. Все трое
    // идут через один кэш и одну очередь «не больше одного рендера за кадр»:
    // каждый такой рендер это полный проход сцены со светом, и папка с двумя
    // десятками префабов иначе уронила бы кадр при первом же открытии.
    const bool renderable = ext == ".sagemat" || ext == ".sageprefab" ||
                            sage::assets::IsConvertibleModel(key) || ext == ".sagemesh";
    if (!renderable) return {};

    // Время правки — с памятью (не чаще раза в секунду на файл): обложка
    // спрашивает его в каждом кадре, и пересохранённый материал всё равно
    // переснимется через секунду.
    const long long stamp = sage::editor::fscache::Stamp(path);

    // Кадр съёмки квадратный, поэтому пропорции у таких обложек 1:1.
    auto it = m_thumbs.find(key);
    if (it != m_thumbs.end() && it->second.Stamp == stamp) {
        const std::shared_ptr<Material> mat = it->second.Mat.lock();
        if (!mat || mat->Fingerprint() == it->second.Print) return {it->second.Id, 1, 1};
    }
    if (m_thumbRenderedThisFrame) {
        // Очередь занята — отдаём прошлую обложку, если она была. Мигание
        // «пусто -> картинка» на каждой правке файла заметнее, чем кадр
        // устаревшего превью.
        return it != m_thumbs.end() ? Cover{it->second.Id, 1, 1} : Cover{};
    }

    // Ключ = путь: у каждой обложки СВОЙ буфер. С общим буфером все запомненные
    // хендлы указывали бы на одну текстуру, и вся папка показывала бы то, что
    // нарисовали последним (см. комментарий про key в AssetPreview).
    uint64_t id = 0;
    // Карты материала ещё едут фоном (редактор грузит их потоково): снимок,
    // сделанный сейчас, запомнил бы серые заглушки навсегда. Такая обложка
    // показывается, но не запоминается — её переснимут, когда карты доедут.
    bool provisional = false;
    std::shared_ptr<Material> shot;
    if (ext == ".sagemat") {
        shot = ResourceManager::Instance().GetMaterial(key);
        if (shot) id = m_preview.RenderMaterial(shot, 96, key);
        provisional = ResourceManager::Instance().PendingAsyncTextures() > 0;
    } else if (ext == ".sageprefab") {
        id = m_preview.RenderPrefab(key, 96, key);
        provisional = ResourceManager::Instance().PendingAsyncTextures() > 0;
    } else {
        // Модель — ФОНОМ (см. AssetPreview::RenderModelCover): разбор файла,
        // материалов и уменьшение карт идут в другом потоке, а кэш ресурсов
        // сцены не трогается. Пока разбор идёт — крутилка на плитке.
        bool pending = false;
        id = m_preview.RenderModelCover(key, 96, key, pending);
        if (pending) {
            return it != m_thumbs.end() ? Cover{it->second.Id, 1, 1} : Cover{};
        }
    }
    // Ноль тоже запоминаем — иначе битый или пустой ассет пытался бы
    // отрисоваться каждый кадр, съедая всю очередь превью и не давая остальным
    // карточкам получить свои обложки.
    m_thumbs[key] = Thumb{id, provisional ? -1 : stamp, shot, shot ? shot->Fingerprint() : 0};
    if (id) m_thumbRenderedThisFrame = true;
    return {id, 1, 1};
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
    // Обложку просят только ВИДИМЫЕ плитки: папка набора — это сотни файлов,
    // и заказывать разбор модели для плитки за краем окна значило грузить
    // машину тем, чего никто не видит.
    const Cover thumb = ImGui::IsItemVisible() ? ThumbnailFor(host, path, isDir) : Cover{};
    if (thumb.Id) {
        // КАРТИНКА ВПИСЫВАЕТСЯ ПО СВОИМ ПРОПОРЦИЯМ, а не в квадрат. Прежний
        // «квадрат по меньшей стороне» сохранял пропорции только у квадратных
        // исходников: панорама 4096x1024 выходила квадратом, тайл-лист 1:4 —
        // квадратом, скриншот 16:9 — квадратом. Узнать по такой обложке свой
        // файл нельзя, а она ровно для этого и нужна.
        const covers::FitRect fit = covers::Fit(sw0, sw1, thumb.W, thumb.H);
        // Шахматка — ПОД САМОЙ КАРТИНКОЙ, а не под всей площадкой: иначе поля
        // по бокам вписанной картинки выглядят её частью.
        covers::DrawChecker(dl, fit.A, fit.B);
        dl->AddImage((ImTextureID)(std::intptr_t)thumb.Id, fit.A, fit.B, ImVec2(0, 1),
                     ImVec2(1, 0));
        dl->AddRect(sw0, sw1, IM_COL32(255, 255, 255, 30), 6.0f);
    } else if (!isDir && thumbs::IsImage(path)) {
        // Картинка ещё читается фоновым потоком — крутилка вместо пустоты:
        // пустая площадка читается как «здесь ничего нет».
        covers::DrawSpinner(dl, ImVec2((sw0.x + sw1.x) * 0.5f, (sw0.y + sw1.y) * 0.5f),
                            std::min(sw1.x - sw0.x, sw1.y - sw0.y) * 0.18f,
                            ImGui::GetColorU32(ImGuiCol_TextDisabled));
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

    // СТАРТОВАЯ СЦЕНА ВИДНА В СПИСКЕ, а не только в меню. Иначе «с какой сцены
    // начнётся игра» — вопрос, на который отвечают, открывая по очереди меню
    // каждой сцены проекта.
    if (!isDir && path.extension() == ".sage" && host.CurrentProject().Loaded() &&
        host.CurrentProject().StartScene() == path.stem().string()) {
        const std::string tag = T("start");
        const ImVec2 size = ImGui::CalcTextSize(tag.c_str());
        const ImVec2 p0(cursor.x + 4.0f, cursor.y + 4.0f);
        const ImVec2 p1(p0.x + size.x + 8.0f, p0.y + size.y + 2.0f);
        dl->AddRectFilled(p0, p1, IM_COL32(40, 120, 60, 210), 4.0f);
        dl->AddText(ImVec2(p0.x + 4.0f, p0.y + 1.0f), IM_COL32(230, 255, 235, 255), tag.c_str());
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

    // ПРЕВЬЮ ПОД КУРСОРОМ — то же самое, что в файловом диалоге
    // (AssetCovers.h). Плитка 80x80 отвечает на вопрос «который из этих
    // файлов» и молчит обо всём остальном: та ли это картинка, что на ней
    // написано, какого она размера, почему весит сорок мегабайт. До сих пор
    // ответ добывался открыванием файла.
    //
    // ПОСЛЕ плитки и с задержкой (ForTooltip): подсказка уходит в своё окно, и
    // открывать её раньше, чем дорисована плитка, значит перемешивать два
    // списка отрисовки, а без задержки она выскакивала бы у каждой карточки,
    // над которой просто провели мышью. Во время перетаскивания ImGui сам не
    // считает плитку наведённой, так что тянущийся файл подсказку не откроет.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        std::error_code sizeEc;
        const uintmax_t bytes = isDir ? 0 : fs::file_size(path, sizeEc);
        // Папка проекта — чтобы у сцены (.sage) показался её снимок, а не
        // значок: обложка у сцены есть, просто лежит она рядом с проектом.
        covers::DrawHoverPreview(path, isDir, filename, sizeEc ? 0 : bytes, &m_preview,
                                 host.CurrentProject().Dir());
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
        // Взялись за выделенный файл — едет весь набор, в порядке сетки.
        std::vector<std::string> group;
        for (const fs::path& g : dragset::Ordered(path, m_multi, m_visibleOrder)) group.push_back(g.string());
        dragset::BeginAssets(path.string(), group);
        assetslot::BeginDrag(path, &m_preview);
        if (group.size() > 1) {
            ImGui::TextDisabled("%s", dragset::Caption(path.filename().string(), group.size()).c_str());
        }
        ImGui::EndDragDropSource();
    }

    // Папка принимает файлы: бросок ПЕРЕМЕЩАЕТ, а не копирует — это раскладка
    // уже своих ассетов по местам, и вторая копия тут никому не нужна. Ссылки
    // в сценах переживают переезд: они держатся за GUID из сайдкара .meta,
    // который едет вместе с файлом (см. sage/assets/AssetDatabase.h).
    if (isDir && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ASSET_PATH"))
            MoveGroupIntoFolder(host, dragset::Assets(p), path);
        AcceptObjectAsPrefab(host, path);
        ImGui::EndDragDropTarget();
    }

    // Рамка выделения: карточка засчитывается, если её прямоугольник задет.
    if (m_rectActive && sage::editor::rectselect::Hits(m_rect, cursor, tileMax)) {
        m_rectHits.push_back(path);
    }

    if (clicked && !m_rectActive) {
        // ТРИ РАЗНЫХ ЩЕЛЧКА, а не два.
        //
        //   • Shift — ДИАПАЗОН от якоря до этого файла. Так выделяют в любом
        //     проводнике, и без него выбрать сорок файлов подряд можно было
        //     только Ctrl-кликом по каждому или рамкой — а рамка в длинном
        //     списке требует прокрутки с зажатой кнопкой.
        //   • Ctrl — добавить или убрать ОДИН файл: набор, собранный рамкой,
        //     иначе разрушался бы первым же щелчком по соседнему.
        //   • Обычный — один файл и новый якорь.
        const ImGuiIO& io = ImGui::GetIO();
        const auto anchorAt = std::find(m_visibleOrder.begin(), m_visibleOrder.end(), m_anchor);
        const auto hereAt = std::find(m_visibleOrder.begin(), m_visibleOrder.end(), path);
        if (io.KeyShift && anchorAt != m_visibleOrder.end() && hereAt != m_visibleOrder.end()) {
            auto from = anchorAt, to = hereAt;
            if (from > to) std::swap(from, to);
            // Ctrl+Shift — ДОБАВИТЬ диапазон к набору; просто Shift — заменить
            // им набор. Якорь не двигается: с него продолжают тянуть выбор.
            if (!io.KeyCtrl) m_multi.clear();
            for (auto it = from; it <= to; ++it) {
                if (std::find(m_multi.begin(), m_multi.end(), *it) == m_multi.end())
                    m_multi.push_back(*it);
            }
            m_selected = path;
        } else if (io.KeyCtrl) {
            auto it = std::find(m_multi.begin(), m_multi.end(), path);
            if (it != m_multi.end()) {
                m_multi.erase(it);
                m_selected = m_multi.empty() ? fs::path{} : m_multi.back();
            } else {
                m_multi.push_back(path);
                m_selected = path;
            }
            m_anchor = path;
        } else if (dragset::DeferSinglePick(
                       std::find(m_multi.begin(), m_multi.end(), path) != m_multi.end(), m_multi.size(), false)) {
            m_pendingPick = path;   // решится на отпускании (см. DragSet.h)
        } else {
            m_selected = path;
            m_multi = {path};
            m_anchor = path;
        }
    }
    if (doubleClicked) OpenItem(host, path, isDir);
    // Отступы темы для меню: всплывающее окно наследует стиль, действующий в
    // момент открытия (см. Sage::UI::MenuScope). Время жизни MenuScope
    // держится в границах if — иначе деструктор снялся бы позже, после
    // End() окна, и ImGui решил бы, что стиль не сняли вовсе.
    if (Sage::UI::MenuScope tileMenu; ImGui::BeginPopupContextItem("##tile_ctx")) {
        // ПКМ по файлу ВНЕ набора выбирает его одного: меню всегда про то, на
        // что нажали. ПКМ по файлу ИЗ набора набор сохраняет — иначе обвести
        // рамкой двадцать файлов и удалить их разом было бы нельзя.
        if (std::find(m_multi.begin(), m_multi.end(), path) == m_multi.end()) {
            m_selected = path;
            m_multi = {path};
        } else {
            m_selected = path;
        }
        // «ОТКРЫТЬ» — первым и только у того, что открывается (AssetOpen.h).
        {
            using sage::editor::assetopen::OpenAs;
            const char* openLabel = nullptr;
            const char* openIcon = "open";
            switch (sage::editor::assetopen::Of(path, isDir)) {
                case OpenAs::Folder: openLabel = T("Open folder"); openIcon = "folder"; break;
                case OpenAs::Scene: openLabel = T("Open scene"); openIcon = "scene"; break;
                case OpenAs::Prefab: openLabel = T("Add to the scene"); openIcon = "prefab"; break;
                case OpenAs::Clip: openLabel = T("Open in the animation tool"); openIcon = "anim"; break;
                case OpenAs::Text: openLabel = T("Open in the code editor"); openIcon = "code"; break;
                case OpenAs::None: break;
            }
            if (openLabel) {
                if (EditorIcons::MenuItem(openIcon, openLabel)) OpenItem(host, path, isDir);
                ImGui::Separator();
            }
        }
        // Переименование — всегда про ОДИН файл: у двадцати файлов общего имени
        // нет, и придумывать правило вроде «имя + номер» здесь не за чем.
        if (isDir) sage::editor::foldercolors::DrawPaletteMenu(path);
        if (EditorIcons::MenuItem("pencil", T("Rename"))) { m_renameTarget = path; m_error.clear(); }
        if (EditorIcons::MenuItem("trash", T("Delete"))) { m_deleteTargets = m_multi; }

        // СТАРТОВАЯ СЦЕНА — здесь же, в меню самого файла.
        //
        // «С какой сцены начинается игра» — решение автора, и до сих пор его
        // нельзя было принять вовсе: собранная игра брала main.sage, а если её
        // нет — первую по алфавиту. То есть переименование файла меняло начало
        // игры, и узнать об этом можно было только собрав её.
        if (!isDir && path.extension() == ".sage" && host.CurrentProject().Loaded()) {
            ImGui::Separator();
            const std::string name = path.stem().string();
            const bool already = host.CurrentProject().StartScene() == name;
            if (already) {
                ImGui::TextDisabled("%s", T("This is the start scene"));
            } else if (EditorIcons::MenuItem("play", T("Make start scene"))) {
                std::string err;
                if (host.CurrentProject().SetStartScene(name, err))
                    host.SetStatusMessage(T("Start scene: ") + name);
                else
                    host.SetStatusMessage(T("Failed to set the start scene: ") + err);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", T("The built game starts from this scene"));
            }
        }

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
            if (EditorIcons::MenuItem("refresh", label)) ConvertOne(host, path);
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
