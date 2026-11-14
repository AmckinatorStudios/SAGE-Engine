// ---------------------------------------------------------------------------
// ДОКУМЕНТ ИНТЕРФЕЙСА — то, что правит пространство «Интерфейс».
//
// Интерфейс живёт в своём файле .sageui, а в сцене стоит ОДНИМ объектом со
// ссылкой на него (sage/ui/InterfaceSource.h). Раньше пространство «Интерфейс»
// правило саму сцену: элементы меню были её объектами, и HUD одного уровня
// во втором уровне был уже другим HUD — копией, расходящейся с оригиналом с
// первой правки.
//
// Как устроено. Документ — отдельная сцена с одним объектом-интерфейсом и его
// главным контейнером. Пока на экране пространство «Интерфейс», документ
// стоит на месте m_scene, а уровень ждёт в m_uiDocOther (SwapInterfaceDocument).
// Поэтому создание, удаление, дублирование, отмена и инспектор работают с
// документом тем же кодом, что и со сценой, а уровень правок вёрстки не видит.
// ---------------------------------------------------------------------------
#include "EditorLayer.h"

#include <filesystem>

#include "Localization.h"

#include "sage/assets/AssetDatabase.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/scene/Components.h"
#include "sage/ui/Interface.h"
#include "sage/ui/InterfaceSource.h"
#include "sage/ui/UI.h"

namespace fs = std::filesystem;

namespace {

// Свободное имя файла в папке интерфейсов проекта: «Menu.sageui», «Menu 2.sageui»…
fs::path FreeInterfacePath(const fs::path& dir, const std::string& name) {
    const std::string base = name.empty() ? std::string("Interface") : name;
    std::error_code ec;
    for (int i = 1; i < 10000; ++i) {
        const std::string stem = i == 1 ? base : base + " " + std::to_string(i);
        const fs::path p = dir / sage::PathFromUtf8(stem + sage::ui::kInterfaceExtension);
        if (!fs::exists(p, ec)) return p;
    }
    return dir / sage::PathFromUtf8(base + sage::ui::kInterfaceExtension);
}

// Документ из ресурса: сцена с одним объектом-интерфейсом. Возвращает id
// объекта-интерфейса через out.
std::unique_ptr<Scene> BuildDocument(const sage::ui::Interface& ui, int& interfaceId) {
    auto doc = std::make_unique<Scene>(ui.Name);
    const entt::entity iface = sage::ui::Instantiate(*doc, ui);
    const IdComponent* id = doc->Registry().try_get<IdComponent>(iface);
    interfaceId = id ? id->Id : -1;
    return doc;
}

} // namespace

// Поменять местами то, что на месте m_scene, и то, что ждёт в стороне. Вместе
// со сценой меняются её история, флаг изменённости и выбранный интерфейс:
// Ctrl+Z в вёрстке не имеет права откатывать уровень, и наоборот.
void EditorLayer::SwapInterfaceDocument() {
    std::swap(m_scene, m_uiDocOther);
    std::swap(m_history, m_uiDocHistory);
    std::swap(m_sceneDirty, m_uiDocDirty);
    std::swap(m_currentInterface, m_uiDocInterface);
    m_uiDocActive = !m_uiDocActive;
    // Выделение — номера объектов ТОЙ сцены, и в другой они значат чужое.
    m_selection.Clear();
    m_scenePtr = m_scene.get();
    UpdateWindowTitle();
}

void EditorLayer::AdoptInterfaceDocument(std::unique_ptr<Scene> doc, const std::string& path) {
    CloseInterfaceDocument();
    // Новый документ ложится «в сторону», и если пространство уже открыто —
    // сразу встаёт на место.
    m_uiDocOther = std::move(doc);
    m_uiDocHistory.Clear();
    m_uiDocDirty = false;
    m_uiDocPath = path;
    m_uiDocOpen = true;
    if (m_workspace == EditorWorkspace::Interface) SwapInterfaceDocument();
}

void EditorLayer::CloseInterfaceDocument(bool save) {
    if (!m_uiDocOpen) return;
    // Правки не теряются молча: у документа с файлом — в его файл; без файла —
    // в папку интерфейсов проекта, если проект есть.
    if (save && InterfaceDocumentDirty() && (!m_uiDocPath.empty() || m_project.Loaded()))
        SaveInterfaceDocument();
    if (m_uiDocActive) SwapInterfaceDocument();
    m_uiDocOther.reset();
    m_uiDocHistory.Clear();
    m_uiDocDirty = false;
    m_uiDocInterface = -1;
    m_uiDocOpen = false;
    m_uiDocPath.clear();
}

bool EditorLayer::OpenInterface(const std::string& assetRef) {
    if (m_uiDocOpen && !assetRef.empty() && assetRef == m_uiDocPath) {
        SetWorkspace(EditorWorkspace::Interface);
        return true;
    }
    sage::ui::Interface ui;
    std::string err;
    const std::string real = sage::AssetDatabase::Instance().LocatePath(assetRef);
    if (!sage::ui::Interface::LoadFile(real, ui, err)) {
        LOG_ERROR("Editor") << "Интерфейс не открылся (" << assetRef << "): " << err;
        SetStatusMessage(T("The interface could not be opened: ") + assetRef);
        return false;
    }
    int interfaceId = -1;
    std::unique_ptr<Scene> doc = BuildDocument(ui, interfaceId);
    // Сначала выйти в пространство сцены не нужно: AdoptInterfaceDocument сам
    // уберёт прежний документ и поставит новый, если вёрстка уже на экране.
    AdoptInterfaceDocument(std::move(doc), assetRef);
    if (m_uiDocActive) m_currentInterface = interfaceId;
    else m_uiDocInterface = interfaceId;
    SetWorkspace(EditorWorkspace::Interface);
    LOG_INFO("Editor") << "Интерфейс открыт: " << assetRef;
    return true;
}

void EditorLayer::NewInterfaceDocument() {
    sage::ui::Interface ui;   // главный контейнер — по умолчанию
    int interfaceId = -1;
    std::unique_ptr<Scene> doc = BuildDocument(ui, interfaceId);
    AdoptInterfaceDocument(std::move(doc), std::string());
    if (m_uiDocActive) m_currentInterface = interfaceId;
    else m_uiDocInterface = interfaceId;
}

bool EditorLayer::SaveInterfaceDocument() {
    if (!m_uiDocOpen) return false;
    Scene& doc = m_uiDocActive ? *m_scene : *m_uiDocOther;
    const int interfaceId = m_uiDocActive ? m_currentInterface : m_uiDocInterface;
    GameObject iface = doc.Get(interfaceId);
    if (!iface.Valid()) return false;
    sage::ui::Interface ui = sage::ui::Capture(doc, iface.Entity());

    // Документ без файла получает его в папке интерфейсов проекта — по имени
    // интерфейса. Спрашивать имя файла незачем: имя уже дано, и файл можно
    // переименовать в панели ассетов, ссылки пойдут за ним (GUID).
    std::string ref = m_uiDocPath;
    // Путь на диске: у нового файла — прямо в папке проекта (LocatePath
    // находит только то, что уже лежит на диске).
    std::string real;
    if (ref.empty()) {
        if (!m_project.Loaded()) {
            SetStatusMessage(T("Open a project to save the interface"));
            return false;
        }
        const fs::path dir = m_project.AssetsDir() / "ui";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path file = FreeInterfacePath(dir, ui.Name);
        ref = m_project.AssetRef(file);
        real = sage::PathToUtf8(file);
    } else {
        real = sage::AssetDatabase::Instance().LocatePath(ref);
    }
    std::string err;
    if (!ui.SaveFile(real, err)) {
        LOG_ERROR("Editor") << "Интерфейс не сохранён (" << ref << "): " << err;
        SetStatusMessage(T("The interface was not saved: ") + err);
        return false;
    }
    m_uiDocPath = ref;
    if (m_uiDocActive) m_sceneDirty = false;
    else m_uiDocDirty = false;
    // Сцены, где стоит этот интерфейс, обязаны показать новый вариант сразу,
    // а не после проверки диска: время записи файла на части систем меняется
    // раз в секунду, и правка внутри той же секунды осталась бы незамеченной.
    sage::ui::ClearInterfaceSourceCache();
    UpdateWindowTitle();
    LOG_INFO("Editor") << "Интерфейс сохранён: " << ref;
    SetStatusMessage(T("Interface saved: ") + fs::path(sage::PathFromUtf8(ref)).filename().string());
    return true;
}

bool EditorLayer::OpenInterfaceObject(int objectId) {
    // Объект ищется в УРОВНЕ, а он может стоять в стороне.
    Scene& level = m_uiDocActive ? *m_uiDocOther : *m_scene;
    GameObject obj = level.Get(objectId);
    if (!obj.Valid()) return false;
    sage::ui::InterfaceComponent* ic =
        level.Registry().try_get<sage::ui::InterfaceComponent>(obj.Entity());
    if (!ic) return false;
    if (!ic->Source.empty()) return OpenInterface(ic->Source);

    // ФАЙЛА ЕЩЁ НЕТ — заводим пустой интерфейс с именем объекта и ставим
    // ссылку на него: «Создать и открыть» одним шагом отмены.
    if (m_uiDocActive) SetWorkspace(EditorWorkspace::Scene);
    if (!m_project.Loaded()) {
        SetStatusMessage(T("Open a project to save the interface"));
        return false;
    }
    sage::ui::Interface ui;
    ui.Name = obj.Name();
    ui.Root = sage::ui::Interface::MainContainerNode(ui.Name);
    const fs::path dir = m_project.AssetsDir() / "ui";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = FreeInterfacePath(dir, ui.Name);
    const std::string ref = m_project.AssetRef(file);
    std::string err;
    if (!ui.SaveFile(sage::PathToUtf8(file), err)) {
        SetStatusMessage(T("The interface was not saved: ") + err);
        return false;
    }
    PushUndoSnapshot();
    m_scene->Registry().get<sage::ui::InterfaceComponent>(obj.Entity()).Source = ref;
    sage::ui::SyncInterfaceSources(*m_scene);
    MarkSceneDirty();
    LOG_INFO("Editor") << "Интерфейс создан: " << ref;
    return OpenInterface(ref);
}
