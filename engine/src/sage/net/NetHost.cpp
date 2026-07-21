#include "sage/net/NetHost.h"

#include "sage/core/Log.h"

namespace sage::net {

namespace {

// Разбор заголовка входящей датаграммы; false — чужой/битый пакет.
bool ParseHeader(const uint8_t* data, size_t bytes, PacketType& outType) {
    if (bytes < 4 + 1 + 2 + 2 + 4) return false;
    ByteReader r{data, bytes, 0, true};
    if (r.U32() != kProtocolMagic) return false;
    uint8_t t = r.U8();
    if (t > (uint8_t)PacketType::Disconnect) return false;
    outType = (PacketType)t;
    return true;
}

// Служебный пакет без состояния соединения (accept/deny/connect/disconnect).
void SendBare(UdpSocket& socket, const NetAddress& to, PacketType type,
              const uint8_t* payload = nullptr, size_t payloadLen = 0) {
    std::vector<uint8_t> p;
    WriteU32(p, kProtocolMagic);
    WriteU8(p, (uint8_t)type);
    WriteU16(p, 0); // seq/ack не участвуют в handshake
    WriteU16(p, 0);
    WriteU32(p, 0);
    if (payload && payloadLen) p.insert(p.end(), payload, payload + payloadLen);
    socket.SendTo(to, p.data(), p.size());
}

constexpr size_t kHeaderSize = 4 + 1 + 2 + 2 + 4;
constexpr double kConnectRetry = 0.3; // повтор ConnectRequest, сек

} // namespace

// ============================================================================
//  NetServer
// ============================================================================

bool NetServer::Start(uint16_t port, int maxClients) {
    Stop();
    if (!m_socket.Open(port)) return false;
    m_maxClients = maxClients;
    LOG_INFO("Net") << "Сервер слушает UDP-порт " << m_socket.LocalPort()
                    << " (максимум клиентов: " << maxClients << ")";
    return true;
}

void NetServer::Stop() {
    if (!m_socket.IsOpen()) return;
    for (auto& [addr, client] : m_clients)
        SendBare(m_socket, addr, PacketType::Disconnect);
    m_clients.clear();
    m_socket.Close();
    m_events.clear();
}

void NetServer::Update(double now) {
    if (!m_socket.IsOpen()) return;

    uint8_t buffer[65536];
    NetAddress from;
    size_t got = 0;
    while (m_socket.RecvFrom(from, buffer, sizeof(buffer), got)) {
        PacketType type;
        if (!ParseHeader(buffer, got, type)) continue;

        auto it = m_clients.find(from);
        if (type == PacketType::ConnectRequest) {
            if (it != m_clients.end()) {
                // Повторный запрос (наш accept потерялся) — подтверждаем ещё раз.
                uint8_t id = (uint8_t)it->second->Id;
                SendBare(m_socket, from, PacketType::ConnectAccept, &id, 1);
                continue;
            }
            if ((int)m_clients.size() >= m_maxClients) {
                SendBare(m_socket, from, PacketType::ConnectDeny);
                continue;
            }
            auto client = std::make_unique<Client>();
            client->Id = m_nextClientId++;
            client->Conn.Reset(from, now);
            uint8_t id = (uint8_t)client->Id;
            SendBare(m_socket, from, PacketType::ConnectAccept, &id, 1);
            LOG_INFO("Net") << "Клиент #" << client->Id << " подключился: " << from.ToString();
            m_events.push_back({NetEvent::Type::ClientConnected, client->Id, {}});
            m_clients.emplace(from, std::move(client));
            continue;
        }

        if (it == m_clients.end()) continue; // пакет не от нашего клиента

        if (type == PacketType::Disconnect) {
            m_events.push_back({NetEvent::Type::ClientDisconnected, it->second->Id, {}});
            LOG_INFO("Net") << "Клиент #" << it->second->Id << " отключился";
            m_clients.erase(it);
            continue;
        }

        it->second->Conn.OnPacket(type, buffer + kHeaderSize - 8, got - (kHeaderSize - 8), now);
    }

    // Доставленные сообщения + отправка + таймауты.
    std::vector<NetAddress> dead;
    for (auto& [addr, client] : m_clients) {
        for (auto& msg : client->Conn.DrainReceived())
            m_events.push_back({NetEvent::Type::Message, client->Id, std::move(msg)});
        client->Conn.Flush(m_socket, now);
        if (client->Conn.TimedOut(now)) dead.push_back(addr);
    }
    for (const NetAddress& addr : dead) {
        auto it = m_clients.find(addr);
        if (it == m_clients.end()) continue;
        LOG_WARN("Net") << "Клиент #" << it->second->Id << " отвалился по таймауту";
        m_events.push_back({NetEvent::Type::ClientDisconnected, it->second->Id, {}});
        m_clients.erase(it);
    }
}

bool NetServer::Send(int clientId, const void* data, size_t bytes, bool reliable) {
    for (auto& [addr, client] : m_clients)
        if (client->Id == clientId)
            return client->Conn.Send((const uint8_t*)data, bytes, reliable);
    return false;
}

void NetServer::Broadcast(const void* data, size_t bytes, bool reliable) {
    for (auto& [addr, client] : m_clients)
        client->Conn.Send((const uint8_t*)data, bytes, reliable);
}

void NetServer::Kick(int clientId) {
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (it->second->Id != clientId) continue;
        SendBare(m_socket, it->first, PacketType::Disconnect);
        m_events.push_back({NetEvent::Type::ClientDisconnected, clientId, {}});
        m_clients.erase(it);
        return;
    }
}

std::vector<NetEvent> NetServer::DrainEvents() {
    std::vector<NetEvent> out;
    out.swap(m_events);
    return out;
}

std::vector<int> NetServer::ClientIds() const {
    std::vector<int> out;
    for (const auto& [addr, client] : m_clients) out.push_back(client->Id);
    return out;
}

// ============================================================================
//  NetClient
// ============================================================================

bool NetClient::Connect(const NetAddress& server) {
    Disconnect();
    if (!m_socket.Open(0)) return false;
    m_server = server;
    m_state = State::Connecting;
    m_clockValid = false;
    LOG_INFO("Net") << "Подключение к " << server.ToString() << "...";
    return true;
}

void NetClient::Disconnect() {
    if (m_state == State::Connected)
        SendBare(m_socket, m_server, PacketType::Disconnect);
    if (m_state != State::Disconnected)
        m_events.push_back({NetEvent::Type::ClientDisconnected, 0, {}});
    m_socket.Close();
    m_state = State::Disconnected;
    m_clientId = -1;
}

void NetClient::Update(double now) {
    if (m_state == State::Disconnected || !m_socket.IsOpen()) return;

    if (!m_clockValid) { // первое обновление после Connect — старт таймеров
        m_connectStarted = now;
        m_lastRequest = -1e9;
        m_clockValid = true;
    }

    if (m_state == State::Connecting) {
        if (now - m_connectStarted > kTimeout) {
            LOG_ERROR("Net") << "Не удалось подключиться к " << m_server.ToString();
            Disconnect();
            return;
        }
        if (now - m_lastRequest >= kConnectRetry) {
            SendBare(m_socket, m_server, PacketType::ConnectRequest);
            m_lastRequest = now;
        }
    }

    uint8_t buffer[65536];
    NetAddress from;
    size_t got = 0;
    while (m_socket.RecvFrom(from, buffer, sizeof(buffer), got)) {
        if (from != m_server) continue;
        PacketType type;
        if (!ParseHeader(buffer, got, type)) continue;

        switch (type) {
            case PacketType::ConnectAccept:
                if (m_state == State::Connecting) {
                    m_clientId = got > kHeaderSize ? buffer[kHeaderSize] : 0;
                    m_state = State::Connected;
                    m_conn.Reset(m_server, now);
                    LOG_INFO("Net") << "Подключено (наш id: " << m_clientId << ")";
                    m_events.push_back({NetEvent::Type::ClientConnected, 0, {}});
                }
                break;
            case PacketType::ConnectDeny:
                LOG_ERROR("Net") << "Сервер отклонил подключение (заполнен)";
                Disconnect();
                return;
            case PacketType::Disconnect:
                LOG_INFO("Net") << "Сервер разорвал соединение";
                m_state = State::Disconnected; // без ответного Disconnect-пакета
                m_socket.Close();
                m_events.push_back({NetEvent::Type::ClientDisconnected, 0, {}});
                return;
            default:
                if (m_state == State::Connected)
                    m_conn.OnPacket(type, buffer + kHeaderSize - 8, got - (kHeaderSize - 8), now);
                break;
        }
    }

    if (m_state == State::Connected) {
        for (auto& msg : m_conn.DrainReceived())
            m_events.push_back({NetEvent::Type::Message, 0, std::move(msg)});
        m_conn.Flush(m_socket, now);
        if (m_conn.TimedOut(now)) {
            LOG_WARN("Net") << "Сервер молчит — соединение потеряно";
            Disconnect();
        }
    }
}

bool NetClient::Send(const void* data, size_t bytes, bool reliable) {
    if (m_state != State::Connected) return false;
    return m_conn.Send((const uint8_t*)data, bytes, reliable);
}

std::vector<NetEvent> NetClient::DrainEvents() {
    std::vector<NetEvent> out;
    out.swap(m_events);
    return out;
}

} // namespace sage::net
