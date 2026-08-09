#include "ProjectTemplates.h"

namespace {

// Порядок здесь — порядок в диалоге. Первым идёт то, с чего чаще всего
// начинают: показать, что движок работает. «Пустой» вторым, а не первым,
// намеренно — человек, который знает, что ему нужен пустой, найдёт его и
// вторым пунктом, а тот, кто открыл редактор впервые, не должен получить
// чёрный экран и решить, что всё сломано.
const std::vector<ProjectTemplate>& Table() {
    static const std::vector<ProjectTemplate> table = {
        {"demo", ProjectTemplateKind::Demo, "Demo scene",
         "Primitives, light and a HUD — shows what the engine can do"},
        {"empty", ProjectTemplateKind::Empty, "Empty project",
         "Only the folders and an empty scene"},
        {"ui", ProjectTemplateKind::UIStarter, "Menu and HUD",
         "Camera, light and ready-made interface screens"},
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
