#pragma once
#include <string>

class EditorHost;

// Панель модальных диалогов File-меню: New Project / Open Project / Save Scene
// As / Build Game / Open Scene. Владеет СВОИМ UI-состоянием (буферы полей ввода,
// текст ошибки, путь готовой сборки) — раньше эти m_dlg*-поля жили god-object'ом
// в EditorLayer. Операции (создать/открыть проект, сохранить/загрузить сцену,
// собрать игру) выполняются через EditorHost.
//
// ВАЖНО: Open() и Draw() должны вызываться на уровне ОКНА-ХОСТА (там же, где
// меню зовёт их) — модалка ImGui совпадает с OpenPopup по ID-стеку окна, поэтому
// открытие и отрисовку нельзя разносить по разным окнам.
class DialogsPanel {
public:
    // Инициализирует дефолтные пути полей: New Project — рядом с бинарником
    // (cwd), Build Game — в cwd/dist. Раньше это делал EditorLayer::OnAttach.
    DialogsPanel();

    // Открывает именованную модалку: сбрасывает прошлую ошибку (а для
    // "Build Game" — и прошлый результат сборки) и зовёт ImGui::OpenPopup.
    void Open(const char* name);

    // Рисует все модалки (активна лишь ранее открытая через Open).
    void Draw(EditorHost& host);

private:
    char m_projectName[128] = "MyGame";
    char m_projectDir[512] = "";
    char m_openPath[512] = "";
    char m_sceneName[128] = "level1";
    char m_buildDir[512] = "";
    std::string m_error;       // сообщение об ошибке текущей модалки
    std::string m_buildResult; // путь готовой сборки (успех Build Game)
};
