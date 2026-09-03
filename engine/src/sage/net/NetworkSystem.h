#pragma once
#include <cstdint>
#include <deque>
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

// Сколько сущностей уезжает ОДНИМ куском снапшота.
//
// Снапшот бьётся на самостоятельные части, а не едет одним длинным сообщением,
// и это не про экономию. Длинное сообщение фрагментируется транспортом, а
// снапшот идёт НЕНАДЁЖНЫМ каналом: потеря одного фрагмента убивала снапшот
// ЦЕЛИКОМ, и чем больше в сцене сущностей, тем реже до клиента доезжал хоть
// один. На тридцати кубах и трети потерь это уже «мир дёргается», а на сотне —
// «мир стоит». Самостоятельные части теряются поштучно: пропал кусок — не
// обновились его шестнадцать сущностей, остальные приехали.
//
// Шестнадцать: 53 байта на сущность плюс заголовок — меньше 900 байт, то есть
// заведомо внутри одного пакета (kMaxFragmentBody = 1024).
constexpr int kSnapshotChunkEntities = 16;
// Потолок частей на снапшот. Больше — сцена реплицирует тысячу сущностей, и об
// этом надо сказать вслух, а не молча резать.
constexpr int kMaxSnapshotChunks = 64;

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
    // Снимает со сцены призраков, помеченных к удалению (разрыв связи, Stop).
    // Отдельным шагом, потому что Stop сцены не знает: её приносит следующий
    // Update — тот же кадр, в котором игра и увидела бы исчезновение.
    void RemovePendingGhosts(Scene& scene);

    Mode m_mode = Mode::Offline;
    NetServer m_server;
    NetClient m_client;
    double m_now = 0.0;
    double m_lastSnapshot = -1e9;

    // Клиент: очередь последних состояний на сущность.
    //
    // ОЧЕРЕДЬ, А НЕ ПАРА. Показ отстаёт от приёма на kInterpDelay — это и есть
    // запас, из которого берётся плавность. Пара «предыдущее и текущее»
    // покрывает лишь ОДИН интервал между снапшотами, а задержка равна двум:
    // момент показа всегда оказывался старше обоих, и код честно защёлкивался
    // на более старом. Со стороны это выглядело как полное отсутствие
    // интерполяции — объекты двигались рывками по двадцать раз в секунду, с
    // какой бы частотой ни шёл кадр. Очередь покрывает задержку целиком:
    // берутся два состояния, между которыми лежит момент показа.
    struct Sample {
        EntityState State;
        double Time = 0.0;
    };
    struct InterpState {
        std::deque<Sample> Samples;
    };
    std::unordered_map<int, InterpState> m_interp;
    std::unordered_set<int> m_ghosts; // созданные репликацией сущности (id)
    std::unordered_set<int> m_removeGhosts; // ждут удаления со сцены

    // --- Приём снапшота по частям ---
    //
    // Номер снапшота нужен потому, что части едут НЕНАДЁЖНО и вправе прийти не
    // в том порядке. Без него устаревшая часть применялась наравне со свежей —
    // и объект дёргался назад ровно на разницу между кадрами сервера.
    uint32_t m_snapshotTick = 0;   // номер снапшота, который собираем сейчас
    bool m_haveTick = false;
    uint64_t m_chunkMask = 0;      // какие части этого снапшота уже пришли
    int m_chunkCount = 0;
    std::unordered_set<int> m_tickSeen; // id из пришедших частей снапшота
    uint32_t m_nextSnapshotTick = 1;    // серверный счётчик

    std::vector<ScriptNetEvent> m_scriptEvents;
};

} // namespace sage::net
