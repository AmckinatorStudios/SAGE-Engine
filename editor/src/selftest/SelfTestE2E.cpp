#include "SelfTestCommon.h"

// ============================================================================
//  E2E: полная игра через редактор (SAGE_EDITOR_E2E=1, headless CI)
//
//  «Без иллюзий»: РЕАЛЬНЫМИ операциями редактора (те же методы, что зовут
//  кнопки UI) создаётся проект-игра «Coin Rush» с Lua-логикой: бот сам едет к
//  монетам, собирает их через SendMessage, монеты уничтожают себя, счёт на
//  HUD обновляется из Lua (GetUI). Сцена сохраняется в .sage, ПЕРЕЧИТЫВАЕТСЯ
//  с диска, проигрывается в Play (проверяем: монеты исчезли, счёт «5/5»),
//  Stop откатывает мир, затем File > Build Game упаковывает ИСПОЛНЯЕМУЮ игру.
//  Отдельный шаг smoke-теста запускает СОБРАННЫЙ бинарник и убеждается по
//  логам/скриншоту, что та же Lua-логика работает и в нём.
// ============================================================================
void EditorLayer::RunE2EGameTest() {
    bool ok = true;
    std::error_code ec;
    fs::remove_all("e2e_game", ec);  // от прошлого прогона
    fs::remove_all("e2e_dist", ec);

    // --- 1. Проект (File > New Project) ---
    std::string err;
    // E2E собирает игру САМ, поэтому берёт пустой шаблон: демо-объекты
    // чужого шаблона в его проверках только мешали бы.
    if (!CreateProject(".", "e2e_game", "empty", err)) {
        LOG_ERROR("Editor") << "E2E: create project failed: " << err;
        LOG_ERROR("Editor") << "E2E: FAIL";
        return;
    }

    // --- 2. Скрипты игры — файлы в assets/scripts проекта (как из панели Assets) ---
    fs::create_directories(m_project.Dir() / "assets" / "scripts", ec);
    auto writeScript = [&](const char* name, const char* body) {
        std::ofstream f(m_project.Dir() / "assets" / "scripts" / name);
        if (!f) { ok = false; LOG_ERROR("Editor") << "E2E: can't write script " << name; }
        f << body;
    };
    // Бот: сам едет к ближайшей живой монете, вблизи шлёт ей "collect".
    writeScript("bot.lua", R"LUA(
function OnUpdate(entity, dt)
    local coin = nil
    for i = 1, 5 do
        local c = FindObject("Coin" .. i)
        if c ~= nil then coin = c break end
    end
    if coin == nil then return end
    local p = entity.Transform.Position
    local d = coin.Transform.Position - p
    d.y = 0.0
    if d:Length() < 0.45 then
        SendMessage(coin, "collect")
    else
        entity.Transform.Position = p + d:Normalized() * (2.5 * dt)
    end
end
)LUA");
    // Монета: крутится; по "collect" объявляет счёт и уничтожает себя.
    writeScript("coin.lua", R"LUA(
function OnUpdate(entity, dt)
    entity.Transform.Rotation.y = entity.Transform.Rotation.y + dt * 240.0
end
function OnMessage(entity, name, data)
    if name == "collect" then
        log("E2E: coin collected: " .. entity.Name)
        Broadcast("scored")
        entity:Destroy()
    end
end
)LUA");
    // HUD: копит счёт по "scored", пишет текст и заполняет полосу из Lua.
    writeScript("hud.lua", R"LUA(
local score = 0
function OnStart(entity)
    local ui = entity:GetUI()
    if ui ~= nil then ui.Text = "Score: 0 / 5" end
end
function OnMessage(entity, name, data)
    if name ~= "scored" then return end
    score = score + 1
    local ui = entity:GetUI()
    if ui ~= nil then ui.Text = "Score: " .. score .. " / 5" end
    local bar = FindObject("Score Bar")
    if bar ~= nil then bar:GetUI().Value = score / 5.0 end
    if score >= 5 then log("E2E: ALL COINS COLLECTED") end
end
)LUA");

    // --- 3. Сцена игры — редакторскими операциями создания сущностей ---
    NewScene(ProjectTemplateKind::Empty);
    m_scene->Lighting.Skybox.Enabled = true;

    GameObject ground = CreatePrimitiveEntity("Ground", MeshRef::Type::Cube);
    ground.GetTransform().Position = {0.0f, -0.55f, 0.0f};
    ground.GetTransform().Scale = {14.0f, 0.3f, 6.0f};
    ground.Renderer().Color = {0.28f, 0.30f, 0.34f};

    GameObject bot = CreatePrimitiveEntity("Bot", MeshRef::Type::Cube);
    bot.GetTransform().Position = {-6.0f, 0.1f, 0.0f};
    bot.GetTransform().Scale = {0.8f, 0.8f, 0.8f};
    bot.Renderer().Color = {0.30f, 0.55f, 0.95f};
    m_scene->Registry().emplace<ScriptComponent>(bot.Entity(), ScriptComponent{"assets/scripts/bot.lua"});

    for (int i = 1; i <= 5; ++i) {
        GameObject coin = CreatePrimitiveEntity("Coin" + std::to_string(i), MeshRef::Type::Sphere);
        coin.GetTransform().Position = {-4.0f + (float)i * 1.6f, 0.35f, 0.0f};
        coin.GetTransform().Scale = {0.45f, 0.45f, 0.45f};
        coin.Renderer().Color = {0.95f, 0.80f, 0.20f};
        m_scene->Registry().emplace<ScriptComponent>(coin.Entity(), ScriptComponent{"assets/scripts/coin.lua"});
    }

    GameObject cam = m_scene->CreateObject("Main Camera");
    cam.GetTransform().Position = {0.0f, 2.5f, 8.5f};
    cam.GetTransform().Rotation = {-12.0f, 0.0f, 0.0f};
    m_scene->Registry().emplace<CameraComponent>(cam.Entity());

    entt::registry& uiReg = m_scene->Registry();
    GameObject hud = m_scene->CreateObject("HUD");
    sage::ui::Element hudXf;
    hudXf.Position = {16.0f, 16.0f};
    hudXf.Size = {240.0f, 64.0f};
    uiReg.emplace<sage::ui::Element>(hud.Entity(), hudXf);
    sage::ui::Fill hudFill;
    hudFill.Rounding = 12.0f;
    hudFill.BorderThickness = 2.0f;
    uiReg.emplace<sage::ui::Fill>(hud.Entity(), hudFill);
    sage::ui::Label hudLabel;
    hudLabel.Text = "Score: 0 / 5";
    hudLabel.Horizontal = sage::ui::Label::Align::Start;
    uiReg.emplace<sage::ui::Label>(hud.Entity(), hudLabel);
    uiReg.emplace<ScriptComponent>(hud.Entity(), ScriptComponent{"assets/scripts/hud.lua"});

    GameObject bar = m_scene->CreateObject("Score Bar");
    sage::ui::Element barXf;
    barXf.Anchor = UIAnchor::BottomLeft;
    barXf.Position = {12.0f, 8.0f};
    barXf.Size = {216.0f, 16.0f};
    uiReg.emplace<sage::ui::Element>(bar.Entity(), barXf);
    sage::ui::Fill barFill;
    barFill.Rounding = 7.0f;
    barFill.Color = {0.0f, 0.0f, 0.0f, 0.55f};
    uiReg.emplace<sage::ui::Fill>(bar.Entity(), barFill);
    sage::ui::Bar barBar;
    barBar.Value = 0.0f;
    barBar.SetFillColor({0.95f, 0.80f, 0.20f, 1.0f});
    uiReg.emplace<sage::ui::Bar>(bar.Entity(), barBar);
    m_scene->SetParent(bar.Entity(), hud.Entity());

    // --- 4. Сохранить и ПЕРЕЧИТАТЬ с диска: играем то, что реально в файле ---
    fs::path scenePath = m_project.ScenesDir() / "main.sage";
    if (!SaveSceneToFile(scenePath) || !LoadSceneFromFile(scenePath)) {
        LOG_ERROR("Editor") << "E2E: scene save/reload failed";
        ok = false;
    }

    // --- 5. Play: логика Lua реально играет (бот собирает все монеты) ---
    if (ok) {
        StartPlay();
        for (int i = 0; i < 240 && ok; ++i) m_play.StepScripts(*m_scene, 0.05f); // ~12 c игры
        bool coinsGone = true;
        for (int i = 1; i <= 5; ++i)
            if (m_scene->FindByName("Coin" + std::to_string(i)).Valid()) coinsGone = false;
        const sage::ui::Label* hudNow =
            m_scene->Registry().try_get<sage::ui::Label>(m_scene->FindByName("HUD").Entity());
        bool scoreOk = hudNow && hudNow->Text == "Score: 5 / 5";
        const sage::ui::Bar* barNow =
            m_scene->Registry().try_get<sage::ui::Bar>(m_scene->FindByName("Score Bar").Entity());
        bool barOk = barNow && std::abs(barNow->Value - 1.0f) < 0.001f;
        if (!coinsGone || !scoreOk || !barOk) {
            LOG_ERROR("Editor") << "E2E: play logic failed (coinsGone=" << coinsGone
                                << " scoreOk=" << scoreOk << " barOk=" << barOk
                                << " hudText='" << (hudNow ? hudNow->Text : "?") << "')";
            ok = false;
        }
        StopPlay();
        // Stop обязан вернуть мир: монеты на месте, счёт исходный.
        if (ok && (!m_scene->FindByName("Coin1").Valid() || !m_scene->FindByName("Coin5").Valid())) {
            LOG_ERROR("Editor") << "E2E: stop did not restore coins";
            ok = false;
        }
    }

    // --- 6. File > Build Game: упаковка ИСПОЛНЯЕМОЙ игры ---
    if (ok) {
        std::string buildErr;
        if (!BuildGame("e2e_dist", buildErr)) {
            LOG_ERROR("Editor") << "E2E: build game failed: " << buildErr;
            ok = false;
        } else {
#ifdef _WIN32
            fs::path exe = fs::path("e2e_dist") / "e2e_game" / "e2e_game.exe";
#else
            fs::path exe = fs::path("e2e_dist") / "e2e_game" / "e2e_game";
#endif
            if (!fs::exists(exe, ec)) {
                LOG_ERROR("Editor") << "E2E: built game exe missing: " << exe.string();
                ok = false;
            }
        }
    }

    if (ok) LOG_INFO("Editor") << "E2E: PASS (project + lua scripts + scene save/reload + "
                                  "play logic [5 coins, messaging, HUD from Lua] + stop restore + build exe)";
    else LOG_ERROR("Editor") << "E2E: FAIL";
}


// ============================================================================
//  Headless-сессия над ЧУЖИМ проектом (SAGE_EDITOR_OPEN_PROJECT=<путь>)
//
//  ЗАЧЕМ. Self-test и E2E выше проверяют редактор на игре, которую редактор же
//  и сочинил внутри своего кода. Настоящая игра живёт в отдельном репозитории,
//  её сцены и скрипты пишет человек — и до сих пор ни одна проверка не открывала
//  такой проект НАСТОЯЩИМ редактором. Эта функция закрывает дыру: CI игры
//  запускает SageEditor поверх своего проекта теми же операциями, что и человек
//  мышкой — Open Project, выбрать сцену, Play, Stop, Build Game.
//
//  Управление — переменными окружения (headless, никаких модалок):
//    SAGE_EDITOR_OPEN_PROJECT=<путь>   папка с project.sageproj (обязательна)
//    SAGE_EDITOR_OPEN_SCENE=<имя>      сцена в scenes/ (по умолчанию main.sage)
//    SAGE_EDITOR_PLAY_SECONDS=<сек>    сколько отыграть в Play (0 — не играть)
//    SAGE_EDITOR_PLAY_STEP=<сек>       шаг симуляции (по умолчанию 1/60)
//    SAGE_GAME_ARGS="autopilot=1"      параметры запуска для скриптов игры
//    SAGE_EDITOR_BUILD_TO=<папка>      собрать игру (File > Build Game)
//  Итог — строкой SESSION: PASS/FAIL в лог, чтобы CI искал её грепом.
// ============================================================================
void EditorLayer::RunHeadlessProjectSession() {
    const std::string projectDir = sage::EnvString("SAGE_EDITOR_OPEN_PROJECT");
    std::string err;
    if (!OpenProject(projectDir, err)) {
        LOG_ERROR("Editor") << "SESSION: open project failed: " << err;
        LOG_ERROR("Editor") << "SESSION: FAIL";
        return;
    }
    LOG_INFO("Editor") << "SESSION: opened project '" << m_project.Name() << "' at " << projectDir;

    const char* sceneName = std::getenv("SAGE_EDITOR_OPEN_SCENE");
    fs::path scenePath = m_project.ScenesDir() / (sceneName ? sceneName : "main.sage");
    if (!LoadSceneFromFile(scenePath)) {
        LOG_ERROR("Editor") << "SESSION: load scene failed: " << scenePath.string();
        LOG_ERROR("Editor") << "SESSION: FAIL";
        return;
    }
    LOG_INFO("Editor") << "SESSION: loaded scene " << scenePath.filename().string()
                       << " (" << m_scene->Count() << " entities)";

    bool ok = true;

    // --- Save Scene: та же кнопка, что у человека (Ctrl+S). Нужна затем, что
    // перезапись файла — единственный способ ПОДНЯТЬ его до текущего формата:
    // миграция при загрузке живёт в памяти и на диск сама не возвращается.
    // Без этого проект, лежащий в старой версии, так в ней и остаётся, а
    // проверить «сохранилось ли то, что прочиталось» нечем.
    if (std::getenv("SAGE_EDITOR_SAVE_SCENE")) {
        if (!SaveSceneToFile(scenePath)) {
            LOG_ERROR("Editor") << "SESSION: save scene failed: " << scenePath.string();
            ok = false;
        } else {
            LOG_INFO("Editor") << "SESSION: saved scene " << scenePath.filename().string();
        }
    }

    // --- Play: та же кнопка, что у человека. Время идёт фиксированным шагом,
    // а не по-настоящему: headless-прогон должен быть воспроизводимым, иначе
    // «успела ли игра дойти до события» зависело бы от загрузки CI-машины. ---
    float playSeconds = 0.0f;
    if (const char* s = std::getenv("SAGE_EDITOR_PLAY_SECONDS")) playSeconds = (float)std::atof(s);
    if (playSeconds > 0.0f) {
        float step = 1.0f / 60.0f;
        if (const char* s = std::getenv("SAGE_EDITOR_PLAY_STEP")) {
            float parsed = (float)std::atof(s);
            if (parsed > 0.0f) step = parsed;
        }
        // Параметры запуска (SAGE_GAME_ARGS) StartPlay раздаёт скриптам сам —
        // до их OnStart, иначе автопрогон не успел бы включиться.
        StartPlay();
        size_t stepCount = (size_t)(playSeconds / step);
        for (size_t i = 0; i < stepCount; ++i) {
            m_play.StepScripts(*m_scene, step);
            if (m_play.Physics()) m_play.Physics()->Step(*m_scene, step);
        }
        LOG_INFO("Editor") << "SESSION: played " << playSeconds << "s in " << stepCount
                           << " steps (" << m_scene->Count() << " entities at the end)";
        size_t duringPlay = m_scene->Count();
        StopPlay();
        // Stop обязан вернуть мир к состоянию до Play. Для игры, которая
        // порождает мир скриптами (воксельный ландшафт — тысячи сущностей),
        // это самая содержательная проверка снапшота из всех: если откат течёт,
        // здесь он потечёт заметно.
        LOG_INFO("Editor") << "SESSION: stop restored scene to " << m_scene->Count()
                           << " entities (was " << duringPlay << " during play)";
    }

    // --- File > Build Game: проект превращается в запускаемую игру ---
    if (ok) {
        if (const std::string out = sage::EnvString("SAGE_EDITOR_BUILD_TO"); !out.empty()) {
            std::string buildErr;
            if (!BuildGame(out, buildErr)) {
                LOG_ERROR("Editor") << "SESSION: build game failed: " << buildErr;
                ok = false;
            } else {
                LOG_INFO("Editor") << "SESSION: built game into " << out;
            }
        }
    }

    if (ok) LOG_INFO("Editor") << "SESSION: PASS";
    else LOG_ERROR("Editor") << "SESSION: FAIL";
}
