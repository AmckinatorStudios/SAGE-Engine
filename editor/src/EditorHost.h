#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "sage/scene/Scene.h"
#include "sage/render/Camera.h"
#include "sage/core/Config.h"

class Project;

// Состояние Play-режима редактора (см. EditorLayer): вынесено из класса,
// чтобы панели зависели от контракта EditorHost, а не от EditorLayer.
enum class EditorPlayState { Editing, Playing, Paused };

// Режим отображения сцены во вьюпорте (View > Render Mode / тулбар).
//   Shaded    — полное освещение (по умолчанию);
//   Wireframe — каркас (полигоны линиями), плоский цвет для читаемости;
//   Unlit     — плоский базовый цвет без освещения;
//   Normals   — визуализация нормалей цветом (отладка).
enum class EditorRenderMode { Shaded, Wireframe, Unlit, Normals };

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
    virtual bool CreateProject(const std::string& dir, const std::string& name, std::string& err) = 0;
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

    // Режим вёрстки интерфейса. Включает показ игрового UI прямо во вьюпорте и
    // редактирование его мышью: выбор, перетаскивание, ручки размера.
    //
    // Отдельный режим, а не «всегда»: 3D-гизмо и рамки UI живут в одних и тех
    // же пикселях экрана, и одновременно они спорят за клик. Верстают интерфейс
    // и расставляют объекты в разное время, поэтому дешевле переключаться.
    virtual bool& UIEditMode() = 0;

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
};
