#pragma once
#include <filesystem>
#include <string>

// ---------------------------------------------------------------------------
// НОВЫЙ ШЕЙДЕР — окно «Создать → Шейдер».
//
// Создаётся ПУСТОЙ шейдер: соблюдён только контракт движка (локации атрибутов,
// инстансные данные, выход), а что он рисует — решает автор. Готовых эффектов
// («вода», «голограмма», «пиксель-арт») здесь нет и не будет: движок —
// инструмент, а не набор готовых решений (CLAUDE.md, «Движок — инструмент»).
//
// Material — пара .vert/.frag в assets/shaders и .sagemat в assets/materials,
// который на неё ссылается: материал-шейдер, который сразу кладут на объект.
// Screen — один .frag для звена «Свой шейдер» пост-обработки камеры.
// ---------------------------------------------------------------------------
namespace sage::editor::newshader {

enum class Kind { Material, Screen };

// Файлы для имени name. projectDir — корень проекта (пути в .sagemat — от
// него), assetsDir — папка assets. created — что выделить и куда назначить:
// .sagemat у материала, .frag у экрана. Существующие файлы не перезаписывает.
bool Create(Kind kind, const std::string& name, const std::filesystem::path& projectDir,
            const std::filesystem::path& assetsDir, std::filesystem::path& created, std::string& error);

// Тексты пустых шейдеров (для проверки, что они собираются).
const char* MaterialVertex();
const char* MaterialFragment();
const char* ScreenFragment();

} // namespace sage::editor::newshader
