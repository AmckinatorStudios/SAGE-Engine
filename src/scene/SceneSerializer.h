#pragma once
#include "Scene.h"
#include <nlohmann/json_fwd.hpp>
#include <functional>
#include <memory>
#include <string>

// Сохраняет/загружает Scene в человекочитаемый JSON-файл.
// Сохраняются: имя сцены, все объекты (имя, transform, ссылка на меш, цвет,
// тег) и освещение. GPU-ресурсы (Mesh) при загрузке пересоздаются через
// AssetManager.
//
// Свободные данные объекта (GameObject::LuaData — Lua-таблица в std::any)
// движковое ядро сериализовать само не может: для этого нужен слой Lua/sol
// (перевод sol::table <-> JSON), живущий в ScriptEngine. Поэтому Save/Load/
// LoadInto принимают необязательный набор хуков (Hooks): если игра хочет,
// чтобы LuaData попадала в файл сцены, ScriptEngine передаёт хуки,
// конвертирующие её через свои LuaValueToJson/JsonToLuaValue (см.
// ScriptEngine::SaveScene/LoadScene). Без хуков LuaData просто пропускается —
// сцена сохраняется/грузится без неё, как раньше.
namespace SceneSerializer {

    // Необязательные хуки (де)сериализации свободных данных объекта.
    // SaveLuaData вызывается на каждый объект при Save и возвращает JSON его
    // LuaData (или null/пустой, чтобы пропустить) — сериализатор кладёт его
    // под ключ "lua". LoadLuaData вызывается при загрузке с этим JSON и
    // выставляет obj.LuaData. Любой из хуков может быть пустым.
    struct Hooks {
        std::function<nlohmann::json(const GameObject&)> SaveLuaData;
        std::function<void(GameObject&, const nlohmann::json&)> LoadLuaData;
    };

    void Save(const Scene& scene, const std::string& path, const Hooks& hooks = {});

    // Возвращает загруженную сцену. Бросает std::runtime_error при ошибке чтения/парсинга.
    std::unique_ptr<Scene> Load(const std::string& path, const Hooks& hooks = {});

    // Загружает сцену ПОВЕРХ существующей (очищает объекты/освещение и
    // заполняет заново на месте), сохраняя валидность внешних ссылок на этот
    // Scene — в отличие от Load, который отдаёт новый объект. Нужен, чтобы
    // перезагрузить привязанную к скриптам/рендеру сцену без переустановки
    // всех ссылок. Бросает std::runtime_error при ошибке чтения/парсинга.
    void LoadInto(Scene& scene, const std::string& path, const Hooks& hooks = {});
}
