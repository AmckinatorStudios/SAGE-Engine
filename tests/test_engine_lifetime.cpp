// ВРЕМЯ ЖИЗНИ: контекст движка (вложенность, порядок сноса, исключения) и
// дескрипторы GameObject, пережившие свою сцену.
#include "TestFramework.h"

#include <memory>
#include <stdexcept>

#include "sage/core/EngineContext.h"
#include "sage/core/JobSystem.h"
#include "sage/scene/Scene.h"

using sage::EngineContext;
using sage::ScopedEngineContext;

TEST(EngineContext_nested_scopes_restore_in_order) {
    EngineContext* const base = EngineContext::CurrentOrNull();
    {
        ScopedEngineContext a;
        CHECK_TRUE(&EngineContext::Current() == &a.Get());
        CHECK_TRUE(a.Previous() == base);
        {
            ScopedEngineContext b;
            CHECK_TRUE(&EngineContext::Current() == &b.Get());
            CHECK_TRUE(&b->Jobs() != &a->Jobs());   // свои подсистемы, не общие
        }
        CHECK_TRUE(&EngineContext::Current() == &a.Get());
    }
    CHECK_TRUE(EngineContext::CurrentOrNull() == base);
}

TEST(EngineContext_scope_restores_previous_on_exception) {
    EngineContext* const base = EngineContext::CurrentOrNull();
    bool caught = false;
    try {
        ScopedEngineContext a;
        ScopedEngineContext b;
        throw std::runtime_error("проверка упала");
    } catch (const std::runtime_error&) {
        caught = true;
    }
    CHECK_TRUE(caught);
    CHECK_TRUE(EngineContext::CurrentOrNull() == base);
}

// Разрушение НЕтекущего контекста не сбивает текущий; разрушение текущего
// без области (как у Application без SAGE_MAIN) оставляет «контекста нет».
TEST(EngineContext_destroying_non_current_keeps_current) {
    EngineContext* const base = EngineContext::CurrentOrNull();
    {
        ScopedEngineContext a;
        auto loose = std::make_unique<EngineContext>();
        loose->Jobs().Initialize(1);
        loose.reset();
        CHECK_TRUE(&EngineContext::Current() == &a.Get());
    }
    CHECK_TRUE(EngineContext::CurrentOrNull() == base);
}

// Повторное освобождение ресурсов и повторная остановка пула — без ошибок.
TEST(EngineContext_release_and_shutdown_are_idempotent) {
    ScopedEngineContext a;
    a->Jobs().Initialize(2);
    a->ReleaseGpuResources();
    a->ReleaseGpuResources();
    a->Jobs().Shutdown();
    a->Jobs().Shutdown();
    CHECK_EQ(a->Jobs().WorkerCount(), 0);
}

// Дескриптор, переживший свою сцену, не трогает освобождённый реестр:
// Valid() — false, доступ к полям — исключение, а не чтение чужой памяти
// (под AddressSanitizer старый код давал heap-use-after-free).
TEST(GameObject_outliving_its_scene_is_invalid) {
    GameObject stale, rawBuilt, copied;
    {
        auto scene = std::make_unique<Scene>("Temp");
        stale = scene->CreateObject("A");
        rawBuilt = GameObject(&scene->Registry(), stale.Entity());   // собран из голого реестра
        copied = stale;
        CHECK_TRUE(stale.Valid() && rawBuilt.Valid() && copied.Valid());
    }
    CHECK_FALSE(stale.Valid());
    CHECK_FALSE(rawBuilt.Valid());
    CHECK_FALSE(copied.Valid());
    bool threw = false;
    try {
        (void)stale.Id();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK_TRUE(threw);

    // Реестр не от сцены — прежнее поведение: проверка по самому реестру.
    entt::registry plain;
    GameObject p(&plain, plain.create());
    CHECK_TRUE(p.Valid());
    plain.destroy(p.Entity());
    CHECK_FALSE(p.Valid());
}
