#include "../PanelWindows.h"
#include "EnvironmentPanel.h"
#include "EditorTheme.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
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

namespace env = sage::env;

namespace {

// Строки свойств — таблицей «подпись | значение»: подписи слева одной
// колонкой читаются быстрее, чем подписи справа от полей разной ширины, и
// узкая панель не обрезает их посередине слова.
// Имя таблицы — со счётчиком: в одной группе таблиц бывает несколько (особый
// виджет во всю ширину разрезает строки), и одинаковые имена слили бы их
// ширины колонок в одну. PushID вокруг BeginTable для этого не годится:
// таблица сама кладёт свой ID в стек до EndTable, и PopID снял бы чужой.
bool BeginPropTable(int index) {
    char id[32];
    std::snprintf(id, sizeof(id), "##props%d", index);
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX))
        return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
    return true;
}

void Label(const env::Prop& p) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T(p.Label));
    if (!p.Hint.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(p.Hint));
}

// Компактный цвет: квадрат-образец и hex рядом. Полная палитра открывается
// щелчком по образцу — цвета не должны занимать пол-инспектора.
bool CompactColor(const char* id, glm::vec3& c, Sage::UI::ColorFieldFlags extra = 0) {
    const bool changed = Sage::UI::ColorField3(id, &c.x, Sage::UI::ColorField_Compact | extra);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", Sage::UI::color::ToHex(&c.x).c_str());
    return changed;
}

} // namespace

void EnvironmentPanel::DrawSunLink(EditorHost& host, Scene& scene, LightingEnvironment& envData) {
    // СОЛНЦЕ ИЩЕТ ДВИЖОК, а не панель: правило «солнце — направленный свет с
    // наименьшим Id» одно на весь редактор и рантайм.
    const entt::entity sunEntity = sage::ecs::FindSunEntity(scene);
    if (sunEntity == entt::null) {
        ImGui::TextWrapped("%s", T("No directional light — no sun and no time of day."));
        if (ImGui::Button(T("Create a sun"))) {
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
    ImGui::TextDisabled(T("Now: %s (sun %.0f° above the horizon)"), phase, elevationDeg);
    const NameComponent* name = scene.Registry().try_get<NameComponent>(sunEntity);
    const IdComponent* id = scene.Registry().try_get<IdComponent>(sunEntity);
    if (envData.Cycle.Enabled)
        ImGui::TextDisabled(T("The cycle turns the object \"%s\""), name ? name->Name.c_str() : "?");
    else
        ImGui::TextDisabled(T("Time of day = rotation of the object \"%s\""), name ? name->Name.c_str() : "?");
    ImGui::SameLine();
    if (ImGui::Button(T("Select"))) host.Selection().SetPrimary(id ? id->Id : 0);
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
        if (ImGui::Button(T("Convert to six files"), ImVec2(-FLT_MIN, 0.0f))) {
            if (env::CubemapFolderToFaces(e.Skybox, host.CurrentProject().Dir())) {
                host.PushUndoSnapshot();
                m_convertFailed = false;
            } else {
                m_convertFailed = true;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Each face px/nx/py/ny/pz/nz of the folder goes into its own slot"));
        if (m_convertFailed)
            ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Danger), "%s",
                               T("Not all six faces px/nx/py/ny/pz/nz are in the folder"));
    } else if (key == "sky.preset") {
        // Готовый вид — отправная точка: собрать узнаваемое небо из трёх
        // десятков полей с нуля — полчаса подбора цветов.
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##preset", T("Apply a ready-made look…"))) {
            for (int i = 0; i < (int)sage::render::SkyPreset::Count; ++i) {
                const auto preset = (sage::render::SkyPreset)i;
                if (ImGui::Selectable(T(sage::render::SkyPresetLabel(preset)))) {
                    sage::render::ApplySkyPreset(e.Skybox, preset);
                    host.PushUndoSnapshot();
                }
            }
            ImGui::EndCombo();
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
            ImGui::TextDisabled("%s", T("Switch to Custom values for light without a sky"));
        } else {
            // РЕЗУЛЬТАТ, а не поля: значения считаются из неба и времени суток.
            glm::vec3 skyC, groundC;
            e.ResolveAmbient(skyC, groundC);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", T("Computed:"));
            ImGui::SameLine();
            CompactColor("##sky", skyC, Sage::UI::ColorField_ReadOnly);
            ImGui::SameLine();
            CompactColor("##ground", groundC, Sage::UI::ColorField_ReadOnly);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", T("Taken from the sky, so it darkens with it"));
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
        dl->AddRectFilled(a, b, path.empty() ? empty : filled, 3.0f);
        dl->AddRect(a, b, hovered ? ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::Accent)) : line,
                    3.0f);
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

// --- Одно свойство — одна строка таблицы ----------------------------------------
bool EnvironmentPanel::DrawProp(EditorHost& host, const env::Prop& p, LightingEnvironment& e) {
    void* field = p.Field ? p.Field(e) : nullptr;
    if (!field) return false;
    m_drawn.push_back(p.Key);
    // Слот картинки или папки — во всю ширину, подписью сверху: у слота обложка
    // и имя файла, и в колонке значения они сжимались до нечитаемого.
    const bool slot = p.Kind == env::PropKind::Texture || p.Kind == env::PropKind::Folder;
    if (slot) {
        Label(p);
    } else {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        Label(p);
        ImGui::TableSetColumnIndex(1);
    }
    ImGui::PushID(p.Key.c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    bool changed = false;
    switch (p.Kind) {
        case env::PropKind::Bool:
            if (ImGui::Checkbox("##v", static_cast<bool*>(field))) {
                host.PushUndoSnapshot();
                changed = true;
            }
            break;
        case env::PropKind::Float:
            changed = ImGui::DragFloat("##v", static_cast<float*>(field), p.Step, p.Min, p.Max,
                                       p.Format.c_str());
            host.TrackLastImGuiItem();
            break;
        case env::PropKind::Slider:
            changed = ImGui::SliderFloat("##v", static_cast<float*>(field), p.Min, p.Max, p.Format.c_str());
            host.TrackLastImGuiItem();
            break;
        case env::PropKind::Vec2:
            changed = ImGui::DragFloat2("##v", static_cast<float*>(field), p.Step, p.Min, p.Max,
                                        p.Format.c_str());
            host.TrackLastImGuiItem();
            break;
        case env::PropKind::Enum: {
            int* v = static_cast<int*>(field);
            const char* preview =
                (*v >= 0 && *v < (int)p.Options.size()) ? T(p.Options[(size_t)*v]) : "?";
            if (ImGui::BeginCombo("##v", preview)) {
                for (int i = 0; i < (int)p.Options.size(); ++i) {
                    if (ImGui::Selectable(T(p.Options[(size_t)i]), i == *v) && i != *v) {
                        *v = i;
                        host.PushUndoSnapshot();
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }
        case env::PropKind::Color:
            changed = CompactColor("##v", *static_cast<glm::vec3*>(field));
            host.TrackLastImGuiItem();
            break;
        case env::PropKind::Texture:
        case env::PropKind::Folder: {
            // СЛОТ, а не поле с путём: картинку и папку неба перетаскивают из
            // панели ассетов, как и всё остальное (см. AssetSlot.h).
            std::string& path = *static_cast<std::string*>(field);
            const bool folder = p.Kind == env::PropKind::Folder;
            assetslot::Result r = assetslot::Draw(host, "slot",
                                                  folder ? assetslot::Kind::Folder : assetslot::Kind::Texture,
                                                  path, nullptr, T(p.Label));
            if (r.Changed) {
                path = r.Path;
                host.PushUndoSnapshot();
                changed = true;
            }
            if (r.BrowseRequested) {
                FileBrowser::Config c;
                c.Title = T(p.Label);
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
    return changed;
}

void EnvironmentPanel::DrawProps(EditorHost& host, const std::vector<env::Prop>& props,
                                 LightingEnvironment& e, bool& changed) {
    // Особые виджеты — во всю ширину, между таблицами: строка «что сейчас на
    // небе» в колонке значения обрезалась бы на первом же слове.
    bool table = false;
    int tables = 0;
    for (const env::Prop& p : props) {
        if (p.Visible && !p.Visible(e)) continue;
        const bool wide = p.Kind == env::PropKind::Custom || p.Kind == env::PropKind::Texture ||
                          p.Kind == env::PropKind::Folder;
        if (wide) {
            if (table) {
                ImGui::EndTable();
                table = false;
            }
            if (p.Kind == env::PropKind::Custom) DrawCustom(host, p.Key, e);
            else if (DrawProp(host, p, e)) changed = true;
            continue;
        }
        if (!table) {
            table = BeginPropTable(tables++);
            if (!table) continue;
        }
        if (DrawProp(host, p, e)) changed = true;
    }
    if (table) ImGui::EndTable();
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
    // Группа со своим включателем (облака, луна): галка в заголовке, а поля —
    // только когда включено. Выключенная группа — одна строка, а не двадцать
    // серых полей, которые ничего не делают.
    bool* toggle = g.Toggle ? static_cast<bool*>(g.Toggle(e)) : nullptr;
    if (toggle) {
        if (ImGui::Checkbox("##on", toggle)) {
            host.PushUndoSnapshot();
            changed = true;
        }
        if (!g.ToggleHint.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(g.ToggleHint));
        ImGui::SameLine();
    }
    if (toggle && !*toggle) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", T(g.Label));
        ImGui::PopID();
        return;
    }
    const ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_SpanAvailWidth | (g.Open ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    const std::string title = std::string(T(g.Label)) + "###grp";
    if (ImGui::TreeNodeEx(title.c_str(), flags)) {
        DrawProps(host, g.Props, e, changed);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void EnvironmentPanel::DrawSystem(EditorHost& host, const env::System& s, LightingEnvironment& e) {
    const std::string header = std::string(T(s.Label)) + "###" + s.Id;
    if (!EditorTheme::SectionHeader(s.Icon.c_str(), header.c_str(), ImGuiTreeNodeFlags_DefaultOpen,
                                    nullptr, s.Hint.empty() ? nullptr : T(s.Hint)))
        return;
    ImGui::PushID(s.Id.c_str());
    bool changed = false;

    // Сначала — включатель и тип: от них зависит, что вообще имеет смысл.
    bool* enabled = s.Enabled ? static_cast<bool*>(s.Enabled(e)) : nullptr;
    const bool pickType = s.GetType && s.Variants.size() > 1;
    if (enabled || pickType) {
        if (BeginPropTable(-1)) {
            if (enabled) {
                m_drawn.push_back(s.Id + ".enabled");
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(T("Enabled"));
                ImGui::TableSetColumnIndex(1);
                if (ImGui::Checkbox("##enabled", enabled)) {
                    host.PushUndoSnapshot();
                    changed = true;
                }
            }
            if (pickType && (!enabled || *enabled)) {
                m_drawn.push_back(s.Id + ".type");
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(T(s.TypeLabel));
                ImGui::TableSetColumnIndex(1);
                const env::Variant* cur = env::CurrentVariant(s, e);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##type", cur ? T(cur->Label) : "?")) {
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
                    ImGui::EndCombo();
                }
                if (cur && !cur->Hint.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(cur->Hint));
            }
            ImGui::EndTable();
        }
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
}

void EnvironmentPanel::Draw(EditorHost& host, bool* open) {
    Scene& scene = host.CurrentScene();
    LightingEnvironment& e = scene.Lighting;
    m_drawn.clear();

    ImGui::Begin(EditorIcons::WindowTitle("sun", T("Environment"), "Lighting").c_str(), open,
                 panelwindows::WindowFlags("Lighting"));

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

    // ВЫПЕЧКА GI ПОКА УБРАНА ИЗ РЕДАКТОРА (см. EnvironmentPanel.h).
    ImGui::End();
}
