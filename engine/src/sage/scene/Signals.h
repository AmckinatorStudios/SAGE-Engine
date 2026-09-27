#pragma once
#include <functional>
#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "sage/vars/Value.h"

// ---------------------------------------------------------------------------
// СИГНАЛЫ ОБЪЕКТА И СВЯЗИ ИЗ ИНСПЕКТОРА.
//
// У объекта SAGE три стороны: свойства, методы и СИГНАЛЫ — «со мной что-то
// случилось» (clicked, hovered, value_changed, collision, player_died). Сигнал
// ничего не делает сам: что последует за нажатием кнопки, решает тот, кто
// подписался, — скрипт или связь, настроенная в инспекторе. Движок не знает ни
// про «начать игру», ни про «открыть дверь».
//
// ОБЪЯВЛЕНИЕ. Какие сигналы у объекта ЕСТЬ, говорят его компоненты: кнопка
// объявляет clicked/pressed/released/hovered/unhovered, ползунок —
// value_changed, тело физики — collision и trigger_*. Отсюда инспектор берёт
// список «+ Add Event», а скрипт — `button.clicked`. Своё событие (любое имя)
// объявлять не обязательно: `obj:emit("player_died")` работает без реестра —
// реестр нужен, чтобы ВСТРОЕННЫЕ сигналы не приходилось вспоминать по буквам.
//
// Новый компонент объявляет свои сигналы одной строкой Declare(...) — движок
// для этого не правится.
//
// СВЯЗИ. «Когда у этого объекта clicked — позвать Menu.start_game()». Это
// данные сцены (SignalLinksComponent) и НЕ отдельный механизм: при запуске игры
// каждая связь становится обычным соединением шины (Bus::Connect) — тем же
// самым, что заводит `button.clicked:connect(...)` из скрипта. Инспектор —
// только оболочка над тем же бэкендом.
// ---------------------------------------------------------------------------
namespace sage::signals {

// Встроенные имена — одной строкой на всех: отправитель (UI, физика),
// подсказка скриптов и инспектор обязаны говорить одинаково.
inline constexpr const char* kClicked = "clicked";
inline constexpr const char* kPressed = "pressed";
inline constexpr const char* kReleased = "released";
inline constexpr const char* kHovered = "hovered";
inline constexpr const char* kUnhovered = "unhovered";
inline constexpr const char* kValueChanged = "value_changed";
inline constexpr const char* kTextChanged = "text_changed";
inline constexpr const char* kCollision = "collision";
inline constexpr const char* kCollisionEnded = "collision_ended";
inline constexpr const char* kTriggerEntered = "trigger_entered";
inline constexpr const char* kTriggerExited = "trigger_exited";

struct SignalInfo {
    std::string Name;     // как в скрипте: "clicked"
    std::string Source;   // кто объявил: "Button", "Rigid Body", "Script"
    std::string Help;     // что приходит в data — по-английски, переводит редактор
};

using HasComponent = std::function<bool(const entt::registry&, entt::entity)>;

// Объявить сигналы компонента. Повторное объявление того же Source
// ЗАМЕНЯЕТ прежнее (горячая перезагрузка модуля не должна удваивать список).
void Declare(const std::string& source, HasComponent has, std::vector<SignalInfo> signals);

// Сигналы, объявленные компонентами этого объекта (без повторов, в порядке
// объявления).
std::vector<SignalInfo> Of(const entt::registry& reg, entt::entity e);
bool IsDeclared(const entt::registry& reg, entt::entity e, const std::string& name);

// Заголовок в инспекторе: "clicked" -> "On Click", "player_died" -> "On Player Died".
// По-английски: строки интерфейса переводит редактор (T()).
std::string Title(const std::string& signal);

// Имя допустимо? Непустое, без пробелов и управляющих символов: имя сигнала —
// идентификатор (`button.clicked`), а «clicked » с пробелом — ошибка, которая
// молча не срабатывает.
bool IsValidName(const std::string& signal);

// --- Связь из инспектора -------------------------------------------------
//
// «Когда здесь Signal — позвать Target.Method(event)». Выбирают только цель и
// метод: всё, что сверх этого (аргументы, условия, цепочки), — логика, и её
// место в скрипте, а не в строке инспектора.
struct Link {
    std::string Signal;
    sage::vars::EntityRef Target;
    std::string Method;
    bool Enabled = true;
    // ТОЛЬКО ИЗ СТАРЫХ СЦЕН: связь прежнего формата, которая не звала метод, а
    // слала глобальное событие по имени. Её не выбросить при открытии (кнопка
    // перестала бы работать), и новой такой не завести — это переходник.
    std::string Broadcast;
};

struct SignalLinksComponent {
    std::vector<Link> Links;
};

// Перевод старых триггеров связей (до сигналов) в имена сигналов:
// "click" -> "clicked", "hoverIn" -> "hovered" и т.д. Неизвестное — как есть.
std::string FromLegacyTrigger(const std::string& trigger);

} // namespace sage::signals
