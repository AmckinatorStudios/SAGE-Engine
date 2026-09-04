#include "ProjectTemplates.h"

#include <system_error>

#include "TemplateStore.h"
#include "sage/core/Paths.h"

namespace fs = std::filesystem;

namespace {

// ВСТРОЕННЫЕ шаблоны — те, что собираются КОДОМ и потому не весят ничего.
// Порядок здесь — порядок в диалоге. Первым идёт то, с чего чаще всего
// начинают: показать, что движок работает. «Пустой» вторым, а не первым,
// намеренно — человек, который знает, что ему нужен пустой, найдёт его и
// вторым пунктом, а тот, кто открыл редактор впервые, не должен получить
// чёрный экран и решить, что всё сломано.
//
// «Витрины» здесь БОЛЬШЕ НЕТ, и это главное изменение. Готовый проект — это
// содержимое, а не часть программы: он лежал внутри сборки и ехал в архиве к
// каждому скачавшему, нужен тот ему или нет, а добавить свой такой же было
// нельзя без пересборки редактора. Теперь готовые проекты ставятся отдельно
// (см. TemplateStore.h) и подхватываются кодом ниже.
std::vector<ProjectTemplate> BuiltIn() {
    return {
        {"demo", ProjectTemplateKind::Demo, "Demo scene",
         "Primitives, light and a HUD — shows what the engine can do"},
        {"empty", ProjectTemplateKind::Empty, "Empty project",
         "Only the folders and an empty scene"},
        {"ui", ProjectTemplateKind::UIStarter, "Menu and HUD",
         "Camera, light and ready-made interface screens"},
    };
}

// Список = встроенные + установленные. Кэшируется: его спрашивают каждый кадр
// при открытом окне создания проекта, а обход каталога на диске — не то, что
// делают шестьдесят раз в секунду. Сбрасывается RefreshProjectTemplates().
std::vector<ProjectTemplate> Build() {
    std::vector<ProjectTemplate> table = BuiltIn();
    for (const sage::editor::templates::Manifest& m : sage::editor::templates::Installed()) {
        ProjectTemplate t;
        t.Id = m.Id;
        t.Kind = ProjectTemplateKind::Copy;
        t.Name = m.Name;
        t.Summary = m.Summary;
        t.Note = m.Note;
        t.SourceDir = m.Id;   // папка внутри templates/ называется как шаблон
        table.push_back(std::move(t));
    }
    return table;
}

std::vector<ProjectTemplate>& Cache() {
    static std::vector<ProjectTemplate> table = Build();
    return table;
}

} // namespace

const std::vector<ProjectTemplate>& ProjectTemplates() { return Cache(); }

void RefreshProjectTemplates() { Cache() = Build(); }

const ProjectTemplate* FindProjectTemplate(const std::string& id) {
    for (const ProjectTemplate& t : Cache())
        if (t.Id == id) return &t;
    return nullptr;
}

const std::string& DefaultProjectTemplate() { return Cache().front().Id; }

fs::path ProjectTemplatesRoot() { return sage::editor::templates::Root(); }

bool ProjectTemplateAvailable(const ProjectTemplate& tpl) {
    if (tpl.Kind != ProjectTemplateKind::Copy) return true;
    std::error_code ec;
    // Проверяется файл проекта, а не просто папка: пустая или недокопированная
    // папка — это отказ, который лучше увидеть в списке шаблонов, чем в виде
    // проекта без сцен.
    return fs::exists(ProjectTemplatesRoot() / tpl.SourceDir / "project.sageproj", ec);
}
