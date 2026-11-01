#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <sol/sol.hpp>

#include "sage/lang/ApiRegistry.h"
#include "sage/scene/Scene.h"
#include "sage/vars/Value.h"

// ---------------------------------------------------------------------------
// API ДВИЖКА ДЛЯ ЛЮБОГО ЯЗЫКА: описание и вызов из одной регистрации.
//
// Функция движка регистрируется ОДИН раз — типизированной функцией C++. Всё
// остальное выводит компилятор из её сигнатуры:
//   • запись каталога (sage/lang/ApiRegistry.h): типы аргументов и возврата
//     для проверки типов, подсказок редактора и внешних инструментов;
//   • универсальный вызыватель на sage::vars::Value — им функцию зовёт ЛЮБОЙ
//     язык (внешний процесс, свой язык игры), а не только Lua.
//
// Два пути регистрации:
//   1. Bind(...) прежнего движка Lua (ScriptEngine) — функция привязывается к
//      Lua напрямую (sol2, без лишних слоёв в кадре) и заодно попадает в
//      каталог и вызыватели. Так все ~300 функций движка описаны без единой
//      строки сверху.
//   2. RegisterApiFunction(...) ниже — регистрация БЕЗ sol: функция игры,
//      плагина или новой системы движка. Каждое состояние скриптов само
//      привязывает её к Lua, кладёт в вызыватели и в каталог. Типы обязаны
//      быть переносимыми (static_assert), иначе функция не была бы доступна
//      другим языкам — а ради этого её так и регистрируют.
//
// Переносимые типы: bool, целые и дробные числа, перечисления, std::string,
// glm::vec2/vec3/vec4/quat, GameObject (Entity), std::optional/sol::optional
// от них, std::vector от них; возврат — то же, void или std::tuple (несколько
// значений). sol::table, sol::function, sol::object — только Lua: такая
// функция есть в каталоге, но её универсального вызова нет (Portable = false).
// ---------------------------------------------------------------------------
namespace sage::scripting {

// Один универсальный вызов: значения туда и обратно, сцена для ссылок на
// объекты, текст ошибки.
struct ApiCall {
    Scene* ScenePtr = nullptr;
    std::vector<sage::vars::Value> Args;
    std::vector<sage::vars::Value> Results;
    std::string Error;
};
using ApiInvoker = std::function<bool(ApiCall&)>;

// Сцена, с которой работают переносимые функции, — на поток. Ставит её
// хозяин состояния скриптов (BindScene) и универсальный вызов на время себя:
// функции, которой нужна «текущая сцена», не надо знать, каким языком её позвали.
Scene* CurrentApiScene();
void SetCurrentApiScene(Scene* scene);

// Вызыватели одного состояния скриптов. Живут столько же, сколько хозяин:
// функции, захватившие этот хозяин, не переживут его в чужом списке.
class ApiBindings {
public:
    void Set(const std::string& name, ApiInvoker invoker, const std::vector<std::string>& aliases = {});
    bool Has(const std::string& name) const { return m_calls.count(name) > 0; }
    // Вызов по полному имени или псевдониму. Исключение функции, неверное
    // число аргументов, неподходящий тип — false и Error, а не падение.
    bool Call(const std::string& name, ApiCall& call) const;
    size_t Size() const;
    std::vector<std::string> Names() const;   // полные имена, без псевдонимов

private:
    std::unordered_map<std::string, std::shared_ptr<ApiInvoker>> m_calls;
    std::vector<std::string> m_primary;
};

namespace api_detail {

template <typename T>
using Bare = std::remove_cv_t<std::remove_reference_t<T>>;

template <typename T> struct IsOptional : std::false_type {};
template <typename T> struct IsOptional<std::optional<T>> : std::true_type { using Inner = T; };
template <typename T> struct IsOptional<sol::optional<T>> : std::true_type { using Inner = T; };
template <typename T> struct IsVector : std::false_type {};
template <typename T, typename A> struct IsVector<std::vector<T, A>> : std::true_type { using Inner = T; };
template <typename T> struct IsTuple : std::false_type {};
template <typename... T> struct IsTuple<std::tuple<T...>> : std::true_type {};

// Аргументы, которые язык подставляет сам (состояние Lua) — в подписи их нет.
template <typename T>
constexpr bool kHidden = std::is_same_v<Bare<T>, sol::this_state> || std::is_same_v<Bare<T>, sol::this_environment> ||
                         std::is_same_v<Bare<T>, sol::this_main_state>;
template <typename T>
constexpr bool kVariadic = std::is_same_v<Bare<T>, sol::variadic_args>;

// Имя типа в каталоге и переносимость.
template <typename T>
struct TypeOf {
    using U = Bare<T>;
    static std::string Name() {
        if constexpr (std::is_same_v<U, void>) return "nil";
        else if constexpr (std::is_same_v<U, bool>) return "boolean";
        else if constexpr (std::is_enum_v<U> || std::is_integral_v<U>) return "integer";
        else if constexpr (std::is_floating_point_v<U>) return "number";
        else if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, const char*> ||
                           std::is_same_v<U, std::string_view>)
            return "string";
        else if constexpr (std::is_same_v<U, glm::vec2>) return "Vec2";
        else if constexpr (std::is_same_v<U, glm::vec3>) return "Vec3";
        else if constexpr (std::is_same_v<U, glm::vec4>) return "Vec4";
        else if constexpr (std::is_same_v<U, glm::quat>) return "Quat";
        else if constexpr (std::is_same_v<U, GameObject>) return "Entity";
        else if constexpr (IsOptional<U>::value) return TypeOf<typename IsOptional<U>::Inner>::Name();
        else if constexpr (IsVector<U>::value) return "table";
        else if constexpr (std::is_same_v<U, sol::table> || std::is_same_v<U, sol::lua_table>) return "table";
        else if constexpr (std::is_same_v<U, sol::function> || std::is_same_v<U, sol::protected_function>)
            return "function";
        else return "any";
    }
    static constexpr bool Portable() {
        if constexpr (std::is_same_v<U, bool> || std::is_enum_v<U> || std::is_integral_v<U> ||
                      std::is_floating_point_v<U> || std::is_same_v<U, std::string> ||
                      std::is_same_v<U, glm::vec2> || std::is_same_v<U, glm::vec3> || std::is_same_v<U, glm::vec4> ||
                      std::is_same_v<U, glm::quat> || std::is_same_v<U, GameObject>)
            return true;
        else if constexpr (IsOptional<U>::value) return TypeOf<typename IsOptional<U>::Inner>::Portable();
        else if constexpr (IsVector<U>::value) return TypeOf<typename IsVector<U>::Inner>::Portable();
        else return false;
    }
};

// Возврат: void — ничего, tuple — несколько значений.
template <typename R>
struct ReturnOf {
    static std::vector<std::string> Names() {
        if constexpr (std::is_void_v<R>) return {};
        else if constexpr (IsTuple<Bare<R>>::value) return Expand(static_cast<Bare<R>*>(nullptr));
        else return {TypeOf<R>::Name()};
    }
    static constexpr bool Portable() {
        if constexpr (std::is_void_v<R>) return true;
        else if constexpr (IsTuple<Bare<R>>::value) return TuplePortable(static_cast<Bare<R>*>(nullptr));
        else return TypeOf<R>::Portable();
    }
    template <typename... T>
    static std::vector<std::string> Expand(std::tuple<T...>*) { return {TypeOf<T>::Name()...}; }
    template <typename... T>
    static constexpr bool TuplePortable(std::tuple<T...>*) { return (TypeOf<T>::Portable() && ...); }
};

// Сигнатура вызываемого: лямбда, функция, указатель на функцию.
template <typename F, typename = void>
struct Callable { static constexpr bool Known = false; };
template <typename F>
struct Callable<F, std::void_t<decltype(&F::operator())>> : Callable<decltype(&F::operator())> {};
template <typename C, typename R, typename... A>
struct Callable<R (C::*)(A...) const, void> {
    static constexpr bool Known = true;
    using Ret = R;
    using Args = std::tuple<A...>;
};
template <typename C, typename R, typename... A>
struct Callable<R (C::*)(A...), void> : Callable<R (C::*)(A...) const, void> {};
template <typename R, typename... A>
struct Callable<R (*)(A...), void> {
    static constexpr bool Known = true;
    using Ret = R;
    using Args = std::tuple<A...>;
};
template <typename R, typename... A>
struct Callable<R(A...), void> : Callable<R (*)(A...), void> {};

// Метка типа: аргументы бывают ссылками, а указатель на ссылку не создать.
template <typename T>
struct Tag { using Type = T; };

template <typename Tuple, size_t... I>
std::vector<sage::lang::ApiParam> ParamsOf(std::index_sequence<I...>, bool& variadic) {
    std::vector<sage::lang::ApiParam> out;
    auto one = [&](auto tag) {
        using A = typename decltype(tag)::Type;
        if constexpr (kHidden<A>) return;
        else if constexpr (kVariadic<A>) variadic = true;
        else out.push_back({std::string(), TypeOf<A>::Name(), IsOptional<Bare<A>>::value});
    };
    (one(Tag<std::tuple_element_t<I, Tuple>>{}), ...);
    return out;
}

template <typename Tuple, size_t... I>
constexpr bool ArgsPortable(std::index_sequence<I...>) {
    return ((!kHidden<std::tuple_element_t<I, Tuple>> && !kVariadic<std::tuple_element_t<I, Tuple>> &&
             TypeOf<std::tuple_element_t<I, Tuple>>::Portable()) && ...);
}

// --- Значение <-> C++ ---------------------------------------------------------

template <typename T>
bool FromValue(const sage::vars::Value& v, ApiCall& call, T& out);

template <typename T>
bool FromValue(const sage::vars::Value& v, ApiCall& call, T& out) {
    using K = sage::vars::Kind;
    using U = Bare<T>;
    if constexpr (std::is_same_v<U, bool>) {
        out = v.AsBool();
        return v.Type() == K::Bool || v.Type() == K::Int || v.Type() == K::Float;
    } else if constexpr (std::is_enum_v<U>) {
        out = static_cast<U>(v.AsInt());
        return v.Type() == K::Int || v.Type() == K::Float;
    } else if constexpr (std::is_integral_v<U>) {
        out = static_cast<U>(v.Type() == K::Float ? (long long)std::llround(v.AsFloat()) : v.AsInt());
        return v.Type() == K::Int || v.Type() == K::Float;
    } else if constexpr (std::is_floating_point_v<U>) {
        out = static_cast<U>(v.Type() == K::Int ? (float)v.AsInt() : v.AsFloat());
        return v.Type() == K::Int || v.Type() == K::Float;
    } else if constexpr (std::is_same_v<U, std::string>) {
        if (v.Type() == K::Asset) out = v.AsAsset().Path;
        else out = v.AsString();
        return v.Type() == K::String || v.Type() == K::Asset;
    } else if constexpr (std::is_same_v<U, glm::vec2>) {
        out = v.AsVec2();
        return v.Type() == K::Vec2;
    } else if constexpr (std::is_same_v<U, glm::vec3>) {
        out = v.AsVec3();
        return v.Type() == K::Vec3;
    } else if constexpr (std::is_same_v<U, glm::vec4>) {
        out = v.AsVec4();
        return v.Type() == K::Color;
    } else if constexpr (std::is_same_v<U, glm::quat>) {
        out = v.AsRotation();
        return v.Type() == K::Rotation;
    } else if constexpr (std::is_same_v<U, GameObject>) {
        if (v.Type() != K::Entity) return false;
        Scene* scene = call.ScenePtr ? call.ScenePtr : CurrentApiScene();
        if (!scene) {
            call.Error = "ссылка на объект без сцены";
            return false;
        }
        out = scene->Get(v.AsEntity().Id);
        return true;
    } else if constexpr (IsOptional<U>::value) {
        typename IsOptional<U>::Inner inner{};
        if (!FromValue(v, call, inner)) return false;
        out = inner;
        return true;
    } else if constexpr (IsVector<U>::value) {
        const sage::vars::ArrayBox* arr = v.Type() == K::Array && v.Get<std::shared_ptr<sage::vars::ArrayBox>>()
                                              ? v.Get<std::shared_ptr<sage::vars::ArrayBox>>()->get()
                                              : nullptr;
        if (!arr) return false;
        out.clear();
        for (const sage::vars::Value& item : arr->Items) {
            typename IsVector<U>::Inner x{};
            if (!FromValue(item, call, x)) return false;
            out.push_back(std::move(x));
        }
        return true;
    } else {
        static_assert(sizeof(U) == 0, "тип не переносим между языками");
        return false;
    }
}

template <typename T>
sage::vars::Value ToValue(const T& x) {
    using U = Bare<T>;
    using sage::vars::Value;
    if constexpr (std::is_same_v<U, bool>) return Value(x);
    else if constexpr (std::is_enum_v<U>) return Value((int)x);
    else if constexpr (std::is_integral_v<U>) return Value((int)x);
    else if constexpr (std::is_floating_point_v<U>) return Value((float)x);
    else if constexpr (std::is_same_v<U, std::string>) return Value(x);
    else if constexpr (std::is_same_v<U, glm::vec2> || std::is_same_v<U, glm::vec3> ||
                       std::is_same_v<U, glm::quat>)
        return Value(x);
    else if constexpr (std::is_same_v<U, glm::vec4>) return Value(x);
    else if constexpr (std::is_same_v<U, GameObject>) return Value(sage::vars::EntityRef{x.Valid() ? x.Id() : 0});
    else if constexpr (IsOptional<U>::value) return x ? ToValue(*x) : Value();
    else if constexpr (IsVector<U>::value) {
        std::vector<Value> items;
        for (const auto& i : x) items.push_back(ToValue(i));
        return Value::Array(std::move(items));
    } else {
        static_assert(sizeof(U) == 0, "тип не переносим между языками");
        return Value();
    }
}

template <typename R>
void PushResult(ApiCall& call, R&& r) {
    using U = Bare<R>;
    if constexpr (IsTuple<U>::value)
        std::apply([&](const auto&... part) { (call.Results.push_back(ToValue(part)), ...); }, r);
    else
        call.Results.push_back(ToValue(r));
}

template <typename F, typename Args, size_t... I>
bool InvokeWith(const F& fn, ApiCall& call, const std::string& name, std::index_sequence<I...>) {
    constexpr size_t N = sizeof...(I);
    constexpr size_t Required = (0 + ... + (IsOptional<Bare<std::tuple_element_t<I, Args>>>::value ? 0 : 1));
    if (call.Args.size() < Required || call.Args.size() > N) {
        call.Error = name + ": ждёт " + (Required == N ? std::to_string(N) : std::to_string(Required) + "–" +
                                                                             std::to_string(N)) +
                     " арг., передано " + std::to_string(call.Args.size());
        return false;
    }
    std::tuple<Bare<std::tuple_element_t<I, Args>>...> values{};
    bool ok = true;
    size_t bad = 0;
    auto take = [&](auto& slot, size_t index) {
        if (!ok || index >= call.Args.size()) return;   // необязательный и не передан
        if (!FromValue(call.Args[index], call, slot)) {
            ok = false;
            bad = index;
        }
    };
    (take(std::get<I>(values), I), ...);
    if (!ok) {
        if (call.Error.empty())
            call.Error = name + ": аргумент " + std::to_string(bad + 1) + " — не " +
                         std::vector<std::string>{TypeOf<std::tuple_element_t<I, Args>>::Name()...}[bad];
        return false;
    }
    using R = decltype(fn(std::get<I>(values)...));
    if constexpr (std::is_void_v<R>) fn(std::get<I>(values)...);
    else PushResult(call, fn(std::get<I>(values)...));
    return true;
}

// Описание + вызыватель для одной функции.
template <typename F>
sage::lang::ApiEntry Describe(const std::string& module, const std::string& name) {
    sage::lang::ApiEntry e;
    e.Module = module;
    e.Name = name;
    e.Qualified = module.empty() ? name : "sage." + module + "." + name;
    using C = Callable<Bare<F>>;
    if constexpr (C::Known) {
        using Args = typename C::Args;
        e.Params = ParamsOf<Args>(std::make_index_sequence<std::tuple_size_v<Args>>{}, e.Variadic);
        e.Returns = ReturnOf<typename C::Ret>::Names();
        e.Portable = ArgsPortable<Args>(std::make_index_sequence<std::tuple_size_v<Args>>{}) &&
                     ReturnOf<typename C::Ret>::Portable();
    } else {
        e.Unknown = true;   // шаблонная лямбда (auto-аргументы): типов нет до вызова
    }
    return e;
}

template <typename F>
ApiInvoker MakeInvoker(F fn, const std::string& name) {
    using C = Callable<Bare<F>>;
    if constexpr (C::Known) {
        using Args = typename C::Args;
        constexpr size_t N = std::tuple_size_v<Args>;
        if constexpr (ArgsPortable<Args>(std::make_index_sequence<N>{}) && ReturnOf<typename C::Ret>::Portable()) {
            return [fn = std::move(fn), name](ApiCall& call) {
                return InvokeWith<F, Args>(fn, call, name, std::make_index_sequence<N>{});
            };
        }
    }
    return {};
}

} // namespace api_detail

// --- Регистрация без sol: функция для всех языков -----------------------------

struct PortableFunction {
    sage::lang::ApiEntry Meta;
    ApiInvoker Invoke;
    // Привязать к таблице Lua напрямую (sol2 сам переводит типы — без
    // промежуточных значений в кадре).
    std::function<void(sol::table&, const std::string&)> BindLua;
};

// Список переносимых функций. Каждое новое состояние скриптов привязывает их
// к Lua и вызывателям; каталог получает запись сразу.
void AddPortableFunction(PortableFunction function);
const std::vector<PortableFunction>& PortableFunctions();
// Снять функции источника (выгрузка плагина).
int RemovePortableFunctions(const std::string& origin);

// Зарегистрировать функцию движка, игры или плагина для ВСЕХ языков:
//   RegisterApiFunction("weather", "rain", [](float k) { ... }, "дождь силой k");
// В Lua — sage.weather.rain(0.5) и weather.rain(0.5), в других языках — по
// имени "sage.weather.rain" через универсальный вызов, в подсказках и
// проверке типов — с типами из сигнатуры.
template <typename F>
void RegisterApiFunction(const std::string& module, const std::string& name, F fn, std::string doc = {},
                         std::string origin = "engine") {
    using C = api_detail::Callable<api_detail::Bare<F>>;
    static_assert(C::Known, "RegisterApiFunction: сигнатура должна быть известна (не auto-аргументы)");
    PortableFunction pf;
    pf.Meta = api_detail::Describe<F>(module, name);
    static_assert(api_detail::ArgsPortable<typename C::Args>(
                      std::make_index_sequence<std::tuple_size_v<typename C::Args>>{}) &&
                      api_detail::ReturnOf<typename C::Ret>::Portable(),
                  "RegisterApiFunction: все типы обязаны быть переносимыми (см. ScriptApi.h) — иначе функция "
                  "недоступна другим языкам; функцию только для Lua регистрируйте через модуль Lua");
    pf.Meta.Doc = std::move(doc);
    pf.Meta.Origin = std::move(origin);
    if (!module.empty()) pf.Meta.Aliases = {module + "." + name};
    pf.Invoke = api_detail::MakeInvoker(fn, module.empty() ? name : "sage." + module + "." + name);
    pf.BindLua = [fn](sol::table& table, const std::string& key) { table.set_function(key, fn); };
    AddPortableFunction(std::move(pf));
}

// Записать функцию Lua-привязки (Bind прежнего движка) в каталог и вызыватели.
template <typename F>
void RecordBoundFunction(ApiBindings& bindings, const char* module, const char* name, const char* legacy,
                         const F& fn) {
    sage::lang::ApiEntry e = api_detail::Describe<F>(module ? module : "", name);
    if (module && *module) e.Aliases.push_back(std::string(module) + "." + name);
    if (legacy) e.Aliases.push_back(legacy);
    const std::string qualified = module && *module ? std::string("sage.") + module + "." + name : name;
    if (ApiInvoker inv = api_detail::MakeInvoker(fn, qualified)) bindings.Set(qualified, std::move(inv), e.Aliases);
    sage::lang::ApiRegistry::Instance().Add(std::move(e));
}

} // namespace sage::scripting
