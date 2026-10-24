#include "sage/core/Problems.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <mutex>
#include <regex>
#include <unordered_map>

#include "sage/core/CrashHandler.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"

namespace sage {

namespace {

struct State {
    std::mutex Mutex;
    std::vector<Problem> Items;                     // по порядку прихода
    std::unordered_map<std::string, size_t> ByKey;  // ключ повтора -> индекс
    uint64_t NextId = 1;
    std::atomic<uint64_t> Revision{1};
    size_t Dropped = 0;                             // вытеснено пределом
};

State& S() {
    static State s;
    return s;
}

// Своя запись в лог — не повод заводить вторую проблему о том же (FromLog).
thread_local bool t_reporting = false;

double Now() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

int Rank(Severity s) { return (int)s; }

std::string Key(const Problem& p) {
    return p.Code + '\x1f' + p.Resource + '\x1f' + std::to_string(p.Line) + '\x1f' + p.Title + '\x1f' +
           p.Subject;
}

// Один и тот же файл пишут по-разному: «assets/x.png», «C:\\proj\\assets\\x.png»,
// «/home/…/proj/assets/x.png». Ресурс — от «assets/», с прямыми слешами:
// иначе первопричина и её последствия не узнали бы друг друга.
std::string NormalizeResource(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    const size_t at = path.rfind("assets/");
    if (at != std::string::npos) path = path.substr(at);
    while (path.rfind("./", 0) == 0) path = path.substr(2);
    return path;
}

// Путь ассета внутри текста ошибки лога — чтобы и «чужие» ошибки, о которых
// движок ничего не знает, сгруппировались вокруг своего файла.
std::string ExtractPath(const std::string& message) {
    static const std::regex kPath(
        R"(([^\s'"«»()<>:,]+[/\\])?[^\s'"«»()<>/\\:,]+\.(sagemat|sagemesh|sageprefab|sagetex|sageanim|sage|gltf|glb|obj|fbx|png|jpe?g|tga|bmp|hdr|ktx2?|wav|ogg|mp3|flac|lua|slua|ttf|otf|json|glsl|vert|frag))",
        std::regex::icase);
    std::smatch m;
    if (!std::regex_search(message, m, kPath)) return {};
    return NormalizeResource(m.str(0));
}

// Подсказки к ошибкам лога, которые пришли без своей (FromLog). Немного и по
// делу: подсказка, которая не про эту ошибку, хуже отсутствующей.
void AdviseLog(Problem& p) {
    const std::string& m = p.Title;
    auto has = [&](const char* s) { return m.find(s) != std::string::npos; };
    if (has("не найден") || has("нет по этому пути") || has("not found") || has("No such file")) {
        p.Detail = "Файла нет там, где его ждут.";
        p.Hints = {"Файл перенесли, переименовали или удалили мимо редактора — верните его на место.",
                   "Или выберите другой файл в слоте инспектора (перетащите его из панели Assets)."};
    } else if (has("Ссылка ведёт в никуда")) {
        p.Detail = "Объект ссылается на ассет, которого больше нет в проекте.";
        p.Hints = {"Выберите ассет заново в слоте инспектора.",
                   "Если файл просто перенесли — переносите его в панели Assets: ссылки едут вместе с ним."};
    } else if (has("Материал не загрузился")) {
        p.Detail = "Файл материала не открылся — объект рисуется материалом по умолчанию.";
        p.Hints = {"Проверьте, что файл материала на месте.",
                   "Битый материал можно пересоздать: Assets → Создать → Материал."};
    } else if (has("Модель не загрузилась")) {
        p.Detail = "Модель не прочиталась — объект остался без меша.";
        p.Hints = {"Проверьте, что файл модели на месте и открывается в программе, где его сделали.",
                   "Поддерживаются .gltf, .glb, .obj, .fbx."};
    } else if ((has("шейдер") || has("Shader") || has("shader")) &&
               (has("не собрал") || has("compile") || has("ошибк"))) {
        p.Detail = "Код шейдера не собрался — до исправления объект рисуется запасным шейдером.";
        p.Hints = {"Строка ошибки — в сообщении компилятора выше; исправьте её и сохраните файл."};
    }
}

// Найти первопричину для новой записи (см. заголовок).
uint64_t FindRoot(const State& s, const Problem& p) {
    if (!p.Resource.empty())
        for (const Problem& q : s.Items)
            if (q.Parent == 0 && q.Resource == p.Resource && Rank(q.Level) >= Rank(Severity::Warning))
                return q.Id;
    if (!p.CausedBy.empty())
        for (const Problem& q : s.Items)
            if (q.Resource == p.CausedBy) return q.Parent ? q.Parent : q.Id;
    return 0;
}

void Reindex(State& s) {
    s.ByKey.clear();
    for (size_t i = 0; i < s.Items.size(); ++i) s.ByKey[Key(s.Items[i])] = i;
    // Причина ушла — последствия становятся самостоятельными, а не висят на
    // несуществующем номере.
    for (Problem& q : s.Items) {
        if (!q.Parent) continue;
        const bool alive = std::any_of(s.Items.begin(), s.Items.end(),
                                       [&](const Problem& r) { return r.Id == q.Parent; });
        if (!alive) q.Parent = 0;
    }
}

uint64_t Insert(Problem p, bool& isNew) {
    State& s = S();
    if (p.Resource.empty() && !p.File.empty()) p.Resource = NormalizeResource(p.File);
    else p.Resource = NormalizeResource(p.Resource);
    if (!p.CausedBy.empty()) p.CausedBy = NormalizeResource(p.CausedBy);
    const double now = Now();
    std::lock_guard<std::mutex> lock(s.Mutex);
    const std::string key = Key(p);
    auto it = s.ByKey.find(key);
    if (it != s.ByKey.end()) {
        Problem& q = s.Items[it->second];
        ++q.Count;
        q.LastSeen = now;
        if (Rank(p.Level) > Rank(q.Level)) q.Level = p.Level;
        s.Revision.fetch_add(1);
        isNew = false;
        return q.Id;
    }
    isNew = true;
    p.Id = s.NextId++;
    p.Count = 1;
    p.FirstSeen = p.LastSeen = now;
    p.Parent = FindRoot(s, p);
    // Пришла причина позже своих последствий — забирает их себе.
    if (p.Parent == 0 && !p.Resource.empty()) {
        for (Problem& q : s.Items) {
            if (q.Parent == 0 && q.CausedBy == p.Resource) {
                const uint64_t old = q.Id;
                q.Parent = p.Id;
                for (Problem& r : s.Items)
                    if (r.Parent == old) r.Parent = p.Id;
            }
        }
    }
    s.Items.push_back(std::move(p));
    s.ByKey[key] = s.Items.size() - 1;
    if (s.Items.size() > Problems::kLimit) {
        // Вытесняем самое старое НЕкритичное: аварию и ошибку держим дольше
        // предупреждений — ради них журнал и открывают.
        auto victim = std::find_if(s.Items.begin(), s.Items.end(),
                                   [](const Problem& q) { return Rank(q.Level) < Rank(Severity::Error); });
        if (victim == s.Items.end()) victim = s.Items.begin();
        s.Items.erase(victim);
        ++s.Dropped;
        Reindex(s);
    }
    s.Revision.fetch_add(1);
    return s.Items.back().Id;
}

LogLevel ToLog(Severity s) {
    switch (s) {
        case Severity::Info: return LogLevel::Info;
        case Severity::Warning: return LogLevel::Warn;
        default: return LogLevel::Error;
    }
}

const char* OsName() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "?";
#endif
}

void ReplaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.size() < 2) return;   // «a» заменять нельзя — испортим весь текст
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
}

} // namespace

const char* SeverityName(Severity s) {
    switch (s) {
        case Severity::Info: return "Сведения";
        case Severity::Warning: return "Предупреждение";
        case Severity::Error: return "Ошибка";
        case Severity::Fatal: return "Авария";
    }
    return "?";
}

std::string Problem::Format() const {
    std::string out = std::string("[") + SeverityName(Level) + "] " + Title + "\n";
    if (!File.empty()) {
        out += "Где: " + File;
        if (Line > 0) out += ":" + std::to_string(Line);
        if (Col > 0) out += ":" + std::to_string(Col);
        if (!Subject.empty()) out += "  (" + Subject + ")";
        out += "\n";
    } else if (!Subject.empty()) {
        out += "Где: " + Subject + "\n";
    }
    if (!Excerpt.empty()) out += Excerpt + (Excerpt.back() == '\n' ? "" : "\n");
    if (!Detail.empty()) out += "Почему: " + Detail + "\n";
    if (!Hints.empty()) {
        out += "Как исправить:\n";
        for (const std::string& h : Hints) out += "  • " + h + "\n";
    }
    if (!Example.empty()) out += "Пример:\n" + Example + (Example.back() == '\n' ? "" : "\n");
    if (!Technical.empty()) out += "Подробности:\n" + Technical + (Technical.back() == '\n' ? "" : "\n");
    if (Count > 1) out += "Повторилась " + std::to_string(Count) + " раз\n";
    return out;
}

uint64_t Problems::Report(Problem p) {
    // Копии для лога — до перемещения в реестр.
    const Severity level = p.Level;
    const std::string category = p.Category.empty() ? "Engine" : p.Category;
    std::string line;
    if (!p.File.empty()) {
        line = p.File;
        if (p.Line > 0) line += ":" + std::to_string(p.Line);
        line += ": ";
    }
    line += p.Title;
    const std::string full = p.Format();
    bool isNew = false;
    const uint64_t id = Insert(std::move(p), isNew);

    // В лог — без блокировки реестра (лог сам зовёт FromLog, см. t_reporting).
    t_reporting = true;
    Log::WriteLine(ToLog(level), category, line);
    // Подробный разбор — один раз на проблему и только в файл: в консоли
    // хватает строки, а журнал, который пришлют, должен объяснять сам.
    if (isNew) Log::WriteFileOnly(ToLog(level), category, full);
    t_reporting = false;
    return id;
}

void Problems::FromLog(int level, const std::string& category, const std::string& message) {
    if (t_reporting || level < (int)LogLevel::Warn || message.empty()) return;
    Problem p;
    p.Level = level >= (int)LogLevel::Error ? Severity::Error : Severity::Warning;
    p.Category = category;
    p.Code = p.Level == Severity::Error ? "log.error" : "log.warning";
    p.Title = message;
    p.Resource = ExtractPath(message);
    AdviseLog(p);
    bool isNew = false;
    Insert(std::move(p), isNew);
}

int Problems::Resolve(const std::string& resource, const std::string& codePrefix) {
    State& s = S();
    const std::string res = NormalizeResource(resource);
    std::lock_guard<std::mutex> lock(s.Mutex);
    const size_t before = s.Items.size();
    s.Items.erase(std::remove_if(s.Items.begin(), s.Items.end(),
                                 [&](const Problem& q) {
                                     return q.Resource == res &&
                                            (codePrefix.empty() || q.Code.rfind(codePrefix, 0) == 0);
                                 }),
                  s.Items.end());
    const int removed = (int)(before - s.Items.size());
    if (removed) {
        Reindex(s);
        s.Revision.fetch_add(1);
    }
    return removed;
}

void Problems::Clear() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.Mutex);
    s.Items.clear();
    s.ByKey.clear();
    s.Dropped = 0;
    s.Revision.fetch_add(1);
}

uint64_t Problems::Revision() { return S().Revision.load(); }

std::vector<Problem> Problems::Snapshot() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.Mutex);
    return s.Items;
}

size_t Problems::Count() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.Mutex);
    return s.Items.size();
}

int Problems::CountAtLeast(Severity level) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.Mutex);
    int n = 0;
    for (const Problem& q : s.Items)
        if (Rank(q.Level) >= Rank(level)) ++n;
    return n;
}

std::string Problems::Redact(const std::string& text) {
    std::string out = text;
    // Домашняя папка — во всех написаниях: с обратными и прямыми слешами.
    for (const char* var : {"HOME", "USERPROFILE"}) {
        std::string home = EnvString(var);
        while (!home.empty() && (home.back() == '/' || home.back() == '\\')) home.pop_back();
        if (home.size() < 3) continue;
        ReplaceAll(out, home, "~");
        std::string forward = home;
        std::replace(forward.begin(), forward.end(), '\\', '/');
        ReplaceAll(out, forward, "~");
    }
    for (const char* var : {"USER", "USERNAME", "LOGNAME"}) {
        const std::string user = EnvString(var);
        // Короткое имя («a», «ai») встречается в обычных словах — менять его
        // значило бы испортить отчёт. Только в путях: /user/ и \user\.
        if (user.size() < 2) continue;
        ReplaceAll(out, "/" + user + "/", "/<user>/");
        ReplaceAll(out, "\\" + user + "\\", "\\<user>\\");
    }
    return out;
}

std::string Problems::CrashSummary(int maxItems) {
    State& s = S();
    std::unique_lock<std::mutex> lock(s.Mutex, std::try_to_lock);
    if (!lock.owns_lock()) return "(реестр проблем занят в момент падения)\n";
    std::string out;
    int shown = 0;
    for (auto it = s.Items.rbegin(); it != s.Items.rend() && shown < maxItems; ++it) {
        if (it->Parent || Rank(it->Level) < Rank(Severity::Warning)) continue;
        out += std::string("  ") + SeverityName(it->Level) + ": ";
        if (!it->File.empty()) out += it->File + (it->Line ? ":" + std::to_string(it->Line) : "") + ": ";
        out += it->Title + (it->Count > 1 ? " ×" + std::to_string(it->Count) : "") + "\n";
        ++shown;
    }
    return out;
}

std::string Problems::BuildReport(const std::string& appName, const std::string& context) {
    const std::vector<Problem> items = Snapshot();
    std::string out;
    out.reserve(4096 + items.size() * 256);
    char when[32] = "?";
    {
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    }
    out += "# Диагностический отчёт " + appName + "\n\n";
    out += std::string("Время: ") + when + "\nСистема: " + OsName() + "\n";
    if (!context.empty()) out += "\n## Контекст\n\n" + context + (context.back() == '\n' ? "" : "\n");

    int counts[4] = {0, 0, 0, 0};
    for (const Problem& q : items) ++counts[(int)q.Level];
    out += "\n## Проблемы: аварий " + std::to_string(counts[3]) + ", ошибок " + std::to_string(counts[2]) +
           ", предупреждений " + std::to_string(counts[1]) + "\n";
    {
        State& s = S();
        std::lock_guard<std::mutex> lock(s.Mutex);
        if (s.Dropped) out += "(ещё " + std::to_string(s.Dropped) + " старых записей вытеснено пределом)\n";
    }
    // Группами: первопричина, под ней последствия — так же, как в редакторе.
    for (const Problem& root : items) {
        if (root.Parent) continue;
        out += "\n### " + std::string(SeverityName(root.Level)) + ": " + root.Title + "\n\n";
        out += "```\n" + root.Format() + "```\n";
        int consequences = 0;
        for (const Problem& c : items)
            if (c.Parent == root.Id) ++consequences;
        if (consequences) {
            out += "\nПоследствия (" + std::to_string(consequences) + "):\n";
            for (const Problem& c : items) {
                if (c.Parent != root.Id) continue;
                out += "- " + std::string(SeverityName(c.Level)) + ": ";
                if (!c.File.empty()) out += c.File + (c.Line ? ":" + std::to_string(c.Line) : "") + ": ";
                out += c.Title;
                if (!c.Subject.empty()) out += " (" + c.Subject + ")";
                if (c.Count > 1) out += " ×" + std::to_string(c.Count);
                out += "\n";
            }
        }
    }
    const std::string notes = CrashHandler::RecentNotes();
    if (!notes.empty()) out += "\n## Последние строки журнала\n\n```\n" + notes + "```\n";
    return Redact(out);
}

std::string Problems::Excerpt(const std::string& source, int line, int col, int contextLines) {
    if (line <= 0 || source.empty()) return {};
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= source.size()) {
        size_t end = source.find('\n', start);
        if (end == std::string::npos) end = source.size();
        std::string l = source.substr(start, end - start);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(std::move(l));
        if (end == source.size()) break;
        start = end + 1;
    }
    if (line > (int)lines.size()) return {};
    const int from = std::max(1, line - contextLines);
    const int to = std::min((int)lines.size(), line + contextLines);
    const int width = (int)std::to_string(to).size();
    std::string out;
    for (int n = from; n <= to; ++n) {
        std::string num = std::to_string(n);
        num.insert(0, (size_t)(width - (int)num.size()), ' ');
        out += (n == line ? "> " : "  ") + num + " | " + lines[(size_t)n - 1] + "\n";
        if (n == line && col > 0) {
            // Отступ под столбец — теми же символами табуляции, что в строке,
            // иначе «^» уехала бы мимо нужного места.
            std::string pad;
            const std::string& src = lines[(size_t)n - 1];
            for (int i = 0; i < col - 1 && i < (int)src.size(); ++i) pad += src[(size_t)i] == '\t' ? '\t' : ' ';
            out += "  " + std::string((size_t)width, ' ') + " | " + pad + "^\n";
        }
    }
    return out;
}

} // namespace sage
