#include "sage/core/ChildProcess.h"

#include <chrono>

#include "sage/core/Paths.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace sage {

#if defined(_WIN32)

struct ChildProcess::Impl {
    HANDLE Process = nullptr;
    HANDLE In = nullptr;    // пишем в stdin ребёнка
    HANDLE Out = nullptr;   // читаем его stdout
    std::string Buffer;
};

namespace {
// Аргумент командной строки Windows по правилам CommandLineToArgvW.
std::wstring Quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}
} // namespace

ChildProcess::ChildProcess() : m(std::make_unique<Impl>()) {}
ChildProcess::~ChildProcess() { Kill(); }

bool ChildProcess::Start(const std::vector<std::string>& argv, const std::string& workDir, std::string& err) {
    Kill();
    if (argv.empty()) {
        err = "пустая команда";
        return false;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE childIn = nullptr, childOut = nullptr;
    if (!CreatePipe(&childIn, &m->In, &sa, 0) || !CreatePipe(&m->Out, &childOut, &sa, 0)) {
        err = "не удалось создать каналы";
        return false;
    }
    SetHandleInformation(m->In, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(m->Out, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd;
    for (const std::string& a : argv) cmd += (cmd.empty() ? L"" : L" ") + Quote(sage::PathFromUtf8(a).wstring());
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = childIn;
    si.hStdOutput = childOut;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    const std::wstring dir = workDir.empty() ? std::wstring() : sage::PathFromUtf8(workDir).wstring();
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    CloseHandle(childIn);
    CloseHandle(childOut);
    if (!ok) {
        err = "не удалось запустить «" + argv[0] + "» (код " + std::to_string(GetLastError()) + ")";
        Kill();
        return false;
    }
    CloseHandle(pi.hThread);
    m->Process = pi.hProcess;
    return true;
}

bool ChildProcess::Running() {
    if (!m->Process) return false;
    DWORD code = 0;
    return GetExitCodeProcess(m->Process, &code) && code == STILL_ACTIVE;
}

bool ChildProcess::WriteLine(const std::string& line) {
    if (!m->In) return false;
    const std::string data = line + "\n";
    DWORD written = 0;
    return WriteFile(m->In, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size();
}

bool ChildProcess::ReadLine(std::string& line, double timeoutSeconds, bool* timedOut) {
    if (timedOut) *timedOut = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    for (;;) {
        const size_t nl = m->Buffer.find('\n');
        if (nl != std::string::npos) {
            line = m->Buffer.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            m->Buffer.erase(0, nl + 1);
            return true;
        }
        if (!m->Out) return false;
        DWORD avail = 0;
        if (!PeekNamedPipe(m->Out, nullptr, 0, nullptr, &avail, nullptr)) return false;   // канал закрыт
        if (avail > 0) {
            char buf[4096];
            DWORD got = 0;
            if (!ReadFile(m->Out, buf, avail < sizeof(buf) ? avail : (DWORD)sizeof(buf), &got, nullptr) || got == 0)
                return false;
            m->Buffer.append(buf, got);
            continue;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            if (timedOut) *timedOut = true;
            return false;
        }
        Sleep(1);
    }
}

void ChildProcess::Kill() {
    if (m->Process) {
        TerminateProcess(m->Process, 1);
        WaitForSingleObject(m->Process, 1000);
        CloseHandle(m->Process);
        m->Process = nullptr;
    }
    if (m->In) CloseHandle(m->In);
    if (m->Out) CloseHandle(m->Out);
    m->In = m->Out = nullptr;
    m->Buffer.clear();
}

#else

struct ChildProcess::Impl {
    pid_t Pid = -1;
    int In = -1;    // пишем в stdin ребёнка
    int Out = -1;   // читаем его stdout
    std::string Buffer;
};

ChildProcess::ChildProcess() : m(std::make_unique<Impl>()) {}
ChildProcess::~ChildProcess() { Kill(); }

bool ChildProcess::Start(const std::vector<std::string>& argv, const std::string& workDir, std::string& err) {
    Kill();
    if (argv.empty()) {
        err = "пустая команда";
        return false;
    }
    int toChild[2], fromChild[2], execFail[2];
    if (pipe(toChild) != 0 || pipe(fromChild) != 0 || pipe(execFail) != 0) {
        err = "не удалось создать каналы";
        return false;
    }
    // Канал сбоя exec закрывается сам при успешном exec: так родитель узнаёт
    // «нет такой программы», а не получает молчащий процесс.
    fcntl(execFail[1], F_SETFD, FD_CLOEXEC);
    const pid_t pid = fork();
    if (pid < 0) {
        err = "fork не удался";
        return false;
    }
    if (pid == 0) {
        dup2(toChild[0], STDIN_FILENO);
        dup2(fromChild[1], STDOUT_FILENO);
        close(toChild[0]); close(toChild[1]);
        close(fromChild[0]); close(fromChild[1]);
        close(execFail[0]);
        if (!workDir.empty() && chdir(workDir.c_str()) != 0) {
            const int e = errno;
            (void)!write(execFail[1], &e, sizeof(e));
            _exit(127);
        }
        std::vector<char*> args;
        for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        const int e = errno;
        (void)!write(execFail[1], &e, sizeof(e));
        _exit(127);
    }
    close(toChild[0]);
    close(fromChild[1]);
    close(execFail[1]);
    int childErr = 0;
    const ssize_t n = read(execFail[0], &childErr, sizeof(childErr));
    close(execFail[0]);
    m->Pid = pid;
    m->In = toChild[1];
    m->Out = fromChild[0];
    if (n > 0) {
        err = "не удалось запустить «" + argv[0] + "»: " + std::strerror(childErr);
        Kill();
        return false;
    }
    // Закрытый процессом вход не должен убивать движок сигналом.
    std::signal(SIGPIPE, SIG_IGN);
    return true;
}

bool ChildProcess::Running() {
    if (m->Pid <= 0) return false;
    int status = 0;
    const pid_t r = waitpid(m->Pid, &status, WNOHANG);
    if (r == 0) return true;
    m->Pid = -1;   // уже завершился (и собран)
    return false;
}

bool ChildProcess::WriteLine(const std::string& line) {
    if (m->In < 0) return false;
    const std::string data = line + "\n";
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t w = write(m->In, data.data() + off, data.size() - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += (size_t)w;
    }
    return true;
}

bool ChildProcess::ReadLine(std::string& line, double timeoutSeconds, bool* timedOut) {
    if (timedOut) *timedOut = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    for (;;) {
        const size_t nl = m->Buffer.find('\n');
        if (nl != std::string::npos) {
            line = m->Buffer.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            m->Buffer.erase(0, nl + 1);
            return true;
        }
        if (m->Out < 0) return false;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            if (timedOut) *timedOut = true;
            return false;
        }
        pollfd p{m->Out, POLLIN, 0};
        const int r = poll(&p, 1, (int)left.count());
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) continue;   // срок проверит следующий круг
        char buf[4096];
        const ssize_t got = read(m->Out, buf, sizeof(buf));
        if (got <= 0) return false;   // процесс закрыл выход
        m->Buffer.append(buf, (size_t)got);
    }
}

void ChildProcess::Kill() {
    if (m->In >= 0) close(m->In);
    if (m->Out >= 0) close(m->Out);
    m->In = m->Out = -1;
    if (m->Pid > 0) {
        kill(m->Pid, SIGKILL);
        int status = 0;
        waitpid(m->Pid, &status, 0);
        m->Pid = -1;
    }
    m->Buffer.clear();
}

#endif

} // namespace sage
