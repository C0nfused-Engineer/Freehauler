// process.cpp - launching the server process (CreateProcess on Windows, fork/exec elsewhere)
#include "process.h"

#include <chrono>
#include <cstdio>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <climits>
    #include <fcntl.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

#if defined(_WIN32)
static std::wstring Widen(const std::string &s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string Narrow(const std::wstring &w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// One argument quoted the way the C runtime splits a command line back into argv
static std::wstring Quote(const std::wstring &a)
{
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    for (size_t i = 0;; i++) {
        size_t slashes = 0;
        while (i < a.size() && a[i] == L'\\') { i++; slashes++; }
        if (i == a.size()) { q.append(slashes * 2, L'\\'); break; }
        if (a[i] == L'"') { q.append(slashes * 2 + 1, L'\\'); q.push_back(L'"'); }
        else              { q.append(slashes, L'\\'); q.push_back(a[i]); }
    }
    q.push_back(L'"');
    return q;
}
#endif

bool LaunchProcess(const std::string &exe, const std::vector<std::string> &args, ProcessHandle &out,
                   std::string &error, const std::string &logFile)
{
#if defined(_WIN32)
    (void)logFile;                                   // it gets its own console window instead
    std::wstring cmd = Quote(Widen(exe));
    for (const std::string &a : args) cmd += L" " + Quote(Widen(a));
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
        error = "Couldn't start the server (Windows error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    out.handle = (intptr_t)pi.hProcess;
    out.pid = (int)pi.dwProcessId;
    return true;
#else
    pid_t pid = fork();
    if (pid < 0) { error = "Couldn't start the server (fork failed)"; return false; }
    if (pid == 0) {
        setsid();                                    // its own session: it outlives the game
        int in = open("/dev/null", O_RDONLY);
        if (in >= 0) { dup2(in, 0); close(in); }
        if (!logFile.empty()) {
            int log = open(logFile.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (log >= 0) { dup2(log, 1); dup2(log, 2); close(log); }
        }
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(exe.c_str()));
        for (const std::string &a : args) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        execv(exe.c_str(), argv.data());
        _exit(127);
    }
    out.pid = (int)pid;
    return true;
#endif
}

bool ProcessHandle::Running() const
{
#if defined(_WIN32)
    return handle && WaitForSingleObject((HANDLE)handle, 0) == WAIT_TIMEOUT;
#else
    if (pid <= 0) return false;
    int status = 0;
    return waitpid((pid_t)pid, &status, WNOHANG) == 0;
#endif
}

std::string SelfPath()
{
#if defined(_WIN32)
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    return Narrow(std::wstring(buf, n));
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, (size_t)n) : std::string();
#endif
}

void SetConsoleTitleText(const std::string &title)
{
#if defined(_WIN32)
    SetConsoleTitleW(Widen(title).c_str());
#else
    (void)title;
#endif
}

std::string RandomKey()
{
    // splitmix64 over a few things that differ every run (std::random_device isn't
    // random on some older MinGW versions)
    uint64_t x = (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    x ^= (uint64_t)std::chrono::system_clock::now().time_since_epoch().count() << 17;
    x ^= (uint64_t)(uintptr_t)&x;
#if defined(_WIN32)
    x ^= (uint64_t)GetCurrentProcessId() << 32;
#else
    x ^= (uint64_t)getpid() << 32;
#endif
    auto mix = [](uint64_t v) {
        v += 0x9E3779B97F4A7C15ull;
        v = (v ^ (v >> 30)) * 0xBF58476D1CE4E5B9ull;
        v = (v ^ (v >> 27)) * 0x94D049BB133111EBull;
        return v ^ (v >> 31);
    };
    char buf[17];
    snprintf(buf, sizeof buf, "%016llx", (unsigned long long)mix(mix(x)));
    return buf;
}
