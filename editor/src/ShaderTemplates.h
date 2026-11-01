#pragma once
#include <filesystem>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// ШАБЛОНЫ ШЕЙДЕРОВ окна «Создать → Шейдер».
//
// Шейдер с нуля пишут единицы: остальным нужен работающий файл, в котором
// уже соблюдён контракт движка (локации атрибутов, инстансные данные, блок
// PBR, uTime) и видно, где менять. Шаблон даёт ровно это. Параметры объявлены
// строками uniform с подсказкой в комментарии — инспектор материала сам
// покажет их ползунками и пипетками (render/ShaderParams.h).
//
// Шаблон материала создаёт три файла: .vert, .frag (в assets/shaders) и
// .sagemat (в assets/materials), который на них ссылается, — материал можно
// сразу положить на объект. Шаблон экрана — один .frag для звена «Свой шейдер»
// пост-обработки камеры.
// ---------------------------------------------------------------------------
namespace sage::editor::shadertemplates {

enum class Target { Material, Screen };

struct Template {
    const char* Id;
    const char* Title;   // ключ перевода
    const char* Note;    // ключ перевода
    const char* Icon;
    Target Kind = Target::Material;
    const char* Vertex = nullptr;    // только у материала
    const char* Fragment = nullptr;
    // Поля .sagemat (кроме путей к шейдерам и параметров): JSON-фрагмент.
    const char* MaterialJson = nullptr;
};

const std::vector<Template>& All();
const Template* Find(const std::string& id);

// Создаёт файлы шаблона с именем name. assetsDir — папка assets проекта,
// projectDir — корень проекта (пути в .sagemat — от него). created — что
// выделить и куда назначить: .sagemat у материала, .frag у экрана.
bool Create(const Template& t, const std::string& name, const std::filesystem::path& projectDir,
            const std::filesystem::path& assetsDir, std::filesystem::path& created, std::string& error);

} // namespace sage::editor::shadertemplates
