#pragma once
#include <functional>
#include <memory>
#include <string>

class EditorHost;
struct Material;

// ---------------------------------------------------------------------------
// ПАРАМЕТРЫ СВОЕГО ШЕЙДЕРА В ИНСПЕКТОРЕ МАТЕРИАЛА.
//
// Список берётся из текста шейдеров материала (render/ShaderParams.h): каждая
// строка uniform с подсказкой становится ползунком, пипеткой или полем. Раньше
// инспектор писал «параметров: 3 (правьте в файле .sagemat)» — то есть нужно
// было знать имена и типы наизусть и перезапускать сцену, чтобы увидеть цвет.
//
// Значение пишется в Material::Params (его заливает движок перед отрисовкой),
// отмена — как у любого поля (TrackLastImGuiItem), на диск — по «Сохранить»
// материала. Текст шейдера читается заново, только если файл изменился: правка
// шейдера на лету добавляет новый ползунок без перезапуска.
// ---------------------------------------------------------------------------
namespace sage::editor::shaderparamsui {

// Рисует параметры. Возвращает число показанных (0 — шейдера нет или он без
// параметров). browseTexture — «Обзор…» у слота текстуры: открыть диалог и
// записать выбор в переданный путь (диалог один на панель, он у инспектора).
int Draw(EditorHost& host, const std::shared_ptr<Material>& material,
         const std::function<void(std::string& target)>& browseTexture = {});

} // namespace sage::editor::shaderparamsui

namespace sage::editor::shaderparamsui {
// Ставит слот шейдера экрана в редактор пост-обработки (PostChainUi.h):
// выбор .frag слотом ассета и «+» — создать шейдер экрана из шаблона.
void InstallPostSourceSlot();
} // namespace sage::editor::shaderparamsui
