#pragma once
#include <string>
#include <vector>

#include "../FileBrowser.h"
#include "../TemplateStore.h"

class EditorHost;

// ---------------------------------------------------------------------------
// ОКНО ШАБЛОНОВ: что установлено, что можно скачать, как поставить своё.
//
// Заведено вместе с решением не возить готовые проекты внутри сборки (см.
// TemplateStore.h). Раньше вопрос «а где взять витрину» не имел ответа в самом
// редакторе: она либо была рядом с ним, либо её не было никогда, и список
// шаблонов молча показывал «не установлен» без единого способа это исправить.
//
// Три источника, и все три — здесь:
//   • КАТАЛОГ — список по ссылке, по умолчанию из релиза этого же движка.
//     Ссылка меняется: каталог это просто JSON, и держать свой (у студии, в
//     локальной сети) должно быть можно без правки редактора.
//   • ФАЙЛ .sagetemplate с диска — на случай, когда сети нет вовсе, и для
//     шаблонов, которые никто никуда не выкладывал.
//   • ТЕКУЩИЙ ПРОЕКТ — «сохранить как шаблон». Тем же файлом, каким шаблоны и
//     ставятся: «поделиться заготовкой» и «поставить чужую» — одно действие с
//     разных сторон.
//
// Скачивание СИНХРОННОЕ, и это осознанно. Асинхронная загрузка потребовала бы
// потока, отмены и состояния «наполовину поставлено» — ради файла, который
// качается секунду. Пока идёт загрузка, окно показывает, чем занято.
// ---------------------------------------------------------------------------
class TemplatesPanel {
public:
    void Draw(EditorHost& host, bool& open);

    // Обновить списки с диска. Зовётся при открытии окна и после установки.
    void Refresh();

private:
    void DrawInstalled(EditorHost& host);
    void DrawCatalog(EditorHost& host);
    void DrawSources(EditorHost& host);
    void Say(EditorHost& host, const std::string& message, bool bad);

    std::vector<sage::editor::templates::Manifest> m_installed;
    std::vector<sage::editor::templates::Manifest> m_catalog;
    bool m_catalogLoaded = false;
    std::string m_status;        // последняя строка о результате
    bool m_statusBad = false;
    bool m_loaded = false;       // читали ли диск хоть раз

    FileBrowser m_browser;
    enum class Pending { None, InstallFile, InstallFolder, SaveAs };
    Pending m_pending = Pending::None;

    char m_catalogUrl[512] = {0};
    char m_saveId[64] = {0};
    char m_saveName[128] = {0};
    bool m_urlEdited = false;
};
