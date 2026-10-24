// ---------------------------------------------------------------------------
// Инспектор — свойства сущности и её компоненты.
//
// Компоненты трёхмерного объекта: трансформ, свет, камера, физика, частицы,
// звук, скрипты. Самая длинная часть инспектора и самая механическая: на каждый
// компонент — свой блок полей.
//
// Часть класса InspectorPanel: объявления остались в InspectorPanel.h, здесь
// только тела. Разбит потому, что дорос до двух тысяч строк, в которых рядом
// лежали редактор материала, якоря интерфейса и список компонентов.
// ---------------------------------------------------------------------------
#include <cctype>
#include <cstdarg>
#include "sage/scene/CharacterFit.h"
#include "sage/scene/ScaleLock.h"
#include "EditorTheme.h"
#include "ui/UI.h"
#include "InspectorPanel.h"
#include "../SignalLinksEditor.h"
#include "../UIElementProperties.h"


#include <cmath>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "PostChainUi.h"
#include "ParticleUi.h"
#include "sage/render/ParticleEffectIO.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/core/Paths.h"
#include "VarsEditor.h"
#include "sage/core/Log.h"
#include "sage/scene/Signals.h"
#include "sage/ecs/LightSystem.h"
#include "sage/physics/PhysicsScene.h"
#include <algorithm>

#include "AssetSlot.h"
#include "ObjectSlot.h"
#include "EditorIcons.h"
#include "../EditorPrefs.h"
#include "ModelMaterialImport.h"
#include "Project.h"
#include "sage/render/ResourceManager.h"
#include "sage/assets/import/Importer.h"
#include "sage/render/ModelLoader.h"
#include "sage/render/ModelMaterial.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/anim/PropertyAnimator.h"
#include "sage/anim/Tween.h"
#include "sage/render/SkinnedModel.h"
#include "sage/scene/Components.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/UIPresets.h"
#include "../Localization.h"
#include "ui/ColorPicker.h"

#include "InspectorPanelDetail.h"

namespace fs = std::filesystem;
using namespace sage::editor::inspectordetail;

// Меню «Добавить компонент» и реестр того, что в нём есть.
// --- Реестр компонентов: ТАБЛИЦА, а не лестница if-ов ---------------------
//
// Список компонентов в меню «Добавить компонент» был написан подряд, строкой на
// каждый, и отставал от движка чаще всего: Mesh Renderer, Decal и Character
// Controller ДОЛГО в нём отсутствовали, хотя секции в инспекторе у них были.
// Получалось противоречие — наклейку можно создать через «Объект > Создать
// наклейку», но нельзя добавить к существующему объекту.
//
// Теперь это данные: строка таблицы — имя, раздел, значок, ОДНА строка о том,
// что компонент делает, и две операции. Новый компонент = новая строка. Отсюда
// же берётся поиск (компонентов два десятка, и листать их глазами дороже, чем
// набрать три буквы) и разбиение по разделам: «Свет» и «Твёрдое тело» в одном
// плоском списке стоят рядом только потому, что их так написали.
namespace {

struct ComponentEntry {
    const char* Name;
    const char* Category;
    const char* Icon;
    const char* Hint;
    bool (*Has)(entt::registry&, entt::entity);
    void (*Add)(entt::registry&, entt::entity);
};

template <typename T>
bool HasComp(entt::registry& reg, entt::entity e) {
    return reg.all_of<T>(e);
}
template <typename T>
void AddComp(entt::registry& reg, entt::entity e) {
    reg.emplace<T>(e);
}

// Контроллер персонажа — сразу по мешу объекта: подошвы на низ меша, рост и
// радиус по габаритам (sage/scene/CharacterFit.h). С размерами по умолчанию
// капсула стояла наполовину в полу.
void AddCharacter(entt::registry& reg, entt::entity e) {
    reg.emplace<CharacterControllerComponent>(e);
    sage::scene::FitCharacterToMesh(reg, e);
}

// Твин добавляется сразу с первым (пустым) твином: пустой список твинов —
// лишний щелчок «Добавить твин» на пути к результату.
void AddTween(entt::registry& reg, entt::entity e) {
    sage::anim::TweenClip c;
    c.Name = "Tween";
    reg.emplace<sage::anim::TweenComponent>(e).Tweens.push_back(c);
}

const std::vector<ComponentEntry>& ComponentRegistry() {
    static const std::vector<ComponentEntry> kEntries = {
        // ПРОСТО «Mesh»: это то, КАК ОБЪЕКТ ВЫГЛЯДИТ — модель, её материалы и
        // настройки отрисовки. Слово «Renderer» в названии описывало не предмет
        // сцены, а внутреннее устройство движка, и делило объекты на «модель» и
        // «модель с рендерером» там, где деления нет. Скелетная модель — тот же
        // Mesh: анимацию к ней добавляет отдельный компонент Animation.
        {"Mesh", "Render", "cube",
         "Model, materials and how it is drawn", HasComp<MeshRendererComponent>,
         AddComp<MeshRendererComponent>},
        // НАКЛЕЙКИ И «GI STATIC» В СПИСКЕ НЕТ — НАМЕРЕННО, и не потому, что их
        // убрали из движка: оба на месте и работают.
        //
        // Наклейка — это ПРЕДМЕТ сцены, а не свойство существующего: её ставят
        // на стену, целясь в место, а не «добавляют кубу». Отдельным
        // компонентом она к тому же приезжала сломанной: DecalComponent на
        // объекте с мешем даёт наклейку, спроецированную саму на себя. Ставится
        // она каталогом: Объект > Эффекты > Наклейка, — там она сразу встаёт
        // перед камерой и смотрит туда же, куда человек.
        //
        // «GI Static» — не решение про ОДИН объект: помечать статикой по
        // одному пятьдесят стен никто не станет, а пометив три, человек
        // получит запечённый свет, который хуже незапечённого. Для этого есть
        // кнопка «Пометить статичную геометрию» в панели Environment, рядом с
        // самой выпечкой, — она размечает всю сцену разом и по понятному
        // правилу.
        {"Particle Emitter", "Render", "particles",
         "Rain, snow, smoke, fire, magic: any effect", HasComp<ParticleEmitterComponent>,
         [](entt::registry& reg, entt::entity e) {
             reg.emplace<ParticleEmitterComponent>(e);
         }},
        {"Reflection Probe", "Render", "probe",
         "Captures the surroundings for reflections", HasComp<ReflectionProbeComponent>,
         AddComp<ReflectionProbeComponent>},

        {"Camera", "View", "camera",
         "The game looks through this object", HasComp<CameraComponent>, AddComp<CameraComponent>},
        {"Post Processing", "View", "camera",
         "What is done to this camera's frame before it is shown",
         HasComp<sage::render::PostProcessComponent>,
         AddComp<sage::render::PostProcessComponent>},
        {"Light", "View", "light",
         "Point, spot or directional light", HasComp<LightComponent>, AddComp<LightComponent>},

        {"Rigid Body", "Physics", "physics",
         "The object falls and collides", HasComp<RigidBodyComponent>, AddComp<RigidBodyComponent>},
        {"Collider", "Physics", "physics",
         "The shape physics uses for collisions", HasComp<ColliderComponent>, AddComp<ColliderComponent>},
        {"Joint", "Physics", "ik",
         "Ties this body to another one", HasComp<JointComponent>, AddComp<JointComponent>},
        {"Character Controller", "Physics", "physics",
         "Walking, jumping, stairs", HasComp<CharacterControllerComponent>,
         AddCharacter},

        // АНИМАЦИЯ, А НЕ «АНИМИРОВАННАЯ МОДЕЛЬ». Второе делило объекты сцены
        // на два сорта — «модель» и «анимированная модель», — хотя анимация
        // это характеристика предмета, а не отдельный вид предмета. Скелет
        // берётся из модели, заданной в Mesh.
        {"Animation", "Animation", "anim",
         "Plays clips on the skeleton of the Mesh model", HasComp<AnimationComponent>,
         AddComp<AnimationComponent>},
        // АНИМАЦИЯ СВОЙСТВ — ДЛЯ ТЕХ, У КОГО НЕТ СКЕЛЕТА, то есть для
        // большинства объектов сцены и для всего интерфейса. Без этой строки
        // компонент можно было завести только руками в .sage, и инструмент
        // анимации оставался единственным местом, где движение «существует», —
        // до закрытия редактора.
        {"Property Animation", "Animation", "clock",
         "Plays a .sageclip: properties of this object and its children",
         HasComp<PropertyAnimatorComponent>, AddComp<PropertyAnimatorComponent>},
        // ТВИН — «из A в B за секунду» без клипа: кнопка вздувается, дверь
        // отъезжает, свет гаснет. Добавленный сразу получает первый твин.
        {"Tween", "Animation", "clock",
         "Quick change of a property over time — no clip needed",
         HasComp<sage::anim::TweenComponent>, AddTween},
        // IK В СПИСКЕ НЕТ — НАМЕРЕННО.
        //
        // Обратная кинематика нужна в считаных случаях (стопа на склоне, взгляд
        // за целью, лекарство от скольжения ног) и требует знания имён костей.
        // В списке «Добавить компонент» она стояла наравне с мешем и звуком, то
        // есть предлагалась каждому — и добавлялась по ошибке, после чего ничем
        // себя не проявляла: без настроенных целей IK не делает НИЧЕГО.
        // Компонент, который нечем отличить от сломанного, из списка убран.
        //
        // Сам механизм на месте: секция IK в инспекторе рисуется у сущности,
        // которая его уже имеет, работает Lua-API, IK переживает сохранение и
        // префабы. Добавляется он скриптом — тем, кто знает, зачем.

        // Скрипт добавляется ПУСТЫМ. Раньше сюда подставлялся путь
        // «assets/scripts/spin.lua» — демонстрационный скрипт движка, которого
        // в проекте человека нет: компонент добавлялся уже сломанным, и при
        // первом же Play в консоль летело «Скрипт не найден». Пустой слот
        // честно говорит «скрипт не прикреплён» и ждёт, когда в него бросят
        // файл.
        // ЗВУК — ТАКОЕ ЖЕ СВОЙСТВО ПРЕДМЕТА, КАК ЕГО МЕШ.
        //
        // Звуковая подсистема в движке была полной с самого начала, а
        // дотянуться до неё можно было ровно из скрипта. То есть чтобы у двери
        // появился скрип, дверь обязана была обзавестись программистом, — и
        // поэтому звука в сценах не было вовсе.
        {"Audio", "Audio", "audio",
         "Sound of this object: ambience, hum, one-shot", HasComp<AudioSourceComponent>,
         AddComp<AudioSourceComponent>},

        {"Script", "Logic", "script",
         "Behaviour written in a script file, with its public variables", HasComp<ScriptComponent>,
         AddComp<ScriptComponent>},
        {"Net Replicated", "Logic", "network",
         "The server replicates this object to clients", HasComp<NetReplicatedComponent>,
         AddComp<NetReplicatedComponent>},
        // Интерфейс добавляется ОБЯЗАТЕЛЬНОЙ частью — прямоугольником; из чего
        // элемент состоит дальше, выбирается в самом инспекторе (или заготовкой).
        // ГРАНИЦА интерфейса — отдельным компонентом и ПЕРВЫМ в разделе:
        // элементы живут внутри интерфейса, а не сами по себе.
        {"Interface", "Interface", "layout",
         "A screen of its own: its elements live inside it and are edited apart",
         HasComp<sage::ui::InterfaceComponent>, AddComp<sage::ui::InterfaceComponent>},
        {"UI Element", "Interface", "rect",
         "Panel, label, image or bar on screen", HasComp<sage::ui::Element>,
         AddComp<sage::ui::Element>},
    };
    return kEntries;
}

} // namespace

std::vector<std::pair<std::string, std::vector<std::string>>>
InspectorPanel::AddComponentMenuContents() {
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    for (const ComponentEntry& c : ComponentRegistry()) {
        if (out.empty() || out.back().first != c.Category) out.push_back({c.Category, {}});
        out.back().second.push_back(c.Name);
    }
    return out;
}

void InspectorPanel::DrawAddComponentMenu(EditorHost& host, GameObject obj) {
    entt::registry& reg = host.CurrentScene().Registry();
    entt::entity e = obj.Entity();

    ImGui::Separator();
    if (ImGui::Button(T("Add Component"), ImVec2(-1, 0))) {
        ImGui::OpenPopup("##add_component");
        m_addComponentFilter[0] = '\0';
        m_addComponentFocus = true;
    }
    // Отступы темы для меню: всплывающее окно наследует стиль, действующий в
    // момент открытия (см. Sage::UI::MenuScope).
    Sage::UI::MenuScope addComponentMenu;
    if (!ImGui::BeginPopup("##add_component")) return;

    ImGui::SetNextItemWidth(280.0f);
    if (m_addComponentFocus) {
        ImGui::SetKeyboardFocusHere(); // курсор сразу в поиске — руки уже на клавиатуре
        m_addComponentFocus = false;
    }
    ImGui::InputTextWithHint("##filter", T("Search..."), m_addComponentFilter,
                             sizeof(m_addComponentFilter));
    ImGui::Separator();

    std::string filter = m_addComponentFilter;
    std::transform(filter.begin(), filter.end(), filter.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    auto matches = [&](const ComponentEntry& c) {
        if (filter.empty()) return true;
        std::string hay = std::string(c.Name) + " " + c.Hint + " " + c.Category;
        std::transform(hay.begin(), hay.end(), hay.begin(),
                       [](unsigned char ch) { return (char)std::tolower(ch); });
        return hay.find(filter) != std::string::npos;
    };

    // СПИСОК СВОРАЧИВАЕТСЯ ПО КАТЕГОРИЯМ.
    //
    // Категории были подписями, а не узлами: список рос с каждым новым
    // компонентом и целиком в окно уже не помещался — приходилось листать,
    // чтобы увидеть, что вообще бывает. Свёрнутые категории показывают ВЕСЬ
    // состав редактора одним экраном из восьми строк, а раскрывается та одна,
    // которая нужна. Состояние раскрытия ImGui держит сам, по имени узла, —
    // открытая категория остаётся открытой и в следующий раз.
    //
    // ПРИ ПОИСКЕ КАТЕГОРИИ РАСКРЫВАЮТСЯ ПРИНУДИТЕЛЬНО: найденное, спрятанное
    // под свёрнутым заголовком, читается как «ничего не нашлось».
    const bool searching = !filter.empty();

    // Сколько подходящих в категории — числом рядом с названием: свёрнутая
    // категория иначе не отвечает на вопрос «а есть ли там что-нибудь».
    auto countIn = [&](const char* category) {
        int n = 0;
        for (const ComponentEntry& c : ComponentRegistry())
            if (std::strcmp(c.Category, category) == 0 && !c.Has(reg, e) && matches(c)) ++n;
        return n;
    };

    const char* lastCategory = nullptr;
    bool categoryOpen = false;
    // ПОДХОДЯЩИЕ, а не НАРИСОВАННЫЕ. Считать нарисованные значило бы писать
    // «все компоненты уже добавлены» всякий раз, когда категории свёрнуты, —
    // то есть по умолчанию.
    int shown = 0;
    auto closeCategory = [&]() {
        if (categoryOpen) ImGui::TreePop();
        categoryOpen = false;
    };
    for (const ComponentEntry& c : ComponentRegistry()) {
        if (c.Has(reg, e) || !matches(c)) continue;
        ++shown;
        if (!lastCategory || std::strcmp(lastCategory, c.Category) != 0) {
            closeCategory();
            lastCategory = c.Category;
            char label[128];
            std::snprintf(label, sizeof(label), "%s (%d)###cat_%s", T(c.Category), countIn(c.Category),
                          c.Category);
            if (searching) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            categoryOpen = ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_SpanAvailWidth |
                                                        ImGuiTreeNodeFlags_FramePadding);
        }
        if (!categoryOpen) continue;
        ImGui::PushID(c.Name);
        const ImVec2 rowPos = ImGui::GetCursorScreenPos();
        const float iconSize = ImGui::GetTextLineHeight();
        // Строка — один Selectable на две строки текста: имя и что оно делает.
        // Без второй строки список компонентов читается только теми, кто и так
        // знает, что такое «GI Static».
        const bool picked = ImGui::Selectable("##row", false, ImGuiSelectableFlags_None,
                                              ImVec2(300.0f, iconSize * 2.0f + 4.0f));
        EditorIcons::Overlay(rowPos.x + 2.0f, rowPos.y + iconSize * 0.5f - 1.0f, iconSize, c.Icon,
                             glm::vec3(0.62f, 0.72f, 0.85f));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(rowPos.x + iconSize + 10.0f, rowPos.y + 1.0f),
                    ImGui::GetColorU32(ImGuiCol_Text), T(c.Name));
        dl->AddText(ImVec2(rowPos.x + iconSize + 10.0f, rowPos.y + iconSize + 2.0f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), T(c.Hint));
        if (picked) {
            host.PushUndoSnapshot();
            c.Add(reg, e);
            ImGui::CloseCurrentPopup();
            // Пустой твин ни на что не годен: сразу — выбрать свойство.
            if (c.Add == AddTween) host.OpenTween(obj.Id(), 0, EditorHost::TweenOpen::AddProperty);
        }
        ImGui::PopID();
    }
    closeCategory();
    if (shown == 0) {
        ImGui::TextDisabled("%s", filter.empty() ? T("All components already added")
                                                 : T("Nothing matches the search."));
    }
    ImGui::EndPopup();
}
