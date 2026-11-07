// ---------------------------------------------------------------------------
// ВИЗУАЛЬНЫЕ ЭФФЕКТЫ ИЗ СКРИПТА — звенья изображения на всём кадре (камера) и
// на отдельных объектах, во время игры (модуль post, стиль API SAGE).
//
//   post.add("blur")                                  -- на весь кадр
//   post.set("blur", {intensity = 0.5, radius = 4})   -- любые параметры одной таблицей
//   post.set("glow", {color = {1, 0.5, 0.2}})         -- цвет — три числа или Vec3
//   post.enable("blur", false)                        -- выключить, не теряя настроек
//   post.remove("blur")
//   post.get("blur", "intensity")
//   post.kinds()                                      -- все виды (в том числе звенья игры)
//
//   post.object_add(obj, "glow")                      -- на объект и его потомков
//   post.object_set(obj, "dissolve", {intensity = t})
//   post.object_enable(obj, "glow", false)
//   post.object_remove(obj, "glow")
//   post.object_get(obj, "dissolve", "intensity")
//   post.object_clear(obj)
//
// Звено ищется по виду (первое такого вида). Нет звена — set/enable заводят
// его; нет цепочки у камеры — она заводится на главной камере. Порядок
// наложения — порядок добавления. Неизвестный вид или параметр — ошибка с
// именем, а не тишина.
// ---------------------------------------------------------------------------
#include "ScriptEngine.h"

#include <stdexcept>

#include "sage/ecs/CameraView.h"
#include "sage/render/ObjectEffects.h"
#include "sage/render/PostEffect.h"
#include "sage/render/PostProcessComponent.h"

namespace {

using sage::render::PostChain;
using sage::render::PostEffect;

PostChain* CameraChain(Scene* scene, bool create) {
    if (!scene) return nullptr;
    const entt::entity cam = sage::ecs::PrimaryCameraEntity(*scene);
    sage::render::PostProcessComponent* pp = sage::render::ActivePostComponent(*scene, cam);
    if (!pp) {
        if (!create || cam == entt::null) return nullptr;
        sage::render::PostProcessComponent fresh;
        fresh.Chain = PostChain{};
        pp = &scene->Registry().emplace<sage::render::PostProcessComponent>(cam, fresh);
    }
    return &pp->Chain;
}

PostChain* ObjectChain(GameObject& obj, bool create) {
    if (!obj.Valid()) return nullptr;
    entt::registry& reg = *obj.Registry();
    if (auto* fx = reg.try_get<sage::render::ObjectEffectsComponent>(obj.Entity())) return &fx->Effects;
    if (!create) return nullptr;
    return &reg.emplace<sage::render::ObjectEffectsComponent>(obj.Entity()).Effects;
}

PostEffect* Find(PostChain* chain, const std::string& kind) {
    if (!chain) return nullptr;
    for (PostEffect& e : chain->Effects)
        if (e.Kind == kind) return &e;
    return nullptr;
}

void RequireKind(const std::string& kind, const char* fn) {
    if (!sage::render::PostEffectCatalog::Instance().Find(kind))
        throw std::runtime_error(std::string(fn) + ": нет эффекта «" + kind + "» (список — post.kinds())");
}

PostEffect* Ensure(PostChain* chain, const std::string& kind, const char* fn) {
    RequireKind(kind, fn);
    if (!chain) return nullptr;
    PostEffect* fx = Find(chain, kind);
    if (!fx) fx = &sage::render::AddPostEffect(*chain, kind);
    return fx;
}

bool Remove(PostChain* chain, const std::string& kind) {
    if (!chain) return false;
    for (auto it = chain->Effects.begin(); it != chain->Effects.end(); ++it) {
        if (it->Kind == kind) {
            chain->Effects.erase(it);
            return true;
        }
    }
    return false;
}

// Значение параметра из Lua: число, true/false, три числа {r, g, b} или Vec3.
void Assign(sage::render::PostValue& v, const sol::object& value, const std::string& kind, const std::string& name,
            const char* fn) {
    if (value.is<float>()) {
        v.V[0] = value.as<float>();
        v.B = v.V[0] != 0.0f;
    } else if (value.is<bool>()) {
        v.B = value.as<bool>();
        v.V[0] = v.B ? 1.0f : 0.0f;
    } else if (value.is<glm::vec3>()) {
        const glm::vec3 c = value.as<glm::vec3>();
        v.V[0] = c.x, v.V[1] = c.y, v.V[2] = c.z;
    } else if (value.is<sol::table>()) {
        sol::table t = value.as<sol::table>();
        for (int i = 0; i < 4; ++i) {
            sol::optional<float> f = t[i + 1];
            if (f) v.V[i] = *f;
        }
    } else {
        throw std::runtime_error(std::string(fn) + ": параметр «" + name + "» эффекта «" + kind +
                                 "» ждёт число или цвет {r, g, b}");
    }
}

void SetAll(PostEffect* fx, const std::string& kind, const sol::table& params, const char* fn) {
    if (!fx) return;
    for (const auto& [key, value] : params) {
        const std::string name = key.as<std::string>();
        sage::render::PostValue* v = fx->Find(name.c_str());
        if (!v) throw std::runtime_error(std::string(fn) + ": у эффекта «" + kind + "» нет параметра «" + name + "»");
        Assign(*v, value, kind, name, fn);
    }
}

float Get(PostChain* chain, const std::string& kind, const std::string& param) {
    PostEffect* fx = Find(chain, kind);
    sage::render::PostValue* v = fx ? fx->Find(param.c_str()) : nullptr;
    return v ? v->V[0] : 0.0f;
}

} // namespace

void ScriptEngine::RegisterPostApi() {
    // --- Весь кадр (цепочка главной камеры) ---------------------------------
    Bind("post", "add", nullptr, [this](const std::string& kind) -> bool {
        PostEffect* fx = Ensure(CameraChain(m_scene, true), kind, "post.add");
        if (fx) fx->Enabled = true;
        return fx != nullptr;
    });
    Bind("post", "remove", nullptr,
         [this](const std::string& kind) -> bool { return Remove(CameraChain(m_scene, false), kind); });
    Bind("post", "has", nullptr,
         [this](const std::string& kind) -> bool { return Find(CameraChain(m_scene, false), kind) != nullptr; });
    Bind("post", "enable", nullptr, [this](const std::string& kind, bool on) {
        if (PostEffect* fx = Ensure(CameraChain(m_scene, true), kind, "post.enable")) fx->Enabled = on;
    });
    Bind("post", "set", nullptr, [this](const std::string& kind, const sol::table& params) {
        SetAll(Ensure(CameraChain(m_scene, true), kind, "post.set"), kind, params, "post.set");
    });
    Bind("post", "get", nullptr, [this](const std::string& kind, const std::string& param) -> float {
        return Get(CameraChain(m_scene, false), kind, param);
    });
    Bind("post", "kinds", nullptr, [this]() -> sol::table {
        sol::table t = m_lua.create_table();
        int i = 1;
        for (const sage::render::PostEffectKind* k : sage::render::PostEffectCatalog::Instance().All())
            t[i++] = k->Id;
        return t;
    });

    // --- Отдельный объект (ObjectEffectsComponent) --------------------------
    Bind("post", "object_add", nullptr, [](GameObject& obj, const std::string& kind) -> bool {
        PostEffect* fx = Ensure(ObjectChain(obj, true), kind, "post.object_add");
        if (fx) fx->Enabled = true;
        return fx != nullptr;
    });
    Bind("post", "object_remove", nullptr,
         [](GameObject& obj, const std::string& kind) -> bool { return Remove(ObjectChain(obj, false), kind); });
    Bind("post", "object_enable", nullptr, [](GameObject& obj, const std::string& kind, bool on) {
        if (PostEffect* fx = Ensure(ObjectChain(obj, true), kind, "post.object_enable")) fx->Enabled = on;
    });
    Bind("post", "object_set", nullptr, [](GameObject& obj, const std::string& kind, const sol::table& params) {
        SetAll(Ensure(ObjectChain(obj, true), kind, "post.object_set"), kind, params, "post.object_set");
    });
    Bind("post", "object_get", nullptr, [](GameObject& obj, const std::string& kind, const std::string& param) -> float {
        return Get(ObjectChain(obj, false), kind, param);
    });
    Bind("post", "object_clear", nullptr, [](GameObject& obj) {
        if (obj.Valid()) obj.Registry()->remove<sage::render::ObjectEffectsComponent>(obj.Entity());
    });

    // Без префикса sage. — стандарт стиля API (docs/scripting.md).
    m_lua["post"] = Module("post");
}
