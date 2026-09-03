#include "ProjectTemplates.h"

#include <system_error>

#include "sage/core/Paths.h"

namespace fs = std::filesystem;

namespace {

// Порядок здесь — порядок в диалоге. Первым идёт то, с чего чаще всего
// начинают: показать, что движок работает. «Пустой» вторым, а не первым,
// намеренно — человек, который знает, что ему нужен пустой, найдёт его и
// вторым пунктом, а тот, кто открыл редактор впервые, не должен получить
// чёрный экран и решить, что всё сломано.
//
// «Витрина» — последней и отдельным видом: это не заготовка сцены, а ГОТОВЫЙ
// проект на скриптах, который копируется целиком. С него не начинают свою
// игру — на нём смотрят, КАК сделано, и правят живой код рядом с результатом.
const std::vector<ProjectTemplate>& Table() {
    static const std::vector<ProjectTemplate> table = {
        {"demo", ProjectTemplateKind::Demo, "Demo scene",
         "Primitives, light and a HUD — shows what the engine can do"},
        {"empty", ProjectTemplateKind::Empty, "Empty project",
         "Only the folders and an empty scene"},
        {"ui", ProjectTemplateKind::UIStarter, "Menu and HUD",
         "Camera, light and ready-made interface screens"},
        {"showcase", ProjectTemplateKind::Copy, "Showcase",
         "A whole working project: eight zones on Lua you can walk through and edit",
         "showcase"},
    };
    return table;
}

} // namespace

const std::vector<ProjectTemplate>& ProjectTemplates() { return Table(); }

const ProjectTemplate* FindProjectTemplate(const std::string& id) {
    for (const ProjectTemplate& t : Table())
        if (t.Id == id) return &t;
    return nullptr;
}

const std::string& DefaultProjectTemplate() { return Table().front().Id; }

fs::path ProjectTemplatesRoot() { return sage::ExecutableDir() / "templates"; }

bool ProjectTemplateAvailable(const ProjectTemplate& tpl) {
    if (tpl.Kind != ProjectTemplateKind::Copy) return true;
    std::error_code ec;
    // Проверяется файл проекта, а не просто папка: пустая или недокопированная
    // папка — это отказ, который лучше увидеть в списке шаблонов, чем в виде
    // проекта без сцен.
    return fs::exists(ProjectTemplatesRoot() / tpl.SourceDir / "project.sageproj", ec);
}
