#include "ExternalGame.h"

#include "sage/core/Paths.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <cstring>
#  include <csignal>
#  include <cstdlib>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <chrono>
#  include <thread>
#endif

#ifdef _WIN32

namespace {
std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), n);
    return w;
}
} // namespace

bool ExternalGame::Start(const std::filesystem::path& player, const std::filesystem::path& projectDir,
                         const std::vector<std::pair<std::string, std::string>>& env, std::string& err) {
    Stop();
    // Окружение дочернего процесса = окружение редактора + наши переменные.
    // Блок строим сами, а не зовём SetEnvironmentVariable: тот правил бы
    // окружение самого редактора.
    std::wstring block;
    if (LPWCH cur = GetEnvironmentStringsW()) {
        for (const wchar_t* p = cur; *p;) {
            const size_t len = wcslen(p);
            block.append(p, len + 1);
            p += len + 1;
        }
        FreeEnvironmentStringsW(cur);
    }
    for (const auto& kv : env) block += Widen(kv.first) + L"=" + Widen(kv.second) + L'\0';
    block += L'\0';

    std::wstring cmd = L"\"" + player.wstring() + L"\" \"" + projectDir.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // Рабочая папка — рядом с плеером: оттуда он берёт assets/ рантайма.
    const std::wstring cwd = player.parent_path().wstring();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                        block.data(), cwd.c_str(), &si, &pi)) {
        err = "CreateProcess failed: " + std::to_string((unsigned long)GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    m_process = pi.hProcess;
    return true;
}

bool ExternalGame::Running() {
    if (!m_process) return false;
    if (WaitForSingleObject((HANDLE)m_process, 0) == WAIT_TIMEOUT) return true;
    CloseHandle((HANDLE)m_process);
    m_process = nullptr;
    return false;
}

void ExternalGame::Stop() {
    if (!m_process) return;
    if (WaitForSingleObject((HANDLE)m_process, 0) == WAIT_TIMEOUT) {
        TerminateProcess((HANDLE)m_process, 0);
        WaitForSingleObject((HANDLE)m_process, 3000);
    }
    CloseHandle((HANDLE)m_process);
    m_process = nullptr;
}

#else

bool ExternalGame::Start(const std::filesystem::path& player, const std::filesystem::path& projectDir,
                         const std::vector<std::pair<std::string, std::string>>& env, std::string& err) {
    Stop();
    const std::string exe = sage::PathToUtf8(player);
    const std::string proj = sage::PathToUtf8(projectDir);
    const std::string cwd = sage::PathToUtf8(player.parent_path());
    const pid_t pid = fork();
    if (pid < 0) {
        err = std::string("fork failed: ") + std::strerror(errno);
        return false;
    }
    if (pid == 0) {
        // Дочерний процесс: окружение правим ЗДЕСЬ, у редактора оно не меняется.
        for (const auto& kv : env) setenv(kv.first.c_str(), kv.second.c_str(), 1);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
        execl(exe.c_str(), exe.c_str(), proj.c_str(), (char*)nullptr);
        _exit(127);
    }
    m_pid = (int)pid;
    return true;
}

bool ExternalGame::Running() {
    if (m_pid <= 0) return false;
    int status = 0;
    const pid_t r = waitpid(m_pid, &status, WNOHANG);
    if (r == 0) return true;
    m_pid = 0; // завершился (или уже собран)
    return false;
}

void ExternalGame::Stop() {
    if (m_pid <= 0) return;
    // Сначала вежливо (плеер закрывает окно и пишет лог), потом наверняка.
    kill(m_pid, SIGTERM);
    for (int i = 0; i < 40; ++i) {
        int status = 0;
        if (waitpid(m_pid, &status, WNOHANG) != 0) { m_pid = 0; return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    kill(m_pid, SIGKILL);
    int status = 0;
    waitpid(m_pid, &status, 0);
    m_pid = 0;
}

#endif
