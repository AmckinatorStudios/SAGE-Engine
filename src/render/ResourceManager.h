#pragma once
#include "Mesh.h"
#include "ModelLoader.h"
#include "Texture.h"
#include "../scene/Transform.h" // подключаем заранее не обязательно, но пусть будет явный порядок
#include "../core/JobSystem.h"
#include "../core/MainThreadDispatcher.h"
#include "../core/AsyncResource.h"
#include <unordered_map>
#include <memory>
#include <string>
#include <exception>

// Простой кэш ресурсов. На вход — описание (тип + путь), на выход — готовый Mesh.
// Один и тот же .obj, запрошенный дважды, не грузится с диска повторно.
//
// Асинхронные варианты (*Async) грузят ресурс, не блокируя кадр: CPU-часть
// (чтение/декод файла) уходит в JobSystem, а GL-загрузка возвращается в
// главный поток через MainThreadDispatcher. Результат приходит через
// AsyncResource<T> — владелец раз в кадр смотрит IsReady()/Get(). Чтобы это
// работало, в главном цикле должно быть:
//   JobSystem::Instance().Start();               // при старте
//   MainThreadDispatcher::Instance().Drain(...);  // раз в кадр
//
// ВНИМАНИЕ: все методы ResourceManager вызываются из ГЛАВНОГО потока (кэш и
// список незавершённых загрузок трогает только он — поэтому без блокировок).
// В фоне выполняется лишь чистая CPU-часть загрузчиков, не касающаяся RM.
class ResourceManager {
public:
    static ResourceManager& Instance() {
        static ResourceManager instance;
        return instance;
    }

    std::shared_ptr<Mesh> GetCube() {
        if (!m_cube) m_cube = std::make_shared<Mesh>(Mesh::CreateCube());
        return m_cube;
    }

    std::shared_ptr<Mesh> GetModel(const std::string& path) {
        auto it = m_models.find(path);
        if (it != m_models.end()) return it->second;
        auto mesh = ModelLoader::LoadObj(path);
        m_models[path] = mesh;
        return mesh;
    }

    // Асинхронная загрузка модели. Если модель уже в кэше — дескриптор сразу
    // Ready. Повторные запросы того же пути, пока идёт загрузка, разделяют
    // один дескриптор (файл читается один раз). GL-меш создаётся в главном
    // потоке при Drain() и попадает в общий кэш GetModel/GetModelAsync.
    std::shared_ptr<AsyncResource<Mesh>> GetModelAsync(const std::string& path) {
        auto cached = m_models.find(path);
        if (cached != m_models.end()) {
            auto ready = std::make_shared<AsyncResource<Mesh>>();
            ready->ResolveReady(cached->second);
            return ready;
        }
        auto pending = m_pendingModels.find(path);
        if (pending != m_pendingModels.end()) return pending->second;

        auto handle = std::make_shared<AsyncResource<Mesh>>();
        m_pendingModels[path] = handle;

        JobSystem::Instance().Enqueue([this, path, handle] {
            try {
                MeshData data = ModelLoader::LoadObjData(path); // фон: CPU-парсинг
                MainThreadDispatcher::Instance().Post(
                    [this, path, handle, data = std::move(data)]() mutable {
                        FinishModel(path, handle, std::move(data), {});
                    });
            } catch (const std::exception& e) {
                std::string msg = e.what();
                MainThreadDispatcher::Instance().Post(
                    [this, path, handle, msg] { FinishModel(path, handle, {}, msg); });
            }
        });
        return handle;
    }

    // Асинхронная загрузка текстуры. Декод пикселей — в фоне, создание
    // GPU-текстуры — в главном потоке при Drain(). Не кэшируется (как и
    // синхронное создание Texture) — каждый вызов даёт новую текстуру.
    std::shared_ptr<AsyncResource<Texture>> LoadTextureAsync(
            const std::string& path, TextureFilter filter = TextureFilter::Trilinear,
            bool generateMipmaps = true) {
        auto handle = std::make_shared<AsyncResource<Texture>>();
        JobSystem::Instance().Enqueue([path, filter, generateMipmaps, handle] {
            std::string error;
            ImageData image = LoadImageFile(path, &error); // фон: декод PNG/JPG
            MainThreadDispatcher::Instance().Post(
                [filter, generateMipmaps, handle, image = std::move(image), error]() mutable {
                    if (!image.Ok()) { handle->ResolveFailed(error); return; }
                    try {
                        handle->ResolveReady(std::make_shared<Texture>(image, filter, generateMipmaps));
                    } catch (const std::exception& e) {
                        handle->ResolveFailed(e.what());
                    }
                });
        });
        return handle;
    }

    void Clear() {
        m_cube.reset();
        m_models.clear();
        m_pendingModels.clear();
    }

private:
    ResourceManager() = default;

    // Финализация асинхронной модели — строго в главном потоке (GL + кэш).
    void FinishModel(const std::string& path, const std::shared_ptr<AsyncResource<Mesh>>& handle,
                     MeshData data, const std::string& cpuError) {
        m_pendingModels.erase(path);
        if (!cpuError.empty()) { handle->ResolveFailed(cpuError); return; }
        try {
            auto mesh = std::make_shared<Mesh>(data);
            m_models[path] = mesh;
            handle->ResolveReady(mesh);
        } catch (const std::exception& e) {
            handle->ResolveFailed(e.what());
        }
    }

    std::shared_ptr<Mesh> m_cube;
    std::unordered_map<std::string, std::shared_ptr<Mesh>> m_models;
    std::unordered_map<std::string, std::shared_ptr<AsyncResource<Mesh>>> m_pendingModels;
};
