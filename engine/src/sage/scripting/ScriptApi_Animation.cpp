#include "ScriptEngine.h"

#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/SkinnedModel.h"
#include <algorithm>

// ---------------------------------------------------------------------------
// Скелетная анимация и обратная кинематика: sage.anim.* и sage.ik.*
//
// Часть Lua-API движка. Раньше ВСЕ привязки жили в одном ScriptEngine.cpp на
// 1800 строк: 126 функций, восемнадцать областей, и чтобы дописать одну
// строчку про анимацию, приходилось листать интерфейс, физику и таймеры.
// Определения разъехались по файлам ScriptApi_*.cpp — по файлу на область;
// объявления методов остались в ScriptEngine.h, поэтому порядок регистрации
// по-прежнему записан в одном месте (RegisterEngineApi) и не зависит от того,
// в каком файле лежит тело.
// ---------------------------------------------------------------------------

void ScriptEngine::RegisterAnimationApi() {
    // Анимированная модель на сущности: .glb/.gltf со скелетом. Без этого
    // скрипт не мог поставить в сцену персонажа вовсе — компонент добавлялся
    // только в редакторе, а игра, которая расставляет NPC сама, обычна.
    Bind("anim", "Add", "AddAnimatedModel", [](GameObject& obj, const std::string& path) {
        if (!obj.Valid()) return;
        auto& am = obj.Registry()->get_or_emplace<AnimatedModelComponent>(obj.Entity());
        am.Path = path;
        am.Ready = false; // загрузку и rig сделает UpdateAnimators на первом кадре
    });

    // --- Скелет напрямую: имена костей и поворот кости из скрипта ----------
    //
    // Наборы персонажей сплошь и рядом приходят ОСНАЩЁННЫМИ, но без единого
    // клипа: анимации продаются отдельно. Без этих функций такой пакет —
    // набор неподвижных статуй. Они же дают «голова смотрит на курсор» поверх
    // обычной ходьбы: переопределения накладываются ПОСЛЕ клипа.
    Bind("anim", "JointNames", "JointNames", [this](GameObject obj) -> sol::table {
        sol::table t = m_lua.create_table();
        if (!obj.Valid()) return t;
        auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity());
        if (!am || !am->Model) return t;
        const sage::anim::Skeleton& sk = am->Model->GetSkeleton();
        for (int i = 0; i < sk.Count(); ++i) t[i + 1] = sk.Joints[(size_t)i].Name;
        return t;
    });
    Bind("anim", "JointIndex", "JointIndex", [](GameObject obj, const std::string& name) -> int {
        if (!obj.Valid()) return -1;
        auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity());
        if (!am || !am->Model) return -1;
        const sage::anim::Skeleton& sk = am->Model->GetSkeleton();
        for (int i = 0; i < sk.Count(); ++i)
            if (sk.Joints[(size_t)i].Name == name) return i;
        return -1;
    });
    // Поворот кости в ГРАДУСАХ. Индекс, а не имя: скрипт ищет индекс один раз, а
    // крутит кость каждый кадр, и сравнивать строки шестьдесят раз в секунду на
    // два десятка костей незачем.
    //
    // Поворот ДОБАВЛЯЕТСЯ к позе покоя, а не заменяет её. Заменять нельзя:
    // у скелета кости уже развёрнуты как надо (руки вдоль тела, ступни по
    // земле), и «повернуть плечо на 30°» абсолютным кватернионом стирает эту
    // ориентацию — персонаж встаёт в раскоряку. Игре нужно именно «отклонить
    // от того, как стоит», и это единственная операция, которая читается
    // одинаково и на руке, и на голове, и на хвосте.
    Bind("anim", "SetJointRotation", "SetJointRotation", [](GameObject obj, int joint, float rx, float ry,
                                              float rz) {
        if (!obj.Valid()) return;
        auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity());
        if (!am || !am->Model) return;
        const sage::anim::Skeleton& sk = am->Model->GetSkeleton();
        if (joint < 0 || joint >= sk.Count()) return;
        if ((int)am->PoseOverrides.size() != sk.Count())
            am->PoseOverrides.assign((size_t)sk.Count(), {});
        sage::anim::JointPose& p = am->PoseOverrides[(size_t)joint];
        p.HasRotation = true;
        p.Rotation = sk.Joints[(size_t)joint].Rotation *
                     glm::quat(glm::radians(glm::vec3(rx, ry, rz)));
    });
    Bind("anim", "ClearJointPoses", "ClearJointPoses", [](GameObject obj) {
        if (!obj.Valid()) return;
        if (auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity()))
            am->PoseOverrides.clear();
    });
    // Сколько клипов в модели — по нему игра решает, играть готовую анимацию
    // или двигать кости самой.
    Bind("anim", "Count", "AnimationCount", [](GameObject obj) -> int {
        if (!obj.Valid()) return 0;
        auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity());
        return (am && am->Model) ? (int)am->Model->Clips().size() : 0;
    });

    // Проиграть клип анимации на сущности с AnimatedModelComponent, плавно
    // перейдя за blend секунд (кросс-фейд; система анимации подхватит смену Clip).
    // false, если у сущности нет анимированной модели.
    Bind("anim", "Play", "PlayAnimation", [](GameObject obj, int clip, sol::optional<float> blend) -> bool {
        if (!obj.Valid()) return false;
        auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity());
        if (!am) return false;
        am->Clip = clip;
        if (blend) am->BlendTime = *blend;
        am->Playing = true;
        return true;
    });

    // --- Обратная кинематика -----------------------------------------------
    //
    // Цель задаётся ИМЕНЕМ кости и МИРОВОЙ точкой: игра знает, где у неё пол и
    // предметы, а в какой кости это отзовётся — дело движка. Возвращает индекс
    // цели, по нему её потом двигают.
    Bind("ik", "AddGoal", "AddIKGoal", [](GameObject obj, const std::string& bone,
                                       sol::optional<int> chainLength) -> int {
        if (!obj.Valid()) return -1;
        auto& ik = obj.Registry()->get_or_emplace<IKComponent>(obj.Entity());
        IKGoal g;
        g.Bone = bone;
        g.ChainLength = chainLength.value_or(2);
        ik.Goals.push_back(std::move(g));
        return (int)ik.Goals.size() - 1;
    });
    // Куда тянуть цель (мировые координаты).
    Bind("ik", "SetTarget", "SetIKTarget", [](GameObject obj, int goal, const glm::vec3& target) {
        if (!obj.Valid()) return;
        auto* ik = obj.Registry()->try_get<IKComponent>(obj.Entity());
        if (!ik || goal < 0 || goal >= (int)ik->Goals.size()) return;
        ik->Goals[(size_t)goal].Target = target;
    });
    // Вес: 0 — цель не влияет, 1 — держим точно. Промежуточные значения нужны,
    // чтобы IK ВКЛЮЧАЛСЯ плавно: резко поставленная нога дёргается.
    Bind("ik", "SetWeight", "SetIKWeight", [](GameObject obj, int goal, float weight) {
        if (!obj.Valid()) return;
        auto* ik = obj.Registry()->try_get<IKComponent>(obj.Entity());
        if (!ik || goal < 0 || goal >= (int)ik->Goals.size()) return;
        ik->Goals[(size_t)goal].Weight = glm::clamp(weight, 0.0f, 1.0f);
    });
    // Куда смотрит колено/локоть. Без подсказки задача неоднозначна: цепочка
    // может вращаться вокруг оси «корень-цель», и колено выгибается куда попало.
    Bind("ik", "SetPole", "SetIKPole", [](GameObject obj, int goal, const glm::vec3& pole) {
        if (!obj.Valid()) return;
        auto* ik = obj.Registry()->try_get<IKComponent>(obj.Entity());
        if (!ik || goal < 0 || goal >= (int)ik->Goals.size()) return;
        ik->Goals[(size_t)goal].UsePole = true;
        ik->Goals[(size_t)goal].Pole = pole;
    });
    // Нормаль поверхности под ступнёй: она ляжет на склон, а не воткнётся носком.
    Bind("ik", "SetAlign", "SetIKAlign", [](GameObject obj, int goal, const glm::vec3& normal) {
        if (!obj.Valid()) return;
        auto* ik = obj.Registry()->try_get<IKComponent>(obj.Entity());
        if (!ik || goal < 0 || goal >= (int)ik->Goals.size()) return;
        ik->Goals[(size_t)goal].AlignNormal = normal;
    });
    // Прилипание к земле: пока стопа опущена, её мировая точка держится. Это
    // лекарство от скольжения ног — при переносе анимации с чужого скелета
    // длина шага не совпадает с корневым движением, и нога едет по полу.
    Bind("ik", "SetFootLock", "SetIKFootLock", [](GameObject obj, int goal, bool on,
                                           sol::optional<float> plantHeight,
                                           sol::optional<float> releaseTime) {
        if (!obj.Valid()) return;
        auto* ik = obj.Registry()->try_get<IKComponent>(obj.Entity());
        if (!ik || goal < 0 || goal >= (int)ik->Goals.size()) return;
        ik->Goals[(size_t)goal].Lock = on;
        if (plantHeight) ik->Goals[(size_t)goal].PlantHeight = *plantHeight;
        if (releaseTime) ik->Goals[(size_t)goal].ReleaseTime = *releaseTime;
    });
    // Держится ли сейчас нога за землю. Игре это нужно не для IK: под «нога
    // встала» вешают шаги, пыль и тряску камеры — а знает об этом только замок.
    Bind("ik", "Locked", "IKLocked", [](GameObject obj, int goal) -> bool {
        if (!obj.Valid()) return false;
        auto* ik = obj.Registry()->try_get<IKComponent>(obj.Entity());
        if (!ik || goal < 0 || goal >= (int)ik->Goals.size()) return false;
        return ik->Goals[(size_t)goal].Locked;
    });
    // Взгляд: кость доворачивается так, чтобы её ось axis смотрела на цель.
    Bind("ik", "SetAim", "SetIKAim", [](GameObject obj, int goal, const glm::vec3& axis,
                                      sol::optional<float> maxAngle) {
        if (!obj.Valid()) return;
        auto* ik = obj.Registry()->try_get<IKComponent>(obj.Entity());
        if (!ik || goal < 0 || goal >= (int)ik->Goals.size()) return;
        IKGoal& g = ik->Goals[(size_t)goal];
        g.Aim = true;
        g.AimAxis = axis;
        g.AimMaxAngle = maxAngle.value_or(80.0f);
    });
    Bind("ik", "SetEnabled", "SetIKEnabled", [](GameObject obj, bool on) {
        if (!obj.Valid()) return;
        if (auto* ik = obj.Registry()->try_get<IKComponent>(obj.Entity())) ik->Enabled = on;
    });
    // Мировая позиция кости — по ней игра берёт, где сейчас ступня или кисть
    // (например, чтобы поставить эффект или проверить опору).
    Bind("anim", "BonePosition", "BonePosition", [this](GameObject obj, const std::string& bone) -> glm::vec3 {
        if (!obj.Valid()) return glm::vec3(0.0f);
        auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity());
        if (!am || !am->Model) return glm::vec3(0.0f);
        const sage::anim::Skeleton& sk = am->Model->GetSkeleton();
        const std::vector<glm::mat4>& g = am->Anim.GlobalMatrices();
        for (int i = 0; i < sk.Count() && i < (int)g.size(); ++i) {
            if (sk.Joints[(size_t)i].Name != bone) continue;
            // Матрицы аниматора — в пространстве МОДЕЛИ. Игра рассуждает в
            // мировых координатах (пол, предметы), поэтому переводим через
            // трансформ сущности. Без сцены отдаём как есть — лучше локальная
            // позиция, чем ноль.
            const glm::vec3 local = glm::vec3(g[(size_t)i][3]);
            if (!m_scene) return local;
            return glm::vec3(m_scene->WorldMatrix(obj.Entity()) * glm::vec4(local, 1.0f));
        }
        return glm::vec3(0.0f);
    });

    // Куда смотрит ОСЬ кости в мире. Позиции мало: чтобы выпустить пулю из
    // ствола, поставить взгляд или проверить, куда развернулась голова, нужно
    // направление, а не точка.
    Bind("anim", "BoneAxis", "BoneAxis", [this](GameObject obj, const std::string& bone,
                                          const glm::vec3& axis) -> glm::vec3 {
        if (!obj.Valid()) return glm::vec3(0.0f);
        auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity());
        if (!am || !am->Model) return glm::vec3(0.0f);
        const sage::anim::Skeleton& sk = am->Model->GetSkeleton();
        const std::vector<glm::mat4>& g = am->Anim.GlobalMatrices();
        for (int i = 0; i < sk.Count() && i < (int)g.size(); ++i) {
            if (sk.Joints[(size_t)i].Name != bone) continue;
            glm::vec3 dir(g[(size_t)i] * glm::vec4(axis, 0.0f));
            if (m_scene) dir = glm::vec3(m_scene->WorldMatrix(obj.Entity()) * glm::vec4(dir, 0.0f));
            const float len = glm::length(dir);
            return len > 1e-6f ? dir / len : glm::vec3(0.0f);
        }
        return glm::vec3(0.0f);
    });

    // Забрать анимации из ЧУЖОГО файла на скелет своей модели. Ровно то, ради
    // чего существуют библиотеки анимаций: один набор движений на любого героя.
    // Возвращает, сколько клипов перенеслось (0 — скелеты не сошлись именами).
    Bind("anim", "Borrow", "BorrowAnimations", [](GameObject obj, const std::string& path) -> int {
        if (!obj.Valid()) return 0;
        auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity());
        if (!am || !am->Model) return 0; // модель ещё не загрузилась
        std::shared_ptr<sage::render::SkinnedModel> src =
            ResourceManager::Instance().GetSkinnedModel(path);
        if (!src) return 0;
        const int added = am->Model->BorrowClipsFrom(*src);
        // Клипы уехали в модель — переустанавливаем rig, чтобы аниматор увидел
        // новый список (указатель на вектор тот же, но состояние проигрывания
        // сбрасывать надо: индексы за пределами старого списка теперь валидны).
        if (added > 0) am->Anim.SetRig(&am->Model->GetSkeleton(), &am->Model->Clips());
        return added;
    });

    // --- Клипы по ИМЕНАМ и состояние проигрывания --------------------------
    //
    // Библиотека анимаций — это сорок с лишним клипов в одном файле, и
    // обращаться к ним номерами нельзя: номер зависит от порядка в файле,
    // молча меняется при переэкспорте, и «клип 17» не читается ни в коде, ни в
    // логе. Игра знает свои анимации по именам — «Walk_Loop», «Jump_Start».
    Bind("anim", "Names", "AnimationNames", [this](GameObject obj) -> sol::table {
        sol::table t = m_lua.create_table();
        if (!obj.Valid()) return t;
        auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity());
        if (!am || !am->Model) return t;
        const auto& clips = am->Model->Clips();
        for (size_t i = 0; i < clips.size(); ++i) t[i + 1] = clips[i].Name;
        return t;
    });
    Bind("anim", "Index", "AnimationIndex", [](GameObject obj, const std::string& name) -> int {
        if (!obj.Valid()) return -1;
        auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity());
        if (!am || !am->Model) return -1;
        const auto& clips = am->Model->Clips();
        for (size_t i = 0; i < clips.size(); ++i)
            if (clips[i].Name == name) return (int)i;
        return -1;
    });
    Bind("anim", "PlayNamed", "PlayAnimationNamed", [](GameObject obj, const std::string& name,
                                                sol::optional<float> blend) -> bool {
        if (!obj.Valid()) return false;
        auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity());
        if (!am || !am->Model) return false;
        const auto& clips = am->Model->Clips();
        for (size_t i = 0; i < clips.size(); ++i) {
            if (clips[i].Name != name) continue;
            am->Clip = (int)i;
            if (blend) am->BlendTime = *blend;
            am->Playing = true;
            return true;
        }
        return false; // клипа нет — молчать нельзя, вызывающий обязан узнать
    });
    Bind("anim", "Duration", "AnimationDuration", [](GameObject obj, int clip) -> float {
        if (!obj.Valid()) return 0.0f;
        auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity());
        if (!am || !am->Model) return 0.0f;
        const auto& clips = am->Model->Clips();
        if (clip < 0 || clip >= (int)clips.size()) return 0.0f;
        return clips[(size_t)clip].Duration;
    });
    // Время внутри клипа: чтение и перемотка. Перемотка нужна всему, что
    // показывает позу в ПРОИЗВОЛЬНЫЙ момент, а не «через dt от прошлого кадра»:
    // покадровый рендер, превью, синхронизация двух персонажей.
    Bind("anim", "Time", "AnimationTime", [](GameObject obj) -> float {
        if (!obj.Valid()) return 0.0f;
        auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity());
        return am ? am->Anim.Time() : 0.0f;
    });
    Bind("anim", "Seek", "SeekAnimation", [](GameObject obj, float time) {
        if (!obj.Valid()) return;
        if (auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity()))
            am->Anim.Seek(time);
    });
    Bind("anim", "SetSpeed", "SetAnimationSpeed", [](GameObject obj, float speed) {
        if (!obj.Valid()) return;
        if (auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity()))
            am->Speed = speed;
    });
    Bind("anim", "SetPlaying", "SetAnimationPlaying", [](GameObject obj, bool playing) {
        if (!obj.Valid()) return;
        if (auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity()))
            am->Playing = playing;
    });
    // Корневое движение: перемещение из клипа применяется к сущности, а не к
    // мешу. Отдельным переключателем, потому что это ВЫБОР игры: платформеру
    // нужна своя скорость передвижения, а катсцене — ровно та, что в клипе.
    Bind("anim", "SetRootMotion", "SetRootMotion", [](GameObject obj, bool on) {
        if (!obj.Valid()) return;
        if (auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity()))
            am->RootMotion = on;
    });
    Bind("anim", "SetLoop", "SetAnimationLoop", [](GameObject obj, bool loop) {
        if (!obj.Valid()) return;
        if (auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity()))
            am->Loop = loop;
    });
    // Идёт ли сейчас кросс-фейд и с каким весом — по нему видно, что переход
    // действительно СМЕШИВАЕТ позы, а не переключает клип рывком.
    Bind("anim", "Fade", "AnimationFade", [](GameObject obj) -> float {
        if (!obj.Valid()) return 0.0f;
        auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity());
        if (!am || !am->Anim.Fading()) return 0.0f;
        return am->Anim.FadeWeight();
    });
}

