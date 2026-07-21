#include "TestFramework.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "sage/net/NetHost.h"
#include "sage/net/NetworkSystem.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scripting/ScriptEngine.h"

// ---------------------------------------------------------------------------
// Тесты мультиплеера (sage/net) — НАСТОЯЩИЕ UDP-сокеты на loopback, headless.
// Время передаётся явно: «кадры» прокачиваются циклом с ручными часами, без
// wall-clock ожиданий (loopback доставляет датаграммы синхронно). Потери
// моделируются детерминированной симуляцией на приёме (UdpSocket).
// ---------------------------------------------------------------------------

using namespace sage::net;

namespace {

// Прокачивает пару сервер/клиент count шагов по step секунд.
void Pump(NetServer& server, NetClient& client, double& now, int count, double step = 0.02) {
    for (int i = 0; i < count; ++i) {
        now += step;
        client.Update(now);
        server.Update(now);
    }
}

std::string WriteTempScript(const std::string& name, const std::string& body) {
    std::string path = std::string("sage_net_test_") + name + ".lua";
    std::ofstream f(path);
    f << body;
    return path;
}

} // namespace

TEST(Net_address_parse_roundtrip) {
    NetAddress a = NetAddress::Parse("192.168.1.42", 7777);
    CHECK_TRUE(a.Valid());
    CHECK_EQ(a.Ip, 0xC0A8012Au);
    CHECK_EQ(a.ToString(), std::string("192.168.1.42:7777"));
    CHECK_FALSE(NetAddress::Parse("300.1.1.1", 1).Valid());
    CHECK_FALSE(NetAddress::Parse("1.2.3", 1).Valid());
    CHECK_FALSE(NetAddress::Parse("abc", 1).Valid());
}

TEST(Net_connect_and_exchange) {
    NetServer server;
    CHECK_TRUE(server.Start(0)); // эфемерный порт
    NetClient client;
    CHECK_TRUE(client.Connect(NetAddress::Loopback(server.Port())));

    double now = 0.0;
    Pump(server, client, now, 10);
    CHECK_TRUE(client.Connected());

    bool serverSawConnect = false;
    for (auto& ev : server.DrainEvents())
        if (ev.Kind == NetEvent::Type::ClientConnected) serverSawConnect = true;
    CHECK_TRUE(serverSawConnect);

    // Клиент -> сервер (надёжно).
    const char* hello = "privet-server";
    client.Send(hello, strlen(hello), true);
    Pump(server, client, now, 5);
    std::string got;
    int senderId = -1;
    for (auto& ev : server.DrainEvents())
        if (ev.Kind == NetEvent::Type::Message) {
            got.assign(ev.Data.begin(), ev.Data.end());
            senderId = ev.ClientId;
        }
    CHECK_EQ(got, std::string(hello));
    CHECK_TRUE(senderId >= 1);

    // Сервер -> клиент.
    const char* reply = "privet-client";
    server.Send(senderId, reply, strlen(reply), true);
    Pump(server, client, now, 5);
    std::string back;
    for (auto& ev : client.DrainEvents())
        if (ev.Kind == NetEvent::Type::Message) back.assign(ev.Data.begin(), ev.Data.end());
    CHECK_EQ(back, std::string(reply));

    client.Disconnect();
    Pump(server, client, now, 3);
    bool sawDisconnect = false;
    for (auto& ev : server.DrainEvents())
        if (ev.Kind == NetEvent::Type::ClientDisconnected) sawDisconnect = true;
    CHECK_TRUE(sawDisconnect);
}

TEST(Net_reliable_ordered_under_loss) {
    NetServer server;
    CHECK_TRUE(server.Start(0));
    NetClient client;
    CHECK_TRUE(client.Connect(NetAddress::Loopback(server.Port())));

    double now = 0.0;
    Pump(server, client, now, 10);
    CHECK_TRUE(client.Connected());

    // 35% потерь в ОБЕ стороны: и данные, и ack-и теряются.
    server.Socket().SetSimulatedLoss(0.35f, 7);
    client.Socket().SetSimulatedLoss(0.35f, 13);

    const int kCount = 60;
    for (int i = 0; i < kCount; ++i) {
        std::string msg = "msg-" + std::to_string(i);
        client.Send(msg.data(), msg.size(), true);
    }

    // Прокачиваем достаточно долго, чтобы переотправки пробились сквозь потери.
    std::vector<std::string> received;
    for (int step = 0; step < 400 && (int)received.size() < kCount; ++step) {
        now += 0.03;
        client.Update(now);
        server.Update(now);
        for (auto& ev : server.DrainEvents())
            if (ev.Kind == NetEvent::Type::Message)
                received.emplace_back(ev.Data.begin(), ev.Data.end());
    }

    CHECK_EQ((int)received.size(), kCount);
    // Надёжный канал сохраняет ПОРЯДОК отправки, несмотря на потери.
    bool ordered = true;
    for (int i = 0; i < (int)received.size(); ++i)
        if (received[i] != "msg-" + std::to_string(i)) { ordered = false; break; }
    CHECK_TRUE(ordered);
}

TEST(Net_fragmentation_large_message) {
    NetServer server;
    CHECK_TRUE(server.Start(0));
    NetClient client;
    CHECK_TRUE(client.Connect(NetAddress::Loopback(server.Port())));
    double now = 0.0;
    Pump(server, client, now, 10);
    CHECK_TRUE(client.Connected());

    // 10 КБ — с запасом больше MTU: уйдёт фрагментами и соберётся байт в байт.
    std::vector<uint8_t> big(10000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = (uint8_t)(i * 31 + 7);
    client.Send(big.data(), big.size(), true);

    std::vector<uint8_t> got;
    for (int step = 0; step < 100 && got.empty(); ++step) {
        now += 0.02;
        client.Update(now);
        server.Update(now);
        for (auto& ev : server.DrainEvents())
            if (ev.Kind == NetEvent::Type::Message) got = ev.Data;
    }
    CHECK_EQ(got.size(), big.size());
    CHECK_TRUE(got == big);
}

TEST(Net_replication_ghost_lifecycle) {
    // Сервер реплицирует сущность: у клиента появляется «призрак» с тем же id,
    // движение доезжает (с интерполяционной задержкой), удаление — тоже.
    Scene serverScene("srv");
    Scene clientScene("cli");

    GameObject cube = serverScene.CreateObject("cube");
    cube.Renderer().Ref = MeshRef{MeshRef::Type::Cube, ""};
    cube.Renderer().Color = {1.0f, 0.2f, 0.2f};
    cube.GetTransform().Position = {5.0f, 1.0f, -3.0f};
    serverScene.Registry().emplace<NetReplicatedComponent>(cube.Entity());
    const int netId = cube.Id();

    NetworkSystem server, client;
    CHECK_TRUE(server.StartServer(0));
    CHECK_TRUE(client.Connect("127.0.0.1", server.ServerPort()));

    double now = 0.0;
    auto pump = [&](int steps) {
        for (int i = 0; i < steps; ++i) {
            now += 0.05;
            server.UpdateAt(serverScene, now);
            client.UpdateAt(clientScene, now);
        }
    };

    pump(20);
    CHECK_TRUE(client.IsConnected());

    // Призрак появился с серверным id, мешом и цветом.
    GameObject ghost = clientScene.Get(netId);
    CHECK_TRUE(ghost.Valid());
    CHECK_TRUE(ghost.Renderer().Ref.type == MeshRef::Type::Cube);
    CHECK_NEAR(ghost.Renderer().Color.r, 1.0f, 1e-3f);

    // Движение на сервере доезжает до клиента (учитывая задержку интерполяции).
    cube.GetTransform().Position = {9.0f, 1.0f, -3.0f};
    pump(20);
    CHECK_NEAR(clientScene.Get(netId).GetTransform().Position.x, 9.0f, 0.05f);

    // Сервер удалил сущность -> призрак исчезает из клиентской сцены.
    serverScene.RemoveObject(netId);
    pump(10);
    CHECK_FALSE(clientScene.Get(netId).Valid());

    client.Stop();
    server.Stop();
}

TEST(Net_lua_messages_end_to_end) {
    // Полный путь Lua->сеть->Lua: клиентский скрипт шлёт таблицу Net.Send,
    // серверный принимает её в OnNetMessage и отвечает; клиент видит ответ.
    Scene serverScene("srv");
    Scene clientScene("cli");
    NetworkSystem serverNet, clientNet;
    CHECK_TRUE(serverNet.StartServer(0));
    CHECK_TRUE(clientNet.Connect("127.0.0.1", serverNet.ServerPort()));

    ScriptEngine serverScripts, clientScripts;
    serverScripts.BindScene(serverScene);
    serverScripts.BindNetwork(serverNet);
    clientScripts.BindScene(clientScene);
    clientScripts.BindNetwork(clientNet);

    // Окружения скриптов изолированы — результаты наблюдаем C++-колбэками.
    double gotPing = 0.0, gotReply = 0.0;
    std::string gotTag;
    serverScripts.Lua().set_function("TestPing", [&](double v) { gotPing = v; });
    clientScripts.Lua().set_function("TestPong", [&](double v, const std::string& t) {
        gotReply = v;
        gotTag = t;
    });

    std::string serverLua = WriteTempScript("srv", R"LUA(
        function OnNetMessage(name, data, sender)
            if name == "ping" then
                TestPing(data.value)
                Net.SendTo(sender, "pong", {reply = data.value * 2, tag = "ok"})
            end
        end
        function OnUpdate(dt) end
    )LUA");
    std::string clientLua = WriteTempScript("cli", R"LUA(
        local sent = false
        function OnUpdate(dt)
            if Net.IsConnected() and not sent then
                sent = true
                Net.Send("ping", {value = 21})
            end
        end
        function OnNetMessage(name, data, sender)
            if name == "pong" then TestPong(data.reply, data.tag) end
        end
    )LUA");
    serverScripts.RunScript(serverLua);
    clientScripts.RunScript(clientLua);

    double now = 0.0;
    for (int i = 0; i < 40; ++i) {
        now += 0.05;
        serverNet.UpdateAt(serverScene, now);
        clientNet.UpdateAt(clientScene, now);
        serverScripts.UpdateAll(0.05f);
        clientScripts.UpdateAll(0.05f);
    }

    CHECK_NEAR(gotPing, 21.0, 1e-6);
    CHECK_NEAR(gotReply, 42.0, 1e-6);
    CHECK_EQ(gotTag, std::string("ok"));

    clientNet.Stop();
    serverNet.Stop();
    std::remove(serverLua.c_str());
    std::remove(clientLua.c_str());
}

TEST(Net_server_full_denies_connection) {
    NetServer server;
    CHECK_TRUE(server.Start(0, /*maxClients=*/1));
    NetClient a, b;
    CHECK_TRUE(a.Connect(NetAddress::Loopback(server.Port())));
    double now = 0.0;
    for (int i = 0; i < 10; ++i) { now += 0.02; a.Update(now); server.Update(now); }
    CHECK_TRUE(a.Connected());

    CHECK_TRUE(b.Connect(NetAddress::Loopback(server.Port())));
    for (int i = 0; i < 10; ++i) { now += 0.02; b.Update(now); server.Update(now); }
    CHECK_FALSE(b.Connected()); // сервер заполнен -> ConnectDeny
    CHECK_EQ(server.ClientCount(), 1);
}
