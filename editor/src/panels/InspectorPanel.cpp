#include <cstdarg>
#include "../PanelWindows.h"
#include "InspectorPanel.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "sage/core/Log.h"
#include <algorithm>

#include "AssetSlot.h"
#include "EditorIcons.h"
#include "ModelMaterialImport.h"
#include "Project.h"
#include "sage/render/ResourceManager.h"
#include "sage/assets/import/Importer.h"
#include "sage/render/ModelLoader.h"
#include "sage/render/ModelMaterial.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/render/ParticlePresets.h"
#include "sage/render/SkinnedModel.h"
#include "sage/scene/Components.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/UIPresets.h"
#include "../Localization.h"

namespace fs = std::filesystem;

void InspectorPanel::Draw(EditorHost& host, bool* open) {
    // Обложки в файловом диалоге рисуются ТЕМ ЖЕ превью, что и слоты этой
    // панели: второй набор буферов в видеопамяти ради модального окна не нужен.
    m_browser.SetPreview(&m_preview);
    // Результат обзора приходит через кадр после нажатия — кладём его туда, ради
    // чего диалог открывали. Draw() зовётся РОВНО ОДИН РАЗ за кадр: у модалки
    // ImGui одно состояние на всю панель, и второй вызов открывал бы её поверх
    // себя же.
    if (m_browser.Draw()) {
        // AssetRef, а не Result().string(): диалог отдаёт АБСОЛЮТНЫЙ путь, и в
        // таком виде он до сих пор уезжал в сцену — работая в этом редакторе на
        // этой машине и нигде больше (см. Project::AssetRef).
        const std::string picked = host.CurrentProject().AssetRef(m_browser.Result());

        if (m_browseScriptEntity >= 0) {
            // Скрипт адресован СУЩНОСТИ, а не полю: за кадр ожидания сцену могли
            // перезагрузить, и указатель на поле компонента уже никуда не годился бы.
            GameObject target = host.CurrentScene().Get(m_browseScriptEntity);
            m_browseScriptEntity = -1;
            if (target.Valid()) {
                host.PushUndoSnapshot();
                host.CurrentScene().Registry().emplace_or_replace<ScriptComponent>(
                    target.Entity(), ScriptComponent{picked});
            }
        } else if (m_browseAudioEntity >= 0) {
            // Звук адресован сущности — ровно как скрипт выше.
            GameObject target = host.CurrentScene().Get(m_browseAudioEntity);
            m_browseAudioEntity = -1;
            if (target.Valid()) {
                host.PushUndoSnapshot();
                entt::registry& reg = host.CurrentScene().Registry();
                AudioSourceComponent& au = reg.get_or_emplace<AudioSourceComponent>(target.Entity());
                au.Clip = picked;
            }
        } else if (m_browseTarget) {
            *m_browseTarget = picked;
            m_browseTarget = nullptr;
            if (m_browseIsMesh) {
                m_browseIsMesh = false;
                m_pendingMeshLoad = true;   // грузим в том же кадре, ниже по панели
            }
            if (m_browseIsMaterial && m_browseCreateMaterial) {
                // Путь спросили ради СОЗДАНИЯ: файла ещё нет, его надо записать.
                m_browseIsMaterial = false;
                m_browseCreateMaterial = false;
                if (GameObject sel = host.InspectedObject(); sel.Valid()) {
                    WriteMaterialFromOverrides(host, sel.Renderer(), m_browser.Result().string());
                }
            } else if (m_browseIsMaterial) {
                m_browseIsMaterial = false;
                // Материал грузим сразу: без указателя объект остался бы с путём
                // и без вида — «выбрал материал, ничего не произошло».
                if (GameObject sel = host.InspectedObject(); sel.Valid()) {
                    MeshRendererComponent& mr = sel.Renderer();
                    if (!mr.MaterialPath.empty())
                        mr.MaterialPtr = ResourceManager::Instance().GetMaterial(mr.MaterialPath);
                    // Выбирали могли и для слота части модели. Какой именно это
                    // был слот, здесь уже не известно — путь ушёл прямо в поле, —
                    // а перечитать их все стоит одного обращения к кэшу на слот.
                    for (MaterialSlot& slot : mr.Slots) {
                        slot.Ptr = slot.Path.empty()
                                       ? nullptr
                                       : ResourceManager::Instance().GetMaterial(slot.Path);
                    }
                }
            }
            if (m_browseIsShader) {
                if (std::shared_ptr<Material> m =
                        ResourceManager::Instance().GetMaterial(host.InspectedAssetPath().string())) {
                    m->ShaderPtr.reset();
                }
            }
        }
    }

    ImGui::Begin(T("Inspector" "###Inspector"), open, panelwindows::WindowFlags("Inspector"));

    // --- ЗАМОК --------------------------------------------------------------
    //
    // Пока он заперт, панель показывает то же, что и в момент запирания, — и
    // это единственный способ перетащить сюда текстуру из Assets: чтобы начать
    // перетаскивание, по файлу надо нажать, а нажатие меняет выбранный ассет и
    // уводит панель на него же. Слота, в который тащили, к моменту отпускания
    // кнопки просто не остаётся.
    const bool locked = host.InspectorLocked();
    if (EditorIcons::IconOnlyButton("lock",
                                    locked ? T("Unlock: the panel will follow the selection again")
                                           : T("Lock: the panel will keep showing this while you "
                                               "pick something else (for drag and drop)"),
                                    locked)) {
        host.SetInspectorLocked(!locked);
    }
    if (locked) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T("Locked"));
    }
    ImGui::Separator();

    // ------------------------------------------------------------------------
    // Две ПРИНЦИПИАЛЬНО разные вещи — в две вкладки, а не в одну простыню.
    //
    // Раньше панель просто складывала одно под другое: сверху редактор
    // материала, выбранного в Assets, снизу свойства выбранной сущности, между
    // ними голая черта. Это два независимых предмета правки — файл на диске и
    // объект в сцене, — и у них даже разная область действия: материал общий
    // для всех, кто им покрашен, а Transform принадлежит одной сущности.
    // Соседство без границы читалось как один список свойств одного объекта, и
    // человек не понимал, что именно он сейчас меняет.
    //
    // Вкладка САМА переключается на то, что человек выбрал последним: щёлкнул
    // по объекту в сцене — открыт объект, щёлкнул по файлу в Assets — открыт
    // ассет. Иначе за разделение пришлось бы платить лишним кликом на каждое
    // переключение, и оно бы только мешало.
    // ------------------------------------------------------------------------
    const AssetKind assetKind = ClassifyAsset(host.InspectedAssetPath());
    const bool hasEntity = host.InspectedObject().Valid();
    const bool hasAsset = assetKind != AssetKind::None;

    // Что выбрали последним. Сравниваем с прошлым кадром: событий выбора панель
    // не получает, а сравнение состояния даёт ровно тот же ответ.
    const int entityId = hasEntity ? host.InspectedObject().Id() : -1;
    const std::string assetPath = host.InspectedAssetPath().string();
    // Под замком вкладка тоже не переключается: иначе замок держал бы предмет
    // правки, но панель всё равно уезжала бы с «Ассета» на «Объект».
    if (!locked) {
        if (entityId != m_lastEntityId && hasEntity) { m_focus = Focus::Object; m_forceFocus = true; }
        if (assetPath != m_lastAssetPath && hasAsset) { m_focus = Focus::Asset; m_forceFocus = true; }
    }
    m_lastEntityId = entityId;
    m_lastAssetPath = assetPath;

    if (!hasEntity && !hasAsset) {
        ImGui::TextDisabled("%s", T("Nothing selected"));
        ImGui::Spacing();
        // TextWrapped, а не две строки текста: панель узкая и её ширину меняют,
        // а обрезанная посередине подсказка бесполезнее отсутствующей.
        ImGui::TextWrapped("%s", T("Select an object in the viewport or Hierarchy, or a file in Assets."));
        ImGui::End();
        return;
    }

    // Одна сущность выбрана — вкладки не нужны: они бы только съедали строку.
    if (hasEntity && !hasAsset) {
        DrawObjectSection(host);
    } else if (!hasEntity && hasAsset) {
        DrawAssetSection(host, assetKind);
    } else if (ImGui::BeginTabBar("##inspector_tabs", ImGuiTabBarFlags_None)) {
        // SetSelected ставится РОВНО НА ОДИН КАДР — тот, в котором сменился
        // выбор. Передавать его каждый кадр, пока m_focus равен вкладке, нельзя:
        // человек щёлкает по «Ассету», ImGui его открывает, а на следующем кадре
        // флаг у «Объекта» всё ещё выставлен и утаскивает выбор обратно. Вкладка
        // выглядела намертво залипшей — ровно так эта панель и сломалась.
        const bool force = m_forceFocus;
        m_forceFocus = false;
        const ImGuiTabItemFlags objFlags = (force && m_focus == Focus::Object)
                                               ? ImGuiTabItemFlags_SetSelected
                                               : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem(T("Object" "###Object"), nullptr, objFlags)) {
            m_focus = Focus::Object;
            DrawObjectSection(host);
            ImGui::EndTabItem();
        }
        const ImGuiTabItemFlags assetFlags = (force && m_focus == Focus::Asset)
                                                 ? ImGuiTabItemFlags_SetSelected
                                                 : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem(T("Asset" "###Asset"), nullptr, assetFlags)) {
            m_focus = Focus::Asset;
            DrawAssetSection(host, assetKind);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

// Заголовок раздела: что именно правится. Без него вкладка «Ассет» с полями
// Albedo/Metallic ничем не отличается от материала, назначенного объекту, —
// а это разные вещи: здесь правится ФАЙЛ, общий для всех, кто им покрашен.
void InspectorPanel::DrawSectionHeader(const char* icon, const char* kind, const std::string& name,
                                       const std::string& subtitle) {
    ImGui::Spacing();
    const float s = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    EditorIcons::Overlay(p.x, p.y + s * 0.15f, s, icon, glm::vec3(0.62f, 0.72f, 0.85f));
    ImGui::Dummy(ImVec2(s * 1.35f, s));
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextUnformatted(name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", kind);
    if (!subtitle.empty()) {
        ImGui::TextDisabled("%s", subtitle.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", subtitle.c_str());
    }
    ImGui::Separator();
    ImGui::Spacing();
}

InspectorPanel::AssetKind InspectorPanel::ClassifyAsset(const std::filesystem::path& path) {
    if (path.empty()) return AssetKind::None;
    const std::string ext = path.extension().string();
    if (ext == ".sagemat") return AssetKind::Material;
    if (ext == ".sageprefab") return AssetKind::Prefab;
    if (ext == ".obj" || ext == ".gltf" || ext == ".glb") return AssetKind::Model;
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac")
        return AssetKind::Audio;
    return AssetKind::Other;
}


// ============================================================================
//  Проигрыватель звукового файла
// ============================================================================
//
// Отвечает на вопрос «что это за звук» там же, где его задают, — в Assets.
// Раньше ответ стоил постановки объекта в сцену с компонентом Audio, то есть
// мусора в сцене ради проверки, тот ли это выстрел.

void InspectorPanel::StopAudioPreview(EditorHost& host) {
    if (!m_audioHandle) return;
    if (AudioEngine* audio = host.Audio()) audio->StopSound(m_audioHandle);
    m_audioHandle = 0;
}

void InspectorPanel::DrawAudioPlayer(EditorHost& host) {
    AudioEngine* audio = host.Audio();
    const std::string path = host.InspectedAssetPath().string();

    // СМЕНИЛСЯ ФАЙЛ — снимаем прежний звук и пересчитываем волну. Без первого
    // выбор второго файла играл бы поверх первого, без второго на новом файле
    // осталась бы чужая картинка.
    if (path != m_audioPath) {
        StopAudioPreview(host);
        m_audioPath = path;
        m_audioWave.clear();
        m_audioSeconds = 0.0f;

        std::vector<float> samples;
        int rate = 0;
        // Путь отдаём КАК ЕСТЬ: ссылку проекта движок разрешает сам — и при
        // разборе, и при проигрывании, одной и той же дорогой.
        if (AudioEngine::DecodeToMono(path, samples, rate) && rate > 0 && !samples.empty()) {
            m_audioSeconds = (float)samples.size() / (float)rate;
            // Огибающая столбцами: в одном пикселе тысячи сэмплов, и рисовать
            // их поштучно нельзя. Берём минимум и максимум на столбец — так
            // видно и тихие места, и щелчки, а среднее их бы съело.
            constexpr int kColumns = 512;
            m_audioWave.reserve(kColumns);
            const size_t per = std::max<size_t>(1, samples.size() / kColumns);
            for (size_t i = 0; i < samples.size(); i += per) {
                float lo = samples[i], hi = samples[i];
                const size_t end = std::min(samples.size(), i + per);
                for (size_t k = i; k < end; ++k) {
                    lo = std::min(lo, samples[k]);
                    hi = std::max(hi, samples[k]);
                }
                m_audioWave.emplace_back(lo, hi);
            }
        } else {
            LOG_WARN("Audio") << "Волна не построена для '" << path
                              << "' — причина строкой выше";
        }
    }

    if (!audio || !audio->IsAvailable()) {
        ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.4f, 1.0f), "%s",
                           T("No sound device — nothing will be heard here."));
        ImGui::TextDisabled("%s", T("The file itself is fine; the reason is in the log."));
        return;
    }

    const bool alive = m_audioHandle && audio->IsSoundAlive(m_audioHandle);
    const bool playing = alive && audio->IsSoundPlaying(m_audioHandle);
    // Доигравший звук отпускаем сам: дескриптор живёт, пока владелец его не
    // снял, и без этого второй «Играть» ничего бы не запустил.
    if (alive && !playing && !m_audioLoop) StopAudioPreview(host);

    if (playing) {
        if (EditorIcons::Button("stop", T("Stop"))) StopAudioPreview(host);
    } else {
        if (EditorIcons::Button("play", T("Play"))) {
            StopAudioPreview(host);
            AudioEngine::SoundParams p;
            p.Volume = m_audioVolume;
            p.Loop = m_audioLoop;
            p.Spatial = false;   // слушаем ФАЙЛ, а не источник в сцене
            p.Cat = AudioEngine::Category::Sfx;
            m_audioHandle = audio->Play(path, p);
            if (!m_audioHandle) {
                LOG_ERROR("Audio") << "Проигрыватель: звук не запустился — '" << path
                                   << "' (причина строкой выше)";
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(T("Loop##audioplayer"), &m_audioLoop) && alive) {
        // Смена зацикленности на лету: перезапускать звук ради галочки — значит
        // терять место, до которого дослушали.
        StopAudioPreview(host);
    }
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderFloat(T("Volume##audioplayer"), &m_audioVolume, 0.0f, 1.0f) && alive) {
        audio->SetSoundVolume(m_audioHandle, m_audioVolume);
    }

    // --- Полоса времени -----------------------------------------------------
    const float length = alive && audio->SoundLength(m_audioHandle) > 0.0f
                             ? audio->SoundLength(m_audioHandle)
                             : m_audioSeconds;
    float position = alive ? audio->SoundPosition(m_audioHandle) : 0.0f;
    if (length > 0.0f) {
        ImGui::SetNextItemWidth(-1.0f);
        // Бегунок ведёт себя как бегунок: тянут — перематываем. Без этого
        // послушать конец длинного файла можно было бы только дослушав его.
        if (ImGui::SliderFloat("##audiopos", &position, 0.0f, length, "%.2f s") && alive) {
            audio->SeekSound(m_audioHandle, position);
        }
        ImGui::TextDisabled(T("%.2f of %.2f s"), position, length);
    } else {
        ImGui::TextDisabled("%s", T("Length unknown — the file did not decode."));
    }

    // --- Волна --------------------------------------------------------------
    if (!m_audioWave.empty()) {
        const float height = 64.0f;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float width = std::max(64.0f, ImGui::GetContentRegionAvail().x);
        ImGui::Dummy(ImVec2(width, height));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p1(p0.x + width, p0.y + height);
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImVec4(0, 0, 0, 0.28f)), 4.0f);
        const float mid = p0.y + height * 0.5f;
        const ImU32 col = ImGui::GetColorU32(ImVec4(0.45f, 0.68f, 0.95f, 0.95f));
        dl->PushClipRect(p0, p1, true);
        for (int x = 0; x < (int)width; ++x) {
            const size_t i = (size_t)((float)x / width * (float)m_audioWave.size());
            if (i >= m_audioWave.size()) break;
            const float lo = m_audioWave[i].first, hi = m_audioWave[i].second;
            dl->AddLine(ImVec2(p0.x + (float)x, mid - hi * height * 0.5f),
                        ImVec2(p0.x + (float)x, mid - lo * height * 0.5f), col);
        }
        // Где мы сейчас — вертикальная черта. Она и делает волну полезной:
        // видно, к какому месту записи относится то, что слышно.
        if (length > 0.0f) {
            const float x = p0.x + width * std::clamp(position / length, 0.0f, 1.0f);
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y),
                        ImGui::GetColorU32(ImVec4(1.0f, 0.85f, 0.35f, 1.0f)), 1.5f);
        }
        dl->PopClipRect();
    }
}

void InspectorPanel::DrawObjectSection(EditorHost& host) {
    GameObject obj = host.InspectedObject();
    DrawSectionHeader("cube", T("scene object"), obj.Name(),
                      T("The properties of this entity belong to it alone."));

    // Мультивыделение: правим первичную, но подсказываем размер набора
    // (гизмо двигает все; Delete/Duplicate — по всем выбранным).
    if (host.Selection().size() > 1) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), T("%zu selected — editing the primary one"),
                           host.Selection().size());
        ImGui::Separator();
    }
    DrawEntityProperties(host);
}

void InspectorPanel::DrawAssetSection(EditorHost& host, AssetKind kind) {
    const std::filesystem::path& path = host.InspectedAssetPath();
    const std::string name = path.filename().string();

    switch (kind) {
        case AssetKind::Material:
            DrawSectionHeader("material", T("material"), name,
                              T("A file on disk — the change affects EVERY object using this material."));
            DrawMaterialEditor(host);
            break;
        case AssetKind::Prefab:
            DrawSectionHeader("cube", T("prefab"), name,
                              T("A subtree template: double-clicking in Assets places a copy into the scene."));
            DrawPrefabPreview(host);
            break;
        case AssetKind::Model:
            DrawSectionHeader("model", T("model"), name,
                              T("Import settings are baked into the mesh on load."));
            DrawModelImportEditor(host);
            break;
        case AssetKind::Audio:
            DrawSectionHeader("play", T("sound"), name,
                              T("Listen to it right here — no need to put it on an object first."));
            DrawAudioPlayer(host);
            break;
        default: {
            // Для остальных типов редактора нет — но пустая вкладка выглядит как
            // поломка, поэтому показываем то, что известно о файле.
            DrawSectionHeader("file", T("file"), name, path.string());
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            if (!ec) ImGui::TextDisabled(T("Size: %llu bytes"), (unsigned long long)size);
            ImGui::Spacing();
            ImGui::TextDisabled("%s", T("There is no editor for this file type."));
            ImGui::TextDisabled("%s", T("Materials (.sagemat) and models (.obj/.gltf/.glb)"));
            ImGui::TextDisabled("%s", T("and are edited right here."));
            break;
        }
    }
}
