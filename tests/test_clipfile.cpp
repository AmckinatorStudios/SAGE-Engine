// ---------------------------------------------------------------------------
// Анимационный клип как отдельный файл (.sageanim).
//
// ЧТО ИМЕННО ПРОВЕРЯЕТСЯ. Не «файл записался и прочитался» — это следствие.
// Проверяется то, ради чего формат и заводился: клип перестал зависеть от
// ПОРЯДКА КОСТЕЙ в модели. Раньше компонент хранил индекс клипа в файле модели,
// а каналы адресовали кости номерами; художник переэкспортировал персонажа,
// порядок сменился — и движение уезжало на чужие кости молча, без единой
// ошибки. Найти такое по симптому почти невозможно: скелет дёргается, а
// виноватым выглядит экспортёр.
// ---------------------------------------------------------------------------
#include "TestFramework.h"

#include "sage/anim/ClipFile.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace sage::anim;

namespace {

fs::path TempDir(const char* name) {
    fs::path dir = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

Skeleton MakeSkeleton(const std::vector<std::string>& names) {
    Skeleton s;
    for (const std::string& n : names) {
        Joint j;
        j.Name = n;
        j.Parent = s.Joints.empty() ? -1 : 0;
        s.Joints.push_back(j);
    }
    return s;
}

// Клип, двигающий ровно одну кость — ту, что стоит вторым номером.
AnimationClip MakeClip(int joint) {
    AnimationClip c;
    c.Name = "Walk";
    c.Duration = 1.0f;
    AnimChannel ch;
    ch.Joint = joint;
    ch.Target = AnimPath::Translation;
    ch.Times = {0.0f, 1.0f};
    ch.Values = {glm::vec4(0, 0, 0, 0), glm::vec4(5, 0, 0, 0)};
    c.Channels.push_back(ch);

    AnimChannel rot;
    rot.Joint = joint;
    rot.Target = AnimPath::Rotation;
    rot.Interp = AnimInterp::Step;
    rot.Times = {0.0f, 0.5f};
    rot.Values = {glm::vec4(0, 0, 0, 1), glm::vec4(0, 0.7071f, 0, 0.7071f)};
    c.Channels.push_back(rot);
    return c;
}

} // namespace

// --- 1. Клип переживает запись и чтение целиком -----------------------------
TEST(Clip_survives_save_and_load) {
    const fs::path dir = TempDir("sage_clip_roundtrip");
    const Skeleton skel = MakeSkeleton({"Hips", "Spine", "Head"});
    const ClipAsset asset = ToAsset(MakeClip(1), skel);
    CHECK_EQ(asset.Name, std::string("Walk"));
    CHECK_EQ(asset.Channels.size(), (size_t)2);
    // Кость записана ИМЕНЕМ — в этом весь смысл формата.
    CHECK_EQ(asset.Channels[0].Joint, std::string("Spine"));

    const std::string file = (dir / "hero.Walk.sageanim").string();
    SaveClip(asset, file);
    const ClipAsset back = LoadClip(file);

    CHECK_EQ(back.Name, std::string("Walk"));
    CHECK_NEAR(back.Duration, 1.0f, 1e-5f);
    CHECK_EQ(back.Channels.size(), (size_t)2);
    CHECK_EQ(back.Channels[0].Joint, std::string("Spine"));
    CHECK_NEAR(back.Channels[0].Values[1].x, 5.0f, 1e-4f);
    // Кватернион — четыре числа: потерянное w развернуло бы кость.
    CHECK_NEAR(back.Channels[1].Values[1].y, 0.7071f, 1e-4f);
    CHECK_NEAR(back.Channels[1].Values[1].w, 0.7071f, 1e-4f);
    // Ступенчатая интерполяция — не украшение: линейная вместо неё «размазала»
    // бы щелчок по времени.
    CHECK_TRUE(back.Channels[1].Interp == AnimInterp::Step);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// --- 2. ГЛАВНОЕ: переэкспорт модели не ломает клип --------------------------
//
// Та же модель, пересобранная с другим порядком костей. Клип, адресующий кости
// номерами, после такого двигал бы ЧУЖУЮ кость; клип по именам находит свою.
TEST(Clip_survives_reexport_with_reordered_joints) {
    const Skeleton before = MakeSkeleton({"Hips", "Spine", "Head"});
    const ClipAsset asset = ToAsset(MakeClip(1), before);   // двигали Spine

    const Skeleton after = MakeSkeleton({"Head", "Hips", "Spine"});  // переэкспорт
    int missing = -1;
    const AnimationClip bound = Bind(asset, after, &missing);

    CHECK_EQ(missing, 0);
    CHECK_EQ(bound.Channels.size(), (size_t)2);
    // Spine в новом скелете стоит третьим — канал обязан указывать на него.
    CHECK_EQ(bound.Channels[0].Joint, 2);
    CHECK_NEAR(bound.Channels[0].Values[1].x, 5.0f, 1e-4f);
}

// --- 3. Кость, которой в скелете нет, не теряется молча ----------------------
//
// Клип от чужого персонажа — обычное дело: анимации покупают отдельно. Часть
// каналов не найдёт своих костей, и об этом надо СКАЗАТЬ, а не отбросить их
// втихую: «половина движения пропала» человек объяснить не сможет.
TEST(Clip_reports_bones_it_could_not_find) {
    const Skeleton skel = MakeSkeleton({"Hips", "Spine", "Head"});
    ClipAsset asset = ToAsset(MakeClip(1), skel);
    asset.Channels[0].Joint = "mixamorig:LeftFoot";   // такой кости здесь нет

    int missing = -1;
    const AnimationClip bound = Bind(asset, skel, &missing);
    CHECK_EQ(missing, 1);
    CHECK_EQ(bound.Channels.size(), (size_t)1);   // второй канал нашёлся
}

// --- 4. Имя файла годится в имя файла ---------------------------------------
//
// Клипы из чужих пакетов зовутся «Armature|Walk» (Blender) и «mixamo.com»
// (Mixamo). Такое имя нельзя положить на диск как есть.
TEST(Clip_file_name_is_usable_on_disk) {
    CHECK_EQ(ClipFileName("hero", "Walk"), std::string("hero.Walk.sageanim"));
    CHECK_EQ(ClipFileName("hero", "Armature|Walk"), std::string("hero.Armature_Walk.sageanim"));
    CHECK_EQ(ClipFileName("hero", "walk 01"), std::string("hero.walk_01.sageanim"));
    // Имя из одних запрещённых символов именем не является.
    CHECK_EQ(ClipFileName("hero", "///"), std::string("hero.clip.sageanim"));
    CHECK_EQ(ClipFileName("hero", ""), std::string("hero.clip.sageanim"));
}

// --- 5. Битый файл — исключение с причиной, а не тишина ---------------------
TEST(Clip_broken_file_says_why) {
    const fs::path dir = TempDir("sage_clip_broken");
    const std::string file = (dir / "broken.sageanim").string();
    { std::ofstream f(file); f << "{ это не JSON"; }

    bool threw = false;
    try {
        LoadClip(file);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK_TRUE(threw);

    // Несуществующий файл — тоже исключение, а не пустой клип: «клип без
    // каналов» и «клипа нет» это разные вещи, и путать их нельзя.
    threw = false;
    try {
        LoadClip((dir / "нет-такого.sageanim").string());
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK_TRUE(threw);

    std::error_code ec;
    fs::remove_all(dir, ec);
}
