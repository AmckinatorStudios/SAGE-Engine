#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "sage/net/NetHost.h"

class Scene;

// ---------------------------------------------------------------------------
// NetworkSystem — мультиплеер уровня СЦЕНЫ: серверная авторитарная репликация
// ECS-сущностей + именованные пользовательские сообщения (доступны из Lua,
// см. ScriptEngine::BindNetwork).
//
// Модель (классический server-authoritative):
//   • Сервер: сущности с NetReplicatedComponent попадают в снапшот
//     (id, меш-примитив, цвет, позиция/поворот/масштаб), который с частотой
//     kSnapshotRate уходит всем клиентам ненадёжно (важна свежесть, не
//     доставка каждого).
//   • Клиент: применяет снапшоты к своей сцене — существующая сущность с тем
//     же id обновляется (обе стороны загрузили одну сцену — кооп), отсутствующая
//     создаётся «призраком», исчезнувшая из снапшота удаляется. Движение
//     сглаживается интерполяцией между двумя последними снапшотами с задержкой
//     kInterpDelay (стандартная техника снапшот-интерполяции).
//   • Пользовательские сообщения: Send*/Broadcast с именем и байтовой
//     нагрузкой (Lua-значения кодирует ScriptEngine), надёжно или нет — на
//     выбор отправителя. Входящие копятся и забираются DrainScriptEvents.
//
// Прокачивается из главного цикла: Update(scene, dt). Часы — свои
// (монотонные внутренние секунды), тестам доступен UpdateAt(scene, now).
// ---------------------------------------------------------------------------
namespace sage::net {

constexpr double kSnapshotInterval = 1.0 / 20.0; // 20 снапшотов/с
constexpr double kInterpDelay = 0.1;             // задержка интерполяции, с

// Событие для скриптового слоя (см. ScriptEngine::BindNetwork).
struct ScriptNetEvent {
    enum class Type { Message, ClientConnected, ClientDisconnected, Connected, Disconnected };
    Type Kind = Type::Message;
    int ClientId = 0;                // отправитель (0 — сервер) / подключившийся
    std::string Name;                // имя пользовательского сообщения
    std::vector<uint8_t> Payload;    // закодированное Lua-значение
};

class NetworkSystem {
public:
    enum class Mode { Offline, Server, Client };

    Mode GetMode() const { return m_mode; }
    bool IsServer() const { return m_mode == Mode::Server; }
    bool IsClient() const { return m_mode == Mode::Client; }
    bool IsConnected() const {
        return m_mode == Mode::Server || (m_mode == Mode::Client && m_client.Connected());
    }

    // Поднять сервер (port 0 — эфемерный, для тестов). false — ошибка сокета.
    bool StartServer(uint16_t port, int maxClients = 8);
    // Подключиться к серверу (ip — числовой IPv4).
    bool Connect(const std::string& ip, uint16_t port);
    void Stop();

    uint16_t ServerPort() { return m_server.Port(); } // после StartServer
    std::vector<int> ClientIds() const { return m_server.ClientIds(); }
    int ClientCount() const { return m_server.ClientCount(); }

    // Прокачка кадра: транспорт + снапшоты (сервер) / применение и
    // интерполяция (клиент). Обычный путь — Update(dt); UpdateAt — явные часы.
    void Update(Scene& scene, float dt) { UpdateAt(scene, m_now + dt); }
    void UpdateAt(Scene& scene, double now);

    // --- Пользовательские сообщения (Lua: Net.Send / Net.SendTo / Net.Broadcast) --
    bool SendToServer(const std::string& name, const std::vector<uint8_t>& payload, bool reliable);
    bool SendToClient(int clientId, const std::string& name, const std::vector<uint8_t>& payload,
                      bool reliable);
    void BroadcastToClients(const std::string& name, const std::vector<uint8_t>& payload,
                            bool reliable);

    // События для скриптов (подключения/отключения/сообщения).
    std::vector<ScriptNetEvent> DrainScriptEvents();

    // Тестовые ручки: доступ к транспорту (симуляция потерь и т.п.).
    NetServer& Server() { return m_server; }
    NetClient& Client() { return m_client; }

private:
    // Снапшот одной сущности на проводе.
    struct EntityState {
        int Id = 0;
        uint8_t MeshType = 0;
        glm::vec3 Color{1.0f};
        glm::vec3 Position{0.0f};
        glm::vec3 Rotation{0.0f};
        glm::vec3 Scale{1.0f};
    };

    void BuildAndSendSnapshot(Scene& scene);
    void ApplySnapshot(Scene& scene, const uint8_t* data, size_t bytes, double now);
    void InterpolateRemote(Scene& scene, double now);
    void HandleMessage(int senderId, const std::vector<uint8_t>& msg, Scene& scene, double now);

    Mode m_mode = Mode::Offline;
    NetServer m_server;
    NetClient m_client;
    double m_now = 0.0;
    double m_lastSnapshot = -1e9;

    // Клиент: интерполяция — по паре последних состояний на сущность.
    struct InterpState {
        EntityState A, B;
        double TimeA = 0.0, TimeB = 0.0;
    };
    std::unordered_map<int, InterpState> m_interp;
    std::unordered_set<int> m_ghosts; // созданные репликацией сущности (id)

    std::vector<ScriptNetEvent> m_scriptEvents;
};

} // namespace sage::net
