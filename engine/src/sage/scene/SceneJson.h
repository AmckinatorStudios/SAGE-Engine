#pragma once
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

// ---------------------------------------------------------------------------
// Векторы в JSON и обратно — общий словарь формата сцены.
//
// Отдельным заголовком, потому что этими четырьмя парами пользуются ДВА файла:
// сериализатор текущего формата (SceneSerializer.cpp) и миграции старых
// (SceneMigrations.cpp). Пока они были статиками первого, второй не мог их
// взять — и либо повторил бы их у себя (два словаря одного формата, которые
// однажды разойдутся), либо остался бы внутри первого навсегда.
//
// ИМЕНОВАННЫЕ ПОЛЯ, а не массив из трёх чисел. Сцена — текстовый файл, который
// читают и правят руками (в том числе при разборе конфликтов слияния), и
// {"x":0,"y":1,"z":0} говорит, что это, а [0,1,0] — нет.
//
// ЗНАЧЕНИЕ ПО УМОЛЧАНИЮ У КАЖДОГО ПОЛЯ ОТДЕЛЬНО (value("x", def.x)): в старом
// файле поля может не быть вовсе, и «нет z» обязано означать «z по умолчанию»,
// а не «весь вектор по умолчанию».
// ---------------------------------------------------------------------------
namespace sage::scene {

inline nlohmann::json Vec2ToJson(const glm::vec2& v) {
    return nlohmann::json{{"x", v.x}, {"y", v.y}};
}

inline glm::vec2 Vec2FromJson(const nlohmann::json& j, const glm::vec2& fallback) {
    if (!j.is_object()) return fallback;
    return {j.value("x", fallback.x), j.value("y", fallback.y)};
}

inline nlohmann::json Vec3ToJson(const glm::vec3& v) {
    return nlohmann::json{{"x", v.x}, {"y", v.y}, {"z", v.z}};
}

inline glm::vec3 Vec3FromJson(const nlohmann::json& j) {
    return glm::vec3(j.value("x", 0.0f), j.value("y", 0.0f), j.value("z", 0.0f));
}

inline nlohmann::json Vec4ToJson(const glm::vec4& v) {
    return nlohmann::json{{"x", v.x}, {"y", v.y}, {"z", v.z}, {"w", v.w}};
}

inline glm::vec4 Vec4FromJson(const nlohmann::json& j, const glm::vec4& def = glm::vec4(1.0f)) {
    return glm::vec4(j.value("x", def.x), j.value("y", def.y),
                     j.value("z", def.z), j.value("w", def.w));
}

} // namespace sage::scene
