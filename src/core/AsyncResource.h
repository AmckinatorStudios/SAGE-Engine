#pragma once
#include <atomic>
#include <memory>
#include <string>

// ---------------------------------------------------------------------
// AsyncResource<T> — дескриптор ресурса, который грузится асинхронно.
// Возвращается *Async-методами (см. render/ResourceManager.h): пока ресурс
// готовится в фоне, дескриптор в состоянии Loading; когда главный поток
// завершит GL-загрузку — переходит в Ready и Get() отдаёт готовый объект.
// При ошибке — Failed (Get() вернёт nullptr, причина в Error()).
//
// Владелец просто держит shared_ptr<AsyncResource<T>> и раз в кадр смотрит
// IsReady()/Get() — никакой ручной синхронизации не нужно: сам объект
// заполняется в главном потоке (там же, где его читают), а межпоточное —
// только атомарное поле состояния. Часть ЯДРА движка.
// ---------------------------------------------------------------------
enum class LoadState { Loading, Ready, Failed };

template <class T>
class AsyncResource {
public:
    LoadState State() const { return m_state.load(std::memory_order_acquire); }
    bool IsReady() const { return State() == LoadState::Ready; }
    bool IsFailed() const { return State() == LoadState::Failed; }
    bool IsLoading() const { return State() == LoadState::Loading; }

    // Готовый ресурс, если IsReady(); иначе nullptr.
    std::shared_ptr<T> Get() const { return IsReady() ? m_result : nullptr; }

    const std::string& Error() const { return m_error; }

    // --- Служебное: заполняется загрузчиком, ТОЛЬКО из главного потока ---
    // (единственное межпоточное поле — атомарное m_state; результат/ошибку
    //  пишет и читает главный поток, поэтому дополнительная синхронизация не
    //  нужна). Владельцу вызывать эти методы не следует.
    void ResolveReady(std::shared_ptr<T> value) {
        m_result = std::move(value);
        m_state.store(LoadState::Ready, std::memory_order_release);
    }
    void ResolveFailed(std::string reason) {
        m_error = std::move(reason);
        m_state.store(LoadState::Failed, std::memory_order_release);
    }

private:
    std::atomic<LoadState> m_state{LoadState::Loading};
    std::shared_ptr<T> m_result;
    std::string m_error;
};
