#pragma once
#include <string>

class EditorHost;
class RecentProjects;

// Стартовое окно проектов (hub): показывается по центру, пока проект не
// открыт — список недавних проектов + создание нового + открытие по пути.
// Редактор остаётся доступным позади (можно работать и без проекта — например
// с одиночной сценой), окно лишь настойчиво предлагает выбрать проект.
class LauncherPanel {
public:
    // Рисует окно. Возвращает true, пока окно нужно показывать.
    void Draw(EditorHost& host, RecentProjects& recent);

    bool Dismissed() const { return m_dismissed; }
    void Dismiss() { m_dismissed = true; } // headless-режимы (AUTOPLAY) скрывают hub

private:
    char m_newName[128] = "MyGame";
    char m_newDir[512] = "";
    char m_openPath[512] = "";
    std::string m_error;
    bool m_dismissed = false; // «Skip» — работать без проекта
};
