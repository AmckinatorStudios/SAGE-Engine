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
#include <cmath>
#include <ctime>
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

// ЧТО ЭТО ЗА ФАЙЛ — одним словом, одинаково для всех расширений одного предмета.
// Расширение отвечает на вопрос «чем открыть», а человек в дереве проекта ищет
// предмет: модель, текстуру, звук. Для .obj, .gltf, .glb, .fbx и .sagemesh ответ
// один и тот же.
std::string sage::editor::assetspanel_detail::KindLabel(const fs::path& path, bool isDir) {
    if (isDir) return T("Folder");
    const std::string ext = ToLower(path.extension().string());
    if (ext == ".sage") return T("Scene");
    if (ext == ".sageprefab") return T("Prefab");
    if (ext == ".sagemat") return T("Material");
    if (ext == ".lua") return T("Lua script (old)");
    if (ext == ".slua") return T("SAGE Lua script");
    if (ext == ".sgraph") return T("Script Graph");
    if (ext == ".aigraph") return T("AI Graph");
    if (ext == ".sageui") return T("Interface");
    if (ext == ".vgraph") return T("Visual Script (old)");
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

AssetStyle sage::editor::assetspanel_detail::StyleForPath(const fs::path& path, bool isDir) {
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
    if (ext == ".sgraph" || ext == ".vgraph") return { ImVec4(0.45f, 0.70f, 0.98f, 1.0f), "graph", "wire" };
    if (ext == ".aigraph") return { ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "ai", "wire" };
    if (ext == ".sageui") return { ImVec4(0.55f, 0.85f, 0.60f, 1.0f), "ui", "layout" };
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

// Обрезка по ширине — по границам символов UTF-8 и двоичным поиском: на
// каждую подпись каждого кадра один-два десятка замеров, а не по замеру на байт.
std::string sage::editor::assetspanel_detail::TruncateToWidth(const std::string& s, float maxWidth,
                                                             bool* cut) {
    if (cut) *cut = false;
    if (ImGui::CalcTextSize(s.c_str()).x <= maxWidth) return s;
    if (cut) *cut = true;
    const char* ellipsis = "...";
    const float room = maxWidth - ImGui::CalcTextSize(ellipsis).x;
    std::vector<size_t> bounds;   // начала символов: продолжение UTF-8 — 10xxxxxx
    for (size_t i = 0; i < s.size(); ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) bounds.push_back(i);
    size_t lo = 0, hi = bounds.size();   // ищем наибольшее число символов, что влезает
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        const size_t end = mid < bounds.size() ? bounds[mid] : s.size();
        if (ImGui::CalcTextSize(s.data(), s.data() + end).x <= room) lo = mid;
        else hi = mid - 1;
    }
    const size_t keep = std::max<size_t>(lo, 1);
    return s.substr(0, keep < bounds.size() ? bounds[keep] : s.size()) + ellipsis;
}

std::string sage::editor::assetspanel_detail::TruncateMiddle(const std::string& s, float maxWidth, bool* cut) {
    if (cut) *cut = false;
    if (ImGui::CalcTextSize(s.c_str()).x <= maxWidth) return s;
    if (cut) *cut = true;
    const char* ellipsis = "...";
    const float room = maxWidth - ImGui::CalcTextSize(ellipsis).x;
    std::vector<size_t> bounds;
    for (size_t i = 0; i < s.size(); ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) bounds.push_back(i);
    const size_t n = bounds.size();
    // Голова и хвост растут по символу по очереди (хвосту — чуть больше: в нём
    // номер и расширение), пока влезают.
    size_t head = 0, tail = 0;
    auto width = [&](size_t h, size_t t) {
        const size_t hEnd = h < n ? bounds[h] : s.size();
        const size_t tBeg = t == 0 ? s.size() : bounds[n - t];
        return ImGui::CalcTextSize(s.data(), s.data() + hEnd).x +
               ImGui::CalcTextSize(s.data() + tBeg, s.data() + s.size()).x;
    };
    for (bool grew = true; grew && head + tail < n;) {
        grew = false;
        if (head + tail < n && width(head, tail + 1) <= room) { ++tail; grew = true; }
        if (head + tail < n && tail > head + 1 && width(head + 1, tail) <= room) { ++head; grew = true; }
    }
    if (head == 0 && tail == 0) return TruncateToWidth(s, maxWidth);
    const size_t hEnd = head < n ? bounds[head] : s.size();
    const size_t tBeg = tail == 0 ? s.size() : bounds[n - tail];
    return s.substr(0, hEnd) + ellipsis + s.substr(tBeg);
}

std::string sage::editor::assetspanel_detail::FormatStamp(long long stamp) {
    if (stamp == 0) return {};
    // Часы файловой системы и системные часы в C++17 не переводятся друг в
    // друга напрямую — через «сейчас» обоих: погрешность меньше минуты,
    // а показываем минуты.
    using namespace std::chrono;
    const fs::file_time_type ft{fs::file_time_type::duration(stamp)};
    const auto sys = time_point_cast<system_clock::duration>(ft - fs::file_time_type::clock::now() +
                                                             system_clock::now());
    const std::time_t t = system_clock::to_time_t(sys);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

// Превью для карточки. 0 — превью нет (папка, скрипт, сцена): для них тег
// честнее пустого квадрата.
//
// Картинки показываются сами собой, материал — шариком с этим материалом.
// Материал рендерится НЕ БОЛЬШЕ ОДНОГО ЗА КАДР и запоминается: это полный
// проход сцены со светом, и делать двадцать таких на открытие папки значит
// уронить редактор . Остальные карточки получат своё превью в
// следующих кадрах — за десяток кадров это незаметно глазу.
AssetsPanel::Cover AssetsPanel::ThumbnailFor(EditorHost& host, const fs::path& path, bool isDir, int px) {
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
    // Карточку увеличили — снимок того размера, что был, размыт: переснимаем
    // крупнее (уменьшение — нет, крупный кадр в мелкой карточке и так чёткий).
    auto it = m_thumbs.find(key);
    if (it != m_thumbs.end() && it->second.Stamp == stamp && it->second.Px >= px) {
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
    // Буфер — свой на каждый размер: AssetPreview не меняет размер уже
    // заведённого буфера, и крупный снимок лёг бы в мелкий.
    const std::string buffer = key + "@" + std::to_string(px);
    uint64_t id = 0;
    // Карты материала ещё едут фоном (редактор грузит их потоково): снимок,
    // сделанный сейчас, запомнил бы серые заглушки навсегда. Такая обложка
    // показывается, но не запоминается — её переснимут, когда карты доедут.
    bool provisional = false;
    std::shared_ptr<Material> shot;
    if (ext == ".sagemat") {
        shot = ResourceManager::Instance().GetMaterial(key);
        if (shot) id = m_preview.RenderMaterial(shot, px, buffer);
        provisional = ResourceManager::Instance().PendingAsyncTextures() > 0;
    } else if (ext == ".sageprefab") {
        id = m_preview.RenderPrefab(key, px, buffer);
        provisional = ResourceManager::Instance().PendingAsyncTextures() > 0;
    } else {
        // Модель — ФОНОМ (см. AssetPreview::RenderModelCover): разбор файла,
        // материалов и уменьшение карт идут в другом потоке, а кэш ресурсов
        // сцены не трогается. Пока разбор идёт — крутилка на плитке.
        bool pending = false;
        id = m_preview.RenderModelCover(key, px, buffer, pending);
        if (pending) {
            return it != m_thumbs.end() ? Cover{it->second.Id, 1, 1} : Cover{};
        }
    }
    // Ноль тоже запоминаем — иначе битый или пустой ассет пытался бы
    // отрисоваться каждый кадр, съедая всю очередь превью и не давая остальным
    // карточкам получить свои обложки.
    // Прежний буфер другого размера больше не нужен.
    if (it != m_thumbs.end() && it->second.Px != px) m_preview.ReleaseTarget(key + "@" + std::to_string(it->second.Px));
    m_thumbs[key] = Thumb{id, provisional ? -1 : stamp, shot, shot ? shot->Fingerprint() : 0, px};
    if (id) m_thumbRenderedThisFrame = true;
    return {id, 1, 1};
}


// Сторона снимка обложки для карточки: крупной карточке — крупный кадр
// (с учётом плотности экрана), иначе обложка в ней размыта.
static int CoverPx(float side) {
    const float scale = std::max(1.0f, ImGui::GetIO().DisplayFramebufferScale.x);
    return side * scale > 100.0f ? 192 : 96;
}

void AssetsPanel::DrawTile(EditorHost& host, size_t index) {
    const Item& item = m_items[index];
    const fs::path& path = item.Path;
    const bool isDir = item.IsDir;
    const TileMetrics M = MetricsFor(m_tileSize, ImGui::GetTextLineHeight());
    AssetStyle style = StyleForPath(path, isDir);
    // Метка папки перекрывает общий жёлтый: две сотни одинаковых значков —
    // ровно то, из-за чего папку и приходится искать чтением имён.
    if (isDir) {
        glm::vec3 tint;
        if (sage::editor::foldercolors::Get(path, tint)) style.Color = ImVec4(tint.r, tint.g, tint.b, 1.0f);
    }

    ImGui::PushID(item.Name.c_str());
    // Весь тайл — ОДИН item (InvisibleButton на полную высоту карточки):
    // подпись внутри его границ, строки сетки не налезают друг на друга.
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##tile", ImVec2(M.W, M.H));
    const bool hovered = ImGui::IsItemHovered();
    const bool visible = ImGui::IsItemVisible();
    const bool isSelected = std::find(m_multi.begin(), m_multi.end(), path) != m_multi.end();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 tileMax(cursor.x + M.W, cursor.y + M.H);
    const float radius = EditorTheme::KitMetrics().Rounding + 2.0f;

    // Подложка: выбранная — заливка выделения и тонкая акцентная рамка (видно
    // и на картинке со светлыми краями), наведённая — подсветка строки.
    ImU32 cardBg = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.03f));
    if (isSelected) cardBg = EditorTheme::Color32(EditorTheme::Role::Selection);
    else if (hovered) cardBg = EditorTheme::Color32(EditorTheme::Role::Hover);
    dl->AddRectFilled(cursor, tileMax, cardBg, radius);
    if (isSelected) dl->AddRect(cursor, tileMax, EditorTheme::Color32(EditorTheme::Role::Accent), radius, 0, 1.5f);

    // Поле обложки — нейтральное у ВСЕХ типов: тип читается по значку и
    // метке, а на поле видно само превью, а не краску вокруг него.
    const ImVec2 sw0(cursor.x + M.Inset, cursor.y + M.Inset);
    const ImVec2 sw1(tileMax.x - M.Inset, cursor.y + M.SwatchH);
    dl->AddRectFilled(sw0, sw1, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.28f)), radius - 2.0f);

    // Обложку просят только ВИДИМЫЕ плитки: заказывать разбор модели для
    // плитки за краем окна значило грузить машину тем, чего никто не видит.
    const Cover thumb = visible ? ThumbnailFor(host, path, isDir, CoverPx(sw1.x - sw0.x)) : Cover{};
    if (thumb.Id) {
        // Картинка вписывается ПО СВОИМ ПРОПОРЦИЯМ (панорама — лентой), а
        // шахматка — под самой картинкой, а не под полями вокруг неё.
        const covers::FitRect fit = covers::Fit(sw0, sw1, thumb.W, thumb.H);
        covers::DrawChecker(dl, fit.A, fit.B);
        dl->AddImage((ImTextureID)(std::intptr_t)thumb.Id, fit.A, fit.B, ImVec2(0, 1), ImVec2(1, 0));
    } else if (!isDir && visible && thumbs::IsImage(path) && !thumbs::Get(path, thumbs::Size::Tile).Failed) {
        // Картинка ещё читается фоном — крутилка вместо пустоты.
        covers::DrawSpinner(dl, ImVec2((sw0.x + sw1.x) * 0.5f, (sw0.y + sw1.y) * 0.5f),
                            std::min(sw1.x - sw0.x, sw1.y - sw0.y) * 0.18f,
                            ImGui::GetColorU32(ImGuiCol_TextDisabled));
    } else {
        // Значок типа по центру поля — тот же набор, что в иерархии и
        // инспекторе. Битая картинка тоже получает значок, а не вечную крутилку.
        const float glyph = std::floor((sw1.y - sw0.y) * 0.5f);
        const float k = hovered || isSelected ? 1.0f : 0.82f;
        EditorIcons::Overlay(std::floor(sw0.x + (sw1.x - sw0.x - glyph) * 0.5f),
                             std::floor(sw0.y + (sw1.y - sw0.y - glyph) * 0.5f), glyph, style.Icon,
                             glm::vec3(style.Color.x * k, style.Color.y * k, style.Color.z * k));
    }

    // Метка расширения в углу поля: у .png и .sagetex обложка одна — сама
    // картинка. В мелкой карточке метка закрыла бы превью — там её нет.
    if (M.KindLine && !style.Tag.empty()) {
        const ImVec2 size = ImGui::CalcTextSize(style.Tag.c_str());
        const ImVec2 p1(sw1.x - 4.0f, sw1.y - 4.0f);
        const ImVec2 p0(p1.x - size.x - 8.0f, p1.y - size.y - 2.0f);
        dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 150), 4.0f);
        dl->AddText(ImVec2(p0.x + 4.0f, p0.y + 1.0f), ImGui::ColorConvertFloat4ToU32(style.Color), style.Tag.c_str());
    }

    // Имя — по центру, одной строкой, обрезанное по символам. Нормального
    // цвета всегда: имя — главное, что в этой панели читают.
    bool cut = false;
    const std::string label = TruncateMiddle(item.Name, M.W - 8.0f, &cut);
    const ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
    const ImVec2 labelPos(std::floor(cursor.x + (M.W - labelSize.x) * 0.5f), sw1.y + 4.0f);
    dl->AddText(labelPos, ImGui::GetColorU32(ImGuiCol_Text, isSelected || hovered ? 1.0f : 0.88f), label.c_str());

    // Вторая строка — ЧТО ЭТО, одним словом для всех расширений предмета.
    if (M.KindLine) {
        const std::string kindShown = TruncateToWidth(item.Kind, M.W - 24.0f);
        const ImVec2 kindSize = ImGui::CalcTextSize(kindShown.c_str());
        dl->AddText(ImVec2(std::floor(cursor.x + (M.W - kindSize.x) * 0.5f), labelPos.y + M.LabelH),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), kindShown.c_str());
    }

    // СТАРТОВАЯ СЦЕНА ВИДНА В СПИСКЕ, а не только в меню каждой сцены.
    if (!isDir && M.KindLine && path.extension() == ".sage" && host.CurrentProject().Loaded() &&
        host.CurrentProject().StartScene() == path.stem().string()) {
        const std::string tag = T("start");
        const ImVec2 size = ImGui::CalcTextSize(tag.c_str());
        const ImVec2 p0(cursor.x + 4.0f, cursor.y + 4.0f);
        const ImVec2 p1(p0.x + size.x + 8.0f, p0.y + size.y + 2.0f);
        dl->AddRectFilled(p0, p1, EditorTheme::Color32Alpha(EditorTheme::Role::Ok, 0.85f), 4.0f);
        dl->AddText(ImVec2(p0.x + 4.0f, p0.y + 1.0f), EditorTheme::Color32(EditorTheme::Role::TextOnAccent), tag.c_str());
    }

    ItemBehavior(host, item, cut);

    // «Ещё» — то же меню без правой кнопки (ноутбук, планшет). В мелкой
    // карточке места под него нет — там меню по правой кнопке.
    if (M.KindLine) {
        const float dots = ImGui::GetTextLineHeight();
        const ImVec2 at(tileMax.x - dots - 4.0f, tileMax.y - dots - 3.0f);
        const ImVec2 mouse = ImGui::GetMousePos();
        const bool overDots = hovered && mouse.x >= at.x && mouse.x <= at.x + dots && mouse.y >= at.y &&
                              mouse.y <= at.y + dots;
        if (hovered || isSelected) {
            const ImVec4 c = EditorTheme::Color(overDots ? EditorTheme::Role::Accent : EditorTheme::Role::TextDim);
            EditorIcons::Overlay(at.x, at.y, dots, "dots", glm::vec3(c.x, c.y, c.z));
        }
        if (overDots && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_selected = path;
            if (std::find(m_multi.begin(), m_multi.end(), path) == m_multi.end()) m_multi = {path};
            m_pendingPick.clear();
            ImGui::OpenPopup("##tile_ctx");
        }
    }
    ImGui::PopID();
}

// СЕТКА — только видимые ряды (ImGuiListClipper): папка набора с тысячей
// файлов рисовала тысячу карточек за кадр, из которых видно три десятка.
void AssetsPanel::DrawGrid(EditorHost& host) {
    const TileMetrics M = MetricsFor(m_tileSize, ImGui::GetTextLineHeight());
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(M.Spacing, M.Spacing));
    const float avail = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>((avail + M.Spacing) / (M.W + M.Spacing)));
    const int count = static_cast<int>(m_items.size());
    const int rows = (count + columns - 1) / columns;
    ImGuiListClipper clipper;
    clipper.Begin(rows, M.H + M.Spacing);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            for (int c = 0; c < columns; ++c) {
                const int i = r * columns + c;
                if (i >= count) break;
                if (c > 0) ImGui::SameLine(0, M.Spacing);
                DrawTile(host, static_cast<size_t>(i));
            }
        }
    }
    clipper.End();
    ImGui::PopStyleVar();
}

// ТАБЛИЦА — те же элементы, тот же порядок и то же поведение, что у сетки:
// строка подаёт один элемент (Selectable во всю ширину) и зовёт ItemBehavior.
void AssetsPanel::DrawList(EditorHost& host) {
    const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
    if (!ImGui::BeginTable("##assets_list", 4, flags)) return;
    const float digit = ImGui::CalcTextSize("0").x;
    ImGui::TableSetupColumn(T("Name"), ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide |
                                           ImGuiTableColumnFlags_DefaultSort, 3.0f);
    ImGui::TableSetupColumn(T("Type"), ImGuiTableColumnFlags_WidthFixed, digit * 16.0f);
    ImGui::TableSetupColumn(T("Size"), ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
                            digit * 10.0f);
    ImGui::TableSetupColumn(T("Modified"),
                            ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
                            digit * 17.0f);
    ImGui::TableHeadersRow();

    // Сортировка у таблицы и у шапки ОДНА: щелчок по заголовку меняет общую
    // (её же видит сетка), выбор в шапке — показывается стрелкой в заголовке.
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
        if (m_tableSortSync) {
            ImGui::TableSetColumnSortDirection(static_cast<int>(m_sortBy),
                                               m_sortDesc ? ImGuiSortDirection_Descending
                                                          : ImGuiSortDirection_Ascending,
                                               false);
            m_tableSortSync = false;
        } else if (specs->SpecsDirty && specs->SpecsCount > 0) {
            const ImGuiTableColumnSortSpecs& s0 = specs->Specs[0];
            const SortBy by = static_cast<SortBy>(std::clamp<int>(s0.ColumnIndex, 0, 3));
            const bool desc = s0.SortDirection == ImGuiSortDirection_Descending;
            if (by != m_sortBy || desc != m_sortDesc) {
                SetSort(by, desc);
                m_tableSortSync = false;
                RefreshItems(host);
            }
            specs->SpecsDirty = false;
        }
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_items.size()));
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) DrawListRow(host, static_cast<size_t>(i));
    clipper.End();
    ImGui::EndTable();
}

void AssetsPanel::DrawListRow(EditorHost& host, size_t index) {
    const Item& item = m_items[index];
    const float rowH = m_rowHeight;
    ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
    ImGui::TableSetColumnIndex(0);
    ImGui::PushID(item.Name.c_str());

    const bool isSelected = std::find(m_multi.begin(), m_multi.end(), item.Path) != m_multi.end();
    const float nameW = ImGui::GetContentRegionAvail().x;
    const ImVec2 rowPos = ImGui::GetCursorScreenPos();
    ImGui::Selectable("##row", isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                      ImVec2(0.0f, rowH));
    const bool visible = ImGui::IsItemVisible();

    // Миниатюра или значок типа — квадрат высотой со строку. Превью есть —
    // миниатюра (картинка, материал, модель), нет — значок цвета типа.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float side = std::max(12.0f, rowH - 4.0f);
    const ImVec2 b0(rowPos.x, rowPos.y + (rowH - side) * 0.5f);
    const ImVec2 b1(b0.x + side, b0.y + side);
    AssetStyle style = StyleForPath(item.Path, item.IsDir);
    if (item.IsDir) {
        glm::vec3 tint;
        if (sage::editor::foldercolors::Get(item.Path, tint)) style.Color = ImVec4(tint.r, tint.g, tint.b, 1.0f);
    }
    // Миниатюры — со строки в 28 px: в плотной таблице картинка размером с
    // букву не узнаётся, а значок типа читается сразу.
    const Cover thumb = visible && rowH >= 28.0f ? ThumbnailFor(host, item.Path, item.IsDir, 96) : Cover{};
    if (thumb.Id) {
        const covers::FitRect fit = covers::Fit(b0, b1, thumb.W, thumb.H);
        covers::DrawChecker(dl, fit.A, fit.B);
        dl->AddImage((ImTextureID)(std::intptr_t)thumb.Id, fit.A, fit.B, ImVec2(0, 1), ImVec2(1, 0));
    } else {
        const float glyph = std::floor(std::min(side, ImGui::GetTextLineHeight() * 1.1f));
        EditorIcons::Overlay(std::floor(b0.x + (side - glyph) * 0.5f), std::floor(b0.y + (side - glyph) * 0.5f),
                             glyph, style.Icon, glm::vec3(style.Color.x, style.Color.y, style.Color.z));
    }
    const float textY = std::floor(rowPos.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f);
    const float textX = b1.x + 6.0f;
    bool cut = false;
    const std::string label = TruncateMiddle(item.Name, nameW - (textX - rowPos.x), &cut);
    dl->AddText(ImVec2(textX, textY), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());

    // Сразу после строки: «последний элемент» для меню, перетаскивания и
    // подсказки — это она, а не текст колонок ниже.
    ItemBehavior(host, item, cut);

    auto cell = [&](int column, const std::string& text, bool right) {
        ImGui::TableSetColumnIndex(column);
        const float w = ImGui::GetContentRegionAvail().x;
        const std::string shown = TruncateToWidth(text, w);
        const float x = ImGui::GetCursorScreenPos().x + (right ? w - ImGui::CalcTextSize(shown.c_str()).x : 0.0f);
        dl->AddText(ImVec2(x, textY), ImGui::GetColorU32(ImGuiCol_TextDisabled), shown.c_str());
    };
    cell(1, item.Kind, false);
    cell(2, item.IsDir ? std::string() : covers::HumanSize(item.Size), true);
    cell(3, FormatStamp(item.Stamp), false);
    ImGui::PopID();
}
