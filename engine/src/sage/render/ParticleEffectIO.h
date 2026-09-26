#pragma once
#include <string>

#include "sage/render/ParticleEffect.h"

// ---------------------------------------------------------------------------
// Эффект частиц ⇄ текст JSON.
//
// Один формат на два места: эффект, встроенный в объект сцены (.sage), и
// эффект отдельным файлом (.sagefx) — тот, что кладут в проект, передают
// между сценами и цепляют дочерним эмиттером. Разойдись эти форматы, и файл,
// сохранённый из инспектора, перестал бы открываться там же.
//
// Строки, а не объект JSON: тип библиотеки JSON не торчит из заголовка движка.
//
// Чтение ТЕРПИМО: отсутствующее поле — значение по умолчанию, лишнее —
// пропускается. Файл, записанный более новым движком, откроется со всем, что
// этот движок понимает, а не упадёт целиком.
// ---------------------------------------------------------------------------
namespace sage::fx {

std::string EffectToJson(const ParticleEffect& fx);
// false — текст не JSON или не объект; причина — в err.
bool EffectFromJson(const std::string& text, ParticleEffect& out, std::string* err = nullptr);

// Файл .sagefx. Путь — настоящий путь на диске (ссылку проекта разрешает
// вызывающий или ParticleSystem).
bool SaveEffectFile(const std::string& path, const ParticleEffect& fx, std::string* err = nullptr);
bool LoadEffectFile(const std::string& path, ParticleEffect& out, std::string* err = nullptr);

// Расширение файла эффекта (с точкой).
inline const char* EffectFileExtension() { return ".sagefx"; }

} // namespace sage::fx
