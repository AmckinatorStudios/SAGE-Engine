#pragma once
#include <memory>
#include <unordered_map>
#include <vector>

#include "sage/net/NetConnection.h"

// ---------------------------------------------------------------------------
// NetServer / NetClient — управление соединениями поверх NetConnection.
//
// Сервер слушает порт, принимает до maxClients подключений (handshake:
// ConnectRequest -> ConnectAccept с выданным clientId), клиент повторяет
// запрос, пока не получит ответ. Обе стороны «прокачиваются» из главного цикла:
// Update(now) — приём/маршрутизация датаграмм + отправка/переотправка +
// таймауты; события (подключение/отключение/сообщение) копятся в очередь и
// забираются DrainEvents(). Время — явный параметр (тесты управляют часами).
// ---------------------------------------------------------------------------
namespace sage::net {

struct NetEvent {
    enum class Type { ClientConnected, ClientDisconnected, Message };
    Type Kind = Type::Message;
    int ClientId = 0;               // у клиента всегда 0 (события про сервер)
    std::vector<uint8_t> Data;      // только для Message
};

class NetServer {
public:
    // Открывает слушающий сокет. port 0 — эфемерный (тесты). false — ошибка.
    bool Start(uint16_t port, int maxClients = 8);
    void Stop();
    bool Running() const { return m_socket.IsOpen(); }
    uint16_t Port() const { return m_socket.LocalPort(); }

    void Update(double now);

    bool Send(int clientId, const void* data, size_t bytes, bool reliable);
    void Broadcast(const void* data, size_t bytes, bool reliable);
    void Kick(int clientId);

    std::vector<NetEvent> DrainEvents();
    std::vector<int> ClientIds() const;
    int ClientCount() const { return (int)m_clients.size(); }

    UdpSocket& Socket() { return m_socket; } // симуляция потерь в тестах

private:
    struct Client {
        int Id = 0;
        NetConnection Conn;
    };

    // Свободный id из 1..255, наименьший. НЕ счётчик: id уходит на провод
    // ОДНИМ БАЙТОМ (ConnectAccept), и растущий счётчик после 255 подключений за
    // сессию выдавал бы клиенту одно число, а у себя держал другое — клиент
    // считал бы чужие объекты своими. Сервер живёт долго, а 256 входов и
    // выходов набираются за вечер. 0 — свободных нет (отвечаем ConnectDeny).
    int AllocateClientId() const;

    UdpSocket m_socket;
    int m_maxClients = 8;
    std::unordered_map<NetAddress, std::unique_ptr<Client>> m_clients;
    std::vector<NetEvent> m_events;
};

class NetClient {
public:
    // Начинает подключение (неблокирующе): запрос повторяется из Update, пока
    // сервер не ответит или не истечёт kTimeout. false — сокет не открылся.
    bool Connect(const NetAddress& server);
    void Disconnect();

    enum class State { Disconnected, Connecting, Connected };
    State GetState() const { return m_state; }
    bool Connected() const { return m_state == State::Connected; }
    int ClientId() const { return m_clientId; } // выданный сервером id

    void Update(double now);

    bool Send(const void* data, size_t bytes, bool reliable);

    std::vector<NetEvent> DrainEvents();

    UdpSocket& Socket() { return m_socket; } // симуляция потерь в тестах

private:
    UdpSocket m_socket;
    NetAddress m_server;
    State m_state = State::Disconnected;
    int m_clientId = -1;
    NetConnection m_conn;
    double m_connectStarted = 0.0;
    double m_lastRequest = -1e9;
    bool m_clockValid = false;
    std::vector<NetEvent> m_events;
};

} // namespace sage::net
