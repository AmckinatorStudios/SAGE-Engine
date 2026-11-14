#include "sage/scene/SceneMigrations.h"

#include <cmath>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "sage/assets/AssetDatabase.h"
#include "sage/core/Log.h"
#include "sage/render/PostChainIO.h"
#include "sage/render/PostProcessComponent.h"
#include "sage/scene/Light.h"
#include "sage/scene/SceneJson.h"
#include "sage/scene/Transform.h"
#include "sage/ecs/LightSystem.h"
#include "sage/ui/UIAnchor.h"

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
// Переносит солнце из НАСТРОЕК СЦЕНЫ в объект и гасит поле. Зовут дважды:
// миграцией v4->v5 (переезд) и v10->v11 (уборка тех сцен, где поле осталось
// гореть и после переезда — их писал код мимо редактора).
// EulerFromForward в соглашении до v17 (R = Rx*Ry*Rz) — для миграций, которые
// пишут поворот в файл старого формата.
glm::vec3 LegacyEulerFromForward(const glm::vec3& dir) {
    const float len = glm::length(dir);
    if (len < 1e-6f) return glm::vec3(0.0f);
    const glm::vec3 f = dir / len;
    return glm::degrees(glm::vec3(std::atan2(f.y, -f.z), std::atan2(-f.x, std::sqrt(f.y * f.y + f.z * f.z)), 0.0f));
}

void SunFieldToEntity(json& root) {
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
    // Углы — в соглашении ТОГО формата (R = Rx*Ry*Rz): дальше по цепочке их
    // переведёт v16 -> v17, как и все остальные повороты старых сцен.
    entity["rotation"] = Vec3ToJson(LegacyEulerFromForward(direction));
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

void MigrateV4toV5(json& root) { SunFieldToEntity(root); }


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



// --- 9 -> 10: у ПАПКИ не бывает выключателя видимости -----------------------
//
// Папка заведена как способ навести порядок в списке и НИЧЕГО НЕ МЕНЯЕТ В ИГРЕ
// — так и написано на кнопке, которой её создают. Но выключатель видимости
// рисовался у каждой строки, папку в том числе, а гаснет по цепочке всё
// поддерево: один случайный щелчок по глазу у папки — и десяток объектов
// пропадал из кадра. Найти причину было нечем: пропавшие объекты в списке
// остаются на месте, а виноват глаз у строки, на которую никто не смотрел.
//
// Теперь у папки выключателя нет (см. HierarchyPanel.cpp). Значит, уже
// сохранённую «выключенную папку» надо расколдовать: иначе её содержимое
// осталось бы невидимым НАВСЕГДА — кнопки, которая вернёт его, на экране
// больше не будет.
void MigrateV9toV10(json& root) {
    for (json& obj : root["objects"]) {
        if (obj.contains("folder") && obj.value("hidden", false)) obj.erase("hidden");
    }
}

// v10 -> v11. Свет в сцене дают ТОЛЬКО объекты.
//
// ПОЧЕМУ ПОМЕНЯЛОСЬ. У настроек сцены осталось поле «солнце», и его яркость по
// умолчанию была единицей. То есть сцена, в которой нет ни одного объекта
// света, всё равно была освещена — направленным светом ниоткуда. В иерархии
// пусто, удалять нечего, а предметы видно; объяснить это можно было только
// чтением исходников. Миграция v4->v5 гасила поле у сцен, которые переезжали
// с формата 4, — но сцена, СОЗДАННАЯ уже после того переезда (кодом, шаблоном
// или движком без редактора), получала горящее поле заново.
//
// ЧТО ДЕЛАЕТ. Если поле ещё светит — заводит объект «Sun» с тем же цветом,
// яркостью и направлением и гасит поле. Освещение сцены не меняется ни на
// оттенок: тот же свет, только теперь он объект — его видно в иерархии, можно
// выбрать, повернуть и удалить.
void MigrateV10toV11(json& root) {
    if (!root.contains("lighting")) return;
    const json& lighting = root["lighting"];
    if (!lighting.contains("sun")) return;
    if (lighting["sun"].value("intensity", 0.0f) <= 0.0f) return;
    SunFieldToEntity(root);
}

// v11 -> v12. Пост-обработка переехала из НАСТРОЕК ПРОЕКТА в компонент камеры.
//
// ПОЧЕМУ ПОМЕНЯЛОСЬ. Экспозиция, свечение, цвет, тон-маппинг и виньетка жили в
// `sage.cfg` — то есть были настройкой проекта, общей для всех камер сцены
// сразу. Ни сохранить их в сцену, ни положить в префаб, ни откатить через
// Ctrl+Z, ни сделать разный вид у главной камеры и у камеры мини-карты было
// нельзя. Разбор — в комментарии к PostProcessComponent.
//
// ЧТО ДЕЛАЕТ МИГРАЦИЯ. Даёт каждой камере компонент «Пост-обработка» с
// начальным трактом. Без этого сцены, собранные раньше, открылись бы БЕЗ
// обработки вовсе — «все мои сцены стали плоскими и тусклыми», — потому что
// настройка, которая их обрабатывала, теперь не существует.
//
// Камера, у которой компонент уже есть (его добавили до переезда, ключом
// postChain), не трогается: своя настройка важнее умолчания.
void MigrateV11toV12(json& root) {
    const nlohmann::json chain = sage::render::PostChainToJson(sage::render::PostChain::Default());
    for (json& obj : root["objects"]) {
        if (!obj.contains("camera")) continue;
        if (obj.contains("postProcess") || obj.contains("postChain")) continue;
        obj["postProcess"] = chain;
        obj["postProcess"]["enabled"] = true;
    }
}





// v16 -> v17: углы Эйлера — в соглашении R = Ry*Rx*Rz (см. Transform.h).
//
// Было R = Rx*Ry*Rz: наклон X шёл вокруг ГЛОБАЛЬНОЙ оси, и камера, повёрнутая
// по Y, при наклоне заваливалась набок. Поворот каждого объекта и каждой части
// составного коллайдера пересчитывается в новые углы — ТОТ ЖЕ поворот, другие
// числа; у поворота только вокруг одной оси числа не меняются вовсе.
void MigrateV16toV17(json& root) {
    auto convert = [](json& holder, const char* key) {
        if (!holder.contains(key) || !(holder[key].is_object() || holder[key].is_array())) return;
        const glm::vec3 legacy = Vec3FromJson(holder[key]);
        if (legacy == glm::vec3(0.0f)) return;
        holder[key] = Vec3ToJson(sage::EulerDegreesFromLegacyXYZ(legacy));
    };
    for (json& obj : root["objects"]) {
        convert(obj, "rotation");
        if (obj.contains("collider") && obj["collider"].contains("parts") && obj["collider"]["parts"].is_array())
            for (json& part : obj["collider"]["parts"]) convert(part, "euler");
    }
}

// v17 -> v18: твины поворота — в том же соглашении, что и сам поворот
// (v16 -> v17 перевёл Transform, а цели твинов остались в старых углах). Цель
// ставится ближайшей записью к повороту самого объекта: твин «от текущего»
// не должен проворачивать объект через полный оборот.
void MigrateV17toV18(json& root) {
    for (json& obj : root["objects"]) {
        if (!obj.contains("tweens") || !obj["tweens"].is_array()) continue;
        const glm::vec3 own = obj.contains("rotation") ? Vec3FromJson(obj["rotation"]) : glm::vec3(0.0f);
        for (json& clip : obj["tweens"]) {
            if (!clip.contains("tracks") || !clip["tracks"].is_array()) continue;
            for (json& t : clip["tracks"]) {
                if (t.value("property", std::string()) != "object.rotation") continue;
                for (const char* key : {"from", "to"}) {
                    if (!t.contains(key) || !t[key].is_object()) continue;
                    json& v = t[key];
                    const glm::vec3 legacy(v.value("x", 0.0f), v.value("y", 0.0f), v.value("z", 0.0f));
                    if (legacy == glm::vec3(0.0f)) continue;
                    const glm::vec3 now =
                        sage::ClosestEquivalentEuler(sage::EulerDegreesFromLegacyXYZ(legacy), own);
                    v["x"] = now.x;
                    v["y"] = now.y;
                    v["z"] = now.z;
                }
            }
        }
    }
}

// Шаги старых версий, которые переводили ИНТЕРФЕЙС ВНУТРИ СЦЕНЫ (типы
// элементов, девятина, фильтрация, холст корня, объекты-интерфейсы). Интерфейс
// больше в сцене не живёт — его элементы в файлах .sageui, и шаг v18 -> v19
// убирает их из старых сцен целиком; переводить их по дороге незачем.
void Skip(json&) {}

// v18 -> v19: ИНТЕРФЕЙС УШЁЛ ИЗ СЦЕНЫ.
//
// Элементы интерфейса прямо в сцене (и россыпью, и внутри объекта-интерфейса
// без файла) — старый вид: интерфейс теперь отдельный файл .sageui, а в сцене
// стоит один объект со ссылкой на него. Старые элементы убираются вместе с
// потомками; объект-интерфейс остаётся — ему назначают файл в инспекторе.
void MigrateV18toV19(json& root) {
    if (!root.contains("objects")) return;
    json& objects = root["objects"];
    std::unordered_map<int, int> parentOf;
    std::unordered_set<int> doomed;
    for (const json& obj : objects) {
        const int id = obj.value("id", 0);
        if (obj.contains("parent")) parentOf[id] = obj.value("parent", -1);
        if (obj.contains("ui")) doomed.insert(id);
    }
    auto underDoomed = [&](int id) {
        for (int guard = 0; guard < 256; ++guard) {
            auto it = parentOf.find(id);
            if (it == parentOf.end()) return false;
            id = it->second;
            if (doomed.count(id)) return true;
        }
        return false;
    };
    json kept = json::array();
    for (json& obj : objects) {
        const int id = obj.value("id", 0);
        if (doomed.count(id) || underDoomed(id)) continue;
        if (obj.contains("interface") && obj["interface"].is_object()) {
            json& ij = obj["interface"];
            for (const char* k : {"canvasMode", "reference", "matchWidthOrHeight"}) ij.erase(k);
        }
        kept.push_back(std::move(obj));
    }
    objects = std::move(kept);
}

const MigrationFn kMigrations[] = {
    &MigrateV1toV2,
    &MigrateV2toV3,
    &MigrateV3toV4,
    &MigrateV4toV5,
    &Skip,
    &MigrateV6toV7,
    &Skip,
    &Skip,
    &MigrateV9toV10,
    &MigrateV10toV11,
    &MigrateV11toV12,
    &Skip,
    &Skip,
    &Skip,
    &Skip,
    &MigrateV16toV17,
    &MigrateV17toV18,
    &MigrateV18toV19,
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
