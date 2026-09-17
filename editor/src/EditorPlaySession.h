#pragma once
#include <filesystem>
#include <memory>
#include <string>

#include "EditorTypes.h"    // EditorPlayState
#include "EditorPlayInput.h"

#include "sage/audio/AudioEngine.h"
#include "sage/core/SystemScheduler.h"
#include "sage/input/GlfwBridge.h"
#include "sage/input/InputSystem.h"
#include "sage/physics/PhysicsScene.h"
#include "sage/scripting/ScriptEngine.h"

class Scene;
class ParticleSystem;

// ---------------------------------------------------------------------------
// EditorPlaySession — ИГРА, ЗАПУЩЕННАЯ ВНУТРИ РЕДАКТОРА.
//
// Play/Stop и всё, что между ними: снимок сцены на входе, скрипты, физика,
// звук, ввод, захват курсора, пауза и шаг — и возврат сцены ровно в то
// состояние, в котором её оставил человек.
//
// ПОЧЕМУ ОТДЕЛЬНО. Это единственное место редактора, где сцена живёт не как
// документ: она запускается и её меняют скрипты. Смешивать это с
// редактированием опасно — именно на границе «играем/правим» теряется работа.
// Полтора десятка полей этого режима лежали вперемешку с проектом, панелями,
// автосохранением и плагинами в EditorLayer, а порядок их гашения в Stop —
// шина событий уходит раньше сцены, звук глушится до замены компонентов,
// курсор возвращается человеку первым — держался комментариями посреди
// пятитысячестрочного класса. Каждый из этих порядков однажды был нарушен, и
// каждый раз падало не там, где ошибка.
//
// ЧТО СЕССИЯ ЗНАЕТ О РЕДАКТОРЕ. Ничего сверх PlayContext — короткого списка
// ссылок, которые ей передают на Start/Stop. Ни панелей, ни проекта, ни
// рендера: фокус окон, заметки шаблона и прочее — забота того, кто сессию
// держит.
// ---------------------------------------------------------------------------

// Что сессии нужно от редактора, чтобы запуститься и остановиться.
struct PlayContext {
    Scene** ScenePtr = nullptr;            // сцену заменяет Stop — отсюда двойной указатель
    sage::SystemScheduler* Systems = nullptr;
    ParticleSystem* Particles = nullptr;
    std::filesystem::path ProjectDir;      // корень проекта: модули Lua и пути скриптов

    // Восстановить сцену из снимка. Знание о сериализаторе и переносе
    // запечённого GI принадлежит редактору, не сессии.
    std::function<bool(const std::string&)> RestoreScene;
    // Раскладка управления ПРОЕКТА поверх объявленной скриптами — зовётся
    // после привязки скриптов (порядок важен, см. Start).
    std::function<void()> ApplyProjectInputMapping;
};

// Что сессии нужно от панели Game, чтобы отдать интерфейсу сцены мышь и текст.
struct PlayUiInput {
    bool MouseInside = false;
    bool MouseDown = false;
    bool Focused = false;
    float MouseX = 0.0f, MouseY = 0.0f;
    std::string TypedText;
    int GameWidth = 0, GameHeight = 0;
    float DeltaTime = 0.0f;

    // Клавиши редактирования текста — из ImGui: он уже слушает окно, и второй
    // обработчик на те же клавиши спорил бы с ним за автоповтор.
    bool Backspace = false, Delete = false, Left = false, Right = false;
    bool Home = false, End = false, Enter = false, Escape = false, Tab = false;
};

class EditorPlaySession {
public:
    // Ввод и мост к окну живут ВСЁ время работы редактора, а не от Play к Play:
    // подписка на события окна снимается только вместе с окном, и второй мост
    // означал бы два комплекта событий на одно нажатие.
    void Attach(sage::input::GlfwBridge& bridge, class Window& window);

    EditorPlayState State() const { return m_state; }
    bool Active() const { return m_state != EditorPlayState::Editing; }
    bool Playing() const { return m_state == EditorPlayState::Playing; }
    bool Paused() const { return m_state == EditorPlayState::Paused; }

    // Возвращает число привязанных скриптов (для лога вызывающего) или -1,
    // если сессия уже шла.
    int Start(const PlayContext& ctx);
    void Pause();
    void Resume();
    // Заказывает ОДИН кадр фиксированной длительности. Заказ, а не прогон:
    // кнопку жмут посреди рисования интерфейса, а кадр игры обязан считаться
    // там же, где считается всегда, — иначе системы пошли бы дважды за кадр.
    void RequestStep();
    // Забирает заказанный шаг (0 — шага нет). Забирает, а не подсматривает:
    // один заказ обязан дать ровно один кадр.
    float TakePendingStep();
    void Stop(const PlayContext& ctx);

    // Интерфейс сцены получает мышь и текст панели Game; что он съел, того игра
    // не получит — ровно как в собранной игре.
    void UpdateUiInput(Scene& scene, const PlayUiInput& in);

    // --- Доступ к живым подсистемам (nullptr вне Play) ---
    ScriptEngine* Scripts() { return m_scripts.get(); }
    PhysicsScene* Physics() { return m_physics.get(); }

    // Звук — единственная подсистема, которая ПЕРЕЖИВАЕТ Stop: устройство и
    // стадия звука принадлежат режиму правки так же, как частицы и анимация, —
    // ими работает кнопка «Послушать» в инспекторе и проигрыватель файлов.
    // Пока звук гасили вместе с игрой, превью работало ровно до первого Play и
    // после него молчало навсегда, а причина не попадала ни в один лог.
    AudioEngine& Audio();
    AudioEngine* AudioIfCreated() { return m_audio.get(); }

    sage::input::InputSystem& Input() { return m_input; }
    const sage::input::InputSystem& Input() const { return m_input; }
    sage::input::GlfwBridge& Bridge();
    EditorPlayInput& Cursor() { return m_cursor; }

private:
    EditorPlayState m_state = EditorPlayState::Editing;
    std::string m_snapshot;   // сцена на момент Play — восстанавливается по Stop

    std::unique_ptr<ScriptEngine> m_scripts;  // живут только в Play
    std::unique_ptr<PhysicsScene> m_physics;
    std::unique_ptr<AudioEngine> m_audio;     // переживает Stop (см. Audio())

    sage::input::InputSystem m_input;
    sage::input::GlfwBridge* m_bridge = nullptr;
    EditorPlayInput m_cursor;

    float m_pendingStep = 0.0f;
    // Левая кнопка на прошлом кадре: из «удерживается» и «удерживалась»
    // получаются «нажата» и «отпущена», а без них щелчка не существует.
    bool m_uiMouseWasDown = false;
};
