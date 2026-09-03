#include "AssetSlot.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <unordered_map>

#include "imgui.h"
#include "imgui_internal.h" // BeginDragDropTargetCustom: цель броска на весь слот

#include "AssetPreview.h"
#include "EditorHost.h"
#include "EditorIcons.h"
#include "Localization.h"
#include "Project.h"

#include "sage/assets/import/Importer.h"
#include "sage/render/Material.h"
#include "sage/render/Mesh.h"
#include "sage/render/ModelLoader.h"
#include "sage/render/ResourceManager.h"

namespace fs = std::filesystem;

namespace assetslot {

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Обложка для слота. Держится в кэше: снять её — это полный проход сцены со
// светом, а слот рисуется каждый кадр. Не больше ОДНОЙ съёмки за кадр на весь
// редактор — иначе инспектор с мешем, материалом и пятью текстурами уронил бы
// частоту кадров .
struct Cached {
    uint64_t Id = 0;
    long long Stamp = 0; // время правки файла: правка материала обязана обновить обложку
    int LastFrame = 0;
};

std::unordered_map<std::string, Cached>& Cache() {
    static std::unordered_map<std::string, Cached> cache;
    return cache;
}

uint64_t Thumbnail(AssetPreview* preview, const fs::path& path, int size) {
    if (path.empty()) return 0;
    const std::string key = path.string();
    const std::string ext = Lower(path.extension().string());

    // Картинки не рендерятся — они уже картинки.
    if (KindOf(path) == Kind::Texture) {
        std::shared_ptr<Texture> tex = ResourceManager::Instance().GetTexture(key);
        return tex ? tex->NativeHandle() : 0;
    }
    if (!preview) return 0;

    const bool renderable = ext == ".sagemat" || ext == ".sageprefab" || ext == ".sagemesh" ||
                            ModelLoader::IsSupportedModel(key);
    if (!renderable) return 0;

    std::error_code ec;
    const auto write = fs::last_write_time(path, ec);
    const long long stamp = ec ? 0 : (long long)write.time_since_epoch().count();

    auto& cache = Cache();
    const int frame = ImGui::GetFrameCount();
    auto it = cache.find(key);
    if (it != cache.end() && it->second.Stamp == stamp) {
        it->second.LastFrame = frame;
        return it->second.Id;
    }

    static int lastRenderFrame = -1;
    if (lastRenderFrame == frame) {
        // Очередь занята этим кадром — отдаём прошлую обложку, если была.
        return it != cache.end() ? it->second.Id : 0;
    }

    const std::string bufferKey = "slot:" + key;
    uint64_t id = 0;
    if (ext == ".sagemat") {
        if (std::shared_ptr<Material> mat = ResourceManager::Instance().GetMaterial(key))
            id = preview->RenderMaterial(mat, size, bufferKey);
    } else if (ext == ".sageprefab") {
        id = preview->RenderPrefab(key, size, bufferKey);
    } else {
        if (std::shared_ptr<Mesh> mesh = ResourceManager::Instance().GetModel(key))
            id = preview->RenderMesh(mesh, size, bufferKey, AssetPreview::MaterialsForModel(key));
    }
    lastRenderFrame = frame;
    cache[key] = Cached{id, stamp, frame};

    // Обложки, на которые давно никто не смотрел, отпускаем вместе с их
    // буферами: слот, которому за сеанс назначили десяток разных материалов,
    // иначе держал бы в видеопамяти их все.
    for (auto it2 = cache.begin(); it2 != cache.end();) {
        if (frame - it2->second.LastFrame > 240) {
            preview->ReleaseTarget("slot:" + it2->first);
            it2 = cache.erase(it2);
        } else {
            ++it2;
        }
    }
    return id;
}

} // namespace

Kind KindOf(const fs::path& path) {
    const std::string ext = Lower(path.extension().string());
    if (ext == ".sagemat") return Kind::Material;
    if (ext == ".sageprefab") return Kind::Prefab;
    if (ext == ".sage") return Kind::Scene;
    if (ext == ".lua") return Kind::Script;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" ||
        ext == ".hdr" || ext == ".sagetex")
        return Kind::Texture;
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3") return Kind::Audio;
    // Модели спрашиваются у РЕЕСТРА импортёров, а не у списка здесь: движок
    // умеет .obj/.gltf/.glb/.fbx/.blend/.bbmodel и пополняется плагинами.
    if (ext == ".sagemesh" || ModelLoader::IsSupportedModel(path.string())) return Kind::Model;
    return Kind::Any;
}

bool Accepts(Kind slot, const fs::path& path) {
    if (slot == Kind::Any) return true;
    return KindOf(path) == slot;
}

const char* KindName(Kind kind) {
    switch (kind) {
        case Kind::Model:    return T("model");
        case Kind::Material: return T("material");
        case Kind::Texture:  return T("texture");
        case Kind::Script:   return T("script");
        case Kind::Prefab:   return T("prefab");
        case Kind::Scene:    return T("scene");
        case Kind::Audio:    return T("sound");
        default:             return T("file");
    }
}

const char* KindIcon(Kind kind) {
    switch (kind) {
        case Kind::Model:    return "model";
        case Kind::Material: return "material";
        case Kind::Texture:  return "texture";
        case Kind::Script:   return "script";
        case Kind::Prefab:   return "prefab";
        case Kind::Scene:    return "scene";
        case Kind::Audio:    return "audio";
        default:             return "file";
    }
}

std::vector<std::string> Extensions(Kind kind) {
    switch (kind) {
        case Kind::Model:    return sage::assets::ImporterRegistry::Instance().Extensions();
        case Kind::Material: return {".sagemat"};
        case Kind::Texture:  return {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".sagetex"};
        case Kind::Script:   return {".lua"};
        case Kind::Prefab:   return {".sageprefab"};
        case Kind::Scene:    return {".sage"};
        case Kind::Audio:    return {".wav", ".ogg", ".mp3"};
        default:             return {};
    }
}

void BeginDrag(const fs::path& path, AssetPreview* preview) {
    const std::string p = path.string();
    ImGui::SetDragDropPayload(kPayload, p.c_str(), p.size() + 1);
    const uint64_t thumb = Thumbnail(preview, path, 64);
    if (thumb) {
        ImGui::Image((ImTextureID)(std::intptr_t)thumb, ImVec2(48, 48), ImVec2(0, 1), ImVec2(1, 0));
        ImGui::SameLine();
    } else {
        EditorIcons::Inline(KindIcon(KindOf(path)));
        ImGui::SameLine();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(path.filename().string().c_str());
}

Result Draw(EditorHost& host, const char* id, Kind kind, const std::string& path,
            AssetPreview* preview, const char* emptyHint) {
    Result result;
    ImGui::PushID(id);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float thumb = 56.0f;
    const float height = thumb + 8.0f;
    const float width = std::max(180.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + width, p0.y + height);

    // Место в раскладке занимается СРАЗУ, до содержимого: дальше всё рисуется
    // абсолютными координатами поверх, и без резерва следующий виджет лёг бы на
    // слот. Кнопки подаются позже фона и поэтому получают наведение первыми.
    ImGui::Dummy(ImVec2(width, height));
    const ImGuiID slotId = ImGui::GetItemID();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.045f)), 6.0f);

    // --- Приём броска на ВЕСЬ слот -----------------------------------------
    //
    // Цель по всей карточке, а не по одному полю ввода: попасть мышью в узкую
    // строку, таща файл, неудобно, а промах выглядит как отказ. И ответ даётся
    // ДО отпускания кнопки (AcceptBeforeDelivery): рамка зелёная — возьму,
    // красная с подсказкой — не тот тип. Молчащий слот неотличим от сломанного.
    bool hoveringPayload = false;
    bool payloadOk = false;
    std::string payloadPath;
    if (ImGui::BeginDragDropTargetCustom(ImRect(p0, p1), slotId)) {
        const ImGuiDragDropFlags flags =
            ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kPayload, flags)) {
            payloadPath.assign((const char*)p->Data, (size_t)p->DataSize);
            if (!payloadPath.empty() && payloadPath.back() == '\0') payloadPath.pop_back();
            hoveringPayload = true;
            payloadOk = Accepts(kind, payloadPath);
            if (payloadOk && p->IsDelivery()) {
                result.Changed = true;
                result.Path = host.CurrentProject().AssetRef(payloadPath);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (hoveringPayload) {
        const ImU32 col = payloadOk ? IM_COL32(120, 210, 130, 255) : IM_COL32(230, 110, 110, 255);
        dl->AddRect(p0, p1, col, 6.0f, 0, 2.0f);
        if (!payloadOk) {
            ImGui::BeginTooltip();
            ImGui::Text(T("Needs a %s"), KindName(kind));
            const std::vector<std::string> exts = Extensions(kind);
            std::string list;
            for (const std::string& e : exts) {
                if (!list.empty()) list += ", ";
                list += e;
            }
            if (!list.empty()) ImGui::TextDisabled("%s", list.c_str());
            ImGui::EndTooltip();
        }
    }

    // --- Обложка ------------------------------------------------------------
    const fs::path assetPath = path;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 4.0f, p0.y + 4.0f));
    ImGui::InvisibleButton("##thumb", ImVec2(thumb, thumb));
    const bool thumbHovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked() && !path.empty()) {
        // Щелчок по обложке показывает файл в панели Assets. Это ответ на
        // вопрос «а где он лежит?», который иначе решался поиском по дереву
        // проекта с именем из этого же слота в голове.
        host.ShowAssetInPanel(path);
    }
    const ImVec2 t0(p0.x + 4.0f, p0.y + 4.0f);
    const ImVec2 t1(t0.x + thumb, t0.y + thumb);
    dl->AddRectFilled(t0, t1, ImGui::GetColorU32(ImVec4(0, 0, 0, 0.28f)), 5.0f);

    const uint64_t tex = Thumbnail(preview, assetPath, (int)thumb);
    // Шахматка под картинкой: без неё прозрачные места сливаются с фоном слота,
    // и текстура с альфой выглядит просто дырявой.
    if (tex && KindOf(assetPath) == Kind::Texture) {
        const float checker = 8.0f;
        dl->PushClipRect(t0, t1, true);
        for (float y = t0.y; y < t1.y; y += checker) {
            for (float x = t0.x; x < t1.x; x += checker) {
                const bool odd = ((int)((x - t0.x) / checker) + (int)((y - t0.y) / checker)) % 2;
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + checker, y + checker),
                                  odd ? IM_COL32(68, 68, 74, 255) : IM_COL32(50, 50, 56, 255));
            }
        }
        dl->PopClipRect();
    }
    if (tex) {
        dl->AddImageRounded((ImTextureID)(std::intptr_t)tex, t0, t1, ImVec2(0, 1), ImVec2(1, 0),
                            IM_COL32_WHITE, 5.0f);
    } else {
        const float glyph = 28.0f;
        EditorIcons::Overlay(t0.x + (thumb - glyph) * 0.5f, t0.y + (thumb - glyph) * 0.5f, glyph,
                             KindIcon(kind),
                             path.empty() ? glm::vec3(0.45f, 0.46f, 0.5f)
                                          : glm::vec3(0.72f, 0.74f, 0.78f));
    }
    if (thumbHovered && !path.empty()) {
        dl->AddRect(t0, t1, ImGui::GetColorU32(ImGuiCol_NavHighlight), 5.0f, 0, 1.5f);
        ImGui::SetTooltip("%s\n%s", path.c_str(), T("Click — show in Assets"));
    }

    // Слот — ИСТОЧНИК перетаскивания: назначенный ассет можно перетащить в
    // другой слот, не разыскивая его файл в дереве проекта.
    if (!path.empty() && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        BeginDrag(assetPath, preview);
        ImGui::EndDragDropSource();
    }

    // --- Имя и кнопки -------------------------------------------------------
    const float rightEdge = p1.x - 6.0f;
    const float btnStripW = ImGui::GetFrameHeight() * 2.0f + style.ItemSpacing.x + 8.0f;
    ImGui::SetCursorScreenPos(ImVec2(t1.x + 8.0f, p0.y + 5.0f));
    ImGui::BeginGroup();
    // Текст переносится по границе кнопок, а не уезжает под них: подпись длиной
    // с путь иначе оказывалась наполовину под «Обзор…» и читалась как обрывок.
    ImGui::PushTextWrapPos(rightEdge - btnStripW);
    if (path.empty()) {
        ImGui::TextDisabled("%s", emptyHint ? emptyHint : T("Not assigned"));
        ImGui::TextDisabled("%s", T("Drag an asset here"));
    } else {
        const std::string name = assetPath.filename().string();
        ImGui::TextUnformatted(name.c_str());
        // Файла нет — это самая частая причина «объект стал белым/пропал», и
        // узнать о ней надо в слоте, а не по виду сцены.
        // Ссылка в компоненте относительная (см. Project::AssetRef), а
        // текущая папка процесса — не обязательно корень проекта, поэтому
        // ищем в обоих местах.
        std::error_code ec;
        const bool missing = !fs::exists(assetPath, ec) &&
                             !fs::exists(host.CurrentProject().Dir() / assetPath, ec);
        if (missing) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", T("File not found"));
        } else {
            ImGui::TextDisabled("%s", KindName(KindOf(assetPath)));
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();

    // Кнопки прижаты к правому краю слота: их место не должно зависеть от длины
    // имени файла, иначе «убрать» каждый раз оказывается в новом месте.
    const float btnH = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(ImVec2(rightEdge - btnH * 2.0f - style.ItemSpacing.x,
                                     p0.y + height - btnH - 5.0f));
    if (EditorIcons::IconOnlyButton("folder", T("Browse..."))) result.BrowseRequested = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(path.empty());
    if (EditorIcons::IconOnlyButton("trash", T("Remove"))) {
        result.Changed = true;
        result.Cleared = true;
        result.Path.clear();
    }
    ImGui::EndDisabled();

    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + style.ItemSpacing.y));
    ImGui::PopID();
    return result;
}

} // namespace assetslot
