#pragma once
// ---------------------------------------------------------------------------
// Общая оснастка эталонных кадров: сцена, камера, проход рендера и учёт
// результатов.
//
// БЫЛА анонимным пространством имён внутри main.cpp на две с половиной тысячи
// строк, где рядом лежали проверка отражений, кэш ассетов и соответствие RHI.
// Сам факт, что оснастка была анонимной, и держал файл целым: разъехаться по
// файлам он не мог, потому что MakeScene и RenderFrame видны только в своей
// единице трансляции.
//
// Теперь это обычный заголовок, и наборы проверок лежат по темам (Frame,
// Reflections, Shadows, Scene). Счётчик пройденного тоже переехал сюда: он
// один на прогон, и складывать его из четырёх файлов иначе было бы нечем.
// ---------------------------------------------------------------------------
#include <memory>
#include <string>

#include <glm/glm.hpp>

#include "RenderTestHarness.h"

#include "sage/ecs/RenderBatch.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/GridRenderer.h"
#include "sage/render/PostFX.h"
#include "sage/render/ShadowMap.h"
#include "sage/scene/Scene.h"

namespace sage::rendertest {

// --- Учёт результатов -------------------------------------------------------
void Report(const std::string& name, const Comparison& c);
void Check(bool condition, const char* what);

// Для проверок, которые печатают свою строку сами: они меряют число со своим
// смыслом, а не сравнивают картинку, но в общий итог входят наравне.
void CountPass();
void CountFail();

int PassedCount();
int FailedCount();
int WrittenCount();

// --- Сцена и камера ---------------------------------------------------------
std::unique_ptr<Scene> MakeScene();

extern const glm::vec3 kEye;
extern const glm::vec3 kTarget;
glm::mat4 TestView();

constexpr int kW = 320;
constexpr int kH = 240;

sage::render::PostFXSettings BaseSettings();
glm::mat4 PerspectiveProj();

// Владелец GPU-объектов прохода. Именно ВЛАДЕЛЕЦ, а не набор статиков внутри
// функции: статик разрушился бы после выхода из main, когда графического
// контекста уже нет, — и удаление буферов упало бы (проверено падением).
struct FrameRenderer {
    sage::ecs::RenderBatch Batch;
    ShadowMap Shadow{1024};
    sage::render::PostFX Fx;
    sage::render::GridRenderer Grid;
};

Image RenderFrame(FrameRenderer& r, Scene& scene, const glm::mat4& proj,
                  const sage::render::PostFXSettings& fx, int width, int height,
                  const sage::render::GridSettings* grid = nullptr,
                  float shadowRadius = 12.0f);

// --- Наборы проверок --------------------------------------------------------
// Объявлены здесь, а вызываются из main: порядок прогона задаётся в одном
// месте, и добавить набор — это одна строка там же, а не поиск по файлам.
void RunFrameChecks(FrameRenderer& r, Scene& scene);        // кадр, пост-обработка, сглаживание
void RunAnimationChecks(FrameRenderer& r);                  // морфинг и обратная кинематика
void RunReflectionChecks(FrameRenderer& r);                 // отражения и блик
void RunShadowChecks(FrameRenderer& r, Scene& scene);       // тени: мягкость, каскады, лампы
void RunSceneChecks(FrameRenderer& r);                      // уровни детализации, кэш, небо, RHI
void RunUIChecks();                                         // интерфейс: части рисуют себя и только себя
void RunMaterialChecks(FrameRenderer& r);                   // материал: от файла до пикселей
void RunVolumetricChecks();                                 // объёмный свет: зерно, полосы, кайма

} // namespace sage::rendertest
