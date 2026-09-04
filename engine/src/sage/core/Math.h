#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

// ---------------------------------------------------------------------------
// Математика движка — вектор, матрица, кватернион.
//
// ЧТО ЭТО. Тонкий слой имён поверх glm, а не своя математическая библиотека.
// Реализация — glm, и это осознанно: писать свои вектора ради чистоты значило
// бы получить менее проверенный код в самом горячем месте движка.
//
// ЗАЧЕМ ТОГДА ЭТОТ ФАЙЛ. Чтобы игре не приходилось ЗНАТЬ про glm, чтобы
// получить вектор. Раньше начало любой игры выглядело так:
//
//     #include <glm/glm.hpp>
//     glm::vec3 pos;
//
// то есть автор игры был обязан выяснить, что внутри движка стоит glm, и
// подключить её к себе. Теперь достаточно `#include <sage/Sage.h>` и
// `sage::Vec3` — а чем оно реализовано, остаётся делом движка.
//
// ЧЕГО ЭТОТ СЛОЙ НЕ ДЕЛАЕТ. Не прячет glm и не запрещает её. Псевдонимы —
// именно псевдонимы: sage::Vec3 И ЕСТЬ glm::vec3, поэтому весь существующий
// код движка, редактора и игр продолжает работать без единой правки, а
// смешивать оба имени в одном файле совершенно законно. Обещать «когда-нибудь
// заменим glm незаметно для игр» было бы враньём: типы совпадают, и подмена
// реализации потребовала бы совместимого API. Ценность здесь не в возможности
// подмены, а в том, что игре не нужно подключать чужую библиотеку ради самой
// частой строчки в коде.
// ---------------------------------------------------------------------------
namespace sage {

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using IVec2 = glm::ivec2;
using IVec3 = glm::ivec3;
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;
using Quat = glm::quat;

// Часто используемые операции — под теми же именами, чтобы в игровом коде не
// чередовались sage:: и glm:: в соседних строках.
using glm::clamp;
using glm::cross;
using glm::distance;
using glm::dot;
using glm::inverse;
using glm::length;
using glm::mix;
using glm::normalize;
using glm::radians;
using glm::degrees;
using glm::rotate;
using glm::scale;
using glm::translate;
using glm::transpose;

// Углы Эйлера (градусы) -> кватернион и обратно. В движке трансформы хранят
// углы в градусах (см. sage/scene/Transform.h), а поворот считается
// кватернионом — перевод нужен постоянно, и делать его каждый раз руками
// значит однажды перепутать порядок осей.
inline Quat QuatFromEulerDegrees(const Vec3& degrees) {
    return glm::quat(glm::radians(degrees));
}
inline Vec3 EulerDegreesFromQuat(const Quat& q) {
    return glm::degrees(glm::eulerAngles(q));
}

} // namespace sage
