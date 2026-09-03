#include "TestFramework.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

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

// ---------------------------------------------------------------------------
// Репликация под нагрузкой и в плохой сети: то, чего одиночный куб на идеальном
// loopback не проверяет.
// ---------------------------------------------------------------------------

namespace {

// Сцена из count реплицируемых кубов в ряд. Тридцать штук — уже больше того,
// что влезает в один пакет: снапшот такой сцены неизбежно бьётся на части.
void FillReplicated(Scene& scene, int count) {
    for (int i = 0; i < count; ++i) {
        GameObject o = scene.CreateObject("cube" + std::to_string(i));
        o.Renderer().Ref = MeshRef{MeshRef::Type::Cube, ""};
        o.Renderer().Color = {0.5f, 0.5f, 0.5f};
        o.GetTransform().Position = {(float)i, 0.0f, 0.0f};
        scene.Registry().emplace<NetReplicatedComponent>(o.Entity());
    }
}

int CountGhosts(Scene& scene, int expectIds) {
    int n = 0;
    for (int i = 1; i <= expectIds; ++i)
        if (scene.Get(i).Valid()) ++n;
    return n;
}

} // namespace

TEST(Net_replicates_more_entities_than_fit_in_one_packet) {
    // ТРИДЦАТЬ сущностей — снапшот заведомо длиннее одного пакета. Пока он ехал
    // одним куском, это означало фрагментацию НЕНАДЁЖНОГО сообщения: любой
    // потерянный кусок убивал снапшот целиком, а недособранные части копились
    // в приёмнике. Снапшот обязан ехать самостоятельными частями.
    Scene serverScene("srv"), clientScene("cli");
    FillReplicated(serverScene, 30);

    NetworkSystem server, client;
    CHECK_TRUE(server.StartServer(0));
    CHECK_TRUE(client.Connect("127.0.0.1", server.ServerPort()));

    double now = 0.0;
    for (int i = 0; i < 30; ++i) {
        now += 0.05;
        server.UpdateAt(serverScene, now);
        client.UpdateAt(clientScene, now);
    }
    CHECK_TRUE(client.IsConnected());
    CHECK_EQ(CountGhosts(clientScene, 30), 30);

    client.Stop();
    server.Stop();
}

TEST(Net_replication_survives_packet_loss) {
    // Треть датаграмм теряется. Снапшоты ненадёжны намеренно (свежесть важнее
    // доставки), поэтому проверяется не «дошло всё», а что сцена клиента
    // СХОДИТСЯ: призраки на месте и стоят там, где им положено.
    Scene serverScene("srv"), clientScene("cli");
    FillReplicated(serverScene, 24);

    NetworkSystem server, client;
    CHECK_TRUE(server.StartServer(0));
    CHECK_TRUE(client.Connect("127.0.0.1", server.ServerPort()));

    double now = 0.0;
    for (int i = 0; i < 20; ++i) {   // сперва подключаемся по чистой сети
        now += 0.05;
        server.UpdateAt(serverScene, now);
        client.UpdateAt(clientScene, now);
    }
    CHECK_TRUE(client.IsConnected());

    client.Client().Socket().SetSimulatedLoss(0.34f, 7);
    for (int i = 0; i < 120; ++i) {
        now += 0.05;
        server.UpdateAt(serverScene, now);
        client.UpdateAt(clientScene, now);
    }
    client.Client().Socket().SetSimulatedLoss(0.0f);
    for (int i = 0; i < 20; ++i) {
        now += 0.05;
        server.UpdateAt(serverScene, now);
        client.UpdateAt(clientScene, now);
    }

    CHECK_TRUE(client.IsConnected());   // потери не должны рвать соединение
    CHECK_EQ(CountGhosts(clientScene, 24), 24);
    // И позиции сошлись: сущность i стоит в x = i.
    for (int i = 0; i < 24; ++i) {
        GameObject g = clientScene.Get(i + 1);
        CHECK_TRUE(g.Valid());
        if (g.Valid()) CHECK_NEAR(g.GetTransform().Position.x, (float)i, 0.05f);
    }

    client.Stop();
    server.Stop();
}

TEST(Net_stale_snapshot_does_not_rewind_entities) {
    // Снапшоты идут ненадёжным каналом, а UDP вправе доставить их НЕ В ТОМ
    // ПОРЯДКЕ. Пока в снапшоте не было номера, устаревший пакет применялся
    // наравне со свежим — и объект дёргался назад. Проверяем ту же ситуацию
    // напрямую: применяем свежий снапшот, затем устаревший.
    Scene serverScene("srv"), clientScene("cli");
    GameObject cube = serverScene.CreateObject("cube");
    cube.Renderer().Ref = MeshRef{MeshRef::Type::Cube, ""};
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
    CHECK_TRUE(clientScene.Get(netId).Valid());

    // Объект уезжает. Клиент теряет ЧАСТЬ снапшотов по дороге — но ни один из
    // дошедших не имеет права откатить его назад.
    float lastX = clientScene.Get(netId).GetTransform().Position.x;
    for (int step = 1; step <= 40; ++step) {
        cube.GetTransform().Position.x = (float)step;
        pump(2);
        const float x = clientScene.Get(netId).GetTransform().Position.x;
        // Небольшой откат допустим только на интерполяции внутри пары
        // снапшотов; настоящий откат — это метры.
        CHECK_TRUE(x >= lastX - 0.51f);
        lastX = x;
    }

    client.Stop();
    server.Stop();
}

TEST(Net_disconnect_removes_ghosts_from_scene) {
    // Отключились — чужие сущности обязаны уйти из сцены. Иначе после разрыва
    // в мире остаются призраки: неуправляемые, неудаляемые и сохраняющиеся
    // вместе со сценой.
    Scene serverScene("srv"), clientScene("cli");
    FillReplicated(serverScene, 5);

    NetworkSystem server, client;
    CHECK_TRUE(server.StartServer(0));
    CHECK_TRUE(client.Connect("127.0.0.1", server.ServerPort()));

    double now = 0.0;
    for (int i = 0; i < 20; ++i) {
        now += 0.05;
        server.UpdateAt(serverScene, now);
        client.UpdateAt(clientScene, now);
    }
    CHECK_EQ(CountGhosts(clientScene, 5), 5);

    // Stop сцены не знает — он её и не получает (его зовут из скрипта, из
    // смены уровня, из деструктора). Призраки помечаются и снимаются ближайшим
    // Update: тем же кадром, в котором игра увидела бы разрыв.
    client.Stop();
    now += 0.05;
    client.UpdateAt(clientScene, now);
    CHECK_EQ(CountGhosts(clientScene, 5), 0);

    // И повторный Update не должен ничего ломать (список уже пуст).
    now += 0.05;
    client.UpdateAt(clientScene, now);
    CHECK_EQ(CountGhosts(clientScene, 5), 0);
    server.Stop();
}

TEST(Net_interpolation_takes_the_short_way_around) {
    // Поворот едет ЕВЛЕРОВЫМИ УГЛАМИ, и на переходе через 180 линейная
    // интерполяция разворачивает объект в обратную сторону через полный круг.
    // Проверяем прямо: угол идёт с 170 на -170 (то есть на 20 градусов вперёд),
    // и промежуточное значение обязано лежать ЗА 180, а не между ними.
    Scene serverScene("srv"), clientScene("cli");
    GameObject cube = serverScene.CreateObject("cube");
    cube.Renderer().Ref = MeshRef{MeshRef::Type::Cube, ""};
    serverScene.Registry().emplace<NetReplicatedComponent>(cube.Entity());
    const int netId = cube.Id();

    NetworkSystem server, client;
    CHECK_TRUE(server.StartServer(0));
    CHECK_TRUE(client.Connect("127.0.0.1", server.ServerPort()));

    double now = 0.0;
    auto pump = [&](int steps, double dt = 0.05) {
        for (int i = 0; i < steps; ++i) {
            now += dt;
            server.UpdateAt(serverScene, now);
            client.UpdateAt(clientScene, now);
        }
    };
    cube.GetTransform().Rotation.y = 170.0f;
    pump(20);

    cube.GetTransform().Rotation.y = -170.0f;
    // Мелкими шагами: ловим момент, когда интерполяция идёт МЕЖДУ 170 и -170.
    bool sawShortWay = true;
    bool sawIntermediate = false;   // проверка не должна пройти вхолостую
    for (int i = 0; i < 40; ++i) {
        pump(1, 0.006);
        const float y = clientScene.Get(netId).GetTransform().Rotation.y;
        // Промежуточное значение короткой дуги лежит ЗА 180 (по модулю больше
        // 170); длинный путь проходит через ноль.
        if (std::abs(y) < 90.0f) sawShortWay = false;
        if (std::abs(std::abs(y) - 170.0f) > 0.5f) sawIntermediate = true;
    }
    CHECK_TRUE(sawIntermediate);
    CHECK_TRUE(sawShortWay);

    client.Stop();
    server.Stop();
}

TEST(Net_interpolation_actually_smooths_motion) {
    // Показ отстаёт от приёма на kInterpDelay ИМЕННО ЗАТЕМ, чтобы между двумя
    // снапшотами было что показывать. Пока состояний хранилось всего два, а
    // задержка равнялась двум интервалам, момент показа всегда оказывался
    // старше обоих — и клиент защёлкивался на снапшоте. Со стороны: мир едет
    // рывками двадцать раз в секунду при любой частоте кадра.
    //
    // Проверка ровно об этом: при кадре вчетверо чаще снапшота положение
    // обязано принимать заметно больше РАЗНЫХ значений, чем приходит снапшотов.
    Scene serverScene("srv"), clientScene("cli");
    GameObject cube = serverScene.CreateObject("cube");
    cube.Renderer().Ref = MeshRef{MeshRef::Type::Cube, ""};
    serverScene.Registry().emplace<NetReplicatedComponent>(cube.Entity());
    const int netId = cube.Id();

    NetworkSystem server, client;
    CHECK_TRUE(server.StartServer(0));
    CHECK_TRUE(client.Connect("127.0.0.1", server.ServerPort()));

    double now = 0.0;
    const double dt = 1.0 / 80.0;   // кадр 80 Гц против снапшота 20 Гц
    auto step = [&]() {
        now += dt;
        cube.GetTransform().Position.x = (float)(now * 4.0);  // ровное движение
        server.UpdateAt(serverScene, now);
        client.UpdateAt(clientScene, now);
    };
    for (int i = 0; i < 80; ++i) step();   // подключение и разгон

    std::vector<float> seen;
    for (int i = 0; i < 80; ++i) {
        step();
        GameObject g = clientScene.Get(netId);
        CHECK_TRUE(g.Valid());
        if (g.Valid()) seen.push_back(g.GetTransform().Position.x);
    }

    int distinct = 1;
    for (size_t i = 1; i < seen.size(); ++i)
        if (std::abs(seen[i] - seen[i - 1]) > 1e-4f) ++distinct;
    std::printf("       позиций за 80 кадров: %d (снапшотов пришло около %d)\n", distinct,
                (int)(80 * dt / kSnapshotInterval));

    // Снапшотов за это время около двадцати. Защёлкивание дало бы примерно
    // столько же различных значений; интерполяция — по значению на кадр.
    CHECK_TRUE(distinct > 60);
    // И движение МОНОТОННОЕ: интерполяция не имеет права ходить назад.
    for (size_t i = 1; i < seen.size(); ++i) CHECK_TRUE(seen[i] >= seen[i - 1] - 1e-3f);

    client.Stop();
    server.Stop();
}

// Id клиента уходит на провод ОДНИМ БАЙТОМ (ConnectAccept), а сервер держит
// его как int. Пока id выдавался растущим счётчиком, эти два числа расходились
// после 255 подключений за сессию: сервер помнил 256, клиенту уезжал 0. Дальше
// клиент считал бы своими чужие объекты — реплицируемые сущности адресуются
// именно владельцем.
//
// Сессия сервера длиннее одного матча: 256 входов и выходов набираются за
// вечер. Поэтому id берётся из СВОБОДНЫХ: вышедший клиент возвращает номер.
TEST(Net_client_ids_never_leave_one_byte) {
    NetServer server;
    CHECK_TRUE(server.Start(0, 2));

    int maxId = 0;
    for (int round = 0; round < 300; ++round) {
        NetClient client;
        CHECK_TRUE(client.Connect(NetAddress::Loopback(server.Port())));
        double now = (double)round * 10.0;   // свои часы у каждого круга
        Pump(server, client, now, 8);
        CHECK_TRUE(client.Connected());

        // Обе стороны обязаны звать клиента ОДНИМ И ТЕМ ЖЕ числом.
        std::vector<int> ids = server.ClientIds();
        CHECK_EQ(ids.size(), (size_t)1);
        if (!ids.empty()) CHECK_EQ(ids[0], client.ClientId());
        maxId = std::max(maxId, client.ClientId());

        client.Disconnect();
        Pump(server, client, now, 3);
        CHECK_EQ(server.ClientCount(), 0);
        server.DrainEvents();
    }

    // Триста подключений — и ни одного номера вне байта.
    std::printf("       300 подключений подряд, наибольший выданный id: %d\n", maxId);
    CHECK_TRUE(maxId <= 255);
    server.Stop();
}

// Больше 255 одновременных клиентов адресовать нечем — сервер обязан не
// принять лишних, а не выдать 256-му чужой номер. Проверяем настоящими
// сокетами: 260 клиентов на loopback, и ни одного повторного id.
TEST(Net_server_caps_max_clients_at_one_byte) {
    NetServer server;
    CHECK_TRUE(server.Start(0, 1000));   // просим заведомо больше, чем влезает
    const NetAddress addr = NetAddress::Loopback(server.Port());

    std::vector<std::unique_ptr<NetClient>> clients;
    for (int i = 0; i < 260; ++i) {
        auto c = std::make_unique<NetClient>();
        if (!c->Connect(addr)) break;   // кончились дескрипторы — не беда теста
        clients.push_back(std::move(c));
    }

    double now = 0.0;
    for (int i = 0; i < 20; ++i) {
        now += 0.02;
        for (auto& c : clients) c->Update(now);
        server.Update(now);
    }

    // Принятых не больше, чем влезает в байт, и все номера РАЗНЫЕ.
    std::vector<int> ids;
    for (auto& c : clients)
        if (c->Connected()) ids.push_back(c->ClientId());
    std::printf("       клиентов заведено %d, принято %d\n", (int)clients.size(), (int)ids.size());
    CHECK_TRUE((int)ids.size() <= 255);
    for (int id : ids) CHECK_TRUE(id >= 1 && id <= 255);
    std::vector<int> sorted = ids;
    std::sort(sorted.begin(), sorted.end());
    CHECK_TRUE(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    CHECK_EQ((size_t)server.ClientCount(), ids.size());

    for (auto& c : clients) c->Disconnect();
    server.Stop();
}
