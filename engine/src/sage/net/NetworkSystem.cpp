#include "sage/net/NetworkSystem.h"

#include <algorithm>

#include "sage/core/Log.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace sage::net {

namespace {

// Вид сообщения поверх канала соединения (первый байт).
constexpr uint8_t kMsgSnapshot = 0;
constexpr uint8_t kMsgUser = 1;

uint8_t MeshTypeToWire(MeshRef::Type t) { return (uint8_t)t; }
MeshRef::Type MeshTypeFromWire(uint8_t v) {
    return v <= (uint8_t)MeshRef::Type::Model ? (MeshRef::Type)v : MeshRef::Type::None;
}

void WriteVec3(std::vector<uint8_t>& b, const glm::vec3& v) {
    WriteF32(b, v.x);
    WriteF32(b, v.y);
    WriteF32(b, v.z);
}

glm::vec3 ReadVec3(ByteReader& r) {
    glm::vec3 v;
    v.x = r.F32();
    v.y = r.F32();
    v.z = r.F32();
    return v;
}

} // namespace

bool NetworkSystem::StartServer(uint16_t port, int maxClients) {
    Stop();
    if (!m_server.Start(port, maxClients)) return false;
    m_mode = Mode::Server;
    return true;
}

bool NetworkSystem::Connect(const std::string& ip, uint16_t port) {
    Stop();
    NetAddress addr = NetAddress::Parse(ip, port);
    if (!addr.Valid()) {
        LOG_ERROR("Net") << "Некорректный адрес сервера: " << ip << ":" << port;
        return false;
    }
    if (!m_client.Connect(addr)) return false;
    m_mode = Mode::Client;
    return true;
}

void NetworkSystem::Stop() {
    m_server.Stop();
    m_client.Disconnect();
    m_client.DrainEvents(); // событие Disconnected от Stop скриптам не нужно
    m_mode = Mode::Offline;
    m_interp.clear();
    m_ghosts.clear();
}

void NetworkSystem::UpdateAt(Scene& scene, double now) {
    m_now = now;
    if (m_mode == Mode::Offline) return;

    if (m_mode == Mode::Server) {
        m_server.Update(now);
        for (NetEvent& ev : m_server.DrainEvents()) {
            switch (ev.Kind) {
                case NetEvent::Type::ClientConnected:
                    m_scriptEvents.push_back({ScriptNetEvent::Type::ClientConnected, ev.ClientId, {}, {}});
                    break;
                case NetEvent::Type::ClientDisconnected:
                    m_scriptEvents.push_back({ScriptNetEvent::Type::ClientDisconnected, ev.ClientId, {}, {}});
                    break;
                case NetEvent::Type::Message:
                    HandleMessage(ev.ClientId, ev.Data, scene, now);
                    break;
            }
        }
        if (now - m_lastSnapshot >= kSnapshotInterval) {
            m_lastSnapshot = now;
            BuildAndSendSnapshot(scene);
        }
    } else {
        m_client.Update(now);
        for (NetEvent& ev : m_client.DrainEvents()) {
            switch (ev.Kind) {
                case NetEvent::Type::ClientConnected:
                    m_scriptEvents.push_back({ScriptNetEvent::Type::Connected, 0, {}, {}});
                    break;
                case NetEvent::Type::ClientDisconnected:
                    m_scriptEvents.push_back({ScriptNetEvent::Type::Disconnected, 0, {}, {}});
                    break;
                case NetEvent::Type::Message:
                    HandleMessage(0, ev.Data, scene, now);
                    break;
            }
        }
        InterpolateRemote(scene, now);
    }
}

// --- Снапшоты ---------------------------------------------------------------

void NetworkSystem::BuildAndSendSnapshot(Scene& scene) {
    std::vector<uint8_t> msg;
    msg.push_back(kMsgSnapshot);

    uint32_t count = 0;
    size_t countPos = msg.size();
    WriteU32(msg, 0); // заполним после подсчёта

    auto view = scene.Registry().view<NetReplicatedComponent, Transform, IdComponent>();
    for (auto e : view) {
        const Transform& tr = view.get<Transform>(e);
        const auto* mr = scene.Registry().try_get<MeshRendererComponent>(e);
        WriteU32(msg, (uint32_t)view.get<IdComponent>(e).Id);
        WriteU8(msg, mr ? MeshTypeToWire(mr->Ref.type) : 0);
        WriteVec3(msg, mr ? EffectiveColor(*mr) : glm::vec3(1.0f));
        WriteVec3(msg, tr.Position);
        WriteVec3(msg, tr.Rotation);
        WriteVec3(msg, tr.Scale);
        ++count;
    }
    // Число сущностей — на своё место.
    msg[countPos] = (uint8_t)(count & 0xFF);
    msg[countPos + 1] = (uint8_t)((count >> 8) & 0xFF);
    msg[countPos + 2] = (uint8_t)((count >> 16) & 0xFF);
    msg[countPos + 3] = (uint8_t)((count >> 24) & 0xFF);

    // Ненадёжно: снапшот ценен свежестью, потерянный заменит следующий (50 мс).
    m_server.Broadcast(msg.data(), msg.size(), /*reliable=*/false);
}

void NetworkSystem::ApplySnapshot(Scene& scene, const uint8_t* data, size_t bytes, double now) {
    ByteReader r{data, bytes, 0, true};
    uint32_t count = r.U32();
    std::unordered_set<int> seen;

    for (uint32_t i = 0; i < count && r.Ok; ++i) {
        EntityState st;
        st.Id = (int)r.U32();
        st.MeshType = r.U8();
        st.Color = ReadVec3(r);
        st.Position = ReadVec3(r);
        st.Rotation = ReadVec3(r);
        st.Scale = ReadVec3(r);
        if (!r.Ok) break;
        seen.insert(st.Id);

        GameObject obj = scene.Get(st.Id);
        if (!obj.Valid()) {
            // Призрак: сущности нет у клиента — создаём с СЕРВЕРНЫМ id.
            obj = scene.CreateObjectWithId("net_" + std::to_string(st.Id), st.Id);
            obj.Renderer().Ref = MeshRef{MeshTypeFromWire(st.MeshType), ""};
            // GPU-меш — только при живом рендере (headless-тесты без GL).
            // Он подтянется отрисовкой через ResourceManager у тех, кто рисует.
            m_ghosts.insert(st.Id);
        }
        obj.Renderer().Color = st.Color;

        // Интерполяция: копим пару последних состояний.
        InterpState& is = m_interp[st.Id];
        if (is.TimeB == 0.0) {
            is.A = is.B = st;
            is.TimeA = is.TimeB = now;
        } else {
            is.A = is.B;
            is.TimeA = is.TimeB;
            is.B = st;
            is.TimeB = now;
        }
    }

    // Сущности, пропавшие из снапшота: призраков удаляем; свои (из локальной
    // сцены) не трогаем — их судьба принадлежит локальной игре.
    std::vector<int> gone;
    for (auto& [id, st] : m_interp)
        if (!seen.count(id)) gone.push_back(id);
    for (int id : gone) {
        m_interp.erase(id);
        if (m_ghosts.count(id)) {
            scene.RemoveObject(id);
            m_ghosts.erase(id);
        }
    }
}

void NetworkSystem::InterpolateRemote(Scene& scene, double now) {
    const double renderTime = now - kInterpDelay;
    for (auto& [id, is] : m_interp) {
        GameObject obj = scene.Get(id);
        if (!obj.Valid()) continue;
        Transform& tr = obj.GetTransform();
        if (is.TimeB <= is.TimeA || renderTime >= is.TimeB) {
            tr.Position = is.B.Position;
            tr.Rotation = is.B.Rotation;
            tr.Scale = is.B.Scale;
        } else if (renderTime <= is.TimeA) {
            tr.Position = is.A.Position;
            tr.Rotation = is.A.Rotation;
            tr.Scale = is.A.Scale;
        } else {
            float t = (float)((renderTime - is.TimeA) / (is.TimeB - is.TimeA));
            tr.Position = glm::mix(is.A.Position, is.B.Position, t);
            tr.Rotation = glm::mix(is.A.Rotation, is.B.Rotation, t); // углы близких снапшотов
            tr.Scale = glm::mix(is.A.Scale, is.B.Scale, t);
        }
    }
}

// --- Пользовательские сообщения ---------------------------------------------

namespace {

std::vector<uint8_t> PackUserMessage(const std::string& name, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> msg;
    msg.reserve(1 + 1 + name.size() + payload.size());
    msg.push_back(kMsgUser);
    WriteU8(msg, (uint8_t)std::min<size_t>(name.size(), 255));
    msg.insert(msg.end(), name.begin(), name.begin() + std::min<size_t>(name.size(), 255));
    msg.insert(msg.end(), payload.begin(), payload.end());
    return msg;
}

} // namespace

bool NetworkSystem::SendToServer(const std::string& name, const std::vector<uint8_t>& payload,
                                 bool reliable) {
    if (m_mode != Mode::Client || !m_client.Connected()) return false;
    auto msg = PackUserMessage(name, payload);
    return m_client.Send(msg.data(), msg.size(), reliable);
}

bool NetworkSystem::SendToClient(int clientId, const std::string& name,
                                 const std::vector<uint8_t>& payload, bool reliable) {
    if (m_mode != Mode::Server) return false;
    auto msg = PackUserMessage(name, payload);
    return m_server.Send(clientId, msg.data(), msg.size(), reliable);
}

void NetworkSystem::BroadcastToClients(const std::string& name,
                                       const std::vector<uint8_t>& payload, bool reliable) {
    if (m_mode != Mode::Server) return;
    auto msg = PackUserMessage(name, payload);
    m_server.Broadcast(msg.data(), msg.size(), reliable);
}

void NetworkSystem::HandleMessage(int senderId, const std::vector<uint8_t>& msg, Scene& scene,
                                  double now) {
    if (msg.empty()) return;
    if (msg[0] == kMsgSnapshot) {
        if (m_mode == Mode::Client) ApplySnapshot(scene, msg.data() + 1, msg.size() - 1, now);
        return;
    }
    if (msg[0] == kMsgUser) {
        ByteReader r{msg.data() + 1, msg.size() - 1, 0, true};
        uint8_t nameLen = r.U8();
        if (!r.Ok || r.Pos + nameLen > r.Size) return;
        std::string name((const char*)r.Data + r.Pos, nameLen);
        r.Pos += nameLen;
        ScriptNetEvent ev;
        ev.Kind = ScriptNetEvent::Type::Message;
        ev.ClientId = senderId;
        ev.Name = std::move(name);
        ev.Payload.assign(r.Data + r.Pos, r.Data + r.Size);
        m_scriptEvents.push_back(std::move(ev));
    }
}

std::vector<ScriptNetEvent> NetworkSystem::DrainScriptEvents() {
    std::vector<ScriptNetEvent> out;
    out.swap(m_scriptEvents);
    return out;
}

} // namespace sage::net
