// ---------------------------------------------------------------------------
// Описание компонентов сцены (sage/scene/SceneReflect.h).
//
// Таблица свойств — единственный источник правды о том, из чего компонент
// состоит для человека, и ошибка в ней не видна ни сборке, ни глазу: неверное
// смещение молча правит СОСЕДНЕЕ поле, а забытое свойство просто не существует
// в редакторе. Здесь проверяется ровно это.
// ---------------------------------------------------------------------------
#include "TestFramework.h"

#include <set>
#include <string>

#include "sage/ecs/CameraLightComponents.h"
#include "sage/ecs/RenderComponents.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneReflect.h"
#include "sage/scene/Transform.h"

using namespace sage::scene;

TEST(Reflect_registry_knows_the_engine_components) {
    const ComponentRegistry& reg = ComponentRegistry::Instance();
    CHECK_TRUE(reg.Types().size() >= 10);
    // Ключи устойчивы: по ним компонент найдут и скрипт, и файл раскладки
    // инспектора. Переименование ключа — это ломающая правка, и она обязана
    // ломать тест, а не тишину.
    for (const char* id : {"Transform", "MeshRenderer", "Camera", "Light", "Script",
                           "RigidBody", "Collider", "UIDocument"}) {
        CHECK_TRUE(reg.Find(id) != nullptr);
    }
}

TEST(Reflect_component_ids_and_property_keys_are_unique) {
    const ComponentRegistry& reg = ComponentRegistry::Instance();
    std::set<std::string> ids;
    for (const ComponentType& t : reg.Types()) {
        CHECK_TRUE(t.Id != nullptr && t.Label != nullptr);
        CHECK_TRUE(ids.insert(t.Id).second);
        // Ключи свойств уникальны ВНУТРИ компонента: по ним адресуют поле, и
        // два одинаковых ключа означают, что одно из них недостижимо.
        std::set<std::string> keys;
        for (int i = 0; i < t.PropCount; ++i) {
            CHECK_TRUE(t.Props[i].Key != nullptr && t.Props[i].Label != nullptr);
            CHECK_TRUE(keys.insert(t.Props[i].Key).second);
        }
        // Действия обязаны быть у всех: инспектор зовёт их не глядя.
        CHECK_TRUE(t.Has && t.Data && t.Add && t.Remove);
    }
}

TEST(Reflect_reads_and_writes_the_right_field) {
    Scene scene;
    GameObject obj = scene.CreateObject("Probe");
    entt::registry& reg = scene.Registry();

    const ComponentType* tr = ComponentRegistry::Instance().Find("Transform");
    CHECK_TRUE(tr != nullptr);
    if (!tr) return;

    void* data = tr->Data(reg, obj.Entity());
    CHECK_TRUE(data != nullptr);
    const Property* pos = tr->FindProp("position");
    const Property* scale = tr->FindProp("scale");
    CHECK_TRUE(pos && scale);
    if (!pos || !scale || !data) return;

    // Пишем через описание — читаем через сам компонент. Неверное смещение
    // здесь и ловится: значение уехало бы в соседнее поле, и Position остался
    // бы нулевым, а Rotation внезапно стал бы 3.
    PropertySetFloat(data, *pos, 0, 3.0f);
    PropertySetFloat(data, *pos, 1, 4.0f);
    PropertySetFloat(data, *pos, 2, 5.0f);
    const Transform& t = reg.get<Transform>(obj.Entity());
    CHECK_NEAR(t.Position.x, 3.0f, 1e-5);
    CHECK_NEAR(t.Position.y, 4.0f, 1e-5);
    CHECK_NEAR(t.Position.z, 5.0f, 1e-5);
    CHECK_NEAR(t.Rotation.x, 0.0f, 1e-5);
    CHECK_NEAR(t.Scale.x, 1.0f, 1e-5);

    // И обратно.
    float out = 0.0f;
    CHECK_TRUE(PropertyGetFloat(data, *pos, 1, out));
    CHECK_NEAR(out, 4.0f, 1e-5);
}

TEST(Reflect_enum_field_matches_the_component_enum) {
    Scene scene;
    GameObject obj = scene.CreateObject("Lamp");
    entt::registry& reg = scene.Registry();
    reg.emplace<LightComponent>(obj.Entity());

    const ComponentType* light = ComponentRegistry::Instance().Find("Light");
    CHECK_TRUE(light != nullptr);
    if (!light) return;
    const Property* kind = light->FindProp("kind");
    CHECK_TRUE(kind && kind->EnumCount == 3);
    if (!kind) return;

    // Перечисление в таблице — тот же int, что и в компоненте. Разъедься они
    // (другой базовый тип, другой порядок), и «Directional» в списке ставило бы
    // «Spot» в сцене — то есть свет вёл бы себя не как выбрано.
    void* data = light->Data(reg, obj.Entity());
    PropertySetFloat(data, *kind, 0, (float)(int)LightComponent::Type::Directional);
    CHECK_TRUE(reg.get<LightComponent>(obj.Entity()).Kind ==
               LightComponent::Type::Directional);
}

TEST(Reflect_ranges_are_applied_on_write) {
    Scene scene;
    GameObject obj = scene.CreateObject("Lamp");
    entt::registry& reg = scene.Registry();
    reg.emplace<LightComponent>(obj.Entity());

    const ComponentType* light = ComponentRegistry::Instance().Find("Light");
    const Property* intensity = light ? light->FindProp("intensity") : nullptr;
    CHECK_TRUE(intensity && intensity->HasRange());
    if (!light || !intensity) return;

    // Границы описаны у СВОЙСТВА, а не у виджета: то же поле правят и скрипт, и
    // анимация, и они обязаны получить те же границы. Иначе «интенсивность −5»
    // приходит из кода и сцена гаснет без объяснений.
    void* data = light->Data(reg, obj.Entity());
    PropertySetFloat(data, *intensity, 0, -5.0f);
    CHECK_NEAR(reg.get<LightComponent>(obj.Entity()).Intensity, intensity->Min, 1e-5);
    PropertySetFloat(data, *intensity, 0, 1e6f);
    CHECK_NEAR(reg.get<LightComponent>(obj.Entity()).Intensity, intensity->Max, 1e-5);
}

TEST(Reflect_lists_only_the_components_the_entity_has) {
    Scene scene;
    GameObject obj = scene.CreateObject("Thing");
    entt::registry& reg = scene.Registry();

    // Свежий объект несёт то, без чего объекта нет: положение и вид. Света на
    // нём нет — на нём и проверяется, что список отражает СУЩНОСТЬ, а не
    // перечень зарегистрированных типов.
    auto has = [&](const char* id) {
        for (const ComponentType* t : ComponentRegistry::Instance().Of(reg, obj.Entity()))
            if (std::string(t->Id) == id) return true;
        return false;
    };
    CHECK_TRUE(has("Transform"));
    CHECK_FALSE(has("Light"));

    const ComponentType* light = ComponentRegistry::Instance().Find("Light");
    CHECK_TRUE(light != nullptr);
    if (!light) return;
    light->Add(reg, obj.Entity());
    CHECK_TRUE(has("Light"));
    light->Remove(reg, obj.Entity());
    CHECK_FALSE(has("Light"));

    // Transform убрать нельзя даже по ошибке: он помечен обязательным, и
    // инспектор не покажет для него кнопку удаления.
    const ComponentType* tr = ComponentRegistry::Instance().Find("Transform");
    CHECK_TRUE(tr && tr->Essential);
}

TEST(Reflect_string_fields_point_at_real_strings) {
    Scene scene;
    GameObject obj = scene.CreateObject("Thing");
    entt::registry& reg = scene.Registry();
    reg.emplace<MeshRendererComponent>(obj.Entity());

    const ComponentType* mesh = ComponentRegistry::Instance().Find("MeshRenderer");
    const Property* mat = mesh ? mesh->FindProp("material") : nullptr;
    CHECK_TRUE(mat && mat->Type == Property::Kind::String);
    if (!mesh || !mat) return;

    // Строковое поле правится по тому же смещению. Ошибка здесь — не «не
    // сработало», а порча памяти: строка легла бы поверх соседнего поля.
    void* data = mesh->Data(reg, obj.Entity());
    FieldAs<std::string>(data, *mat) = "assets/red.sagemat";
    CHECK_EQ(reg.get<MeshRendererComponent>(obj.Entity()).MaterialPath,
             std::string("assets/red.sagemat"));
}
