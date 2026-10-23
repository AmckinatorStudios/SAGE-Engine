#include "TweenPanel.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"   // PushMultiItemsWidths: поля осей ровно по ширине колонки

#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "../ObjectSlot.h"
#include "../PanelWindowId.h"
#include "../PanelWindows.h"
#include "../ui/ColorPicker.h"
#include "../ui/UI.h"
#include "sage/anim/AnimProperty.h"
#include "sage/scene/Scene.h"

namespace anim = sage::anim;

// =============================================================================
//  Слова для человека: свойства и кривые
// =============================================================================

namespace {

// Свойства, которые оживляют чаще всего, — со своим разделом меню и понятным
// названием. У интерфейса «Цвет» есть у заливки, картинки, текста и значка:
// в одном списке четыре «Цвета» не различить, поэтому здесь они названы.
struct Known {
    const char* Id;
    const char* Section;
    const char* Title;
};
const Known kKnown[] = {
    {"object.position", "Transform", "Position"},
    {"object.rotation", "Transform", "Rotation"},
    {"object.scale", "Transform", "Scale"},
    {"material.color", "Rendering", "Colour"},
    {"material.opacity", "Rendering", "Opacity"},
    {"light.color", "Rendering", "Light colour"},
    {"light.intensity", "Rendering", "Light brightness"},
    {"light.range", "Rendering", "Light range"},
    {"element.position", "Interface", "Position"},
    {"element.size", "Interface", "Size"},
    {"element.rotation", "Interface", "Rotation"},
    {"element.scale", "Interface", "Scale"},
    {"element.opacity", "Interface", "Opacity"},
    {"fill.color", "Interface", "Background colour"},
    {"image.tint", "Interface", "Image colour"},
    {"label.color", "Interface", "Text colour"},
    {"label.scale", "Interface", "Text size"},
    {"icon.color", "Interface", "Icon colour"},
    {"fill.rounding", "Interface", "Corner rounding"},
    {"bar.value", "Interface", "Bar fill"},
    {"range.value", "Interface", "Slider value"},
    {"camera.fov", "Camera", "Field of view"},
    {"audio.volume", "Audio", "Volume"},
    {"audio.pitch", "Audio", "Pitch"},
};
const char* const kSections[] = {"Transform", "Rendering", "Interface", "Camera", "Audio"};

const Known* FindKnown(const std::string& id) {
    for (const Known& k : kKnown)
        if (id == k.Id) return &k;
    return nullptr;
}

// Внутренности, которые плавно менять незачем: кадр спрайта, девятина,
// размер запекания шрифта, опорное разрешение холста, границы ползунка.
// В меню их нет вовсе — это настройка, а не движение.
bool Technical(const std::string& id) {
    static const char* const kSkip[] = {"sprite", "slice", "pixelScale", "fontPixelHeight", "canvas.",
                                        "scroll.speed", "range.min", "range.max", "range.step", "caret",
                                        "placeholder", "Brightness", "disabledAlpha", "pressedOffset",
                                        "smoothing", "repeat"};
    for (const char* s : kSkip)
        if (id.find(s) != std::string::npos) return true;
    return false;
}

// Готовые кривые — плитками. Имена говорят, КАК ДВИЖЕТСЯ, а не как
// называется формула: «Cubic Out» ищут в справочнике, «Плавное замедление»
// понятно сразу. Формулы целиком — в «Профессиональных».
struct Preset {
    const char* Code;
    const char* Name;
};
const Preset kPresets[] = {
    {"linear", "Steady"},          {"quad-out", "Smooth stop"},     {"quad-in", "Smooth start"},
    {"quad-inout", "Smooth both ends"},      {"sine-inout", "Gentle"},        {"cubic-out", "Soft landing"},
    {"expo-out", "Fast"},          {"expo-in", "Sharp start"},      {"back-out", "Overshoot"},
    {"back-in", "Wind-up"},        {"elastic-out", "Springy"},      {"bounce-out", "Bouncy"},
};

anim::Ease PresetEase(const Preset& p) {
    anim::Ease e;
    anim::Parse(p.Code, e);
    return e;
}

// Имена форм в «Профессиональных» не переводятся: это общий язык движков,
// по нему ищут примеры.
const char* const kShapeNames[] = {"Linear", "Quad", "Cubic", "Quart", "Quint", "Sine",
                                   "Expo",   "Circ", "Back",  "Elastic", "Bounce"};

const char* ModeLabel(anim::EaseMode m) {
    switch (m) {
        case anim::EaseMode::In: return T("Ease in");
        case anim::EaseMode::Out: return T("Ease out");
        default: return T("Ease in-out");
    }
}

bool IsColor(const anim::PropertyType* p) {
    if (!p || p->Components < 3) return false;
    const std::string& id = p->Id;
    return id.find("color") != std::string::npos || id.find("colour") != std::string::npos ||
           id.find("tint") != std::string::npos;
}

// График кривой в прямоугольнике [a, b]; y с запасом на перелёт (Back,
// Elastic): 0 и 1 — на margin от краёв.
void PlotEase(ImDrawList* dl, ImVec2 a, ImVec2 b, const anim::Ease& e, ImU32 color, float thickness,
              float margin = 0.0f) {
    ImVec2 pts[41];
    const float w = b.x - a.x;
    const float y0 = b.y - margin, y1 = a.y + margin;
    for (int i = 0; i <= 40; ++i) {
        const float t = i / 40.0f;
        const float v = anim::Evaluate(e, t);
        pts[i] = ImVec2(a.x + t * w, y0 + (y1 - y0) * v);
    }
    dl->PushClipRect(a, b, true);
    dl->AddPolyline(pts, 41, color, ImDrawFlags_None, thickness);
    dl->PopClipRect();
}

ImU32 Col(EditorTheme::Role r) { return ImGui::GetColorU32(EditorTheme::Color(r)); }

bool Has(const std::string& id, const char* part) { return id.find(part) != std::string::npos; }

// Значок свойства — тот же, что у этого действия в остальном редакторе
// (стрелки — сдвиг, круг — поворот): карточку находят по форме, до чтения.
const char* PropIcon(const std::string& id) {
    if (id.empty()) return "clock";
    if (Has(id, "position")) return "move";
    if (Has(id, "rotation")) return "rotate";
    if (Has(id, "scale") || Has(id, "size")) return "scale";
    if (Has(id, "color") || Has(id, "tint")) return "color";
    if (Has(id, "opacity")) return "eye";
    if (id.rfind("light.", 0) == 0) return "light";
    if (id.rfind("camera.", 0) == 0) return "camera";
    if (id.rfind("audio.", 0) == 0) return "audio";
    if (id.rfind("bar.", 0) == 0) return "ui-bar";
    if (id.rfind("range.", 0) == 0) return "ui-slider";
    if (id.rfind("fill.", 0) == 0) return "ui-panel";
    return "anim";
}

// В чём меряется значение. Без этого «1» в поле — загадка: метр? градус?
// процент? Пусто — единиц у свойства нет (цвет).
const char* PropUnit(const std::string& id) {
    if (id == "object.position" || id == "light.range") return T("metres");
    if (id.rfind("element.", 0) == 0 && (Has(id, "position") || Has(id, "size"))) return T("screen points");
    if (Has(id, "rotation") || id == "camera.fov") return T("degrees");
    if (Has(id, "scale")) return T("times original size");
    if (Has(id, "opacity") || id == "audio.volume") return T("0 = none, 1 = full");
    if (id == "bar.value") return T("0 = empty, 1 = full");
    if (id == "audio.pitch") return T("1 = normal");
    return "";
}

} // namespace

namespace tweenui {

std::vector<Choice> Choices(const entt::registry& reg, entt::entity e) {
    std::vector<Choice> out;
    if (!reg.valid(e)) return out;
    const std::vector<const anim::PropertyType*> has = anim::PropertiesFor(reg, e);
    auto present = [&](const std::string& id) -> const anim::PropertyType* {
        for (const anim::PropertyType* p : has)
            if (p->Id == id) return p;
        return nullptr;
    };
    // У элемента интерфейса положение, размер и масштаб — свои (в точках
    // экрана); координаты объекта сцены у него тоже есть, но на экране ничего
    // не двигают — в меню они были бы ловушкой.
    const bool ui = present("element.position") != nullptr;
    auto hidden = [&](const std::string& id) { return ui && id.rfind("object.", 0) == 0; };
    // Сначала частые — в порядке разделов, потом прочие под «Ещё».
    for (const char* section : kSections)
        for (const Known& k : kKnown)
            if (std::strcmp(k.Section, section) == 0 && !hidden(k.Id))
                if (const anim::PropertyType* p = present(k.Id)) out.push_back({p, k.Section, T(k.Title)});
    for (const anim::PropertyType* p : has) {
        if (FindKnown(p->Id) || Technical(p->Id) || hidden(p->Id)) continue;
        out.push_back({p, "", std::string(T(p->Group.c_str())) + " · " + T(p->Title.c_str())});
    }
    return out;
}

std::string TitleOf(const std::string& id) {
    if (id.empty()) return T("Pause step");
    if (const Known* k = FindKnown(id)) return T(k->Title);
    if (const anim::PropertyType* p = anim::FindProperty(id))
        return std::string(T(p->Group.c_str())) + " · " + T(p->Title.c_str());
    return id;
}

std::string EaseName(const anim::Ease& e) {
    for (const Preset& p : kPresets)
        if (PresetEase(p) == e) return T(p.Name);
    if (e.Shape == anim::EaseShape::Curve) return T("Custom curve");
    if (e.Shape == anim::EaseShape::Linear) return T("Steady");
    return std::string(kShapeNames[(int)e.Shape]) + " · " + ModeLabel(e.Mode);
}

glm::vec4 SuggestedEnd(const std::string& id, const glm::vec4& current) {
    // Конец, равный началу, — твин, который ничего не делает: человек ставил
    // «Позицию», жал Play, и объект стоял на месте. Поэтому сразу видимое
    // движение, а не копия нынешнего значения; править его — одно число.
    glm::vec4 v = current;
    if (id == "object.position") v.y += 1.0f;
    else if (id == "element.position") v.x += 50.0f;
    else if (id == "object.rotation") v.y += 90.0f;
    else if (id == "element.rotation") v.x += 45.0f;
    else if (Has(id, "scale") || id == "element.size") v = glm::vec4(glm::vec3(v) * 1.5f, v.w);
    else if (Has(id, "opacity") || id == "audio.volume" || id == "bar.value" || id == "range.value")
        v.x = current.x > 0.5f ? 0.0f : 1.0f;
    else if (id == "light.intensity" || id == "light.range") v.x = std::max(current.x * 2.0f, 1.0f);
    else if (id == "camera.fov") v.x = current.x > 30.0f ? current.x - 15.0f : current.x + 15.0f;
    else if (id == "audio.pitch") v.x = current.x * 1.5f;
    else if (id == "fill.rounding") v.x += 10.0f;
    return v;
}

std::string Summary(const anim::TweenClip& clip) {
    int n = 0;
    for (const anim::TweenTrack& t : clip.Tracks)
        if (!t.Property.empty()) ++n;
    if (n == 0) return T("empty");
    // Русское число: 1 свойство, 2 свойства, 5 свойств.
    const int d = n % 10, h = n % 100;
    const char* word = d == 1 && h != 11                             ? T("property")
                       : d >= 2 && d <= 4 && (h < 12 || h > 14) ? T("properties (2-4)")
                                                                   : T("properties");
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%d %s · %.2g %s", n, word, clip.Length(), T("s"));
    return buf;
}

} // namespace tweenui

// =============================================================================
//  Доступ
// =============================================================================

anim::TweenComponent* TweenPanel::Component(EditorHost& host) {
    if (m_ownerId == 0) return nullptr;
    GameObject o = host.CurrentScene().Get(m_ownerId);
    if (!o.Valid()) return nullptr;
    return o.Registry()->try_get<anim::TweenComponent>(o.Entity());
}

anim::TweenClip* TweenPanel::Clip(EditorHost& host) {
    anim::TweenComponent* tc = Component(host);
    if (!tc || tc->Tweens.empty()) return nullptr;
    m_index = std::clamp(m_index, 0, (int)tc->Tweens.size() - 1);
    return &tc->Tweens[(size_t)m_index];
}

void TweenPanel::Edited(EditorHost& host) {
    // Сначала вернуть объект, потом записывать отмену: иначе в снимок отмены
    // попали бы значения, выставленные просмотром, и уехали бы в сцену.
    StopPreview(host);
    host.PushUndoSnapshot();
}

void TweenPanel::Select(int tweenIndex) {
    m_index = tweenIndex;
    m_splitAxes.clear();
}

void TweenPanel::Open(int objectId, int tweenIndex, Action action) {
    m_ownerId = objectId;
    m_seenSelection = objectId;
    Select(tweenIndex);
    m_focusFrames = 3;
    m_wantProps = action == Action::AddProperty;
    m_wantPlay = action == Action::Play;
}

glm::vec4 TweenPanel::Current(entt::registry& reg, entt::entity target, const anim::PropertyType& p) const {
    if (m_previewing)
        for (const Saved& s : m_saved)
            if (s.Entity == target && s.Property == p.Id) return s.Value;
    glm::vec4 v(0.0f);
    anim::ReadProperty(p, reg, target, v);
    return v;
}

bool TweenPanel::AddProperty(EditorHost& host, const std::string& id) {
    if (host.InPlayMode()) return false;
    const anim::PropertyType* p = anim::FindProperty(id);
    Scene& scene = host.CurrentScene();
    GameObject owner = scene.Get(m_ownerId);
    if (!p || !owner.Valid()) return false;
    StopPreview(host);
    entt::registry& reg = scene.Registry();
    anim::TweenComponent& tc = reg.get_or_emplace<anim::TweenComponent>(owner.Entity());
    if (tc.Tweens.empty()) {
        anim::TweenClip clip;
        clip.Name = "Tween";
        tc.Tweens.push_back(clip);
        Select(0);
    }
    anim::TweenClip& clip = tc.Tweens[(size_t)std::clamp(m_index, 0, (int)tc.Tweens.size() - 1)];
    const entt::entity target = anim::TargetOf(scene, owner.Entity(), clip);
    if (target == entt::null || !anim::HasProperty(*p, reg, target)) return false;
    // Сразу рабочий твин: начало — «как есть в момент старта», конец —
    // заметно другой (SuggestedEnd), чтобы Play сразу показал, что твин делает.
    anim::TweenTrack t;
    t.Property = id;
    glm::vec4 now(0.0f);
    anim::ReadProperty(*p, reg, target, now);
    t.From = now;
    t.To = tweenui::SuggestedEnd(id, now);
    t.FromCurrent = true;
    t.Duration = 0.5f;
    t.Curve = anim::Ease::Make(anim::EaseShape::Quad, anim::EaseMode::Out);
    clip.AppendWith(t);   // вместе с остальными: сдвинуть — одним движением полосы
    host.PushUndoSnapshot();
    return true;
}

// =============================================================================
//  Просмотр — общим проигрывателем сцены
// =============================================================================

void TweenPanel::Play(EditorHost& host) {
    if (host.InPlayMode()) return;   // в игре твины идут сами
    Scene& scene = host.CurrentScene();
    if (m_previewing && m_previewScene == &scene && scene.Tweens.IsActive(m_handle)) {
        scene.Tweens.Resume(m_handle);
        m_paused = false;
        return;
    }
    StopPreview(host);   // доигравший — с начала
    anim::TweenClip* clip = Clip(host);
    GameObject owner = scene.Get(m_ownerId);
    if (!clip || !owner.Valid()) return;
    const entt::entity target = anim::TargetOf(scene, owner.Entity(), *clip);
    if (target == entt::null) return;
    // Запомнить всё, что твин тронет, — чтобы «Вернуть» поставил объект как был.
    m_saved.clear();
    entt::registry& reg = scene.Registry();
    for (const anim::TweenTrack& t : clip->Tracks) {
        const anim::PropertyType* p = anim::FindProperty(t.Property);
        if (!p) continue;
        Saved s;
        s.Entity = target;
        s.Property = t.Property;
        if (anim::ReadProperty(*p, reg, target, s.Value)) m_saved.push_back(s);
    }
    anim::TweenClip copy = *clip;
    copy.Delay = 0.0f;   // задержку в просмотре ждать незачем — она видна в настройках
    copy.Then.clear();   // следующий твин — уже другой просмотр
    m_handle = scene.Tweens.Play(reg, target, copy);
    m_previewScene = &scene;
    m_previewing = m_handle != anim::kNoTween;
    m_paused = false;
    m_lastTime = 0.0f;
}

void TweenPanel::Pause(EditorHost& host) {
    if (!m_previewing) return;
    host.CurrentScene().Tweens.Pause(m_handle);
    m_paused = true;
}

void TweenPanel::StopPreview(EditorHost& host) {
    if (!m_previewing) return;
    Scene& scene = host.CurrentScene();
    // Сцену за это время могли сменить — тогда возвращать некуда, и трогать
    // её проигрыватель нельзя: номер твина относится к прежней.
    if (m_previewScene == &scene) {
        scene.Tweens.Stop(m_handle);
        entt::registry& reg = scene.Registry();
        for (const Saved& s : m_saved)
            if (const anim::PropertyType* p = anim::FindProperty(s.Property))
                if (reg.valid(s.Entity)) anim::WriteProperty(*p, reg, s.Entity, s.Value);
    }
    m_saved.clear();
    m_previewing = false;
    m_paused = false;
    m_handle = anim::kNoTween;
    m_previewScene = nullptr;
    m_lastTime = 0.0f;
}

float TweenPanel::PreviewTime(EditorHost& host) const {
    if (!m_previewing || m_previewScene != &host.CurrentScene()) return 0.0f;
    const Scene& scene = host.CurrentScene();
    return scene.Tweens.IsActive(m_handle) ? scene.Tweens.TimeOf(m_handle) : m_lastTime;
}

// =============================================================================
//  Окно
// =============================================================================

void TweenPanel::Draw(EditorHost& host, bool* open, const std::string& windowId) {
    if (!open || !*open) {
        StopPreview(host);
        return;
    }
    // Просмотр идёт по времени кадра редактора: сцена правки системой кадра
    // не шагает, шагаем её проигрыватель сами — тот же, что в игре. ДО Begin:
    // окно, спрятанное за соседней вкладкой, не должно останавливать твин.
    {
        Scene& scene = host.CurrentScene();
        if (m_previewing) {
            if (m_previewScene != &scene || host.InPlayMode()) {
                m_previewing = false;
                m_saved.clear();
            } else if (!m_paused) {
                const bool was = scene.Tweens.IsActive(m_handle);
                if (was) m_lastTime = scene.Tweens.TimeOf(m_handle);
                scene.Tweens.Update(ImGui::GetIO().DeltaTime);
                if (was && !scene.Tweens.IsActive(m_handle)) {
                    const anim::TweenClip* c = Clip(host);
                    m_lastTime = c ? c->Length() : m_lastTime;
                }
            }
        }
    }
    // Фокус окну — несколько кадров (док решает, чья вкладка сверху, не
    // сразу), и каждый такой кадр закрывает всплывающие окна над ним. Меню,
    // о котором просили при открытии, ждёт, пока фокус устоится.
    const bool focusing = m_focusFrames > 0;
    if (focusing) {
        ImGui::SetNextWindowFocus();
        --m_focusFrames;
    }
    ImGui::SetNextWindowSize(ImVec2(960.0f, 340.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(sage::editor::panelid::Title("clock", T("Tween"), windowId).c_str(), open,
                      panelwindows::WindowFlags("Tween"))) {
        ImGui::End();
        return;
    }

    Scene& scene = host.CurrentScene();

    // За выбором в сцене: выбрал другой объект — окно про него.
    GameObject sel = host.SelectedObject();
    const int selId = sel.Valid() ? sel.Id() : 0;
    if (selId != m_seenSelection) {
        m_seenSelection = selId;
        if (selId != 0 && selId != m_ownerId) {
            StopPreview(host);
            m_ownerId = selId;
            Select(0);
        }
    }

    GameObject owner = scene.Get(m_ownerId);
    if (!owner.Valid()) {
        Sage::UI::EmptyState(T("No object selected"), T("Select an object in the scene to animate it with a tween."));
        ImGui::End();
        return;
    }
    anim::TweenComponent* tc = owner.Registry()->try_get<anim::TweenComponent>(owner.Entity());
    anim::TweenClip* clip = Clip(host);
    entt::entity target = clip ? anim::TargetOf(scene, owner.Entity(), *clip) : owner.Entity();

    if (m_wantPlay) {
        m_wantPlay = false;
        Play(host);
    }

    if (!tc || !clip) {
        DrawEmpty(host, false);
    } else {
        DrawHeader(host, *tc);
        clip = Clip(host);   // шапка могла удалить или добавить твин
        tc = owner.Registry()->try_get<anim::TweenComponent>(owner.Entity());
        if (clip && tc) {
            target = anim::TargetOf(scene, owner.Entity(), *clip);
            if (target == entt::null) {
                ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s",
                                   T("The object this tween animates is gone — pick another one under Advanced."));
            } else if (clip->Tracks.empty()) {
                DrawEmpty(host, true);
            } else {
                DrawRuler(host, *clip);
                for (size_t i = 0; i < clip->Tracks.size(); ++i) DrawCard(host, *clip, i, target);
                // Бегунок просмотра — через шкалу и все полосы.
                if (m_previewing) {
                    const float x = m_laneX + std::min(PreviewTime(host), clip->Length()) * m_pxPerSec;
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, m_rulerTop), ImVec2(x, m_cardsBottom),
                                                        Col(EditorTheme::Role::Warn), 2.0f);
                }
                ImGui::BeginDisabled(host.InPlayMode());
                if (EditorIcons::Button("plus", T("Add property"))) m_wantProps = true;
                ImGui::EndDisabled();
            }
            DrawAdvanced(host, *tc, *clip);
        }
    }
    if (!focusing) DrawPopups(host, Clip(host), target);
    ImGui::End();
}

// --- Пусто: ни твина, ни свойств — одна кнопка ------------------------------------------
void TweenPanel::DrawEmpty(EditorHost& host, bool hasTween) {
    const float h = ImGui::GetFrameHeight();
    const char* label = hasTween ? T("Add property") : T("Tween");
    const char* hint = T("Smoothly change position, scale, colour, opacity… over time.");
    const float bw = EditorIcons::LabeledWidth(std::floor(h * 0.68f), label) + ImGui::GetStyle().FramePadding.x * 2.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float blockH = h + ImGui::GetTextLineHeightWithSpacing() * 1.5f;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (avail.y - blockH) * 0.4f));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - bw) * 0.5f));
    ImGui::BeginDisabled(host.InPlayMode());
    if (EditorIcons::Button("plus", label)) m_wantProps = true;
    ImGui::EndDisabled();
    const float tw = ImGui::CalcTextSize(hint).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - tw) * 0.5f));
    ImGui::TextDisabled("%s", hint);
}

// --- Шапка: какой твин, просмотр, время --------------------------------------------------
void TweenPanel::DrawHeader(EditorHost& host, anim::TweenComponent& tc) {
    GameObject owner = host.CurrentScene().Get(m_ownerId);
    const bool play = host.InPlayMode();
    const anim::TweenClip& clip = tc.Tweens[(size_t)m_index];
    const float lineRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;

    // Какой твин: список + «новый» и «удалить» в нём же — отдельные кнопки
    // рядом были бы шумом в шапке ради действия раз в день.
    ImGui::SetNextItemWidth(170.0f);
    if (ImGui::BeginCombo("##tween", clip.Name.c_str())) {
        for (int i = 0; i < (int)tc.Tweens.size(); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(tc.Tweens[(size_t)i].Name.c_str(), i == m_index) && i != m_index) {
                StopPreview(host);
                Select(i);
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::BeginDisabled(play);
        if (EditorIcons::MenuItem("plus", T("New tween"))) {
            StopPreview(host);
            anim::TweenClip c;
            c.Name = "Tween " + std::to_string(tc.Tweens.size() + 1);
            tc.Tweens.push_back(c);
            Select((int)tc.Tweens.size() - 1);
            host.PushUndoSnapshot();
        }
        if (EditorIcons::MenuItem("trash", T("Delete this tween"))) {
            StopPreview(host);
            tc.Tweens.erase(tc.Tweens.begin() + m_index);
            Select(std::max(0, m_index - 1));
            if (tc.Tweens.empty()) owner.Registry()->remove<anim::TweenComponent>(owner.Entity());
            host.PushUndoSnapshot();
        }
        ImGui::EndDisabled();
        ImGui::EndCombo();
        if (!owner.Registry()->all_of<anim::TweenComponent>(owner.Entity())) return;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Which tween of this object you are editing (an object can have several)"));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    EditorIcons::Inline("cube");
    ImGui::SameLine(0.0f, EditorIcons::TextGap());
    ImGui::TextDisabled("%s", owner.Name().c_str());
    // Как это читать — одной подсказкой, а не угадыванием по полям.
    ImGui::SameLine();
    EditorIcons::IconOnlyButton("question", T("How a tween works:\n"
                                              "• each card is one property that changes from Start to End;\n"
                                              "• the bar on the right is when it happens: drag the middle to move it,\n"
                                              "  the dots to change when it starts and how long it lasts;\n"
                                              "• the curve says how the speed changes along the way;\n"
                                              "• Play shows it on the object, the round arrow next to it puts the object back."));

    // Справа: просмотр и время.
    const anim::TweenClip* c = Clip(host);
    char timeText[48];
    std::snprintf(timeText, sizeof(timeText), "%.2f / %.2f %s", PreviewTime(host), c ? c->Length() : 0.0f, T("s"));
    const float btn = ImGui::GetFrameHeight();
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const float right = btn * 2.0f + sp * 2.0f + ImGui::CalcTextSize(timeText).x;
    const float after = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX() + sp;
    ImGui::SameLine(std::max(after, lineRight - right));
    const bool running = m_previewing && !m_paused && host.CurrentScene().Tweens.IsActive(m_handle);
    ImGui::BeginDisabled(play);
    if (EditorIcons::IconOnlyButton(running ? "pause" : "play", running ? T("Pause") : T("Play"), running))
        running ? Pause(host) : Play(host);
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_previewing);
    if (EditorIcons::IconOnlyButton("refresh", T("Put the object back as it was"))) StopPreview(host);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", timeText);
    if (play) ImGui::TextDisabled("%s", T("(in the game tweens run by themselves)"));
}

// --- Шкала времени над полосами ---------------------------------------------------------
void TweenPanel::DrawRuler(EditorHost& host, anim::TweenClip& clip) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float availW = ImGui::GetContentRegionAvail().x;
    // Левая колонка — название и значения; шкала — всё, что справа.
    m_infoW = std::clamp(availW * 0.5f, 330.0f, 520.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    m_laneX = origin.x + m_infoW + st.ItemSpacing.x * 2.0f;
    m_laneW = std::max(origin.x + availW - m_laneX - 36.0f, 60.0f);   // справа — место подписи длительности
    const float length = std::max(clip.Length(), 0.01f);
    const float visible = std::max(length * 1.2f, 0.5f);   // с запасом: полосу есть куда тянуть
    m_pxPerSec = m_laneW / visible;

    const float h = ImGui::GetTextLineHeight() + 4.0f;
    m_rulerTop = origin.y;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 line = Col(EditorTheme::Role::Line);
    const ImU32 text = Col(EditorTheme::Role::TextFaint);
    // Шаг делений — «круглый» и не чаще ширины подписи.
    const float labelW = ImGui::CalcTextSize("0.00").x + 10.0f;
    float step = 60.0f;
    for (float s : {0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 30.0f, 60.0f})
        if (s * m_pxPerSec >= labelW) { step = s; break; }
    for (float t = 0.0f; t <= visible + 1e-4f; t += step) {
        const float x = m_laneX + t * m_pxPerSec;
        dl->AddLine(ImVec2(x, origin.y + h - 4.0f), ImVec2(x, origin.y + h), line);
        char buf[16];
        std::snprintf(buf, sizeof(buf), step < 1.0f ? "%.2f" : "%.0f", t);
        dl->AddText(ImVec2(x + 2.0f, origin.y), text, buf);
    }

    // Подписи колонок: слева — что меняется, справа — когда.
    dl->AddText(ImVec2(origin.x + 6.0f, origin.y), text, T("Property: Start → End"));
    {
        const char* timeLabel = T("time, s");
        const float w = ImGui::CalcTextSize(timeLabel).x;
        dl->AddText(ImVec2(m_laneX - w - st.ItemSpacing.x, origin.y), text, timeLabel);
    }

    // Бегунок: щелчок или протяжка по шкале — посмотреть момент.
    ImGui::SetCursorScreenPos(ImVec2(m_laneX, origin.y));
    ImGui::InvisibleButton("##scrub", ImVec2(std::max(length * m_pxPerSec - 6.0f, 1.0f), h));
    if (ImGui::IsItemActive() && !host.InPlayMode()) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - m_laneX) / m_pxPerSec, 0.0f, length);
        if (!m_previewing) Play(host);
        if (m_previewing) {
            Pause(host);
            host.CurrentScene().Tweens.Seek(m_handle, t);
            m_lastTime = t;
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Click or drag to see the tween at that moment"));

    // Конец твина — ручка: тянешь — растягиваешь весь твин.
    const float endX = m_laneX + length * m_pxPerSec;
    ImGui::SetCursorScreenPos(ImVec2(endX - 6.0f, origin.y));
    ImGui::BeginDisabled(host.InPlayMode());
    ImGui::InvisibleButton("##end", ImVec2(12.0f, h));
    ImGui::EndDisabled();
    const bool endHot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (endHot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActivated()) StopPreview(host);
    if (ImGui::IsItemActive()) clip.Stretch(std::max((ImGui::GetIO().MousePos.x - m_laneX) / m_pxPerSec, 0.05f));
    if (ImGui::IsItemDeactivated()) host.PushUndoSnapshot();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Drag to stretch the whole tween"));
    dl->AddTriangleFilled(ImVec2(endX - 5, origin.y + h - 8), ImVec2(endX + 5, origin.y + h - 8), ImVec2(endX, origin.y + h),
                          Col(endHot ? EditorTheme::Role::AccentHover : EditorTheme::Role::Accent));

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + h + st.ItemSpacing.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

// --- Значение свойства по числу компонент ------------------------------------------------
//
// У каждой оси — своя буква в поле («X 1.00»): три одинаковых числа подряд не
// говорят, какое из них высота.
bool TweenPanel::DrawValue(const char* id, const anim::PropertyType* p, glm::vec4& v, float width, bool uniform) {
    ImGui::PushID(id);
    bool changed = false;
    const int comps = p ? p->Components : 1;
    if (IsColor(p)) {
        ImGui::SetNextItemWidth(width);
        changed = comps >= 4 ? Sage::UI::ColorField4("##c", &v.x) : Sage::UI::ColorField3("##c", &v.x);
    } else if (uniform || comps == 1) {
        // Масштаб «одним числом»: в 9 из 10 твинов оси меняются вместе.
        ImGui::SetNextItemWidth(width);
        float s = v.x;
        const float speed = comps == 1 && s > 10.0f ? 0.1f : 0.01f;
        if ((changed = ImGui::DragFloat("##v", &s, speed, 0.0f, 0.0f, uniform ? "XYZ %.2f" : "%.2f")))
            v = uniform ? glm::vec4(s, s, s, v.w) : glm::vec4(s, v.y, v.z, v.w);
    } else {
        static const char* const kAxis[] = {"X %.2f", "Y %.2f", "Z %.2f", "W %.2f"};
        const int n = std::min(comps, 4);
        ImGui::PushMultiItemsWidths(n, width);
        for (int k = 0; k < n; ++k) {
            ImGui::PushID(k);
            if (k) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            changed |= ImGui::DragFloat("##a", &v[k], 0.05f, 0.0f, 0.0f, kAxis[k]);
            ImGui::PopItemWidth();
            ImGui::PopID();
        }
    }
    ImGui::PopID();
    return changed;
}

// --- Карточка свойства ---------------------------------------------------------------
//
//   [значок] Позиция · метры              [кривая ▾] [⋯]
//   Начало  [X 0.00][Y 0.00][Z 0.00]   [↻ как сейчас]
//   Конец   [X 0.00][Y 1.00][Z 0.00]   [⌖ взять из объекта]
//   (конец = начало — свойство не изменится: …)
//
// Подписи «Начало/Конец» и буквы осей — потому что раньше это были два
// безымянных ряда чисел со стрелкой, и что из них что, приходилось угадывать.
void TweenPanel::DrawCard(EditorHost& host, anim::TweenClip& clip, size_t index, entt::entity target) {
    anim::TweenTrack& t = clip.Tracks[index];
    const anim::PropertyType* p = anim::FindProperty(t.Property);
    const bool gap = t.Property.empty();
    const bool play = host.InPlayMode();
    entt::registry& reg = host.CurrentScene().Registry();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float fh = ImGui::GetFrameHeight();
    const float pad = 6.0f;
    const bool present = p && target != entt::null && anim::HasProperty(*p, reg, target);

    // Значения — до раскладки: от них зависит, нужна ли строка-предупреждение.
    const bool scale = t.Property == "object.scale";
    const glm::vec4 cur = present ? Current(reg, target, *p) : t.From;
    glm::vec4 from = t.FromCurrent ? cur : t.From;
    const int comps = p ? p->Components : 1;
    bool still = !gap && p != nullptr;
    for (int k = 0; k < std::min(comps, 4) && still; ++k) still = std::fabs(from[k] - t.To[k]) < 1e-4f;

    ImGui::PushID((int)index);
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float row = fh + st.ItemSpacing.y;
    const float warnH = still ? ImGui::GetTextLineHeightWithSpacing() : 0.0f;
    const float cardH = gap ? fh + pad * 2.0f : (p ? row * 2.0f + fh : fh) + warnH + pad * 2.0f;
    const float cardR = m_laneX + m_laneW + 34.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(cardR, a.y + cardH), Col(EditorTheme::Role::SurfaceAlt), 6.0f);

    const float x0 = a.x + pad;
    const float infoR = a.x + m_infoW;   // правый край левой колонки
    ImGui::SetCursorScreenPos(ImVec2(x0, a.y + pad));
    ImGui::BeginDisabled(play);

    // Строка 1: значок, название, единицы … кривая, ⋯
    ImGui::AlignTextToFramePadding();
    EditorIcons::Inline(PropIcon(t.Property));
    ImGui::SameLine(0.0f, EditorIcons::TextGap());
    if (gap) {
        ImGui::TextDisabled("%s", T("Pause step"));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("A pause: what comes after it waits. Drag the bar to change how long."));
    } else if (!present) {
        ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s", tweenui::TitleOf(t.Property).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("This object has no such property — it is skipped"));
    }

    // ⋯ — у правого края колонки; кривая — слева от него.
    const float dotsX = infoR - fh - pad;
    const std::string ease = gap ? std::string() : tweenui::EaseName(t.Curve);
    const float thumb = fh - 6.0f;
    const float arrow = ImGui::CalcTextSize("v").x;
    const float chipW = st.FramePadding.x * 2.0f + thumb + 6.0f + ImGui::CalcTextSize(ease.c_str()).x + 8.0f + arrow;
    if (!gap && present) {
        ImGui::TextUnformatted(tweenui::TitleOf(t.Property).c_str());
        // Единицы — только если влезают до кривой; не влезли — в подсказке.
        const char* unit = PropUnit(t.Property);
        const std::string unitText = *unit ? std::string("· ") + unit : std::string();
        const bool fits = *unit && ImGui::GetItemRectMax().x + st.ItemSpacing.x * 2.0f + ImGui::CalcTextSize(unitText.c_str()).x <
                                       dotsX - st.ItemSpacing.x - chipW;
        if (fits) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", unitText.c_str());
        } else if (*unit && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", unit);
        }
    }
    if (!gap) {
        const float chipX = std::max(dotsX - st.ItemSpacing.x - chipW, ImGui::GetItemRectMax().x + st.ItemSpacing.x);
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(chipX, a.y + pad));
        if (ImGui::InvisibleButton("##ease", ImVec2(chipW, fh))) {
            m_popupTrack = (int)index;
            m_wantEase = true;
        }
        const bool hot = ImGui::IsItemHovered();
        if (hot) ImGui::SetTooltip("%s", T("Curve: how the speed changes on the way from Start to End. Click to choose."));
        const ImVec2 c0 = ImGui::GetItemRectMin(), c1 = ImGui::GetItemRectMax();
        dl->AddRectFilled(c0, c1, Col(hot ? EditorTheme::Role::Hover : EditorTheme::Role::Input), st.FrameRounding);
        float cx = c0.x + st.FramePadding.x;
        const float ty = c0.y + (fh - ImGui::GetTextLineHeight()) * 0.5f;
        PlotEase(dl, ImVec2(cx, c0.y + 3.0f), ImVec2(cx + thumb, c1.y - 3.0f), t.Curve, Col(EditorTheme::Role::Accent),
                 1.5f, thumb * 0.15f);
        cx += thumb + 6.0f;
        dl->AddText(ImVec2(cx, ty), Col(EditorTheme::Role::Text), ease.c_str());
        // Стрелка «раскрывается» — треугольником, а не буквой.
        const float ax = c1.x - st.FramePadding.x - arrow * 0.5f, ay = c0.y + fh * 0.5f;
        dl->AddTriangleFilled(ImVec2(ax - 4, ay - 2), ImVec2(ax + 4, ay - 2), ImVec2(ax, ay + 3),
                              Col(EditorTheme::Role::TextDim));
    }
    ImGui::SetCursorScreenPos(ImVec2(dotsX, a.y + pad));
    if (EditorIcons::IconOnlyButton("dots", T("More: order, remove"))) {
        m_popupTrack = (int)index;
        m_wantCardMenu = true;
    }

    // Строки 2–3: Начало и Конец, каждая со своей кнопкой справа.
    if (!gap && p) {
        const char* startLabel = T("Starts at");
        const char* endLabel = T("Ends at");
        const float labelW = std::max(ImGui::CalcTextSize(startLabel).x, ImGui::CalcTextSize(endLabel).x) + st.ItemSpacing.x;
        const float fieldX = x0 + labelW;
        const float fieldW = std::max(infoR - pad - fieldX - fh - st.ItemSpacing.x, 60.0f);
        auto same = [](const glm::vec4& v) { return v.x == v.y && v.y == v.z; };
        const bool uniform = scale && !m_splitAxes.count((int)index) && same(from) && same(t.To);

        // Начало
        const float y1 = a.y + pad + row;
        ImGui::SetCursorScreenPos(ImVec2(x0, y1));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(startLabel);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Where the property starts from"));
        ImGui::SetCursorScreenPos(ImVec2(fieldX, y1));
        // «Как сейчас» — приглушено: это не число, а «то, что будет у объекта
        // в момент старта». Тронул поле — стало своим числом.
        if (t.FromCurrent) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, st.Alpha * 0.55f);
        if (DrawValue("from", p, from, fieldW, uniform)) {
            StopPreview(host);
            t.From = from;
            t.FromCurrent = false;
        }
        if (t.FromCurrent) ImGui::PopStyleVar();
        host.TrackLastImGuiItem();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", t.FromCurrent ? T("Start = the value the object has when the tween starts. Type a number to set your own.")
                                                  : T("Start value"));
        ImGui::SameLine();
        if (EditorIcons::IconOnlyButton("refresh", t.FromCurrent ? T("Starts from the current value (on). Click to use a fixed number instead")
                                                                 : T("Start from the object's current value"),
                                        t.FromCurrent)) {
            t.FromCurrent = !t.FromCurrent;
            if (!t.FromCurrent && present) t.From = cur;
            Edited(host);
        }

        // Конец
        const float y2 = y1 + row;
        ImGui::SetCursorScreenPos(ImVec2(x0, y2));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(endLabel);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Where the property ends up when the tween finishes"));
        ImGui::SetCursorScreenPos(ImVec2(fieldX, y2));
        if (DrawValue("to", p, t.To, fieldW, uniform)) StopPreview(host);
        host.TrackLastImGuiItem();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("End value"));
        ImGui::SameLine();
        ImGui::BeginDisabled(!present);
        if (EditorIcons::IconOnlyButton("eyedropper", T("Take the end from the object: move/rotate/paint the object in the scene, then click"))) {
            t.To = cur;
            Edited(host);
        }
        ImGui::EndDisabled();

        if (still) {
            ImGui::SetCursorScreenPos(ImVec2(fieldX, y2 + fh + st.ItemSpacing.y * 0.5f));
            ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s",
                               T("End = Start: nothing will change. Set the End value."));
        }
    }
    ImGui::EndDisabled();

    DrawLane(host, clip, index, a.y + pad, a.y + cardH - pad);
    m_cardsBottom = a.y + cardH;

    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + cardH + st.ItemSpacing.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopID();
}

// --- Полоса на шкале: ● начало — середина — конец ● --------------------------------------
void TweenPanel::DrawLane(EditorHost& host, anim::TweenClip& clip, size_t index, float y0, float y1) {
    anim::TweenTrack& t = clip.Tracks[index];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool gap = t.Property.empty();
    const float cy = (y0 + y1) * 0.5f;
    const float barH = std::min(y1 - y0, 22.0f);
    const float by0 = cy - barH * 0.5f, by1 = cy + barH * 0.5f;
    const float x0 = m_laneX + t.Start * m_pxPerSec;
    const float x1 = m_laneX + t.End() * m_pxPerSec;
    const float r = 6.0f;

    // Дорожка — тонкая линия во всю шкалу, чтобы было видно «где ноль».
    dl->AddLine(ImVec2(m_laneX, cy), ImVec2(m_laneX + m_laneW, cy), Col(EditorTheme::Role::Line), 1.0f);

    ImGui::BeginDisabled(host.InPlayMode());
    bool anyHot = false;
    auto handle = [&](const char* id, float hx, float hw, int mode) {
        ImGui::SetCursorScreenPos(ImVec2(hx, by0));
        ImGui::InvisibleButton(id, ImVec2(std::max(hw, 1.0f), barH));
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        anyHot |= hot;
        if (hot) ImGui::SetMouseCursor(mode == 0 ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
            ImGui::SetTooltip("%s", mode == 0 ? T("Drag: when it starts") : mode == 1 ? T("Drag: start point")
                                                                                         : T("Drag: end point (how long it lasts)"));
        // Сдвиг копится между кадрами: шаг 0.05 с съел бы медленное движение
        // мыши целиком, и полоса не сдвинулась бы вовсе.
        static float carry = 0.0f;
        if (ImGui::IsItemActivated()) {
            StopPreview(host);
            carry = 0.0f;
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            // Шаг 0.05 с: «ровно 0.3 секунды» ставится мышью, а не угадывается.
            // Shift — без шага.
            carry += ImGui::GetIO().MouseDelta.x / m_pxPerSec;
            auto snap = [](float v) { return ImGui::GetIO().KeyShift ? v : std::round(v / 0.05f) * 0.05f; };
            if (mode == 0) {
                const float s = snap(std::max(0.0f, t.Start + carry));
                carry -= s - t.Start;
                t.Start = s;
            } else if (mode == 1) {
                const float end = t.End();
                const float s = snap(std::clamp(t.Start + carry, 0.0f, end - 0.05f));
                carry -= s - t.Start;
                t.Start = s;
                t.Duration = end - s;
            } else {
                const float d = snap(std::max(0.05f, t.Duration + carry));
                carry -= d - t.Duration;
                t.Duration = std::max(d, 0.05f);
            }
        }
        if (ImGui::IsItemDeactivated()) host.PushUndoSnapshot();
    };
    handle("##body", x0 + r, x1 - x0 - r * 2.0f, 0);
    handle("##from", x0 - r, r * 2.0f, 1);
    handle("##to", x1 - r, r * 2.0f, 2);
    ImGui::EndDisabled();

    const ImU32 fill = Col(gap ? EditorTheme::Role::Input : EditorTheme::Role::AccentMuted);
    dl->AddRectFilled(ImVec2(x0, by0), ImVec2(x1, by1), fill, 4.0f);
    if (anyHot) dl->AddRect(ImVec2(x0, by0), ImVec2(x1, by1), Col(EditorTheme::Role::Accent), 4.0f);
    if (!gap && x1 - x0 > 2.0f * r + 8.0f)
        PlotEase(dl, ImVec2(x0 + r, by0 + 2.0f), ImVec2(x1 - r, by1 - 2.0f), t.Curve, Col(EditorTheme::Role::Accent), 1.0f,
                 barH * 0.12f);
    // Точки начала и конца — то, за что тянут.
    const ImU32 dot = Col(gap ? EditorTheme::Role::TextDim : EditorTheme::Role::Accent);
    dl->AddCircleFilled(ImVec2(x0, cy), r - 1.0f, dot);
    dl->AddCircleFilled(ImVec2(x1, cy), r - 1.0f, dot);
    char dur[24];
    std::snprintf(dur, sizeof(dur), "%.2f %s", t.Duration, T("s"));
    const ImVec2 ts = ImGui::CalcTextSize(dur);
    dl->AddText(ImVec2(x1 + r + 3.0f, cy - ts.y * 0.5f), Col(EditorTheme::Role::TextDim), dur);
}

// --- Дополнительно: всё, что нужно не каждому твину --------------------------------------
void TweenPanel::DrawAdvanced(EditorHost& host, anim::TweenComponent& tc, anim::TweenClip& clip) {
    ImGui::Spacing();
    // Свёрнуто по умолчанию — и одной строкой, без рамки раздела.
    if (!m_advanced) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    const bool toggled =
        EditorIcons::Button("gear", T("Advanced"), T("Order, delay, repeat, speed, what happens next"), m_advanced);
    if (!m_advanced) ImGui::PopStyleColor();
    if (toggled) m_advanced = !m_advanced;
    if (!m_advanced) return;

    const bool play = host.InPlayMode();
    ImGui::BeginDisabled(play);
    Sage::UI::BeginProperties("tweenadv");

    Sage::UI::PropertyLabel(T("Order"), T("How the properties follow each other on the timeline"));
    if (EditorIcons::Button("step", T("One after another"), T("Each property starts when the previous one ends"))) {
        clip.ArrangeSequence();
        Edited(host);
    }
    ImGui::SameLine();
    if (EditorIcons::Button("layout", T("All together"), T("All properties start at the same time"))) {
        clip.ArrangeParallel();
        Edited(host);
    }
    ImGui::SameLine();
    if (EditorIcons::Button("clock", T("Pause step"), T("A pause: what comes after it waits"))) {
        anim::TweenTrack gap;
        gap.Duration = 0.25f;
        clip.AppendAfter(gap);
        Edited(host);
    }

    Sage::UI::PropertyLabel(T("Delay"), T("Wait before the tween starts"));
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::DragFloat("##delay", &clip.Delay, 0.01f, 0.0f, 600.0f, "%.2f s")) StopPreview(host);
    host.TrackLastImGuiItem();

    Sage::UI::PropertyLabel(T("Repeat mode"));
    static const char* kLoops[] = {"Once", "Round and round", "Ping-pong"};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(T(kLoops[i]), (int)clip.Loop == i) && (int)clip.Loop != i) {
            clip.Loop = (anim::TweenLoop)i;
            Edited(host);
        }
    }

    Sage::UI::PropertyLabel(T("Speed"));
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::DragFloat("##speed", &clip.Speed, 0.01f, 0.01f, 20.0f, "x%.2f")) StopPreview(host);
    host.TrackLastImGuiItem();

    Sage::UI::PropertyLabel(T("Direction"));
    if (ImGui::RadioButton(T("Forward"), !clip.Reverse) && clip.Reverse) {
        clip.Reverse = false;
        Edited(host);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(T("Backward"), clip.Reverse) && !clip.Reverse) {
        clip.Reverse = true;
        Edited(host);
    }

    Sage::UI::PropertyLabel(T("When finished"), T("Start another tween of this object right after this one"));
    ImGui::SetNextItemWidth(220.0f);
    const std::string thenLabel = clip.Then.empty() ? std::string(T("Nothing")) : std::string(T("Play")) + " " + clip.Then;
    if (ImGui::BeginCombo("##then", thenLabel.c_str())) {
        if (ImGui::Selectable(T("Nothing"), clip.Then.empty()) && !clip.Then.empty()) {
            clip.Then.clear();
            Edited(host);
        }
        for (size_t i = 0; i < tc.Tweens.size(); ++i) {
            const std::string& name = tc.Tweens[i].Name;
            ImGui::PushID((int)i);
            if (ImGui::Selectable((std::string(T("Play")) + " " + name).c_str(), clip.Then == name) && clip.Then != name) {
                clip.Then = name;
                Edited(host);
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    Sage::UI::PropertyLabel(T("Start"), T("Starts by itself when the game starts"));
    if (ImGui::Checkbox(T("By itself when the game starts"), &clip.PlayOnStart)) Edited(host);

    Sage::UI::PropertyLabel(T("Animates"), T("The object whose properties change; empty — this object"));
    objectslot::Options opt;
    opt.EmptyLabel = "This object";
    opt.SelfId = m_ownerId;
    const objectslot::Result r = objectslot::Draw(host, "target", clip.Target, opt);
    if (r.Changed) {
        clip.Target = r.Id == m_ownerId ? 0 : r.Id;
        Edited(host);
    }

    Sage::UI::PropertyLabel(T("Name"), T("By this name a script plays it: Tween.play(obj, \"Name\")"));
    char name[64];
    std::snprintf(name, sizeof(name), "%s", clip.Name.c_str());
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("##name", name, sizeof(name));
    if (ImGui::IsItemDeactivatedAfterEdit() && name[0] && clip.Name != name) {
        // Ссылки «когда закончится — запустить» идут за именем.
        for (anim::TweenClip& other : tc.Tweens)
            if (other.Then == clip.Name) other.Then = name;
        clip.Name = name;
        Edited(host);
    }

    Sage::UI::EndProperties();
    ImGui::EndDisabled();
}

// =============================================================================
//  Всплывающие окна
// =============================================================================

void TweenPanel::DrawPopups(EditorHost& host, anim::TweenClip* clip, entt::entity target) {
    if (m_wantProps) {
        ImGui::OpenPopup("##tweenprops");
        m_wantProps = false;
    }
    DrawPropertyMenu(host, target);
    if (!clip) return;
    m_popupTrack = std::clamp(m_popupTrack, 0, std::max(0, (int)clip->Tracks.size() - 1));
    if (m_wantEase) {
        ImGui::OpenPopup("##tweenease");
        m_wantEase = false;
    }
    DrawEasePicker(host, *clip);
    if (m_wantCurve) {
        ImGui::OpenPopup("##tweencurve");
        m_wantCurve = false;
    }
    DrawCurveEditor(host, *clip);
    if (m_wantCardMenu) {
        ImGui::OpenPopup("##tweencard");
        m_wantCardMenu = false;
    }
    DrawCardMenu(host, *clip, target);
}

// Меню свойств: разделы, только то, что у объекта есть; редкое — под «Ещё».
void TweenPanel::DrawPropertyMenu(EditorHost& host, entt::entity target) {
    Sage::UI::MenuScope menu;
    if (!ImGui::BeginPopup("##tweenprops")) return;
    const std::vector<tweenui::Choice> choices = tweenui::Choices(host.CurrentScene().Registry(), target);
    std::string picked;
    const char* section = nullptr;
    bool first = true;
    for (const tweenui::Choice& c : choices) {
        if (!*c.Section) continue;
        if (!section || std::strcmp(section, c.Section) != 0) {
            section = c.Section;
            Sage::UI::MenuSection(T(section), first);
            first = false;
        }
        ImGui::PushID(c.Property->Id.c_str());
        if (EditorIcons::MenuItem(PropIcon(c.Property->Id), c.Title.c_str())) picked = c.Property->Id;
        ImGui::PopID();
    }
    const bool more = std::any_of(choices.begin(), choices.end(), [](const tweenui::Choice& c) { return !*c.Section; });
    if (more) {
        if (!first) ImGui::Separator();
        if (EditorIcons::BeginMenu("dots", T("More"))) {
            for (const tweenui::Choice& c : choices) {
                if (*c.Section) continue;
                ImGui::PushID(c.Property->Id.c_str());
                if (EditorIcons::MenuItem(PropIcon(c.Property->Id), c.Title.c_str())) picked = c.Property->Id;
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
    }
    if (choices.empty()) ImGui::TextDisabled("%s", T("This object has nothing to animate"));
    if (!picked.empty()) AddProperty(host, picked);
    ImGui::EndPopup();
}

// Кривые плитками: картинка говорит больше названия; название — при наведении.
void TweenPanel::DrawEasePicker(EditorHost& host, anim::TweenClip& clip) {
    Sage::UI::MenuScope menu;
    if (!ImGui::BeginPopup("##tweenease")) return;
    if (clip.Tracks.empty()) ImGui::CloseCurrentPopup();
    else EasePickerBody(host, clip);
    ImGui::EndPopup();
}

void TweenPanel::EasePickerBody(EditorHost& host, anim::TweenClip& clip) {
    anim::TweenTrack& t = clip.Tracks[(size_t)m_popupTrack];
    ImGui::TextUnformatted(tweenui::EaseName(t.Curve).c_str());
    const ImVec2 tile(64.0f, 46.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int cols = 4;
    int n = 0;
    for (const Preset& pr : kPresets) {
        const anim::Ease e = PresetEase(pr);
        if (n % cols) ImGui::SameLine();
        ImGui::PushID(n++);
        const bool on = t.Curve == e;
        if (ImGui::InvisibleButton("##tile", tile) && !on) {
            t.Curve = e;
            Edited(host);
        }
        const bool hot = ImGui::IsItemHovered();
        if (hot) ImGui::SetTooltip("%s", T(pr.Name));
        const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
        dl->AddRectFilled(p0, p1, Col(on ? EditorTheme::Role::AccentMuted : hot ? EditorTheme::Role::Hover
                                                                              : EditorTheme::Role::Input), 5.0f);
        if (on) dl->AddRect(p0, p1, Col(EditorTheme::Role::Accent), 5.0f, 0, 1.5f);
        PlotEase(dl, ImVec2(p0.x + 8, p0.y + 4), ImVec2(p1.x - 8, p1.y - 4), e,
                 Col(on ? EditorTheme::Role::Accent : EditorTheme::Role::Text), 1.5f, tile.y * 0.2f);
        ImGui::PopID();
    }
    ImGui::Separator();
    if (EditorIcons::BeginMenu("chart", T("Professional"))) {
        for (int s = (int)anim::EaseShape::Quad; s < (int)anim::EaseShape::Curve; ++s) {
            if (!ImGui::BeginMenu(kShapeNames[s])) continue;
            for (int m = 0; m < (int)anim::EaseMode::Count; ++m) {
                const anim::Ease e = anim::Ease::Make((anim::EaseShape)s, (anim::EaseMode)m);
                if (ImGui::MenuItem(ModeLabel((anim::EaseMode)m), nullptr, t.Curve == e) && t.Curve != e) {
                    t.Curve = e;
                    Edited(host);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (EditorIcons::MenuItem("pencil", T("Edit curve..."))) m_wantCurve = true;
}

// Своя кривая — по просьбе. Две ручки: тянешь — меняешь форму.
void TweenPanel::DrawCurveEditor(EditorHost& host, anim::TweenClip& clip) {
    Sage::UI::MenuScope menu;
    if (!ImGui::BeginPopup("##tweencurve")) return;
    if (clip.Tracks.empty()) ImGui::CloseCurrentPopup();
    else CurveEditorBody(host, clip);
    ImGui::EndPopup();
}

void TweenPanel::CurveEditorBody(EditorHost& host, anim::TweenClip& clip) {
    anim::TweenTrack& t = clip.Tracks[(size_t)m_popupTrack];
    ImGui::TextUnformatted(tweenui::EaseName(t.Curve).c_str());
    ImGui::TextDisabled("%s", T("Drag the two points to shape the curve"));

    const float side = 220.0f;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 b(a.x + side, a.y + side);
    const float pad = side * 0.2f;   // y ∈ [-0.25, 1.25] — место на перелёт
    const ImVec2 g0(a.x + 8.0f, a.y + pad), g1(b.x - 8.0f, b.y - pad);
    auto toScreen = [&](float x, float y) { return ImVec2(g0.x + x * (g1.x - g0.x), g1.y - y * (g1.y - g0.y)); };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, Col(EditorTheme::Role::Input), 5.0f);
    const ImU32 grid = Col(EditorTheme::Role::Line);
    dl->AddLine(toScreen(0, 0), toScreen(1, 0), grid);
    dl->AddLine(toScreen(0, 1), toScreen(1, 1), grid);
    dl->AddLine(toScreen(0, 0), toScreen(1, 1), grid);   // прямая — для сравнения
    PlotEase(dl, ImVec2(g0.x, g0.y), ImVec2(g1.x, g1.y), t.Curve, Col(EditorTheme::Role::Accent), 2.0f);

    // Ручки есть всегда: у готовой кривой они стоят там, где её повторяет
    // своя, и первое же движение превращает её в свою.
    const glm::vec4 c = t.Curve.Shape == anim::EaseShape::Curve ? t.Curve.Bezier : anim::BezierFor(t.Curve);
    const ImU32 handleCol = Col(EditorTheme::Role::Warn);
    for (int k = 0; k < 2; ++k) {
        const float hx = k == 0 ? c.x : c.z;
        const float hy = k == 0 ? c.y : c.w;
        const ImVec2 anchor = k == 0 ? toScreen(0, 0) : toScreen(1, 1);
        const ImVec2 p = toScreen(hx, hy);
        dl->AddLine(anchor, p, handleCol);
        ImGui::SetCursorScreenPos(ImVec2(p.x - 7, p.y - 7));
        ImGui::PushID(k);
        ImGui::InvisibleButton("##h", ImVec2(14, 14));
        ImGui::PopID();
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        dl->AddCircleFilled(p, hot ? 6.0f : 4.5f, handleCol);
        if (ImGui::IsItemActivated()) {
            StopPreview(host);
            if (t.Curve.Shape != anim::EaseShape::Curve) {
                t.Curve.Bezier = c;
                t.Curve.Shape = anim::EaseShape::Curve;
            }
        }
        if (ImGui::IsItemActive()) {
            // x держится в [0, 1] (время не идёт назад), y свободен — так
            // кривая перелетает цель.
            const ImVec2 m = ImGui::GetIO().MousePos;
            float& ox = k == 0 ? t.Curve.Bezier.x : t.Curve.Bezier.z;
            float& oy = k == 0 ? t.Curve.Bezier.y : t.Curve.Bezier.w;
            ox = std::clamp((m.x - g0.x) / (g1.x - g0.x), 0.0f, 1.0f);
            oy = std::clamp((g1.y - m.y) / (g1.y - g0.y), -1.0f, 2.0f);
        }
        if (ImGui::IsItemDeactivated()) host.PushUndoSnapshot();
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + 4.0f));
    ImGui::Dummy(ImVec2(side, 0.0f));
}

// ⋯ карточки: редкое про одно свойство.
void TweenPanel::DrawCardMenu(EditorHost& host, anim::TweenClip& clip, entt::entity target) {
    Sage::UI::MenuScope menu;
    if (!ImGui::BeginPopup("##tweencard")) return;
    if (clip.Tracks.empty()) ImGui::CloseCurrentPopup();
    else CardMenuBody(host, clip, target);
    ImGui::EndPopup();
}

void TweenPanel::CardMenuBody(EditorHost& host, anim::TweenClip& clip, entt::entity target) {
    const int i = m_popupTrack;
    anim::TweenTrack& t = clip.Tracks[(size_t)i];
    const anim::PropertyType* p = anim::FindProperty(t.Property);
    entt::registry& reg = host.CurrentScene().Registry();
    const bool has = p && target != entt::null && anim::HasProperty(*p, reg, target);
    if (p) {
        if (EditorIcons::MenuItemSelected("refresh", T("Start from the current value"), t.FromCurrent)) {
            if (!t.FromCurrent) {
                t.FromCurrent = true;
            } else {
                t.FromCurrent = false;
                if (has) t.From = Current(reg, target, *p);
            }
            Edited(host);
        }
        if (EditorIcons::MenuItem("eyedropper", T("Set the end to the current value"), nullptr, has)) {
            t.To = Current(reg, target, *p);
            Edited(host);
        }
        if (t.Property == "object.scale") {
            const bool split = m_splitAxes.count(i) != 0;
            if (EditorIcons::MenuItemSelected("scale", T("Each axis separately"), split)) {
                if (split) m_splitAxes.erase(i);
                else m_splitAxes.insert(i);
            }
        }
        ImGui::Separator();
    }
    if (EditorIcons::MenuItem("up", T("Move up"), nullptr, i > 0)) {
        std::swap(clip.Tracks[(size_t)i], clip.Tracks[(size_t)i - 1]);
        m_splitAxes.clear();
        Edited(host);
    }
    if (EditorIcons::MenuItem("align-bottom", T("Move down"), nullptr, i + 1 < (int)clip.Tracks.size())) {
        std::swap(clip.Tracks[(size_t)i], clip.Tracks[(size_t)i + 1]);
        m_splitAxes.clear();
        Edited(host);
    }
    if (EditorIcons::MenuItem("trash", T("Remove"))) {
        StopPreview(host);
        clip.Tracks.erase(clip.Tracks.begin() + i);
        m_splitAxes.clear();
        host.PushUndoSnapshot();
    }
}
