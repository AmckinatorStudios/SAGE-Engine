#pragma once
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "sage/ui/UIFramework.h"
#include "sage/ui/render/UIEngineResources.h"
#include "sage/ui/sageui/UIElement.h"

class UIRenderer;
class Framebuffer;

// ---------------------------------------------------------------------------
// UIContext — ОДНА САМОСТОЯТЕЛЬНАЯ СЦЕНА ИНТЕРФЕЙСА (§32 ТЗ).
//
// Контекстов может быть сколько угодно и они ничего не знают друг о друге:
// интерфейс игры — один, интерфейс редактора — другой, окно инструмента —
// третий. У каждого свой корень, свой фокус, свой ввод, своя тема и свой
// масштаб. Это прямое требование ТЗ и заодно то, чего в immediate-mode системе
// не бывает в принципе: там контекст один на процесс, и два интерфейса рядом
// начинают делить состояние.
//
// ЧТО КОНТЕКСТ ДЕЛАЕТ КАЖДЫЙ КАДР:
//
//     Update(dt)      обновить элементы -> посчитать раскладку
//     HandleInput()   раздать ввод (capture -> target -> bubble), фокус
//     Render()        собрать команды, склеить в батчи, отдать бэкенду
//
// Ровно три шага и всегда в этом порядке. Порядок не случайный: элемент вправе
// поменять содержимое в Update, и это обязано попасть в ТУ ЖЕ раскладку;
// ввод обязан идти по УЖЕ посчитанной раскладке, иначе кнопка ловит мышь там,
// где была в прошлом кадре.
//
// СЛОИ (§33). Всплывающее меню обязано лечь поверх содержимого, подсказка —
// поверх меню, модальное окно — поверх всего. Решать это порядком создания
// элементов нельзя: добавление панели ломало бы чужую расстановку. Поэтому у
// контекста есть готовые слои с фиксированными номерами, и элемент кладут в
// нужный, а не «повыше».
// ---------------------------------------------------------------------------
namespace sage::ui::sui {

// Слои контекста. Номера с большим шагом: между любыми двумя есть место для
// слоя, о котором сегодня ещё не знают.
enum class UILayer {
    Background = -100, // фон сцены
    Content    = 0,    // обычное содержимое
    Overlay    = 100,  // поверх содержимого, но под всплывающим
    Popup      = 200,  // меню, выпадающие списки
    Tooltip    = 300,  // подсказки
    Modal      = 400,  // модальные окна и затемнение под ними
    Debug      = 500,  // отладочный слой поверх всего
};

class UIContext {
public:
    UIContext();
    ~UIContext();

    UIContext(const UIContext&) = delete;
    UIContext& operator=(const UIContext&) = delete;

    // --- Создание элементов -------------------------------------------------
    //
    // Контекст ВЛАДЕЕТ элементами. Возвращается сырой указатель, и это
    // осознанно: время жизни элемента — это время жизни его узла в документе,
    // а не время жизни переменной у вызывающего. unique_ptr здесь означал бы,
    // что человек может забрать элемент из дерева и уронить документ.
    template <class T, class... Args>
    T* Create(Args&&... args) {
        static_assert(std::is_base_of_v<UIElement, T>, "T должен наследовать UIElement");
        auto owned = std::unique_ptr<T>(new T(std::forward<Args>(args)...));
        T* raw = owned.get();
        Attach(std::move(owned), kUIInvalidNode);
        return raw;
    }

    // То же, но сразу в заданного родителя (чаще всего — в слой).
    template <class T, class... Args>
    T* CreateIn(UIElement* parent, Args&&... args) {
        T* e = Create<T>(std::forward<Args>(args)...);
        if (parent) parent->Add(e);
        return e;
    }

    // Создать по ИМЕНИ ТИПА из реестра — так интерфейс собирается из данных
    // (.uidoc, редактор, плагин), не зная про C++-классы (§39, §40).
    UIElement* CreateByName(const std::string& typeName);

    // Удалить элемент вместе с поддеревом.
    void Destroy(UIElement* element);

    // Элемент по узлу. nullptr — у узла нет объектного лица (узел собран
    // ядром или загружен из файла); это нормально и не ошибка.
    UIElement* Find(UINodeId id) const;
    UIElement* FindByName(const std::string& name) const;

    // --- Слои ---------------------------------------------------------------
    UIElement* Layer(UILayer layer);
    UIElement* Root() { return m_root; }
    UIElement* Content() { return Layer(UILayer::Content); }

    // --- Всплывающие и подсказки ---------------------------------------------
    //
    // Закрытие щелчком мимо и по Escape ведёт КОНТЕКСТ, а не само меню. Иначе
    // каждое меню заводило бы свой обработчик на весь экран, и два открытых
    // меню начинали бы спорить, чей щелчок.
    void RegisterOpenPopup(class Popup* popup);
    void UnregisterOpenPopup(class Popup* popup);
    void CloseAllPopups();
    bool AnyPopupOpen() const { return !m_popups.empty(); }

    // Подсказки — по данным, а не по коду: узел объявляет UIInteraction::
    // TooltipKey, контекст показывает. Так подсказки работают и у документов,
    // загруженных из файла, — а не только у собранных кодом.
    void SetTooltipDelay(float seconds) { m_tooltipDelay = seconds; }

    // --- Кадр ---------------------------------------------------------------
    void SetScreen(glm::vec2 pixels);
    glm::vec2 Screen() const;
    void Update(float dt);
    UIInputReport HandleInput(const UIInputFrame& input);
    // root — цель, в которую идёт кадр (нужна композиции эффектов); nullptr —
    // буфер по умолчанию.
    void Render(UIRenderer& renderer, Framebuffer* root = nullptr);

    // --- Масштаб (§34) ------------------------------------------------------
    void SetScale(float scale);
    float Scale() const;
    void SetPixelPerfect();
    void SetReference(glm::vec2 reference, float matchWidthOrHeight = 0.5f);

    // --- Ядро ---------------------------------------------------------------
    UIRuntime& Runtime() { return m_rt; }
    UIDocument& Doc() { return m_rt.Doc(); }
    UITheme& Theme() { return m_rt.Theme(); }
    UIEventBus& Events() { return m_rt.Events(); }
    const UIProfile& Profile() const { return m_rt.Profile(); }

    // Ресурсы движка (шрифты, текстуры). Ставит тот, кто рисует; без них
    // раскладка текста считается по метрикам-заглушкам, и это видно.
    void InstallEngineResources();

private:
    friend class UIElement;

    void Attach(std::unique_ptr<UIElement> element, UINodeId parent);
    void Forget(UINodeId id);   // элемент и его дети уходят из карты

    UIRuntime m_rt;
    std::unique_ptr<UIEngineResources> m_resources;
    std::unordered_map<UINodeId, std::unique_ptr<UIElement>> m_elements;
    void UpdateTooltip(float dt);

    UIElement* m_root = nullptr;
    std::unordered_map<int, UIElement*> m_layers;
    float m_scale = 1.0f;

    std::vector<class Popup*> m_popups;
    // Подсказка одна на контекст: двух подсказок одновременно не бывает, а
    // создавать узел под каждую наведённую кнопку — мусор в дереве.
    UIElement* m_tooltip = nullptr;
    class Label* m_tooltipText = nullptr;
    UINodeId m_tooltipFor = kUIInvalidNode;
    float m_tooltipDelay = 0.6f;
    UINodeId m_hovered = kUIInvalidNode;
};

} // namespace sage::ui::sui
