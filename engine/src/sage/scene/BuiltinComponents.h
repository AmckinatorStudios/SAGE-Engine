#pragma once

namespace sage::scene {

// Встроенные компоненты сцены, описанные реестром (sage/scene/ComponentRegistry.h).
// Раньше каждый из них был веткой в сохранении, в чтении и строкой в
// копировании (SceneSerializer.cpp, Prefab.cpp): новый компонент требовал
// правки трёх мест, и пропуск одного молча терял данные в копии или файле.
// Ключи остаются на верхнем уровне объекта (TopLevel) — файлы сцен прежние.
// Зовётся реестром при первом обращении к нему.
void RegisterBuiltinSceneComponents();

} // namespace sage::scene
