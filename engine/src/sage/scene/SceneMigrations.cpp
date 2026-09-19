#include "sage/scene/SceneMigrations.h"

#include <stdexcept>
#include <string>

#include <glm/glm.hpp>

#include "sage/assets/AssetDatabase.h"
#include "sage/core/Log.h"
#include "sage/scene/Light.h"
#include "sage/scene/SceneJson.h"
#include "sage/ecs/LightSystem.h"
#include "sage/ui/UIAnchor.h"
#include "sage/scene/SceneLegacyUI.h"

using json = nlohmann::json;

namespace sage::scene {

// ---------------------------------------------------------------------------
//  Версия формата сцены и миграции
// ---------------------------------------------------------------------------
//
// Номер писался в файл с самого начала и НИКОГДА не читался. Это худший из
// вариантов: он создаёт впечатление, что о совместимости позаботились, а на
// деле первое же ломающее изменение формата тихо испортило бы все старые
// сцены — без ошибки, без предупреждения, просто «объекты почему-то не там».
//
// Как это работает теперь. Файл несёт номер версии; загрузчик прогоняет его
// через цепочку миграций до текущей. Каждая миграция — маленькая функция
// «из N в N+1», которая правит JSON, а не сцену: правка на уровне JSON не
// зависит от того, как сегодня выглядят компоненты, и потому не устаревает
// вместе с ними. Иначе миграцию пришлось бы переписывать каждый раз, когда
// меняется структура, ради которой она и была написана.
//
// Отсутствие номера означает версию 1 — сцены, сохранённые до появления
// проверки. Их не за что винить, и загружаться они обязаны.
namespace {

// v1 -> v2. Первая настоящая миграция: в v1 у тел не было ни слоя
// столкновений, ни признака сенсора, и «нет поля» надо превратить в «слой по
// умолчанию, не сенсор» ЯВНО. Само по себе это сделали бы и значения по
// умолчанию при разборе — но именно на таких «и так сработает» миграции и
// перестают писать, а потом однажды не срабатывает.
void MigrateV1toV2(json& root) {
    for (json& obj : root["objects"]) {
        if (!obj.contains("rigidBody")) continue;
        json& rb = obj["rigidBody"];
        if (!rb.contains("layer")) rb["layer"] = 1u;
        if (!rb.contains("sensor")) rb["sensor"] = false;
    }
}

// v2 -> v3. Ссылки на ассеты обзаводятся GUID'ами. Раньше личностью файла был
// ПУТЬ, и переименование молча ломало сцену: она грузилась, объект оставался на
// месте, просто без модели, и в логе не было ни строчки.
//
// Миграция проставляет GUID по текущим путям — то есть фиксирует связь ровно в
// том виде, в каком она сейчас верна. Дальше файл можно переименовывать: GUID
// живёт в сайдкаре рядом с ним и переезжает вместе.
//
// Путь, у которого ассета в базе нет, остаётся БЕЗ GUID'а, а не получает
// выдуманный: несуществующая ссылка должна остаться видимо сломанной, а не
// притвориться целой.
void MigrateV2toV3(json& root) {
    sage::AssetDatabase& db = sage::AssetDatabase::Instance();
    auto stamp = [&](json& holder, const char* key) {
        if (!holder.contains(key) || !holder[key].is_string()) return;
        const std::string path = holder[key].get<std::string>();
        if (path.empty()) return;
        const sage::AssetGuid guid = db.GuidOf(path);
        if (guid.Valid()) holder[std::string(key) + "Guid"] = guid.ToString();
    };
    for (json& obj : root["objects"]) {
        if (obj.contains("mesh") && obj["mesh"].is_object()) stamp(obj["mesh"], "path");
        stamp(obj, "material");
        stamp(obj, "script");
        if (obj.contains("animatedModel") && obj["animatedModel"].is_object())
            stamp(obj["animatedModel"], "path");
    }
}

// v3 -> v4. Цвет экземпляра стал МНОЖИТЕЛЕМ albedo материала, а не заменой ему.
//
// ПОЧЕМУ ПОМЕНЯЛОСЬ. У MeshRendererComponent три поправки поверх материала —
// цвет, свечение, прозрачность — и правила наложения у них были три разных:
// цвет материал ЗАМЕЩАЛ, прозрачность множил, свечение складывал. Замещение
// хуже прочих тем, что молчит: назначил материал — и поле Color в инспекторе
// перестало влиять на что-либо, никак об этом не сообщив. Теперь правило одно:
// поправка модулирует материал.
//
// ЧТО ДЕЛАЕТ МИГРАЦИЯ. У сущности с материалом старый color НЕ участвовал в
// картинке вообще, а по новому правилу он домножил бы albedo — и сцена, которую
// никто не трогал, перекрасилась бы при первом же открытии. Поэтому мёртвое
// значение заменяется на нейтральное: белый множитель даёт ровно тот вид,
// который был. Сущности БЕЗ материала не трогаем — у них color и был цветом.
void MigrateV3toV4(json& root) {
    for (json& obj : root["objects"]) {
        const bool hasMaterial = obj.contains("material") && obj["material"].is_string() &&
                                 !obj["material"].get<std::string>().empty();
        if (!hasMaterial) continue;
        obj["color"] = json::array({1.0f, 1.0f, 1.0f});
    }
}

// v4 -> v5. Солнце переехало из НАСТРОЕК СЦЕНЫ в обычную сущность.
//
// ПОЧЕМУ ПОМЕНЯЛОСЬ. Направленный свет был единственным источником, который не
// являлся объектом: его нельзя было выбрать в иерархии, повернуть гизмо,
// анимировать, привязать к родителю или положить в префаб, и второго такого
// света в сцене быть не могло. Всё это движок умеет делать с сущностями — и не
// умел с солнцем ровно потому, что солнце сущностью не было. Разбор — в
// комментарии к LightComponent (ecs/CameraLightComponents.h).
//
// ЧТО ДЕЛАЕТ МИГРАЦИЯ. Заводит объект «Солнце» с направленным светом, переносит
// в него цвет, яркость и направление (направление превращается в ПОВОРОТ: у
// сущности нет поля «куда светит», у неё есть ориентация), и обнуляет яркость
// старого поля.
//
// Обнуление обязательно, и это не уборка. Настройка сцены осталась основанием
// кадра — её перекрывает направленный свет-сущность, но если такую сущность
// удалить, основание проступит обратно. Сцена, где человек удалил солнце и
// продолжает видеть солнечный свет, объяснима только чтением исходников.
void MigrateV4toV5(json& root) {
    json sun = json::object();
    if (root.contains("lighting") && root["lighting"].contains("sun")) sun = root["lighting"]["sun"];

    const DirectionalLight defaults;
    glm::vec3 direction = defaults.Direction;
    glm::vec3 color = defaults.Color;
    float intensity = defaults.Intensity;
    if (sun.contains("direction")) direction = Vec3FromJson(sun["direction"]);
    if (sun.contains("color")) color = Vec3FromJson(sun["color"]);
    intensity = sun.value("intensity", intensity);

    // Свободный id: сущность добавляется к уже существующим, и совпадение
    // сломало бы связи иерархии (родитель ищется по id).
    int maxId = 0;
    for (const json& obj : root["objects"]) maxId = std::max(maxId, obj.value("id", 0));

    json entity;
    entity["id"] = maxId + 1;
    // Имя латиницей и БЕЗ перевода: это имя объекта в сцене, то есть ДАННЫЕ.
    // Сцена, открытая на другом языке, не должна оказаться со «Sun» вместо
    // «Солнце» — иначе скрипт, ищущий объект по имени, перестал бы его
    // находить от смены языка интерфейса. Остальные объекты демо-сцены
    // называются так же, по-английски.
    entity["name"] = "Sun";
    // Позиция ни на что не влияет (свет из бесконечности), но объект без
    // позиции неудобно найти в сцене — ставим его над началом мира.
    entity["position"] = Vec3ToJson(glm::vec3(0.0f, 10.0f, 0.0f));
    entity["rotation"] = Vec3ToJson(sage::ecs::EulerFromForward(direction));
    entity["scale"] = Vec3ToJson(glm::vec3(1.0f));
    // БЕЗ КОМПОНЕНТА МЕША ВОВСЕ, а не с мешем «none»: у света нет ни модели, ни
    // цвета, ни теней-от-себя, и секция «Меш» в инспекторе лампы обещает
    // настройки, которыми нечем распорядиться (см. "noMesh" при загрузке).
    entity["noMesh"] = true;
    entity["light"]["type"] = "directional";
    entity["light"]["color"] = Vec3ToJson(color);
    entity["light"]["intensity"] = intensity;
    entity["light"]["castShadows"] = true;
    root["objects"].push_back(entity);

    root["lighting"]["sun"]["intensity"] = 0.0f;
}

// v5 -> v6. Части перестали двигать и перекрашивать друг друга.
//
// ПОЧЕМУ ПОМЕНЯЛОСЬ. Часть рисует себя в прямоугольнике СВОЕГО элемента и
// ничего не знает о соседях. Раньше было наоборот, и правила жили только в
// исходниках отрисовки: значок сдвигал подпись вправо на свою ширину, галка —
// на сторону квадратика, ползунок красился подложкой соседа, а «акцент» брал у
// шкалы, которую для этого надо было повесить рядом; при этом подложка,
// оказавшись рядом с диапазоном, переставала закрашивать элемент целиком. Ни
// одно из этих правил нельзя было увидеть в инспекторе или отменить.
//
// ЧТО ДЕЛАЕТ МИГРАЦИЯ — три перевода, каждый сохраняет прежний ВИД:
//   1. Диапазон забирает себе цвета, которые брал у подложки и шкалы, а сами
//      подложка и шкала с такого элемента убираются: раньше их всё равно не
//      рисовали (подложка шла в дорожку, шкала не рисовалась вовсе), а теперь
//      нарисовали бы поверх.
//   2. Значок, стоявший рядом с чем-то ещё, переезжает в свой дочерний объект
//      того же размера и на то же место — раньше он в этом случае жался
//      квадратиком к левому краю, а один в элементе занимал его целиком.
//   3. Подпись, которую двигали значок или галка, переезжает в дочерний объект,
//      растянутый на родителя, с левым полем ровно в прежний сдвиг.
//
// Чего миграция НЕ трогает: элементы без значка и галки (там текст и раньше
// рисовался по всему прямоугольнику) и поля ввода — их текст рисует САМО ПОЛЕ
// (он бывает скрыт точками и обрезан кареткой), и вынос его к ребёнку показал
// бы рядом вторую, неживую копию.
void MigrateV5toV6(json& root) {
    int maxId = 0;
    for (const json& obj : root["objects"]) maxId = std::max(maxId, obj.value("id", 0));

    // Дети собираются отдельно: дописывать в массив, по которому идёт цикл, —
    // это ссылки, протухшие при первом же расширении вектора.
    std::vector<json> born;

    for (json& obj : root["objects"]) {
        if (!obj.contains("ui") || !obj["ui"].is_object()) continue;
        json& uj = obj["ui"];
        const int ownerId = obj.value("id", 0);
        if (ownerId <= 0) continue; // без id ребёнка не к чему привязать

        const bool hasLabel = uj.contains("label") && uj["label"].is_object();
        const bool hasInput = uj.contains("textInput");
        const bool hasImage = uj.contains("image") && uj["image"].is_object();
        const bool hasBar = uj.contains("bar") && uj["bar"].is_object();
        const bool hasRange = uj.contains("range") && uj["range"].is_object();
        const bool hasIcon = uj.contains("icon") && uj["icon"].is_object() &&
                             !uj["icon"].value("name", std::string()).empty();

        const json tj = uj.value("transform", json::object());
        const glm::vec2 size = Vec2FromJson(tj.value("size", json::object()), {200.0f, 56.0f});
        const float w = size.x > 0.0f ? size.x : 200.0f;
        const float h = size.y > 0.0f ? size.y : 56.0f;
        const int layer = tj.value("layer", 0);

        // Заготовка дочернего элемента: те же поля, что пишет SaveUIComponents,
        // — иначе загрузчик подставит СВОИ умолчания вместо наших.
        auto makeChild = [&](const char* name) {
            json child;
            child["id"] = ++maxId;
            // Имя латиницей: это ДАННЫЕ сцены, а не строка интерфейса (см.
            // «Sun» в миграции v4->v5) — от смены языка редактора имя объекта
            // меняться не должно.
            child["name"] = name;
            child["parent"] = ownerId;
            // Элемент интерфейса рисует система UI, а не меш (см. "Sun" выше).
            child["noMesh"] = true;
            json& cu = child["ui"];
            cu["transform"]["anchor"] = (int)UIAnchor::TopLeft;
            cu["transform"]["stretch"] = (int)sage::ui::Element::Stretch::None;
            cu["transform"]["offset"] = Vec2ToJson({0.0f, 0.0f});
            cu["transform"]["size"] = Vec2ToJson(size);
            cu["transform"]["margin"] = Vec4ToJson(glm::vec4(0.0f));
            cu["transform"]["pivot"] = Vec2ToJson({0.0f, 0.0f});
            // Слой берётся у родителя: часть обязана лечь поверх того же, поверх
            // чего лежала, а не всплыть на нулевой слой холста.
            cu["transform"]["layer"] = layer;
            cu["transform"]["visible"] = true;
            return child;
        };

        // --- 1. Диапазон забирает чужие цвета себе --------------------------
        if (hasRange) {
            json& rj = uj["range"];
            if (uj.contains("fill") && uj["fill"].is_object()) {
                const json& fj = uj["fill"];
                if (fj.contains("color")) rj["trackColor"] = fj["color"];
                if (fj.contains("rounding")) rj["rounding"] = fj["rounding"];
                if (fj.contains("borderColor")) rj["borderColor"] = fj["borderColor"];
                if (fj.contains("borderThickness"))
                    rj["borderThickness"] = fj["borderThickness"];
                uj.erase("fill");
            }
            if (hasBar) {
                if (uj["bar"].contains("fillColor")) rj["accentColor"] = uj["bar"]["fillColor"];
                uj.erase("bar");
            }
        }

        // Сдвиг подписи считается ТЕМИ ЖЕ формулами, что были в старой
        // отрисовке, — иначе «выглядит как раньше» станет «примерно как раньше».
        const float iconPad = std::min(4.0f, h * 0.18f);
        const float declaredIcon = hasIcon ? uj["icon"].value("size", 0.0f) : 0.0f;
        const float iconSide =
            declaredIcon > 0.0f ? declaredIcon : std::max(h - iconPad * 2.0f, 4.0f);

        // --- 2. Значок рядом с чем-то ещё — в свой объект --------------------
        if (hasIcon && (hasLabel || hasImage || hasBar || hasRange)) {
            json child = makeChild("Icon");
            child["ui"]["transform"]["offset"] = Vec2ToJson({iconPad, iconPad});
            child["ui"]["transform"]["size"] = Vec2ToJson({iconSide, iconSide});
            child["ui"]["icon"] = uj["icon"];
            uj.erase("icon");
            born.push_back(std::move(child));
        }

        // --- 3. Сдвинутая подпись — в свой объект ----------------------------
        if (hasLabel && !hasInput) {
            const float padX = uj["label"].value("padX", 8.0f);
            float left = padX;
            bool shifted = false;
            if (hasIcon) {
                left = iconPad * 2.0f + iconSide;
                shifted = true;
            }
            if (hasRange && uj["range"].value("toggle", false)) {
                left = std::max(left, std::min(w, h) + padX);
                shifted = true;
            }
            if (shifted) {
                json child = makeChild("Text");
                json& ctj = child["ui"]["transform"];
                // Растянут на родителя: иначе подпись зависела бы от размера,
                // записанного однажды, и разъезжалась бы при растяжении самого
                // элемента.
                ctj["stretch"] = (int)sage::ui::Element::Stretch::Both;
                // Левое поле — прежний сдвиг МИНУС боковой отступ надписи: его
                // надпись добавит сама, и без вычитания он посчитался бы дважды.
                ctj["margin"] = Vec4ToJson({std::max(left - padX, 0.0f), 0.0f, 0.0f, 0.0f});
                child["ui"]["label"] = uj["label"];
                uj.erase("label");
                born.push_back(std::move(child));
            }
        }
    }

    for (json& child : born) root["objects"].push_back(std::move(child));
}

using MigrationFn = void (*)(json&);

// Цепочка миграций: индекс i переводит версию (i+1) в (i+2).
// --- v6 -> v7: «Animated Model» распадается на Mesh + Animation --------------
//
// В шестой версии анимированный персонаж описывался блоком animatedModel, где
// лежали И путь к модели, И проигрывание. В седьмой модель принадлежит mesh —
// как у любого другого объекта, — а animation отвечает только за клип. Это не
// переименование: до правки в сцене существовало ДВА разных вида объекта, и у
// «анимированной модели» не было ни материалов, ни слотов подмешей, ни
// настроек отрисовки.
//
// Миграция здесь, а не в разборе компонентов, ровно поэтому: разбор обязан
// знать один формат — текущий. Иначе каждая новая версия добавляет ветку в
// разбор, и через три версии никто уже не скажет, какой формат считается
// настоящим.
void MigrateV6toV7(json& root) {
    for (json& obj : root["objects"]) {
        if (!obj.contains("animatedModel") || !obj["animatedModel"].is_object()) continue;
        json am = obj["animatedModel"];
        obj.erase("animatedModel");

        // Путь — в mesh. Если объект уже описывает свою модель, старый путь НЕ
        // источник правды: у него не было причин быть верным, а у mesh есть.
        const std::string path = am.value("path", std::string());
        if (!path.empty()) {
            const bool meshHasModel = obj.contains("mesh") && obj["mesh"].is_object() &&
                                      obj["mesh"].value("type", std::string("none")) == "model" &&
                                      !obj["mesh"].value("path", std::string()).empty();
            if (!meshHasModel) {
                obj["mesh"]["type"] = "model";
                obj["mesh"]["path"] = path;
            }
        }
        am.erase("path");
        obj["animation"] = am;
    }
}

// --- 7 -> 8: у картинки появился РЕЖИМ ----------------------------------------
//
// Раньше девятина включалась тем, что рамка переставала быть нулевой: признака
// «режим» не было вовсе. Теперь он есть, и по умолчанию это «растянуть» —
// значит сцена, сохранённая вчера, открылась бы с картинками, у которых рамка
// на месте, а девятины нет. На экране это выглядит как испорченные панели:
// углы размазаны вместе с серединой.
//
// Правило ровно то, по которому работал старый движок: ненулевая рамка —
// девятина. Обратное неверно, и додумывать здесь нечего: замощения в старом
// формате не существовало.
void MigrateV7toV8(json& root) {
    for (json& obj : root["objects"]) {
        if (!obj.contains("ui") || !obj["ui"].is_object()) continue;
        json& uj = obj["ui"];
        if (!uj.contains("image") || !uj["image"].is_object()) continue;
        json& img = uj["image"];
        if (img.contains("mode")) continue;   // уже новый формат — не трогаем
        const json& b = img.contains("sliceBorder") ? img["sliceBorder"] : json();
        bool sliced = false;
        if (b.is_array() && b.size() == 4) {
            for (const json& v : b) {
                if (v.is_number() && v.get<double>() > 0.0) { sliced = true; break; }
            }
        }
        img["mode"] = sliced ? 1 : 0;   // 1 — NineSlice, 0 — Normal
    }
}

const MigrationFn kMigrations[] = {
    &MigrateV1toV2,
    &MigrateV2toV3,
    &MigrateV3toV4,
    &MigrateV4toV5,
    &MigrateV5toV6,
    &MigrateV6toV7,
    &MigrateV7toV8,
};

} // namespace

int MigrateJsonInPlace(json& root) {
    const int from = root.value("sage_scene_version", 1);
    if (from > kSceneVersion) {
        throw std::runtime_error(
            "Сцена сохранена более новой версией движка (формат " + std::to_string(from) +
            ", движок понимает " + std::to_string(kSceneVersion) +
            "). Обновите движок — открыть её сейчас значит потерять часть данных.");
    }
    if (from < 1) throw std::runtime_error("Повреждённый номер версии сцены");
    if (!root.contains("objects") || !root["objects"].is_array()) root["objects"] = json::array();

    for (int v = from; v < kSceneVersion; ++v) {
        kMigrations[v - 1](root);
    }
    if (from < kSceneVersion) {
        LOG_INFO("Scene") << "Сцена обновлена с формата " << from << " до " << kSceneVersion;
    }
    root["sage_scene_version"] = kSceneVersion;
    return from;
}

} // namespace sage::scene
