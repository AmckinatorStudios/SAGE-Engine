#pragma once
#include <string>
#include <vector>

// Список недавних проектов редактора. Хранится в JSON в домашней папке
// пользователя (~/.config/sage/recent_projects.json; на Windows —
// %APPDATA%/sage/...; если домашняя папка не определяется — рядом с
// бинарником), чтобы переживал переустановку/пересборку редактора.
class RecentProjects {
public:
    // Загружает список с диска (отсутствие файла — не ошибка: пустой список).
    void Load();

    // Добавляет путь в начало списка (существующий — поднимает наверх),
    // обрезает до 10 записей и сразу сохраняет на диск.
    void Add(const std::string& projectDir);

    // Убирает запись (например, проект удалён с диска) и сохраняет.
    void Remove(const std::string& projectDir);

    const std::vector<std::string>& List() const { return m_paths; }

    // Путь файла-хранилища (для логов/тестов).
    static std::string StoragePath();

private:
    void Save() const;

    std::vector<std::string> m_paths;
};
