#include "EnvironmentPanel.h"
#include "EditorTheme.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "sage/core/Log.h"
#include "sage/ecs/DayNightCycle.h"
#include "sage/ecs/LightSystem.h"
#include "sage/render/SkyDraw.h"
#include "sage/render/SkyModel.h"
#include "sage/render/SkyPresets.h"
#include "sage/render/Skybox.h"
#include "sage/scene/Components.h"
#include "../AssetSlot.h"
#include "../Localization.h"
#include "../Project.h"
#include "EditorIcons.h"
#include "ui/ColorPicker.h"
#include "../InspectorControls.h"

namespace env = sage::env;

namespace {

// ВСЕ СТРОКИ — КОНТРОЛЫ UI KIT (ictl::): подпись слева в общей колонке,
// значение справа, одинаковые высоты и состояния Hover/Active/Disabled. Своей
// системы контролов у панели нет — только то, чего в kit нет по смыслу: слоты
// ассетов, развёртка куба и вычисленные цвета.
namespace ictl = sage::editor::ictl;

// Подсказка на всю строку, а не только на сам контрол: подпись тоже «горячая».
// Строку знает только вызывающий (y0 — верх строки до отрисовки контрола).
void RowTip(float y0, const std::string& hint) {
    if (hint.empty() || !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) return;
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 a(wp.x, y0);
    const ImVec2 b(wp.x + ImGui::GetWindowWidth(), y0 + ImGui::GetFrameHeight());
    if (!ImGui::IsMouseHoveringRect(a, b)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
    ImGui::TextUnformatted(T(hint));
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

} // namespace


void EnvironmentPanel::DrawSunLink(EditorHost& host, Scene& scene, LightingEnvironment& envData) {
    // СОЛНЦЕ ИЩЕТ ДВИЖОК, а не панель: правило «солнце — направленный свет с
    // наименьшим Id» одно на весь редактор и рантайм.
    const entt::entity sunEntity = sage::ecs::FindSunEntity(scene);
    if (sunEntity == entt::null) {
        ImGui::TextWrapped("%s", T("No directional light — no sun and no time of day."));
        if (ictl::Button("sun", T("Create a sun"))) {
            host.PushUndoSnapshot();
            GameObject sun = sage::ecs::CreateSunEntity(scene);
            host.Selection().SetPrimary(sun.Id());
        }
        return;
    }
    // ЧТО СЕЙЧАС НА НЕБЕ — словами и числом, через сбор освещения кадра: там
    // солнце-объект, а не поле окружения, которое кадр не читает.
    const LightingEnvironment frame = sage::ecs::CollectLighting(scene);
    const sage::render::SkyState state = sage::render::EvaluateSky(frame);
    const float elevationDeg = glm::degrees(std::asin(glm::clamp(state.SunDirection.y, -1.0f, 1.0f)));
    const char* phase = state.DayFactor > 0.85f  ? T("day")
                        : state.DayFactor > 0.15f ? T("twilight")
                                                  : T("night");
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(T("Now: %s (sun %.0f° above the horizon)"), phase, elevationDeg);
    const NameComponent* name = scene.Registry().try_get<NameComponent>(sunEntity);
    const IdComponent* id = scene.Registry().try_get<IdComponent>(sunEntity);
    // Какой объект — солнце, говорит подсказка кнопки: второй строкой рядом с
    // кнопкой она выталкивала «Выбрать» за край узкой панели.
    char tip[256];
    std::snprintf(tip, sizeof(tip),
                  envData.Cycle.Enabled ? T("The cycle turns the object \"%s\"")
                                        : T("Time of day = rotation of the object \"%s\""),
                  name ? name->Name.c_str() : "?");
    if (ictl::Button(nullptr, T("Select"), tip, ictl::ButtonLook::Compact))
        host.Selection().SetPrimary(id ? id->Id : 0);
}


// --- Особые виджеты, отмеченные в схеме как Custom ------------------------------
void EnvironmentPanel::DrawCustom(EditorHost& host, const std::string& key, LightingEnvironment& e) {
    m_drawn.push_back(key);
    ImGui::PushID(key.c_str());
    if (key == "sky.faces.map") {
        DrawFaceMap(host, e);
    } else if (key == "sky.folder.convert") {
        // Прежний тип неба: перевести одним щелчком, а не собирать шесть
        // граней заново руками. Не вышло — небо остаётся как было.
        if (ictl::Button("import", T("Convert to six files"),
                         T("Each face px/nx/py/ny/pz/nz of the folder goes into its own slot"))) {
            if (env::CubemapFolderToFaces(e.Skybox, host.CurrentProject().Dir())) {
                host.PushUndoSnapshot();
                m_convertFailed = false;
            } else {
                m_convertFailed = true;
            }
        }
        if (m_convertFailed)
            ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Danger), "%s",
                               T("Not all six faces px/nx/py/ny/pz/nz are in the folder"));
    } else if (key == "sky.preset") {
        // Готовый вид — отправная точка: собрать узнаваемое небо из трёх
        // десятков полей с нуля — полчаса подбора цветов.
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ictl::BeginDropdown("##preset", T("Apply a ready-made look…"))) {
            for (int i = 0; i < (int)sage::render::SkyPreset::Count; ++i) {
                const auto preset = (sage::render::SkyPreset)i;
                if (ImGui::Selectable(T(sage::render::SkyPresetLabel(preset)))) {
                    sage::render::ApplySkyPreset(e.Skybox, preset);
                    host.PushUndoSnapshot();
                }
            }
            ictl::EndDropdown();
        }
    } else if (key == "sky.sunlink" || key == "cycle.sunlink") {
        DrawSunLink(host, host.CurrentScene(), e);
    } else if (key == "sky.status") {
        // ЧТО РАСПОЗНАНО — словами и сразу, а не «попробуй и посмотри».
        const SkyboxSettings& sky = e.Skybox;
        if (sky.Kind == SkyboxSettings::Source::Faces && !sky.HasFaces()) {
            ImGui::TextDisabled("%s", T("Fill all six — the sky needs every face"));
        } else if (sky.HasCubemap()) {
            if (sage::render::SceneSkyCubemap(e))
                ImGui::TextDisabled("%s", T("The sky is assembled and in the frame"));
            else
                ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Danger), "%s",
                                   T("The sky did not assemble — the reason is in the console"));
        }
    } else if (key == "ambient.computed") {
        if (!e.Skybox.Enabled) {
            // НЕБА НЕТ — И СВЕТА ОТ НЕГО НЕТ: поля другого режима тут ни при чём.
            ImGui::TextDisabled("%s", T("The sky is off — there is no ambient light"));
            ImGui::TextDisabled("%s", T("Switch to Custom for light without a sky"));
        } else {
            // РЕЗУЛЬТАТ, а не поля: значения считаются из неба и времени суток.
            glm::vec3 skyC, groundC;
            e.ResolveAmbient(skyC, groundC);
            // Два образца подписаны словами: «какой из них небо, какой земля»
            // не должно быть загадкой. Редактировать их нельзя — это результат.
            const float y0 = ImGui::GetCursorScreenPos().y;
            ictl::Label(T("Computed"));
            Sage::UI::ColorField3("##sky", &skyC.x, Sage::UI::ColorField_Compact | Sage::UI::ColorField_ReadOnly);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", T("Sky"));
            ImGui::SameLine();
            Sage::UI::ColorField3("##ground", &groundC.x,
                                  Sage::UI::ColorField_Compact | Sage::UI::ColorField_ReadOnly);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", T("Ground"));
            RowTip(y0, "Taken from the sky, so it darkens with it");
        }
    }
    ImGui::PopID();
}

// --- Развёртка куба: где какая грань ------------------------------------------
//
// Подписи «+X», «-Z» ничего не говорят тому, кто держит шесть картинок с
// названиями «лес», «горы», «закат». Крест развёртки показывает, как грани
// сложатся в небо: перед в середине, по бокам лево и право, за правым — зад,
// сверху и снизу — верх и низ. Каждая клетка — отдельная кнопка: подсказка
// «что видно в эту сторону» и выбор файла щелчком.
void EnvironmentPanel::DrawFaceMap(EditorHost& host, LightingEnvironment& e) {
    struct Cell { int Face, Col, Row; const char* Name; const char* Key; };
    // Грани по порядку FacePaths: +X, -X, +Y, -Y, +Z, -Z.
    static const Cell kCells[6] = {
        {0, 2, 1, "Right face", "sky.face.px"}, {1, 0, 1, "Left face", "sky.face.nx"},
        {2, 1, 0, "Top face", "sky.face.py"},   {3, 1, 2, "Bottom face", "sky.face.ny"},
        {4, 1, 1, "Front face", "sky.face.pz"}, {5, 3, 1, "Back face", "sky.face.nz"},
    };
    // Клетка — прямоугольник по ширине панели, а не квадрат: в квадрате на
    // узкой панели не помещалось даже слово «Перед».
    const float gap = 2.0f;
    const float cellW = std::floor(std::min((ImGui::GetContentRegionAvail().x - gap * 3.0f) / 4.0f,
                                            ImGui::GetFrameHeight() * 4.0f));
    const float cellH = std::floor(ImGui::GetFrameHeight() * 1.4f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 filled = ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::AccentMuted));
    const ImU32 empty = ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::Input));
    const ImU32 line = ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::LineStrong));
    const ImU32 textOn = ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::Text));
    const ImU32 textOff = ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::TextDim));
    for (const Cell& c : kCells) {
        const ImVec2 a(origin.x + c.Col * (cellW + gap), origin.y + c.Row * (cellH + gap));
        const ImVec2 b(a.x + cellW, a.y + cellH);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(c.Face);
        const bool clicked = ImGui::InvisibleButton("##face", ImVec2(cellW, cellH));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const std::string& path = e.Skybox.FacePaths[c.Face];
        const float round = ImGui::GetStyle().FrameRounding;
        dl->AddRectFilled(a, b, path.empty() ? empty : filled, round);
        dl->AddRect(a, b, hovered ? ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::Accent)) : line,
                    round);
        const char* name = T(c.Name);   // ключи «… face» — в ru.json
        const ImVec2 ts = ImGui::CalcTextSize(name);
        dl->PushClipRect(a, b, true);
        dl->AddText(ImVec2(a.x + std::floor(std::max(cellW - ts.x, 0.0f) * 0.5f),
                           a.y + std::floor((cellH - ts.y) * 0.5f)),
                    path.empty() ? textOff : textOn, name);
        dl->PopClipRect();
        if (const env::Prop* p = env::FindProp(c.Key)) {
            if (hovered) {
                const std::string tip = std::string(T(p->Label)) + "\n" + T(p->Hint) + "\n\n" +
                                        (path.empty() ? std::string(T("Not set — click to pick a picture"))
                                                      : path);
                ImGui::SetTooltip("%s", tip.c_str());
            }
            if (clicked) {
                FileBrowser::Config cfg;
                cfg.Title = T(p->Label);
                cfg.Filters = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr"};
                cfg.FilterLabel = T("Images");
                cfg.StartDir = cfg.Root = assetslot::ProjectRoot(host);
                m_browser.Open(cfg);
                m_pickKey = p->Key;
            }
        }
    }
    // Курсор — под крестом: следующие строки не должны лечь поверх клеток.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 3.0f * cellH + 2.0f * gap));
    ImGui::Dummy(ImVec2(4.0f * cellW + 3.0f * gap, 0.0f));
}

// --- Одно свойство — одна строка kit ---------------------------------------------
bool EnvironmentPanel::DrawProp(EditorHost& host, const env::Prop& p, LightingEnvironment& e) {
    void* field = p.Field ? p.Field(e) : nullptr;
    if (!field) return false;
    m_drawn.push_back(p.Key);
    const char* label = T(p.Label);
    const float y0 = ImGui::GetCursorScreenPos().y;
    ImGui::PushID(p.Key.c_str());
    bool changed = false;
    // Диапазон поля — из схемы; шаг кнопок «−/+» и формат kit подбирает по нему.
    auto numOpts = [&p] {
        ictl::NumOpts o = ictl::OptsForRange(p.Min, p.Max);
        o.Min = p.Min;
        o.Max = p.Max;
        o.Format = p.Format.c_str();
        return o;
    };
    switch (p.Kind) {
        case env::PropKind::Bool:
            changed = ictl::Switch(host, label, static_cast<bool*>(field));
            break;
        case env::PropKind::Float:
            changed = ictl::Number(host, label, static_cast<float*>(field), numOpts());
            break;
        case env::PropKind::Slider:
            changed = ictl::SliderNumber(host, label, static_cast<float*>(field), p.Min, p.Max, p.Format.c_str());
            break;
        case env::PropKind::Vec2:
            changed = ictl::Vector2(host, label, static_cast<glm::vec2*>(field), numOpts());
            break;
        case env::PropKind::Enum: {
            int* v = static_cast<int*>(field);
            std::vector<const char*> items;
            for (const std::string& o : p.Options) items.push_back(T(o));
            if (ictl::Dropdown(label, v, items.data(), (int)items.size())) {
                host.PushUndoSnapshot();
                changed = true;
            }
            break;
        }
        case env::PropKind::Color:
            changed = ictl::Color3(host, label, static_cast<glm::vec3*>(field));
            break;
        case env::PropKind::Texture:
        case env::PropKind::Folder: {
            // СЛОТ, а не поле с путём: картинку и папку неба перетаскивают из
            // панели ассетов, как и всё остальное (см. AssetSlot.h). Слот
            // широкий — подпись над ним, как у групп значений kit.
            ictl::GroupTitle(label);
            std::string& path = *static_cast<std::string*>(field);
            const bool folder = p.Kind == env::PropKind::Folder;
            assetslot::Result r = assetslot::Draw(host, "slot",
                                                  folder ? assetslot::Kind::Folder : assetslot::Kind::Texture,
                                                  path, nullptr, label);
            if (r.Changed) {
                path = r.Path;
                host.PushUndoSnapshot();
                changed = true;
            }
            if (r.BrowseRequested) {
                FileBrowser::Config c;
                c.Title = label;
                if (folder) {
                    c.Mode = FileBrowser::PickMode::PickFolder;
                } else {
                    c.Filters = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr"};
                    c.FilterLabel = T("Images");
                }
                // Диалог заперт в проекте: ассет выбирается ИЗНУТРИ.
                c.StartDir = c.Root = assetslot::ProjectRoot(host);
                m_browser.Open(c);
                m_pickKey = p.Key;
            }
            break;
        }
        case env::PropKind::Custom: break;
    }
    ImGui::PopID();
    // Слот выше строки — подсказка только для однострочных контролов.
    if (p.Kind != env::PropKind::Texture && p.Kind != env::PropKind::Folder) RowTip(y0, p.Hint);
    return changed;
}

void EnvironmentPanel::DrawProps(EditorHost& host, const std::vector<env::Prop>& props,
                                 LightingEnvironment& e, bool& changed) {
    for (const env::Prop& p : props) {
        if (p.Visible && !p.Visible(e)) continue;
        if (p.Kind == env::PropKind::Custom) DrawCustom(host, p.Key, e);
        else if (DrawProp(host, p, e)) changed = true;
    }
}

void EnvironmentPanel::DrawGroup(EditorHost& host, const env::Group& g, LightingEnvironment& e,
                                 bool& changed) {
    if (g.Visible && !g.Visible(e)) return;
    ImGui::PushID(g.Id.c_str());
    if (g.Label.empty()) {
        DrawProps(host, g.Props, e, changed);
        ImGui::PopID();
        return;
    }
    // Группа со своим включателем (облака, луна) — вложенная группа kit с
    // состоянием «Вкл/Выкл» справа в заголовке: свёрнутая, она всё равно
    // говорит, работает ли. Поля — только когда включено, а не двадцать серых
    // полей, которые ничего не делают.
    bool* toggle = g.Toggle ? static_cast<bool*>(g.Toggle(e)) : nullptr;
    const std::string sub = "env." + m_systemId + "." + g.Id;
    auto status = [&] {
        if (!toggle) return;
        const char* text = *toggle ? T("On") : T("Off");
        const ImVec2 b = ImGui::GetItemRectMax();
        const ImVec2 sz = ImGui::CalcTextSize(text);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(b.x - sz.x - ImGui::GetStyle().FramePadding.x, ImGui::GetItemRectMin().y + ImGui::GetStyle().FramePadding.y),
            EditorTheme::Color32(*toggle ? EditorTheme::Role::Accent : EditorTheme::Role::TextDim), text);
    };
    if (ictl::SubSection(sub.c_str(), T(g.Label), g.Open, status)) {
        if (toggle) {
            const float y0 = ImGui::GetCursorScreenPos().y;
            if (ictl::Switch(host, T("Enabled"), toggle)) changed = true;
            RowTip(y0, g.ToggleHint);
        }
        if (!toggle || *toggle) DrawProps(host, g.Props, e, changed);
        ictl::EndSubSection();
    }
    ImGui::PopID();
}

void EnvironmentPanel::DrawSystem(EditorHost& host, const env::System& s, LightingEnvironment& e) {
    const std::string header = std::string(T(s.Label)) + "###" + s.Id;
    if (!EditorTheme::SectionHeader(s.Icon.c_str(), header.c_str(),
                                    s.Open ? ImGuiTreeNodeFlags_DefaultOpen : 0, nullptr,
                                    s.Hint.empty() ? nullptr : T(s.Hint)))
        return;
    ImGui::PushID(s.Id.c_str());
    m_systemId = s.Id;
    bool changed = false;

    // Сначала — включатель и тип: от них зависит, что вообще имеет смысл.
    bool* enabled = s.Enabled ? static_cast<bool*>(s.Enabled(e)) : nullptr;
    const bool pickType = s.GetType && s.Variants.size() > 1;
    if (enabled) {
        m_drawn.push_back(s.Id + ".enabled");
        const float y0 = ImGui::GetCursorScreenPos().y;
        if (ictl::Switch(host, T(s.EnabledLabel), enabled)) changed = true;
        RowTip(y0, s.EnabledHint);
    }
    if (pickType && (!enabled || *enabled)) {
        m_drawn.push_back(s.Id + ".type");
        const env::Variant* cur = env::CurrentVariant(s, e);
        ImGui::PushID("type");
        ictl::Label(T(s.TypeLabel));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ictl::BeginDropdown("##type", cur ? T(cur->Label) : "?")) {
            for (const env::Variant& v : s.Variants) {
                // Прежний тип не предлагается: на нём остаются только
                // сцены, которые им уже пользуются (см. Variant::Legacy).
                if (v.Legacy) continue;
                // Пути и цвета других типов НЕ стираются при переключении:
                // вернуться к своему набору неба надо уметь без повторного выбора.
                if (ImGui::Selectable(T(v.Label), &v == cur) && &v != cur) {
                    s.SetType(e, v.Value);
                    host.PushUndoSnapshot();
                    changed = true;
                }
                if (!v.Hint.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(v.Hint));
            }
            ictl::EndDropdown();
        }
        if (cur && !cur->Hint.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(cur->Hint));
        ImGui::PopID();
    }

    if (!enabled || *enabled) {
        if (const env::Variant* v = env::CurrentVariant(s, e))
            for (const env::Group& g : v->Groups) DrawGroup(host, g, e, changed);
        for (const env::Group& g : s.Common) DrawGroup(host, g, e, changed);
    }
    if (s.Normalize) s.Normalize(e);
    // Цикл суток двигает солнце СРАЗУ при правке: «поставил шесть вечера —
    // увидел закат», а не после запуска игры.
    if (s.Id == "cycle") sage::ecs::ApplyDayNight(host.CurrentScene());
    (void)changed;
    ImGui::PopID();
    // Разделитель между системами — общий отступ kit, а не случайный Dummy.
    ImGui::Dummy(ImVec2(0.0f, EditorTheme::KitMetrics().SectionGap));
}


void EnvironmentPanel::Draw(EditorHost& host, bool* open) {
    Scene& scene = host.CurrentScene();
    LightingEnvironment& e = scene.Lighting;
    m_drawn.clear();

    if (std::getenv("SAGE_EDITOR_SHOW_ENVIRONMENT")) ImGui::SetNextWindowFocus();   // снимок: окно впереди вкладки
    ImGui::Begin(EditorIcons::WindowTitle("sun", T("Environment"), "Lighting").c_str(), open);

    // Ответ диалога приходит ЧЕРЕЗ КАДР: цель — ключом свойства, а поле
    // находится заново по схеме в уже текущей сцене.
    if (m_browser.Draw()) {
        if (const env::Prop* p = env::FindProp(m_pickKey); p && p->Field) {
            *static_cast<std::string*>(p->Field(e)) = host.CurrentProject().AssetRef(m_browser.Result());
            host.PushUndoSnapshot();
        }
        m_pickKey.clear();
    }

    // Без вводной строки сверху: три строки текста над каждым открытием окна
    // отнимали место у самих настроек, а читают их один раз. Что делает
    // система — в подсказке её заголовка.
    for (const env::System& s : env::Systems()) DrawSystem(host, s, e);

    // Запечённое освещение — своим модулем (GIBakeSection.h): фоновое
    // запекание, разметка статики и статус «устарело» к небу и туману
    // отношения не имеют.
    m_giBake.Draw(host);
    ImGui::End();
}