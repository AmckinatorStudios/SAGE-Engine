#include "sage/net/NetworkSystem.h"

#include <algorithm>
#include <cmath>

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

// Интерполяция УГЛА по короткой дуге.
//
// Поворот едет эйлеровыми углами, и линейная смесь 170 -> -170 идёт не на
// двадцать градусов вперёд, а на триста сорок назад — через ноль. На экране это
// объект, который на переходе через 180 разворачивается кругом и возвращается.
// Разница приводится к (-180, 180] и добавляется к началу.
float LerpAngle(float a, float b, float t) {
    float d = std::fmod(b - a + 540.0f, 360.0f) - 180.0f;
    return a + d * t;
}

glm::vec3 LerpAngles(const glm::vec3& a, const glm::vec3& b, float t) {
    return {LerpAngle(a.x, b.x, t), LerpAngle(a.y, b.y, t), LerpAngle(a.z, b.z, t)};
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
    // Призраки — ЧУЖИЕ сущности, и без связи они не имеют права оставаться в
    // сцене: управлять ими некому, удалить их игрок не может, а сохранение
    // унесёт их на диск. Сцены здесь нет, поэтому помечаем — снимет следующий
    // Update (см. RemovePendingGhosts).
    m_removeGhosts.insert(m_ghosts.begin(), m_ghosts.end());
    m_ghosts.clear();
    m_haveTick = false;
    m_chunkMask = 0;
    m_tickSeen.clear();
}

void NetworkSystem::RemovePendingGhosts(Scene& scene) {
    if (m_removeGhosts.empty()) return;
    for (int id : m_removeGhosts) scene.RemoveObject(id);
    m_removeGhosts.clear();
}

void NetworkSystem::UpdateAt(Scene& scene, double now) {
    m_now = now;
    // ПЕРЕД ранним выходом: связи уже нет, а призраков со сцены снять надо
    // именно поэтому.
    RemovePendingGhosts(scene);
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
                    // Связь оборвалась сама (таймаут, сервер ушёл) — чужие
                    // сущности из сцены уходят так же, как при явном Stop.
                    m_removeGhosts.insert(m_ghosts.begin(), m_ghosts.end());
                    m_ghosts.clear();
                    m_interp.clear();
                    m_haveTick = false;
                    m_chunkMask = 0;
                    m_tickSeen.clear();
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
    // Снапшот едет САМОСТОЯТЕЛЬНЫМИ ЧАСТЯМИ (см. kSnapshotChunkEntities).
    // Каждая несёт номер снапшота, свой номер и общее число частей — этого
    // хватает и чтобы отбросить устаревшую часть, и чтобы понять, собран ли
    // снапшот целиком (только тогда можно удалять пропавшие сущности).
    //
    // Формат части:
    //   [u8 kMsgSnapshot][u32 tick][u16 index][u16 count][u16 entities] + тела
    auto view = scene.Registry().view<NetReplicatedComponent, Transform, IdComponent>();

    std::vector<entt::entity> entities;
    for (auto e : view) entities.push_back(e);

    const int perChunk = kSnapshotChunkEntities;
    int chunkCount = (int)((entities.size() + perChunk - 1) / perChunk);
    if (chunkCount == 0) chunkCount = 1;   // пустой снапшот — тоже снапшот: по
                                           // нему клиент снимает всё, что ушло
    if (chunkCount > kMaxSnapshotChunks) {
        LOG_WARN("Net") << "Реплицируется " << entities.size()
                        << " сущностей — больше, чем помещается в снапшот ("
                        << kMaxSnapshotChunks * perChunk << "); лишние не поедут";
        chunkCount = kMaxSnapshotChunks;
    }

    const uint32_t tick = m_nextSnapshotTick++;
    for (int c = 0; c < chunkCount; ++c) {
        std::vector<uint8_t> msg;
        msg.push_back(kMsgSnapshot);
        WriteU32(msg, tick);
        WriteU16(msg, (uint16_t)c);
        WriteU16(msg, (uint16_t)chunkCount);

        const size_t first = (size_t)c * perChunk;
        const size_t last = std::min(first + (size_t)perChunk, entities.size());
        WriteU16(msg, (uint16_t)(last > first ? last - first : 0));

        for (size_t i = first; i < last; ++i) {
            const entt::entity e = entities[i];
            const Transform& tr = view.get<Transform>(e);
            const auto* mr = scene.Registry().try_get<MeshRendererComponent>(e);
            WriteU32(msg, (uint32_t)view.get<IdComponent>(e).Id);
            WriteU8(msg, mr ? MeshTypeToWire(mr->Ref.type) : 0);
            WriteVec3(msg, mr ? EffectiveColor(*mr) : glm::vec3(1.0f));
            WriteVec3(msg, tr.Position);
            WriteVec3(msg, tr.Rotation);
            WriteVec3(msg, tr.Scale);
        }

        // Ненадёжно: снапшот ценен свежестью, потерянный заменит следующий.
        m_server.Broadcast(msg.data(), msg.size(), /*reliable=*/false);
    }
}

void NetworkSystem::ApplySnapshot(Scene& scene, const uint8_t* data, size_t bytes, double now) {
    ByteReader r{data, bytes, 0, true};
    const uint32_t tick = r.U32();
    const uint16_t index = r.U16();
    const uint16_t chunkCount = r.U16();
    const uint16_t count = r.U16();
    if (!r.Ok || chunkCount == 0 || index >= chunkCount) return;

    // УСТАРЕВШАЯ ЧАСТЬ — В МУСОР. Части едут ненадёжным каналом и вправе
    // прийти не в том порядке; применить старую поверх новой значит откатить
    // объект назад на кадр сервера. Именно так и выглядит «дёргание» в сети.
    if (m_haveTick && tick < m_snapshotTick) return;

    if (!m_haveTick || tick > m_snapshotTick) {
        m_snapshotTick = tick;
        m_haveTick = true;
        m_chunkMask = 0;
        m_chunkCount = chunkCount;
        m_tickSeen.clear();
    }
    const uint64_t bit = index < 64 ? (uint64_t)1 << index : 0;
    if (bit && (m_chunkMask & bit)) return;   // дубликат части
    m_chunkMask |= bit;

    for (uint16_t i = 0; i < count && r.Ok; ++i) {
        EntityState st;
        st.Id = (int)r.U32();
        st.MeshType = r.U8();
        st.Color = ReadVec3(r);
        st.Position = ReadVec3(r);
        st.Rotation = ReadVec3(r);
        st.Scale = ReadVec3(r);
        if (!r.Ok) break;
        m_tickSeen.insert(st.Id);

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

        // Копим состояния с их временем прихода. Старше момента показа на
        // полсекунды — уже мусор: столько не бывает ни задержки, ни просадки,
        // после которой имело бы смысл интерполировать, а не защёлкнуться.
        InterpState& is = m_interp[st.Id];
        is.Samples.push_back({st, now});
        while (is.Samples.size() > 2 &&
               is.Samples[1].Time < now - kInterpDelay - 0.5) {
            is.Samples.pop_front();
        }
        if (is.Samples.size() > 16) is.Samples.pop_front();
    }

    // УДАЛЕНИЕ — ТОЛЬКО ПО ПОЛНОМУ СНАПШОТУ. Пока пришли не все части, «этой
    // сущности в снапшоте нет» значит лишь «её часть ещё в пути»; снести её по
    // такому основанию — это мигание объектов при каждой потере пакета.
    int have = 0;
    for (int i = 0; i < m_chunkCount && i < 64; ++i)
        if (m_chunkMask & ((uint64_t)1 << i)) ++have;
    if (have < m_chunkCount) return;

    std::vector<int> gone;
    for (auto& [id, st] : m_interp)
        if (!m_tickSeen.count(id)) gone.push_back(id);
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
        if (is.Samples.empty()) continue;
        GameObject obj = scene.Get(id);
        if (!obj.Valid()) continue;
        Transform& tr = obj.GetTransform();

        // Пара, между которой лежит момент показа. Ищем последнее состояние не
        // позже него и первое после — это и есть отрезок интерполяции.
        const Sample* a = nullptr;
        const Sample* b = nullptr;
        for (const Sample& s : is.Samples) {
            if (s.Time <= renderTime) a = &s;
            else { b = &s; break; }
        }

        if (!a) {
            // Момент показа старше всего, что мы получили: показывать нечего,
            // кроме самого раннего известного состояния.
            const EntityState& st = is.Samples.front().State;
            tr.Position = st.Position;
            tr.Rotation = st.Rotation;
            tr.Scale = st.Scale;
            continue;
        }
        if (!b) {
            // Свежих состояний нет (сервер молчит или сеть встала) — стоим на
            // последнем известном, а не экстраполируем в никуда.
            tr.Position = a->State.Position;
            tr.Rotation = a->State.Rotation;
            tr.Scale = a->State.Scale;
            continue;
        }

        const double span = b->Time - a->Time;
        const float t = span > 1e-6 ? (float)((renderTime - a->Time) / span) : 1.0f;
        tr.Position = glm::mix(a->State.Position, b->State.Position, t);
        tr.Rotation = LerpAngles(a->State.Rotation, b->State.Rotation, t);
        tr.Scale = glm::mix(a->State.Scale, b->State.Scale, t);
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
