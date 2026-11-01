#include "sage/scripting/ScriptApi.h"

#include <algorithm>
#include <exception>
#include <mutex>

namespace sage::scripting {

namespace {
thread_local Scene* t_scene = nullptr;

std::mutex& PortableMutex() {
    static std::mutex m;
    return m;
}
std::vector<PortableFunction>& PortableList() {
    static std::vector<PortableFunction> list;
    return list;
}
} // namespace

Scene* CurrentApiScene() { return t_scene; }
void SetCurrentApiScene(Scene* scene) { t_scene = scene; }

void ApiBindings::Set(const std::string& name, ApiInvoker invoker, const std::vector<std::string>& aliases) {
    auto shared = std::make_shared<ApiInvoker>(std::move(invoker));
    if (!m_calls.count(name)) m_primary.push_back(name);
    m_calls[name] = shared;
    // Псевдоним не перебивает чужое полное имя: каталог уже сообщил о
    // конфликте, а вызов по полному имени обязан вести туда, куда обещано.
    for (const std::string& a : aliases)
        if (!m_calls.count(a)) m_calls[a] = shared;
}

bool ApiBindings::Call(const std::string& name, ApiCall& call) const {
    auto it = m_calls.find(name);
    if (it == m_calls.end() || !it->second || !*it->second) {
        call.Error = "нет функции API «" + name + "» (или она только для Lua)";
        return false;
    }
    Scene* previous = CurrentApiScene();
    if (call.ScenePtr) SetCurrentApiScene(call.ScenePtr);
    bool ok = false;
    try {
        ok = (*it->second)(call);
    } catch (const std::exception& e) {
        // Исключение функции движка — ошибка вызова, а не падение языка,
        // который её позвал (чужой процесс, свой язык игры).
        call.Error = name + ": " + e.what();
        ok = false;
    } catch (...) {
        call.Error = name + ": неизвестная ошибка";
        ok = false;
    }
    SetCurrentApiScene(previous);
    return ok;
}

size_t ApiBindings::Size() const { return m_primary.size(); }

std::vector<std::string> ApiBindings::Names() const {
    std::vector<std::string> out = m_primary;
    std::sort(out.begin(), out.end());
    return out;
}

void AddPortableFunction(PortableFunction function) {
    sage::lang::ApiRegistry::Instance().Add(function.Meta);
    std::lock_guard<std::mutex> lock(PortableMutex());
    std::vector<PortableFunction>& list = PortableList();
    // Повторная регистрация того же имени заменяет прежнюю (перезагрузка плагина).
    for (PortableFunction& f : list) {
        if (f.Meta.Module == function.Meta.Module && f.Meta.Name == function.Meta.Name) {
            f = std::move(function);
            return;
        }
    }
    list.push_back(std::move(function));
}

const std::vector<PortableFunction>& PortableFunctions() { return PortableList(); }

int RemovePortableFunctions(const std::string& origin) {
    sage::lang::ApiRegistry::Instance().RemoveOrigin(origin);
    std::lock_guard<std::mutex> lock(PortableMutex());
    std::vector<PortableFunction>& list = PortableList();
    const size_t before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const PortableFunction& f) { return f.Meta.Origin == origin; }),
               list.end());
    return (int)(before - list.size());
}

} // namespace sage::scripting
