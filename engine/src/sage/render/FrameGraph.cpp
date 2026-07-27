#include "sage/render/FrameGraph.h"

#include <algorithm>
#include <cstdio>
#include <unordered_set>

namespace sage::render {
namespace {

const std::string kUnknownResource = "<нет ресурса>";

const char* LoadOpName(LoadOp op) {
    switch (op) {
        case LoadOp::Load: return "загрузить";
        case LoadOp::Clear: return "очистить";
        case LoadOp::DontCare: return "неважно";
    }
    return "?";
}

} // namespace

void FrameGraph::Reset() {
    m_passes.clear();
    m_resources.clear();
    m_order.clear();
    m_stats = FrameGraphStats{};
    m_compiled = false;
}

ResourceId FrameGraph::DeclareResource(std::string name) {
    m_resources.push_back(std::move(name));
    return (ResourceId)m_resources.size() - 1;
}

void FrameGraph::AddPass(RenderPassDesc pass) {
    m_passes.push_back(std::move(pass));
    m_compiled = false;
}

const std::string& FrameGraph::ResourceName(ResourceId id) const {
    if (id < 0 || id >= (ResourceId)m_resources.size()) return kUnknownResource;
    return m_resources[(size_t)id];
}

bool FrameGraph::Compile(ResourceId output, std::string& error) {
    m_order.clear();
    m_compiled = false;
    m_stats = FrameGraphStats{};
    m_stats.PassesDeclared = (int)m_passes.size();

    for (const RenderPassDesc& p : m_passes) {
        if (!p.Enabled) ++m_stats.PassesDisabled;
    }

    if (output < 0 || output >= (ResourceId)m_resources.size()) {
        error = "выход кадра не объявлен как ресурс";
        return false;
    }

    // 1) ВЕРСИИ РЕСУРСОВ.
    //
    // Наивная запись «кто пишет в ресурс R» ломается на первом же настоящем
    // кадре, и ломается неочевидно. Небо, геометрия, сетка и служебная графика
    // все читают HDR-буфер и все в него пишут. Если считать, что читатель
    // зависит от ВСЕХ писателей, получается, что каждый из них ждёт остальных,
    // — граф объявляет цикл и кадр не собирается. Ровно это и произошло при
    // первом запуске.
    //
    // Правильная модель: каждая запись создаёт НОВУЮ ВЕРСИЮ ресурса, а чтение
    // относится к той версии, которая существовала на момент объявления
    // прохода. «Геометрия» читает HDR после неба, «Сетка» — после геометрии.
    // Зависимости при этом всегда указывают назад по порядку объявления, и
    // цикл становится невозможен по построению.
    std::vector<int> lastWriter(m_resources.size(), -1);
    std::vector<std::vector<int>> deps(m_passes.size()); // проход -> от кого зависит

    for (int i = 0; i < (int)m_passes.size(); ++i) {
        const RenderPassDesc& p = m_passes[(size_t)i];
        if (!p.Enabled) continue; // выключенный не создаёт версий и зависимостей

        for (ResourceId r : p.Reads) {
            if (r < 0 || r >= (ResourceId)m_resources.size()) {
                error = "проход «" + p.Name + "» читает несуществующий ресурс";
                return false;
            }
            const int w = lastWriter[(size_t)r];
            if (w < 0) {
                // Чтение до записи — почти всегда ошибка описания кадра:
                // проход объявлен раньше того, кто готовит ему данные.
                // Сообщить об этом здесь дешевле, чем искать причину по
                // чёрному экрану.
                error = "проход «" + p.Name + "» читает «" + ResourceName(r) +
                        "», в который до него никто не записал";
                return false;
            }
            if (w != i) deps[(size_t)i].push_back(w);
        }
        // Запись — ПОСЛЕ чтений: проход, читающий и пишущий один ресурс
        // (дорисовка поверх), должен прочитать предыдущую версию, а не свою.
        for (ResourceId r : p.Writes) {
            if (r < 0 || r >= (ResourceId)m_resources.size()) {
                error = "проход «" + p.Name + "» пишет в несуществующий ресурс";
                return false;
            }
            lastWriter[(size_t)r] = i;
        }
    }

    if (lastWriter[(size_t)output] < 0) {
        error = "в выход кадра «" + ResourceName(output) + "» никто не пишет";
        return false;
    }

    // 2) Обход от выхода назад: нужны только те проходы, от которых он зависит.
    //
    // Именно от ВЫХОДА, а не «все подряд»: проход, чей результат никто не
    // читает, — это чистая трата кадра. Такое возникает не от глупости, а
    // естественно: выключили эффект, и проход, готовивший для него данные,
    // остался. Раньше его пришлось бы вычёркивать руками, помня, что он
    // существует.
    std::vector<bool> needed(m_passes.size(), false);
    std::vector<int> stack{lastWriter[(size_t)output]};
    needed[(size_t)lastWriter[(size_t)output]] = true;
    while (!stack.empty()) {
        const int pass = stack.back();
        stack.pop_back();
        for (int d : deps[(size_t)pass]) {
            if (!needed[(size_t)d]) { needed[(size_t)d] = true; stack.push_back(d); }
        }
    }

    // 3) Порядок — ПОРЯДОК ОБЪЯВЛЕНИЯ нужных проходов.
    //
    // И это не упрощение из лени, а следствие модели версий: зависимость
    // всегда указывает на проход, объявленный раньше, поэтому порядок
    // объявления УЖЕ является топологическим. Сортировать нечего.
    //
    // Отсюда же и честная граница возможностей: этот граф НЕ переставляет
    // проходы. Он выбрасывает лишние, проверяет описание и хранит информацию
    // о проходах как данные — но порядок кадра задаёт человек. Обещать
    // «объявляйте как угодно, граф разберётся» было бы неправдой.
    for (int i = 0; i < (int)m_passes.size(); ++i) {
        if (needed[(size_t)i]) m_order.push_back(i);
    }

    m_stats.PassesExecuted = (int)m_order.size();
    m_stats.PassesCulled = m_stats.PassesDeclared - m_stats.PassesExecuted - m_stats.PassesDisabled;
    if (m_stats.PassesCulled < 0) m_stats.PassesCulled = 0;
    m_compiled = true;
    return true;
}

void FrameGraph::Execute() {
    if (!m_compiled) return;
    for (int index : m_order) {
        const RenderPassDesc& p = m_passes[(size_t)index];
        if (p.Execute) p.Execute();
    }
}

std::string FrameGraph::Describe() const {
    std::string out;
    char line[256];
    std::snprintf(line, sizeof(line), "Кадр: объявлено %d, выполняется %d, отброшено %d, выключено %d\n",
                  m_stats.PassesDeclared, m_stats.PassesExecuted, m_stats.PassesCulled,
                  m_stats.PassesDisabled);
    out += line;
    for (size_t i = 0; i < m_order.size(); ++i) {
        const RenderPassDesc& p = m_passes[(size_t)m_order[i]];
        std::snprintf(line, sizeof(line), "  %zu. %s (цвет: %s, глубина: %s)\n", i + 1,
                      p.Name.c_str(), LoadOpName(p.ColorLoad), LoadOpName(p.DepthLoad));
        out += line;
        for (ResourceId r : p.Reads) {
            std::snprintf(line, sizeof(line), "       читает %s\n", ResourceName(r).c_str());
            out += line;
        }
        for (ResourceId r : p.Writes) {
            std::snprintf(line, sizeof(line), "       пишет  %s\n", ResourceName(r).c_str());
            out += line;
        }
    }
    return out;
}

} // namespace sage::render
