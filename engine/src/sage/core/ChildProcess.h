#pragma once
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// ДОЧЕРНИЙ ПРОЦЕСС С КАНАЛАМИ — запустить программу и разговаривать с ней
// строками через stdin/stdout.
//
// Зачем движку. Языки скриптов, которых нет внутри движка (Python, Node, свой
// язык игры, внешний компилятор), работают отдельным процессом: движок пишет
// запрос строкой, процесс отвечает строкой (sage/scripting/ProcessLanguage.h).
// popen так не умеет — у него один канал и нет срока ожидания, а зависший
// интерпретатор не должен вешать кадр игры.
//
// Ошибки процесса (stderr) идут в журнал родителя как есть — их пишет сам
// интерпретатор, и прятать их значит прятать причину.
// ---------------------------------------------------------------------------
namespace sage {

class ChildProcess {
public:
    ChildProcess();
    ~ChildProcess();   // завершает процесс, если он ещё жив
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // argv[0] — программа (ищется в PATH), остальное — аргументы как есть
    // (без разбора оболочкой). workDir пустой — текущий каталог.
    bool Start(const std::vector<std::string>& argv, const std::string& workDir, std::string& err);

    bool Running();
    // Записать строку (перевод строки добавляется). false — процесс закрыл вход.
    bool WriteLine(const std::string& line);
    // Прочитать строку, ожидая не дольше timeoutSeconds. false — срок вышел
    // или процесс закрыл выход (timedOut говорит, что именно).
    bool ReadLine(std::string& line, double timeoutSeconds, bool* timedOut = nullptr);
    // Завершить немедленно.
    void Kill();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace sage
