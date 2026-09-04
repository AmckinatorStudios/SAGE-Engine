#pragma once
#include "UIToolSettings.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "sage/scene/Scene.h"
#include "sage/render/Camera.h"
#include "sage/core/Config.h"

// Проект — класс ДВИЖКА (sage::project::Project), см. editor/src/Project.h.
// Поэтому не предобъявление, а включение: у псевдонима типа предобъявления
// не бывает, а «class Project;» рядом с ним — уже другой, несуществующий тип.
#include "Project.h"

// Состояние Play-режима редактора (см. EditorLayer): вынесено из класса,
// чтобы панели зависели от контракта EditorHost, а не от EditorLayer.
enum class EditorPlayState { Editing, Playing, Paused };

// Режим отображения сцены во вьюпорте (View > Render Mode / тулбар).
//   Shaded    — полное освещение (по умолчанию);
//   Wireframe — каркас (полигоны линиями), плоский цвет для читаемости;
//   дальше    — отладочные виды из sage/render/DebugView.h: вместо результата
//               освещения показывается одно его слагаемое.
//
// Порядок первых двух зафиксирован (на них завязан каркасный режим), остальные
// идут ровно как в DebugView и переводятся в него сдвигом — держать два разных
// порядка значило бы однажды показать шероховатость под именем нормалей.
enum class EditorRenderMode {
    Shaded,
    Wireframe,
    Unlit,
    Normals,
    Albedo,
    Roughness,
    Metallic,
    Emissive,
    AmbientOcclusion,
    Shadow,
    Depth,
    Cascades,
    WorldGrid,
    Count
};

// Панели редактора, у которых есть кнопка быстрого доступа в верхней панели и
// пункт в меню Window. Перечислением, а не строкой: опечатка в имени панели
// должна быть ошибкой компиляции, а не тихо не работающей кнопкой.
enum class EditorPanel {
    Hierarchy,
    Inspector,
    Environment,   // окно среды сцены (небо, воздух, окружающий свет)
    Assets,
    Console,
    Code,
    Profiler,
    Game,
    Viewport,
    UIEditor,      // редактор интерфейса (холст игрового кадра + элементы)
    Settings,      // окно настроек движка (качество и цена кадра)
    Count
};

// Пространство манипулятора гизмо: Local — оси объекта, World — оси мира.
enum class EditorGizmoSpace { Local, World };

// ---------------------------------------------------------------------------
// EditorHost — контракт операций редактора, доступных панелям.
//
// Архитектура редактора v3: каждая панель (panels/*.h) — самостоятельный
// класс, владеющий СВОИМ UI-состоянием (поиск, буферы ввода, цели модалок) и
// зависящий только от этого интерфейса. EditorLayer реализует хост и
// оркестрирует панели. Это даёт:
//   • независимую разработку/тестирование панелей (хост подменяем);
//   • у EditorLayer остаётся ядро: сцена/проект/Play/undo/рендер/диалоги;
//   • новая панель = новый файл в panels/ + вызов Draw() в кадре, без
//     врастания в god-object.
// ---------------------------------------------------------------------------
class EditorHost {
public:
    virtual ~EditorHost() = default;

    // --- сцена и выбор ---
    virtual Scene& CurrentScene() = 0;
    // Выбор — МНОЖЕСТВЕННЫЙ. SelectedId() — «первичная» (последняя кликнутая)
    // сущность: под неё Inspector и пивот гизмо. Selection() — весь набор
    // (включает первичную); гизмо двигает все, Delete/Duplicate — по всем.
    virtual int SelectedId() const = 0;
    virtual void SetSelectedId(int id) = 0;               // одиночный выбор (набор = {id})
    virtual GameObject SelectedObject() = 0;              // первичная; invalid, если пусто
    virtual const std::vector<int>& Selection() const = 0; // весь набор выбранных id
    virtual bool IsSelected(int id) const = 0;
    virtual void ToggleSelection(int id) = 0;             // Ctrl-клик: добавить/убрать из набора

    // --- префабы (переиспользуемые сущности-поддеревья) ---
    // Сохраняет сущность (с детьми) в .sageprefab; false + err при ошибке.
    virtual bool SaveSelectedAsPrefab(const std::filesystem::path& path, std::string& err) = 0;
    // Инстанцирует префаб из файла в сцену (новые id), выделяет корень. Возвращает
    // id корня инстанса или -1 при ошибке.
    virtual int InstantiatePrefab(const std::filesystem::path& path) = 0;

    // --- проект и файлы сцен ---
    virtual Project& CurrentProject() = 0;
    virtual bool LoadSceneFromFile(const std::filesystem::path& path) = 0;
    virtual bool SaveSceneToFile(const std::filesystem::path& path) = 0;
    // Создать проект по ШАБЛОНУ (идентификатор из ProjectTemplates.h).
    // Шаблон приходит строкой, а не флагом: вариантов больше двух, и список
    // читают три места сразу (диалог, стартовое окно, самопроверка).
    virtual bool CreateProject(const std::string& dir, const std::string& name,
                               const std::string& templateId, std::string& err) = 0;
    virtual bool OpenProject(const std::string& path, std::string& err) = 0;
    // Упаковывает открытый проект в готовую к запуску игру (SagePlayer + ассеты).
    // false + err при ошибке. Нужен панели диалогов (Build Game...).
    virtual bool BuildGame(const std::filesystem::path& outputDir, std::string& err) = 0;
    virtual std::filesystem::path& AssetsCwd() = 0; // текущая папка панели Assets

    // Гибкая конфигурация игры (EngineConfig) — редактируется панелью Settings,
    // сохраняется в <проект>/sage.cfg, Build Game кладёт её в собранную игру.
    virtual sage::EngineConfig& Settings() = 0;
    // Короткое сообщение в статус-баре (обратная связь панелей/плагинов).
    virtual void SetStatusMessage(const std::string& message) = 0;

    // --- undo/redo ---
    // Подмешать в публичные переменные объекта то, что объявил его скрипт
    // (см. sage/vars/ScriptVars.h). Зовётся инспектором ПЕРЕД показом секции:
    // .lua правят и снаружи редактора, и переменная, добавленная в скрипт
    // минуту назад, обязана появиться сама, а не после переназначения файла.
    //
    // В хосте, а не в панели: путь скрипта разрешается корнем ПРОЕКТА, и знание
    // о том, где лежат ассеты, панели инспектора не принадлежит.
    // Заметка только что созданного проекта — «что делать дальше», если это не
    // видно само. Пусто — заметки нет.
    //
    // Живёт у хоста, а не в панели: заметку рождает создание проекта (из
    // стартового окна ИЛИ из диалога), а показывает вьюпорт, и связать их
    // иначе нечем. Гасится закрытием и запуском игры — она отвечает ровно на
    // один вопрос и не должна висеть дальше.
    virtual const std::string& TemplateNote() const = 0;

    // Что с рендером не так — одной строкой, для показа В КАДРЕ. Пусто — всё в
    // порядке. Человек, у которого чёрный вьюпорт, смотрит во вьюпорт, а не в
    // консоль: «не понятно из-за чего» начинается ровно с молчания на экране.
    virtual const std::string& RenderWarning() const = 0;
    virtual void ClearTemplateNote() = 0;

    virtual void MergeScriptVars(GameObject object) = 0;

    virtual void PushUndoSnapshot() = 0;
    // Трекинг «размазанных» правок (перетаскивание DragFloat, набор текста):
    // Capture — запомнить состояние «до» (на активации виджета/наведении
    // гизмо), Commit — положить запомненное в undo-стек (на факте изменения).
    virtual void CapturePendingSnapshot() = 0;
    virtual void CommitPendingSnapshot() = 0;
    // Обёртка над Capture/Commit для только что нарисованного ImGui-виджета.
    virtual void TrackLastImGuiItem() = 0;

    // --- сущности ---
    virtual GameObject CreateCubeEntity(const std::string& name) = 0;
    virtual void DuplicateSelected() = 0;
    virtual void DeleteSelected() = 0;

    // --- Play-режим ---
    virtual EditorPlayState GetPlayState() const = 0;
    virtual bool InPlayMode() const = 0;
    virtual void StartPlay() = 0;
    virtual void PausePlay() = 0;
    virtual void ResumePlay() = 0;
    virtual void StopPlay() = 0;

    // --- общее состояние инструментов (делят тулбар и вьюпорт) ---
    virtual int& GizmoOp() = 0;              // значение ImGuizmo::OPERATION
    virtual bool& GizmoSnap() = 0;
    virtual EditorGizmoSpace& GizmoSpace() = 0;
    virtual bool& ShowGrid() = 0;
    virtual EditorRenderMode& RenderMode() = 0;

    // Шаг привязки — СВОЙ для переноса, поворота и масштаба.
    //
    // Раньше это были три константы, зашитые в панель вьюпорта: 0.5 / 15° / 0.1.
    // Для игры про постройку из блоков шаг переноса — главный инструмент
    // выравнивания, и он обязан совпадать с размером блока: при шаге 0.5 и блоке
    // в 1 единицу половина построек встаёт со сдвигом на полблока. Меняется
    // прямо в тулбаре; живёт до перезапуска редактора (своего файла настроек у
    // редактора пока нет).
    virtual float& SnapMove() = 0;
    virtual float& SnapRotate() = 0;   // градусы
    virtual float& SnapScale() = 0;
    // Шаг для текущего режима гизмо — то, что реально уходит в ImGuizmo.
    virtual float SnapStepForCurrentOp() = 0;

    // Показывать габаритную коробку выделенного. Это ровно та коробка, по
    // которой считается попадание мышью, — увидеть её полезно именно тогда,
    // когда «клик не туда» непонятен.
    virtual bool& ShowBounds() = 0;

    // Настройки инструментов вёрстки: сетка, привязки, подписи расстояний.
    // Одни на редактор — их читает редактор интерфейса и операции над
    // выделением (см. UIToolSettings.h).
    //
    // РЕЖИМА ВЁРСТКИ ЗДЕСЬ БОЛЬШЕ НЕТ. Раньше рядом стоял UIEditMode(): он
    // превращал ВЬЮПОРТ в холст интерфейса, отбирая у него клик, гизмо и полёт
    // камеры. Это и был главный дефект прежней вёрстки — она жила не в своём
    // окне, а поверх чужого, поэтому её приходилось включать и выключать, а
    // включённой она мешала работать со сценой. Теперь интерфейс верстается в
    // своём окне (панель «Интерфейс»), где показан игровой кадр в его
    // собственном разрешении, и никакого режима для этого не нужно.
    virtual UIToolSettings& UITools() = 0;

    // Создать элемент интерфейса по имени заготовки (sage::ui::PresetNames).
    // Новый элемент становится дочерним к выделенному элементу — интерфейс
    // собирается из вложенных прямоугольников, и это самый частый шаг.
    virtual GameObject CreateUIEntity(const std::string& preset) = 0;

    // Режим правки КОЛЛАЙДЕРА: гизмо масштаба тянет форму столкновения, а не
    // сам объект. Отдельным режимом, а не ещё одной операцией гизмо: масштаб
    // объекта и размер его коллайдера — разные величины, и путать их нельзя.
    // Размеры коллайдера правились только числами в инспекторе — то есть
    // подбирались вслепую, сверяясь с зелёным каркасом во вьюпорте.
    virtual bool& ColliderEditMode() = 0;

    // --- инструменты над выделением ---------------------------------------
    //
    // Всё это делалось руками через поля Transform: подвести камеру к объекту,
    // посадить объект на поверхность, выстроить несколько объектов по одной
    // линии. Каждая операция — арифметика в уме и несколько попыток.

    // Подвести камеру так, чтобы выделенное поместилось в кадр (клавиша F).
    // Без выделения — ничего не делает.
    virtual void FocusSelected() = 0;

    // Опустить выделенное на первую поверхность под ним (клавиша End).
    // Объект встаёт НИЗОМ своей габаритной коробки на точку попадания, а не
    // центром: иначе куб наполовину утонул бы в полу.
    virtual void DropSelectedToSurface() = 0;

    // Выровнять выделенные по оси axis (0=X, 1=Y, 2=Z) по первичной сущности.
    // Для одного выделенного смысла не имеет и ничего не делает.
    virtual void AlignSelection(int axis) = 0;

    // Габаритная коробка выделенного в МИРОВЫХ координатах. false — выделения
    // нет или у него нет геометрии (пустышка, свет).
    virtual bool SelectionBounds(glm::vec3& outMin, glm::vec3& outMax) = 0;

    // --- вьюпорт/камера ---
    virtual Camera& EditorCamera() = 0;
    virtual const glm::mat4& ViewMatrix() const = 0;
    virtual const glm::mat4& ProjMatrix() const = 0;
    // Сырое число бэкенда для ImGui::Image — единственная легальная дверь
    // наружу (см. rhi::Texture2D::NativeHandle). Панель показывает кадр сцены
    // сторонним API, и это бэкенд-специфично по своей природе.
    virtual uint64_t SceneTexture() const = 0;
    virtual void SetViewportSize(int w, int h) = 0;    // панель сообщает размер под FBO

    // --- Мультивьюпорт ------------------------------------------------------
    //
    // Панель раскладывает виды и говорит хосту, что и какого размера рисовать;
    // сам рендер идёт в начале СЛЕДУЮЩЕГО кадра — там, где живёт графический
    // контекст. Кадр задержки здесь был всегда (SetViewportSize работает так же
    // с самого начала), и на глаз он незаметен: раскладку меняют редко.
    struct ViewRequest {
        bool Active = false;
        int W = 0, H = 0;
        bool Ortho = false;        // ортогональный вид (сверху/спереди/сбоку)
        glm::mat4 View{1.0f};
        glm::mat4 Proj{1.0f};
        glm::vec3 EyePos{0.0f};
    };
    static constexpr int kMaxViews = 4;
    virtual void SetViewRequests(const ViewRequest* requests, int count) = 0;
    virtual uint64_t ViewTexture(int slot) const = 0;

    // Открыть файл во встроенном редакторе кода (.lua, .vert, .frag).
    // Двойной клик по скрипту в Assets должен ОТКРЫВАТЬ его, а не молчать: до
    // этого редактор умел скрипты только запускать, и править их приходилось во
    // внешнем редакторе — то есть вся ценность горячей перезагрузки упиралась в
    // переключение окон.
    virtual void OpenCodeFile(const std::filesystem::path& path) = 0;
    // u,v в [0..1] — выбор сущности лучом. additive (Ctrl) — добавить/убрать из набора.
    virtual void PickAtViewport(float u, float v, bool additive = false) = 0;
    // То же, но ЯВНЫМИ матрицами вида и проекции. Нужно, когда активен не
    // главный слот раскладки или он показывает ортогональный вид: у панели
    // матрицы свои, а PickAtViewport берёт всегда матрицы главного слота.
    // Ассет, брошенный МЫШЬЮ во вьюпорт (из панели Assets или из проводника).
    //
    // Это отдельная операция, а не «выбрать файл и нажать кнопку»: смысл
    // перетаскивания именно в том, ГДЕ отпустили. Модель и префаб встают на
    // поверхность под курсором, материал назначается тому объекту, на который
    // его уронили. Возвращает false, если ронять было нечего (тип файла не
    // ставится в сцену) — панель по этому ответу решает, показывать ли отказ.
    virtual bool DropAssetAtViewport(const glm::mat4& view, const glm::mat4& proj, float u,
                                     float v, const std::filesystem::path& asset) = 0;

    // Ассет, бро́шенный НА СУЩНОСТЬ (иерархия): материал красит её, скрипт
    // вешается на неё, модель заменяет её меш. Тип файла и решает, что значит
    // «применить», — спрашивать об этом человека нечестно: он уже показал
    // мышью и что, и куда.
    virtual bool ApplyAssetToEntity(int entityId, const std::filesystem::path& asset) = 0;

    // Ассет, бро́шенный в СПИСОК сцены: модель или префаб становятся новым
    // объектом в начале координат. Точки под курсором в списке нет, поэтому
    // место то же, что у создания через меню Entity.
    virtual bool AddAssetToScene(const std::filesystem::path& asset) = 0;

    virtual void PickAtViewportWith(const glm::mat4& view, const glm::mat4& proj, float u, float v,
                                    bool additive) = 0;

    // --- панель Game (игровое окно: рендер от Primary-камеры сцены) ---
    virtual uint64_t GameTexture() const = 0;
    virtual void SetGameViewportSize(int w, int h) = 0;
    virtual bool HasPrimaryCamera() = 0;

    // --- выбор в Assets (нужен Inspector'у для назначения материала) ---
    virtual const std::filesystem::path& SelectedAssetPath() const = 0;

    // Показать файл в панели Assets: перейти в его папку, выделить его и
    // открыть панель, если её закрыли. Слот ассета отвечает этим на вопрос «а
    // где он лежит?» — иначе на него отвечали поиском по дереву проекта,
    // держа имя из слота в голове.
    virtual void ShowAssetInPanel(const std::filesystem::path& path) = 0;

    // Открыть окно настроек движка. Нужно панелям, у которых настройка лежит
    // «через дорогу»: объёмный свет ищут в разделе тумана, а живёт он в
    // настройках движка, потому что это цена кадра, а не свойство сцены.
    virtual void ShowSettingsWindow() = 0;

    // --- Видимость панелей одной ручкой -------------------------------------
    //
    // Понадобилась верхней панели быстрого доступа: она включает и выключает
    // окна кнопками, и делать это через десяток отдельных методов значило бы
    // дописывать в интерфейс по методу на каждую новую панель. Флаг у панели
    // всё равно один и тот же — тот, что рисует крестик на её вкладке.
    virtual bool& PanelVisible(EditorPanel panel) = 0;

    // Что открыто сейчас: имя текущей сцены (файл, а если она ещё не сохранена
    // — имя из самой сцены) и признак несохранённых правок. Раньше это знала
    // только строка состояния внизу; верхней панели оно нужно ровно затем же —
    // подписать кнопку Play тем, что именно она запустит.
    virtual std::string CurrentSceneName() const = 0;
    virtual bool SceneDirty() const = 0;
};
