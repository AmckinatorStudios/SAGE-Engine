#include "SelfTestCommon.h"

// --- физика, анимация, частицы, отсечение, настройки -----------------------
//
// Подсистемы движка глазами редактора: тело падает, кукла собирается, скелет
// проигрывается, эмиттер рождает частицы, отсечение отбрасывает невидимое,
// пресет качества доходит до картинки. Проверяется не сама подсистема (на это
// есть тесты движка), а то, что редактор её ЗАПУСКАЕТ.
bool EditorLayer::SelfTestSystems() {
    bool ok = true;
    std::error_code ec;
    std::string err;
    (void)ec; (void)err;

    // --- Физика: динамическое тело падает под гравитацией, Stop откатывает ---
    if (ok) {
        GameObject green = m_scene->FindByName("Green Cube");
        if (green.Valid()) {
            // Убираем крутящий скрипт, вешаем твёрдое тело — чистая физика.
            m_scene->Registry().remove<ScriptComponent>(green.Entity());
            m_scene->Registry().emplace_or_replace<RigidBodyComponent>(green.Entity());
            float yBefore = green.GetTransform().Position.y;

            StartPlay();
            if (!m_play.Physics() || m_play.Physics()->BodyCount() < 1) {
                LOG_ERROR("Editor") << "SELFTEST: physics failed - no bodies created";
                ok = false;
            }
            // Тоже через планировщик — по той же причине, что и скрипты выше.
            for (int i = 0; i < 10; ++i) m_systems.Run(*m_scene, 0.1f);
            float yDuring = m_scene->FindByName("Green Cube").GetTransform().Position.y;
            StopPlay();
            float yAfter = m_scene->FindByName("Green Cube").GetTransform().Position.y;

            if (ok && yDuring >= yBefore - 0.01f) {
                LOG_ERROR("Editor") << "SELFTEST: physics failed - body did not fall ("
                                    << yBefore << " -> " << yDuring << ")";
                ok = false;
            }
            if (ok && std::abs(yAfter - yBefore) > 0.001f) {
                LOG_ERROR("Editor") << "SELFTEST: physics stop failed - scene not restored (y "
                                    << yAfter << ", expected " << yBefore << ")";
                ok = false;
            }
        }
    }

    // --- Ragdoll + соединения: собранная кукла строит тела и суставы, падает ---
    if (ok) {
        int rootId = sage::physics::BuildRagdoll(*m_scene, {0.0f, 8.0f, 0.0f}, 1.0f);
        GameObject root = m_scene->Get(rootId);
        int jointComps = 0;
        m_scene->Registry().view<JointComponent>().each([&](auto) { ++jointComps; });
        bool built = root.Valid() &&
                     m_scene->Registry().all_of<RigidBodyComponent>(root.Entity()) &&
                     jointComps >= 6; // 11 костей -> 10 суставов (минимум 6)
        if (!built) {
            LOG_ERROR("Editor") << "SELFTEST: ragdoll build failed (root " << root.Valid()
                                << ", joints " << jointComps << ")";
            ok = false;
        } else {
            float yBefore = root.GetTransform().Position.y;
            StartPlay();
            // Суставы строятся и на Jolt, и на встроенном движке.
            bool jointsOk = !m_play.Physics()->SupportsJoints() || m_play.Physics()->JointCount() >= 6;
            for (int i = 0; i < 40; ++i) m_play.Physics()->Step(*m_scene, 1.0f / 60.0f);
            float yAfter = m_scene->Get(rootId).GetTransform().Position.y;
            StopPlay();
            if (!jointsOk) {
                LOG_ERROR("Editor") << "SELFTEST: ragdoll joints not built on Jolt ("
                                    << m_play.Physics()->JointCount() << ")";
                ok = false;
            } else if (yAfter >= yBefore - 0.05f) {
                LOG_ERROR("Editor") << "SELFTEST: ragdoll did not fall (" << yBefore
                                    << " -> " << yAfter << ")";
                ok = false;
            }
        }
    }

    // --- Модель персонажа доезжает ЦЕЛИКОМ ------------------------------------
    //
    // Отказ, с которого началась правка: «половина модели не рисуется, на
    // рассеянный свет не реагирует, выглядит бледной». Обе беды — у скелетного
    // пути: части, подвешенные к костям БЕЗ весов (так риггят зубы, глаза,
    // панели, провода — у проверочного Springtrap таких 161 из 179), он
    // выбрасывал, а материал читал на четверть — без карт металличности,
    // шероховатости, нормалей, затенения и свечения.
    //
    // Проверяется здесь ровно то, что ломалось: число частей и то, что у них
    // есть материал с картами. Как это ВЫГЛЯДИТ — дело кадровых тестов
    // (tests/render/Checks_Animation.cpp), здесь графики нет.
#ifdef SAGE_TEST_RIG
    if (ok) {
        GameObject full = m_scene->CreateObject("SelftestRigFull");
        MeshRendererComponent& fullMesh =
            m_scene->Registry().emplace<MeshRendererComponent>(full.Entity());
        fullMesh.Ref.type = MeshRef::Type::Model;
        fullMesh.Ref.path = SAGE_TEST_RIG;
        m_scene->Registry().emplace<AnimationComponent>(full.Entity());
        sage::anim::UpdateAnimators(*m_scene, 0.0f);

        const AnimationComponent& fam =
            m_scene->Registry().get<AnimationComponent>(full.Entity());
        if (!fam.Model) {
            LOG_ERROR("Editor") << "SELFTEST: оснастка " << SAGE_TEST_RIG << " не загрузилась";
            ok = false;
        } else if (fam.Model->SubMeshCount() != 3) {
            LOG_ERROR("Editor") << "SELFTEST: у модели " << fam.Model->SubMeshCount()
                                << " частей вместо 3 — жёсткие детали на костях потеряны";
            ok = false;
        }
        m_scene->RemoveObject(full.Id());
    }
#endif

    // --- Анимация: клип проигрывается, палитра костей меняется во времени ------
    //
    // НА НАСТОЯЩЕЙ МОДЕЛИ, а не на демо-щупальце. Раньше компонент без модели в
    // Mesh строил встроенное щупальце, и проверка шла по нему — то есть по
    // пути, которым человек не ходит. Щупальца больше нет (оно сбивало с толку:
    // на вопрос «почему не работает моя анимация» показывалась чужая
    // работающая), и проверка пошла тем же путём, что и человек: модель в Mesh,
    // скелет из неё, клип из неё.
#ifdef SAGE_TEST_MODEL
    if (ok) {
        GameObject rig = m_scene->CreateObject("SelftestRig");
        MeshRendererComponent& rigMesh =
            m_scene->Registry().emplace<MeshRendererComponent>(rig.Entity());
        rigMesh.Ref.type = MeshRef::Type::Model;
        rigMesh.Ref.path = SAGE_TEST_MODEL;
        m_scene->Registry().emplace<AnimationComponent>(rig.Entity());

        sage::anim::UpdateAnimators(*m_scene, 0.0f); // инициализация (загрузка + rig)
        AnimationComponent& am = m_scene->Registry().get<AnimationComponent>(rig.Entity());
        if (!am.Model || am.Anim.BoneCount() < 2) {
            LOG_ERROR("Editor") << "SELFTEST: animation failed - rig not built";
            ok = false;
        } else if (am.Anim.ClipCount() < 1) {
            LOG_ERROR("Editor") << "SELFTEST: animation failed - no clips";
            ok = false;
        } else {
            glm::mat4 boneA = am.Anim.BoneMatrices()[1]; // поза в начале
            for (int i = 0; i < 5; ++i) sage::anim::UpdateAnimators(*m_scene, 0.1f); // t=0.5s
            glm::mat4 boneB = am.Anim.BoneMatrices()[1];
            float diff = 0.0f;
            for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
                diff += std::abs(boneA[c][r] - boneB[c][r]);
            if (diff < 0.001f) {
                LOG_ERROR("Editor") << "SELFTEST: animation failed - bone pose did not change over time";
                ok = false;
            }
            // Кросс-фейд между клипами демо (Wave -> Curl) через ШТАТНЫЙ путь:
            // меняем Clip в компоненте, система сама запускает переход. Во время
            // — Fading()==true и вес в диапазоне; после — CurrentClip == Curl.
            if (ok && am.Anim.ClipCount() >= 2) {
                am.Clip = 1;
                am.BlendTime = 0.4f;
                sage::anim::UpdateAnimators(*m_scene, 0.2f); // система стартует фейд + шаг
                bool midOk = am.Anim.Fading() && am.Anim.FadeWeight() > 0.2f &&
                             am.Anim.FadeWeight() < 0.9f;
                sage::anim::UpdateAnimators(*m_scene, 0.4f); // добить переход
                bool doneOk = !am.Anim.Fading() && am.Anim.CurrentClip() == 1;
                if (!midOk || !doneOk) {
                    LOG_ERROR("Editor") << "SELFTEST: animation crossfade failed (mid " << midOk
                                        << ", done " << doneOk << ")";
                    ok = false;
                }
            }
        }
        m_scene->RemoveObject(rig.Id());
    }
#endif

    // --- Конфиг: сохранение и загрузка настроек сохраняют значения (round-trip) ---
    if (ok) {
        sage::EngineConfig a;
        a.Shadows = false;
        a.ShadowResolution = 1024;
        a.Aspect = sage::AspectMode::R21x9;
        a.RenderScale = 0.75f;
        a.VSync = false;
        a.Msaa = 4;
        std::string cfgPath = "selftest_settings.cfg";
        if (!a.SaveFile(cfgPath)) {
            LOG_ERROR("Editor") << "SELFTEST: config save failed";
            ok = false;
        } else {
            sage::EngineConfig b; // из значений по умолчанию
            if (!b.LoadFile(cfgPath)) {
                LOG_ERROR("Editor") << "SELFTEST: config load failed";
                ok = false;
            } else if (b.Shadows != false || b.ShadowResolution != 1024 || b.Aspect != sage::AspectMode::R21x9 ||
                       std::abs(b.RenderScale - 0.75f) > 0.001f || b.VSync != false || b.Msaa != 4) {
                LOG_ERROR("Editor") << "SELFTEST: config round-trip mismatch";
                ok = false;
            }
            std::error_code cfgEc;
            fs::remove(cfgPath, cfgEc);
        }
    }

    // --- Частицы: эмиттер рождает живые частицы, пул растёт со временем ---
    if (ok) {
        ParticleSystem& particles = m_renderer.Particles();
        GameObject fx = m_scene->CreateObject("SelftestEmitter");
        fx.GetTransform().Position = {0.0f, 1.0f, 0.0f};
        m_scene->Registry().emplace<ParticleEmitterComponent>(fx.Entity());

        for (int i = 0; i < 8; ++i) sage::fx::UpdateEmitters(*m_scene, particles, 0.05f);
        if (particles.AliveCount() == 0) {
            LOG_ERROR("Editor") << "SELFTEST: particles failed - no live particles emitted";
            ok = false;
        }
        m_scene->RemoveObject(fx.Id());
    }

    // --- Отсечение по фрустуму + инстансный батчинг ---
    if (ok) {
        // 1. Математика фрустума: точка перед камерой видна, за камерой — нет.
        glm::mat4 v = glm::lookAt(glm::vec3(0, 0, 10), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
        glm::mat4 p = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
        sage::render::Frustum fr = sage::render::Frustum::FromViewProj(p * v);
        bool inFront = fr.IntersectsSphere(glm::vec3(0, 0, 0), 1.0f);
        bool behind  = fr.IntersectsSphere(glm::vec3(0, 0, 60), 1.0f); // за спиной камеры
        if (!inFront || behind) {
            LOG_ERROR("Editor") << "SELFTEST: frustum cull math failed (front=" << inFront
                                << " behind=" << behind << ")";
            ok = false;
        }

        // 2. Батчинг: демо-сцена рендерится, группы мешей < числа сущностей
        //    (несколько кубов -> один инстанс-вызов), учёт видимых сходится.
        if (ok) {
            LightingEnvironment env = sage::ecs::CollectLighting(*m_scene);
            glm::mat4 cv = m_camera.GetViewMatrix();
            glm::mat4 cp = m_camera.GetProjectionMatrix(16.0f / 9.0f);
            sage::ecs::RenderStats st = m_renderer.RenderColorForTest(*m_scene, cv, cp, m_camera.Position, env);
            if (st.Total < 1 || st.Batches < 1 || st.Batches > st.Total ||
                st.Drawn + st.Culled != st.Total) {
                LOG_ERROR("Editor") << "SELFTEST: batch render stats inconsistent (total=" << st.Total
                                    << " drawn=" << st.Drawn << " culled=" << st.Culled
                                    << " batches=" << st.Batches << ")";
                ok = false;
            }
        }
    }

    // --- Дубликат: копия несёт ВСЕ движковые компоненты (свет/тело/коллайдер),
    // рантайм-состояние своё (RuntimeBody сброшен) ---
    if (ok) {
        GameObject src = m_scene->CreateObject("DupSource");
        m_scene->Registry().emplace<LightComponent>(src.Entity());
        RigidBodyComponent rb; rb.RuntimeBody = 12345; // «живое» тело — не должно скопироваться
        m_scene->Registry().emplace<RigidBodyComponent>(src.Entity(), rb);
        m_scene->Registry().emplace<ColliderComponent>(src.Entity());
        m_selection.SetPrimary(src.Id());
        DuplicateSelected();
        GameObject copy = m_scene->Get(m_selection.Primary());
        bool compOk = copy.Valid() && copy.Id() != src.Id() &&
                      m_scene->Registry().all_of<LightComponent>(copy.Entity()) &&
                      m_scene->Registry().all_of<RigidBodyComponent>(copy.Entity()) &&
                      m_scene->Registry().all_of<ColliderComponent>(copy.Entity());
        bool runtimeOk = compOk &&
            m_scene->Registry().get<RigidBodyComponent>(copy.Entity()).RuntimeBody ==
                sage::physics::kInvalidBody;
        if (!compOk || !runtimeOk) {
            LOG_ERROR("Editor") << "SELFTEST: duplicate failed (components " << compOk
                                << ", runtime reset " << runtimeOk << ")";
            ok = false;
        }
        if (copy.Valid()) m_scene->RemoveObject(copy.Id());
        m_scene->RemoveObject(src.Id());
        m_selection.SetPrimary(-1);
    }

    // --- Звук объекта: компонент, файл сцены, запуск вместе с игрой ---------
    //
    // Проверяется не «слышно ли» (на машине сборки звука может не быть вовсе),
    // а то, ради чего компонент заводился: звук ставится в сцену как свойство
    // предмета, переживает сохранение и оживает вместе с Play.
    if (ok) {
        GameObject obj = m_scene->CreateObject("SelftestSound");
        AudioSourceComponent& au =
            m_scene->Registry().emplace<AudioSourceComponent>(obj.Entity());
        au.Clip = "assets/audio/selftest.wav";
        au.Volume = 0.42f;
        au.Loop = true;
        au.Spatial = true;
        au.MaxDistance = 33.0f;
        au.Category = AudioCategory::Ambient;

        const std::string snapshot = SceneSerializer::SaveToString(*m_scene);
        std::unique_ptr<Scene> back = SceneSerializer::LoadFromString(snapshot);
        const AudioSourceComponent* loaded = nullptr;
        if (back) {
            GameObject same = back->FindByName("SelftestSound");
            if (same.Valid()) loaded = back->Registry().try_get<AudioSourceComponent>(same.Entity());
        }
        if (!loaded || loaded->Clip != au.Clip || std::abs(loaded->Volume - 0.42f) > 1e-4f ||
            !loaded->Loop || std::abs(loaded->MaxDistance - 33.0f) > 1e-4f ||
            loaded->Category != AudioCategory::Ambient) {
            LOG_ERROR("Editor") << "SELFTEST: звук объекта не пережил запись и чтение сцены";
            ok = false;
        }

        // Команда, поставленная скриптом или кнопкой «Послушать», обязана
        // сниматься ровно одним кадром системы — иначе звук перезапускался бы
        // каждый кадр, пока команда висит.
        au.Play();
        if (m_play.AudioIfCreated()) {
            sage::audio::Update(*m_scene, *m_play.AudioIfCreated());
            if (au.Request != AudioRequest::None) {
                LOG_ERROR("Editor") << "SELFTEST: команда звуку не снялась за кадр";
                ok = false;
            }
            sage::audio::StopScene(*m_scene, *m_play.AudioIfCreated());
            if (au.Playing || au.Handle != 0) {
                LOG_ERROR("Editor") << "SELFTEST: остановка сцены не заглушила звук";
                ok = false;
            }
        }
        m_scene->RemoveObject(obj.Id());
    }

    return ok;
}
