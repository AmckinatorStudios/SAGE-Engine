// Граф кадра: порядок по зависимостям, отбрасывание лишнего, обнаружение
// циклов. Всё без GL — граф ничего не рисует сам, он решает, что и когда
// вызывать.
#include "TestFramework.h"

#include "sage/render/FrameGraph.h"

#include <string>
#include <vector>

using sage::render::FrameGraph;
using sage::render::LoadOp;
using sage::render::RenderPassDesc;
using sage::render::ResourceId;

namespace {

// Проход, который при выполнении дописывает своё имя в общий журнал. По
// журналу и проверяется порядок: спрашивать граф, что он собирается сделать,
// и верить ему на слово — значит проверять намерение, а не результат.
RenderPassDesc MakePass(const std::string& name, std::vector<std::string>& log,
                        std::vector<ResourceId> reads, std::vector<ResourceId> writes) {
    RenderPassDesc p;
    p.Name = name;
    p.Reads = std::move(reads);
    p.Writes = std::move(writes);
    p.Execute = [&log, name] { log.push_back(name); };
    return p;
}

} // namespace

TEST(framegraph_executes_declaration_order_of_needed_passes) {
    FrameGraph fg;
    const ResourceId hdr = fg.DeclareResource("HDR");
    const ResourceId velocity = fg.DeclareResource("Скорости");
    const ResourceId output = fg.DeclareResource("Выход");

    std::vector<std::string> log;
    fg.AddPass(MakePass("Скорости", log, {}, {velocity}));
    fg.AddPass(MakePass("Сцена", log, {}, {hdr}));
    fg.AddPass(MakePass("Пост", log, {hdr, velocity}, {output}));

    std::string err;
    CHECK_TRUE(fg.Compile(output, err));
    CHECK_TRUE(err.empty());
    fg.Execute();

    CHECK_TRUE(log.size() == 3);
    CHECK_TRUE(log[0] == "Скорости" && log[1] == "Сцена" && log[2] == "Пост");
}

TEST(framegraph_versions_resources_so_read_modify_write_works) {
    // Четыре прохода подряд дорисовывают в ОДИН буфер: читают его и пишут в
    // него же. Без версий ресурсов такая цепочка выглядит как взаимная
    // зависимость — каждый писатель ждёт остальных, — и кадр не собирается.
    // Это не выдуманный случай: небо, геометрия, сетка и служебная графика в
    // Director 3D устроены ровно так, и первая версия графа на них и упала.
    FrameGraph fg;
    const ResourceId hdr = fg.DeclareResource("HDR");
    const ResourceId output = fg.DeclareResource("Выход");

    std::vector<std::string> log;
    fg.AddPass(MakePass("Небо", log, {}, {hdr}));
    fg.AddPass(MakePass("Геометрия", log, {hdr}, {hdr}));
    fg.AddPass(MakePass("Сетка", log, {hdr}, {hdr}));
    fg.AddPass(MakePass("Служебная графика", log, {hdr}, {hdr}));
    fg.AddPass(MakePass("Пост", log, {hdr}, {output}));

    std::string err;
    CHECK_TRUE(fg.Compile(output, err));
    CHECK_TRUE(err.empty());
    fg.Execute();

    CHECK_TRUE(log.size() == 5);
    CHECK_TRUE(log[0] == "Небо");
    CHECK_TRUE(log[4] == "Пост");
}

TEST(framegraph_culls_passes_nobody_reads) {
    FrameGraph fg;
    const ResourceId hdr = fg.DeclareResource("HDR");
    const ResourceId unusedRes = fg.DeclareResource("Ненужный буфер");
    const ResourceId output = fg.DeclareResource("Выход");

    std::vector<std::string> log;
    fg.AddPass(MakePass("Сцена", log, {}, {hdr}));
    // Проход, готовящий данные для выключенного эффекта. Возникает не от
    // глупости: эффект отключили, а проход остался.
    fg.AddPass(MakePass("Подготовка для эффекта", log, {}, {unusedRes}));
    fg.AddPass(MakePass("Пост", log, {hdr}, {output}));

    std::string err;
    CHECK_TRUE(fg.Compile(output, err));
    fg.Execute();

    CHECK_TRUE(log.size() == 2);
    for (const std::string& name : log) CHECK_TRUE(name != "Подготовка для эффекта");
    CHECK_TRUE(fg.Stats().PassesCulled == 1);
    CHECK_TRUE(fg.Stats().PassesExecuted == 2);
}

TEST(framegraph_skips_disabled_passes) {
    FrameGraph fg;
    const ResourceId hdr = fg.DeclareResource("HDR");
    const ResourceId output = fg.DeclareResource("Выход");

    std::vector<std::string> log;
    fg.AddPass(MakePass("Сцена", log, {}, {hdr}));
    RenderPassDesc off = MakePass("Сетка", log, {hdr}, {hdr});
    off.Enabled = false;
    fg.AddPass(std::move(off));
    fg.AddPass(MakePass("Пост", log, {hdr}, {output}));

    std::string err;
    CHECK_TRUE(fg.Compile(output, err));
    fg.Execute();

    CHECK_TRUE(log.size() == 2);
    CHECK_TRUE(fg.Stats().PassesDisabled == 1);
    // Выключенный проход не считается отброшенным: это разные причины, и
    // смешав их, нельзя понять, почему прохода нет в кадре.
    CHECK_TRUE(fg.Stats().PassesCulled == 0);
}

TEST(framegraph_rejects_reading_before_writing) {
    // Потребитель объявлен раньше того, кто готовит ему данные. Это ошибка
    // описания кадра, и она обязана обнаружиться при сборке, а не проявиться
    // чёрным экраном.
    FrameGraph fg;
    const ResourceId hdr = fg.DeclareResource("HDR");
    const ResourceId output = fg.DeclareResource("Выход");

    std::vector<std::string> log;
    fg.AddPass(MakePass("Пост", log, {hdr}, {output}));
    fg.AddPass(MakePass("Сцена", log, {}, {hdr}));

    std::string err;
    CHECK_TRUE(!fg.Compile(output, err));
    // Сообщение должно называть и проход, и ресурс: «ошибка в графе кадра» без
    // имён — это приглашение искать вручную.
    CHECK_TRUE(err.find("Пост") != std::string::npos);
    CHECK_TRUE(err.find("HDR") != std::string::npos);

    // Несобранный граф не выполняется: выполнить его значило бы выполнить в
    // заведомо неверном порядке.
    fg.Execute();
    CHECK_TRUE(log.empty());
}

TEST(framegraph_rejects_output_nobody_writes) {
    FrameGraph fg;
    const ResourceId hdr = fg.DeclareResource("HDR");
    const ResourceId output = fg.DeclareResource("Выход");

    std::vector<std::string> log;
    fg.AddPass(MakePass("Сцена", log, {}, {hdr}));

    std::string err;
    CHECK_TRUE(!fg.Compile(output, err));
    CHECK_TRUE(err.find("Выход") != std::string::npos);

    // И обратный случай: выход, которого вообще нет среди ресурсов.
    std::string err2;
    CHECK_TRUE(!fg.Compile(999, err2));
    CHECK_TRUE(!err2.empty());
}

TEST(framegraph_long_chain_survives) {
    // Длинная цепочка: каждый проход читает результат предыдущего. Проверяет,
    // что обход не рекурсивный по стеку (сотня проходов на рекурсии — это
    // переполнение стека в релизе и загадочное падение) и что отбрасывание
    // ничего не теряет.
    FrameGraph fg;
    const int kCount = 200;
    std::vector<ResourceId> res;
    res.reserve(kCount + 1);
    for (int i = 0; i <= kCount; ++i) res.push_back(fg.DeclareResource("R" + std::to_string(i)));

    std::vector<std::string> log;
    fg.AddPass(MakePass("Начало", log, {}, {res[0]}));
    for (int i = 0; i < kCount; ++i) {
        fg.AddPass(MakePass("P" + std::to_string(i), log, {res[(size_t)i]}, {res[(size_t)i + 1]}));
    }

    std::string err;
    CHECK_TRUE(fg.Compile(res[(size_t)kCount], err));
    fg.Execute();

    CHECK_TRUE(log.size() == (size_t)kCount + 1);
    CHECK_TRUE(log[0] == "Начало");
    for (int i = 0; i < kCount; ++i) CHECK_TRUE(log[(size_t)i + 1] == "P" + std::to_string(i));
}

TEST(framegraph_culls_whole_branch_not_just_leaf) {
    // Отбрасываться должна ВСЯ ветка, а не только её последний проход: если
    // выброшен потребитель, готовивший его данные проход тоже никому не нужен.
    FrameGraph fg;
    const ResourceId hdr = fg.DeclareResource("HDR");
    const ResourceId aux = fg.DeclareResource("Промежуточный");
    const ResourceId aux2 = fg.DeclareResource("Промежуточный 2");
    const ResourceId output = fg.DeclareResource("Выход");

    std::vector<std::string> log;
    fg.AddPass(MakePass("Сцена", log, {}, {hdr}));
    fg.AddPass(MakePass("Ветка 1", log, {}, {aux}));
    fg.AddPass(MakePass("Ветка 2", log, {aux}, {aux2}));
    fg.AddPass(MakePass("Пост", log, {hdr}, {output}));

    std::string err;
    CHECK_TRUE(fg.Compile(output, err));
    fg.Execute();

    CHECK_TRUE(log.size() == 2);
    CHECK_TRUE(fg.Stats().PassesCulled == 2);
}

TEST(framegraph_describe_explains_the_frame) {
    FrameGraph fg;
    const ResourceId hdr = fg.DeclareResource("HDR");
    const ResourceId output = fg.DeclareResource("Выход");
    std::vector<std::string> log;

    RenderPassDesc sky = MakePass("Небо", log, {}, {hdr});
    sky.ColorLoad = LoadOp::Clear;
    fg.AddPass(std::move(sky));
    fg.AddPass(MakePass("Пост", log, {hdr}, {output}));

    std::string err;
    CHECK_TRUE(fg.Compile(output, err));
    const std::string text = fg.Describe();
    // Описание — это ответ на вопрос «почему прохода нет в кадре», ради
    // которого граф во многом и делается.
    CHECK_TRUE(text.find("Небо") != std::string::npos);
    CHECK_TRUE(text.find("очистить") != std::string::npos);
    CHECK_TRUE(text.find("HDR") != std::string::npos);
}
