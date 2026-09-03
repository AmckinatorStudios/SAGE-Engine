// ---------------------------------------------------------------------------
// Сеть в Lua: таблица Net.*, кодек значений и доставка событий в хуки скриптов.
//
// Отдельным файлом, как и остальные области привязок (см. ScriptApiCommon.h):
// сеть — это и API, и собственный бинарный формат сообщений, и разбор событий,
// то есть три связанные вещи, которым нечего делать в общем ScriptEngine.cpp.
//
// ЧТО ВИДИТ ИГРА:
//   Net.Host(port [, maxClients])   — поднять сервер
//   Net.Connect(ip, port)           — подключиться клиентом
//   Net.Stop() / Net.IsServer() / Net.IsClient() / Net.IsConnected()
//   Net.ClientCount() / Net.Clients()
//   Net.Send(name, data [, reliable])          — клиент -> сервер
//   Net.SendTo(clientId, name, data [, rel])   — сервер -> клиенту
//   Net.Broadcast(name, data [, reliable])     — сервер -> всем
// и хуки: OnNetMessage(name, data, sender), OnClientConnected(id),
// OnClientDisconnected(id), OnNetConnected(), OnNetDisconnected().
//
// data — любое nil/bool/число/строка/таблица (рекурсивно). Кодек СВОЙ, а не
// JSON: сообщения летят каждый кадр, и разбор текста на этом пути был бы самой
// дорогой частью сети.
// ---------------------------------------------------------------------------
#include "ScriptApiCommon.h"

#include <cstring>
#include <stdexcept>
#include <vector>

#include "sage/core/Log.h"
#include "sage/net/NetConnection.h"
#include "sage/net/NetworkSystem.h"

namespace {

// Глубина вложенности таблиц. Ограничение обязательное: рекурсивная таблица
// (t.self = t) иначе уронила бы кодировщик переполнением стека — и это ошибка
// скрипта, о которой надо СКАЗАТЬ, а не упасть.
constexpr int kMaxCodecDepth = 16;

void WriteF64(std::vector<uint8_t>& b, double v) {
    uint64_t u;
    std::memcpy(&u, &v, 8);
    sage::net::WriteU32(b, (uint32_t)(u & 0xFFFFFFFFu));
    sage::net::WriteU32(b, (uint32_t)(u >> 32));
}

double ReadF64(sage::net::ByteReader& r) {
    uint64_t lo = r.U32(), hi = r.U32();
    uint64_t u = lo | (hi << 32);
    double v;
    std::memcpy(&v, &u, 8);
    return v;
}

void EncodeLuaValue(const sol::object& v, std::vector<uint8_t>& out, int depth) {
    if (depth > kMaxCodecDepth)
        throw std::runtime_error("Net: слишком глубокая таблица в сообщении");
    switch (v.get_type()) {
        case sol::type::lua_nil:
            out.push_back(0);
            break;
        case sol::type::boolean:
            out.push_back(v.as<bool>() ? 2 : 1);
            break;
        case sol::type::number:
            out.push_back(3);
            WriteF64(out, v.as<double>());
            break;
        case sol::type::string: {
            out.push_back(4);
            const std::string str = v.as<std::string>();
            sage::net::WriteU32(out, (uint32_t)str.size());
            out.insert(out.end(), str.begin(), str.end());
            break;
        }
        case sol::type::table: {
            out.push_back(5);
            sol::table t = v.as<sol::table>();
            uint32_t count = 0;
            for (auto& kv : t) { (void)kv; ++count; }
            sage::net::WriteU32(out, count);
            for (auto& kv : t) {
                EncodeLuaValue(kv.first, out, depth + 1);
                EncodeLuaValue(kv.second, out, depth + 1);
            }
            break;
        }
        default:
            throw std::runtime_error("Net: в сообщении можно слать nil/bool/число/строку/таблицу");
    }
}

sol::object DecodeLuaValue(sol::state& lua, sage::net::ByteReader& r, int depth) {
    if (depth > kMaxCodecDepth || !r.Ok) return sol::make_object(lua, sol::lua_nil);
    uint8_t tag = r.U8();
    switch (tag) {
        case 0: return sol::make_object(lua, sol::lua_nil);
        case 1: return sol::make_object(lua, false);
        case 2: return sol::make_object(lua, true);
        case 3: return sol::make_object(lua, ReadF64(r));
        case 4: {
            uint32_t len = r.U32();
            if (!r.Ok || r.Pos + len > r.Size) {
                r.Ok = false;
                return sol::make_object(lua, sol::lua_nil);
            }
            std::string str((const char*)r.Data + r.Pos, len);
            r.Pos += len;
            return sol::make_object(lua, str);
        }
        case 5: {
            uint32_t count = r.U32();
            sol::table t = lua.create_table();
            for (uint32_t i = 0; i < count && r.Ok; ++i) {
                sol::object key = DecodeLuaValue(lua, r, depth + 1);
                sol::object val = DecodeLuaValue(lua, r, depth + 1);
                if (key.get_type() != sol::type::lua_nil) t[key] = val;
            }
            return t;
        }
        default:
            // Незнакомая метка — испорченный или чужой пакет. Дальше читать
            // нечего: остаток буфера уже ничему не соответствует.
            r.Ok = false;
            return sol::make_object(lua, sol::lua_nil);
    }
}

std::vector<uint8_t> EncodePayload(const sol::object& v) {
    std::vector<uint8_t> out;
    EncodeLuaValue(v, out, 0);
    return out;
}

} // namespace

void ScriptEngine::RegisterNetApi() {
    sol::table net = m_lua.create_named_table("Net");
    // Сеть привязывают не всегда (одиночная игра, редактор, тесты), поэтому
    // таблица есть ВСЕГДА, а вызов без BindNetwork даёт понятную ошибку. Иначе
    // скрипт спотыкался бы об «attempt to index a nil value (global 'Net')» и
    // это выглядело бы как опечатка в его собственном коде.
    auto requireNet = [this]() -> sage::net::NetworkSystem& {
        if (!m_network) throw std::runtime_error("Net: сеть не привязана (BindNetwork не вызван)");
        return *m_network;
    };

    net.set_function("IsServer", [requireNet]() { return requireNet().IsServer(); });
    net.set_function("IsClient", [requireNet]() { return requireNet().IsClient(); });
    net.set_function("IsConnected", [requireNet]() { return requireNet().IsConnected(); });
    net.set_function("Host", [requireNet](uint16_t port, sol::optional<int> maxClients) {
        return requireNet().StartServer(port, maxClients.value_or(8));
    });
    net.set_function("Connect", [requireNet](const std::string& ip, uint16_t port) {
        return requireNet().Connect(ip, port);
    });
    net.set_function("Stop", [requireNet]() { requireNet().Stop(); });
    net.set_function("ClientCount", [requireNet]() { return requireNet().ClientCount(); });
    net.set_function("Clients", [this, requireNet]() {
        sol::table t = m_lua.create_table();
        int i = 1;
        for (int id : requireNet().ClientIds()) t[i++] = id;
        return t;
    });

    // reliable по умолчанию true: геймплейная команда («выстрелил», «поднял»)
    // обязана дойти. Ненадёжно шлют то, что и так придёт следующим кадром.
    net.set_function("Send", [requireNet](const std::string& name, sol::object data,
                                          sol::optional<bool> reliable) {
        return requireNet().SendToServer(name, EncodePayload(data), reliable.value_or(true));
    });
    net.set_function("SendTo", [requireNet](int clientId, const std::string& name, sol::object data,
                                            sol::optional<bool> reliable) {
        return requireNet().SendToClient(clientId, name, EncodePayload(data),
                                         reliable.value_or(true));
    });
    net.set_function("Broadcast", [requireNet](const std::string& name, sol::object data,
                                               sol::optional<bool> reliable) {
        requireNet().BroadcastToClients(name, EncodePayload(data), reliable.value_or(true));
    });
}

void ScriptEngine::DispatchNetEvents() {
    if (!m_network) return;
    std::vector<sage::net::ScriptNetEvent> events = m_network->DrainScriptEvents();
    if (events.empty()) return;

    // Хуки ищутся в окружении каждого скрипта на момент доставки: события сети
    // редки, и динамический поиск дешевле, чем кэшировать ещё пять функций на
    // каждый скрипт сцены.
    for (sage::net::ScriptNetEvent& ev : events) {
        const char* hookName = nullptr;
        switch (ev.Kind) {
            case sage::net::ScriptNetEvent::Type::Message: hookName = "OnNetMessage"; break;
            case sage::net::ScriptNetEvent::Type::ClientConnected:
                hookName = "OnClientConnected";
                break;
            case sage::net::ScriptNetEvent::Type::ClientDisconnected:
                hookName = "OnClientDisconnected";
                break;
            case sage::net::ScriptNetEvent::Type::Connected: hookName = "OnNetConnected"; break;
            case sage::net::ScriptNetEvent::Type::Disconnected:
                hookName = "OnNetDisconnected";
                break;
        }
        if (!hookName) continue;

        sol::object data;
        if (ev.Kind == sage::net::ScriptNetEvent::Type::Message) {
            sage::net::ByteReader r{ev.Payload.data(), ev.Payload.size(), 0, true};
            data = ev.Payload.empty() ? sol::make_object(m_lua, sol::lua_nil)
                                      : DecodeLuaValue(m_lua, r, 0);
        }

        // План вызовов собирается ДО исполнения: обработчик вправе создать или
        // удалить сущность, а это переаллоцирует m_instances у нас под ногами.
        struct Call { sol::protected_function Fn; std::string Path; };
        std::vector<Call> calls;
        for (ScriptInstance& inst : m_instances) {
            if (inst.HasObject && !inst.Object.Valid()) continue;
            sol::protected_function fn = inst.Env[hookName];
            if (fn.valid()) calls.push_back({fn, inst.Path});
        }
        for (Call& c : calls) {
            sol::protected_function_result result;
            switch (ev.Kind) {
                case sage::net::ScriptNetEvent::Type::Message:
                    result = c.Fn(ev.Name, data, ev.ClientId);
                    break;
                case sage::net::ScriptNetEvent::Type::ClientConnected:
                case sage::net::ScriptNetEvent::Type::ClientDisconnected:
                    result = c.Fn(ev.ClientId);
                    break;
                default:
                    result = c.Fn();
                    break;
            }
            if (result.valid()) continue;
            sol::error err = result;
            LOG_ERROR("ScriptEngine")
                << "Ошибка в " << hookName << " (" << c.Path << "): " << err.what();
        }
    }
}
