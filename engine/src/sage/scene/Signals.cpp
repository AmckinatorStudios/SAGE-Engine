#include "sage/scene/Signals.h"

#include <algorithm>
#include <cctype>
#include <mutex>

#include "sage/physics/PhysicsComponents.h"
#include "sage/ui/components/Interact.h"

namespace sage::signals {

namespace {

struct Declaration {
    std::string Source;
    HasComponent Has;
    std::vector<SignalInfo> Signals;
};

std::vector<Declaration>& Registry() {
    static std::vector<Declaration> list;
    return list;
}

std::mutex& Guard() {
    static std::mutex m;
    return m;
}

void DeclareUnlocked(const std::string& source, HasComponent has, std::vector<SignalInfo> signals) {
    for (SignalInfo& s : signals) s.Source = source;
    for (Declaration& d : Registry()) {
        if (d.Source != source) continue;
        d.Has = std::move(has);
        d.Signals = std::move(signals);
        return;
    }
    Registry().push_back({source, std::move(has), std::move(signals)});
}

// Встроенные компоненты объявляют свои сигналы здесь, один раз, при первом
// вопросе. Не статическими объектами в файлах компонентов: порядок их
// создания между единицами трансляции не определён, и список мог бы
// оказаться пустым ровно в тот момент, когда его впервые спросят.
void EnsureBuiltins() {
    static bool done = false;
    if (done) return;
    done = true;

    DeclareUnlocked(
        "Button",
        [](const entt::registry& r, entt::entity e) { return r.all_of<sage::ui::Interactable>(e); },
        {{kClicked, "", "Pressed and released over the element"},
         {kPressed, "", "Mouse button went down over the element"},
         {kReleased, "", "Mouse button went up over the element"},
         {kHovered, "", "The cursor entered the element"},
         {kUnhovered, "", "The cursor left the element"}});
    DeclareUnlocked(
        "Value",
        [](const entt::registry& r, entt::entity e) { return r.all_of<sage::ui::Range>(e); },
        {{kValueChanged, "", "data: the new value (number, or true/false for a checkbox)"}});
    DeclareUnlocked(
        "Text Input",
        [](const entt::registry& r, entt::entity e) { return r.all_of<sage::ui::TextInput>(e); },
        {{kTextChanged, "", "data: the new text"}});
    DeclareUnlocked(
        "Physics",
        [](const entt::registry& r, entt::entity e) {
            return r.any_of<RigidBodyComponent, ColliderComponent, CharacterControllerComponent>(e);
        },
        {{kCollision, "", "data: the other object"},
         {kCollisionEnded, "", "data: the other object"},
         {kTriggerEntered, "", "data: the other object"},
         {kTriggerExited, "", "data: the other object"}});
}

} // namespace

void Declare(const std::string& source, HasComponent has, std::vector<SignalInfo> signals) {
    std::lock_guard<std::mutex> lk(Guard());
    EnsureBuiltins();
    DeclareUnlocked(source, std::move(has), std::move(signals));
}

std::vector<SignalInfo> Of(const entt::registry& reg, entt::entity e) {
    std::vector<SignalInfo> out;
    if (!reg.valid(e)) return out;
    std::lock_guard<std::mutex> lk(Guard());
    EnsureBuiltins();
    for (const Declaration& d : Registry()) {
        if (!d.Has || !d.Has(reg, e)) continue;
        for (const SignalInfo& s : d.Signals) {
            const bool seen = std::any_of(out.begin(), out.end(),
                                          [&](const SignalInfo& o) { return o.Name == s.Name; });
            if (!seen) out.push_back(s);
        }
    }
    return out;
}

bool IsDeclared(const entt::registry& reg, entt::entity e, const std::string& name) {
    for (const SignalInfo& s : Of(reg, e))
        if (s.Name == name) return true;
    return false;
}

std::string Title(const std::string& signal) {
    // Причастие в заголовке читается хуже глагола: «On Clicked» — так не
    // говорят; инспектор показывает «On Click», скрипт пишет `clicked`.
    static const std::pair<const char*, const char*> kKnown[] = {
        {kClicked, "On Click"},           {kPressed, "On Press"},
        {kReleased, "On Release"},        {kHovered, "On Hover"},
        {kUnhovered, "On Unhover"},       {kValueChanged, "On Value Changed"},
        {kTextChanged, "On Text Changed"}, {kCollision, "On Collision"},
        {kCollisionEnded, "On Collision End"}, {kTriggerEntered, "On Trigger Enter"},
        {kTriggerExited, "On Trigger Exit"},
    };
    for (const auto& k : kKnown)
        if (signal == k.first) return k.second;
    std::string out = "On ";
    bool up = true;
    for (char c : signal) {
        if (c == '_' || c == '.' || c == '-') {
            out += ' ';
            up = true;
            continue;
        }
        out += up ? (char)std::toupper((unsigned char)c) : c;
        up = false;
    }
    return out;
}

bool IsValidName(const std::string& signal) {
    if (signal.empty()) return false;
    for (unsigned char c : signal)
        if (std::isspace(c) || c < 32) return false;
    return true;
}

std::string FromLegacyTrigger(const std::string& trigger) {
    if (trigger == "click" || trigger.empty()) return kClicked;
    if (trigger == "press") return kPressed;
    if (trigger == "release") return kReleased;
    if (trigger == "hoverIn") return kHovered;
    if (trigger == "hoverOut") return kUnhovered;
    if (trigger == "change") return kValueChanged;
    return trigger;
}

} // namespace sage::signals
