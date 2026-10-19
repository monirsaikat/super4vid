// idle_probe: per-thread wake-ups (context switches) and CPU time of a process over a sampling window (P11 11.1).
// Works without admin rights (WPR's kernel providers need them): NtQuerySystemInformation(SystemProcessInformation)
// gives every thread's context-switch count and kernel/user time; GetThreadDescription names our ct-* threads and
// the Win32 start address maps the others to a module (ntdll = thread pool worker, AudioSes, MFPlat, the driver).
//
//   idle_probe --pid N [--seconds 60] [--warmup 0]
//   idle_probe [--seconds 60] [--warmup 10] [--keep] -- super4vid.exe --tray ...
//       launches the command (stdout/stderr inherited), waits the warm-up, samples, then ends it with WM_QUIT posted
//       to the threads owning its windows (terminated after 10 s); --keep leaves it running. Exit code 0, or 2 when
//       the process exited early.
// Output: one line per thread with wakes/s and CPU ms, then the process totals (wakes/s, CPU %, working set).

#include <ct/platform/win.h>

#include <psapi.h>
#include <winternl.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// Full x64 layouts (winternl.h declares them only partially).
struct ThreadInfo {
    LARGE_INTEGER kernel_time, user_time, create_time;
    ULONG wait_time;
    PVOID start_address;
    HANDLE pid, tid;
    LONG priority, base_priority;
    ULONG context_switches, state, wait_reason;
};
struct ProcessInfo {
    ULONG next_offset, thread_count;
    LARGE_INTEGER ws_private;
    ULONG hard_faults, threads_high;
    ULONGLONG cycle_time;
    LARGE_INTEGER create_time, user_time, kernel_time;
    UNICODE_STRING image_name;
    LONG base_priority;
    HANDLE pid, parent_pid;
    ULONG handle_count, session_id;
    ULONG_PTR key;
    SIZE_T peak_vs, vs;
    ULONG page_faults;
    SIZE_T peak_ws, ws, quota[4], pagefile, peak_pagefile, private_pages;
    LARGE_INTEGER io[6];
    ThreadInfo threads[1];
};
static_assert(sizeof(ThreadInfo) == 80, "SYSTEM_THREAD_INFORMATION x64 layout");
static_assert(offsetof(ProcessInfo, threads) == 256, "SYSTEM_PROCESS_INFORMATION x64 layout");

using NtQsiFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using NtQitFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

struct Sample {
    struct T {
        ULONG cs = 0;
        int64_t cpu_100ns = 0;
        void* start = nullptr;
    };
    std::unordered_map<DWORD, T> threads;
    int64_t cpu_100ns = 0;
    SIZE_T ws = 0, private_bytes = 0;
    ULONG handles = 0;
    bool found = false;
};

bool TakeSample(NtQsiFn qsi, DWORD pid, Sample* out) {
    static std::vector<unsigned char> buf(1 << 20);
    for (;;) {
        ULONG need = 0;
        const LONG st = qsi(5 /*SystemProcessInformation*/, buf.data(), static_cast<ULONG>(buf.size()), &need);
        if (st == static_cast<LONG>(0xC0000004) /*STATUS_INFO_LENGTH_MISMATCH*/) {
            buf.resize(std::max<size_t>(need + 65536, buf.size() * 2));
            continue;
        }
        if (st < 0) return false;
        break;
    }
    *out = Sample{};
    for (size_t off = 0;;) {
        const auto* p = reinterpret_cast<const ProcessInfo*>(buf.data() + off);
        if (static_cast<DWORD>(reinterpret_cast<uintptr_t>(p->pid)) == pid) {
            out->found = true;
            out->cpu_100ns = p->user_time.QuadPart + p->kernel_time.QuadPart;
            out->ws = p->ws;
            out->private_bytes = p->private_pages;
            out->handles = p->handle_count;
            for (ULONG i = 0; i < p->thread_count; ++i) {
                const ThreadInfo& t = p->threads[i];
                Sample::T s;
                s.cs = t.context_switches;
                s.cpu_100ns = t.kernel_time.QuadPart + t.user_time.QuadPart;
                s.start = t.start_address;
                out->threads[static_cast<DWORD>(reinterpret_cast<uintptr_t>(t.tid))] = s;
            }
            return true;
        }
        if (!p->next_offset) break;
        off += p->next_offset;
    }
    return true;
}

std::string Narrow(const wchar_t* w) {
    char b[512];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof(b), nullptr, nullptr);
    return b;
}

// "ct-log", or "<module>+0x1234" from the Win32 start address.
std::string ThreadLabel(DWORD tid, HANDLE process, NtQitFn qit) {
    std::string label;
    HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!th) th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!th) return "?";
    PWSTR desc = nullptr;
    if (SUCCEEDED(GetThreadDescription(th, &desc)) && desc && *desc) label = Narrow(desc);
    if (desc) LocalFree(desc);
    void* start = nullptr;
    if (qit && qit(th, 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof(start), nullptr) >= 0 && start && process) {
        HMODULE mods[1024];
        DWORD bytes = 0;
        if (EnumProcessModulesEx(process, mods, sizeof(mods), &bytes, LIST_MODULES_ALL)) {
            for (DWORD i = 0; i < bytes / sizeof(HMODULE); ++i) {
                MODULEINFO mi{};
                if (!GetModuleInformation(process, mods[i], &mi, sizeof(mi))) continue;
                const auto base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
                const auto a = reinterpret_cast<uintptr_t>(start);
                if (a >= base && a < base + mi.SizeOfImage) {
                    wchar_t name[MAX_PATH];
                    GetModuleBaseNameW(process, mods[i], name, MAX_PATH);
                    char where[300];
                    std::snprintf(where, sizeof(where), "%s+0x%llx", Narrow(name).c_str(), static_cast<unsigned long long>(a - base));
                    label = label.empty() ? where : label + " (" + where + ")";
                    break;
                }
            }
        }
    }
    CloseHandle(th);
    return label.empty() ? "?" : label;
}

BOOL CALLBACK CloseWindowsOf(HWND hwnd, LPARAM pid) {
    DWORD owner = 0;
    GetWindowThreadProcessId(hwnd, &owner);
    // WM_QUIT, not WM_CLOSE: the recorder bar hides to the tray on close; WM_QUIT ends every app mode's loop cleanly.
    if (owner == static_cast<DWORD>(pid)) PostThreadMessageW(GetWindowThreadProcessId(hwnd, nullptr), WM_QUIT, 0, 0);
    return TRUE;
}

int Usage() {
    std::fprintf(stderr, "usage: idle_probe --pid N [--seconds S] [--warmup S]\n"
                         "       idle_probe [--seconds S] [--warmup S] [--keep] -- command line...\n");
    return 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    DWORD pid = 0;
    double seconds = 60, warmup = -1;
    bool keep = false;
    std::wstring cmd;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--pid") && i + 1 < argc) pid = static_cast<DWORD>(_wtoi(argv[++i]));
        else if (!wcscmp(argv[i], L"--seconds") && i + 1 < argc) seconds = _wtof(argv[++i]);
        else if (!wcscmp(argv[i], L"--warmup") && i + 1 < argc) warmup = _wtof(argv[++i]);
        else if (!wcscmp(argv[i], L"--keep")) keep = true;
        else if (!wcscmp(argv[i], L"--")) {
            // Re-quote the rest (simple rule: quote arguments with spaces; ours have no embedded quotes).
            for (int j = i + 1; j < argc; ++j) {
                if (!cmd.empty()) cmd += L' ';
                const bool q = wcschr(argv[j], L' ') != nullptr;
                if (q) cmd += L'"';
                cmd += argv[j];
                if (q) cmd += L'"';
            }
            break;
        } else return Usage();
    }
    if (!pid && cmd.empty()) return Usage();
    if (warmup < 0) warmup = cmd.empty() ? 0 : 10;

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto qsi = reinterpret_cast<NtQsiFn>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
    auto qit = reinterpret_cast<NtQitFn>(GetProcAddress(ntdll, "NtQueryInformationThread"));
    if (!qsi) return 1;

    PROCESS_INFORMATION pi{};
    if (!cmd.empty()) {
        STARTUPINFOW si{sizeof(si)};
        std::vector<wchar_t> line(cmd.begin(), cmd.end());
        line.push_back(0);
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
            std::fprintf(stderr, "idle_probe: CreateProcess failed (%lu)\n", GetLastError());
            return 1;
        }
        pid = pi.dwProcessId;
    }
    HANDLE process = pi.hProcess ? pi.hProcess : OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
    std::printf("idle_probe: pid %lu, warm-up %.0f s, sampling %.0f s\n", pid, warmup, seconds);
    if (process && WaitForSingleObject(process, static_cast<DWORD>(warmup * 1000)) == WAIT_OBJECT_0) {
        std::printf("idle_probe: process exited during warm-up\n");
        return 2;
    }

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    Sample a, b;
    QueryPerformanceCounter(&t0);
    TakeSample(qsi, pid, &a);
    const bool exited = process && WaitForSingleObject(process, static_cast<DWORD>(seconds * 1000)) == WAIT_OBJECT_0;
    TakeSample(qsi, pid, &b);
    QueryPerformanceCounter(&t1);
    if (!a.found || !b.found || exited) {
        std::printf("idle_probe: process exited during sampling\n");
        return 2;
    }
    const double wall_s = static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);

    struct Row {
        DWORD tid;
        ULONG cs;
        double cpu_ms;
        bool is_new;
    };
    std::vector<Row> rows;
    ULONG total_cs = 0;
    for (const auto& [tid, t] : b.threads) {
        const auto it = a.threads.find(tid);
        const ULONG cs = it != a.threads.end() ? t.cs - it->second.cs : t.cs;
        const int64_t cpu = it != a.threads.end() ? t.cpu_100ns - it->second.cpu_100ns : t.cpu_100ns;
        rows.push_back({tid, cs, static_cast<double>(cpu) / 1e4, it == a.threads.end()});
        total_cs += cs;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.cs != y.cs ? x.cs > y.cs : x.tid < y.tid; });
    std::printf("%-7s %9s %8s %9s  %s\n", "tid", "wakes", "wakes/s", "cpu ms", "thread");
    for (const Row& r : rows)
        std::printf("%-7lu %9lu %8.2f %9.1f  %s%s\n", r.tid, r.cs, r.cs / wall_s, r.cpu_ms, ThreadLabel(r.tid, process, qit).c_str(),
                    r.is_new ? " [new]" : "");
    const double cpu_ms = static_cast<double>(b.cpu_100ns - a.cpu_100ns) / 1e4;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    std::printf("total: %zu threads, %lu wakes (%.2f/s), CPU %.1f ms over %.1f s = %.4f %% of one core, %.4f %% of the machine (%lu cores)\n",
                rows.size(), total_cs, total_cs / wall_s, cpu_ms, wall_s, cpu_ms / (wall_s * 10.0),
                cpu_ms / (wall_s * 10.0 * si.dwNumberOfProcessors), si.dwNumberOfProcessors);
    std::printf("memory: working set %.1f MB, private %.1f MB, handles %lu\n", static_cast<double>(b.ws) / 1048576.0,
                static_cast<double>(b.private_bytes) / 1048576.0, b.handles);

    if (pi.hProcess && !keep) {
        EnumWindows(&CloseWindowsOf, static_cast<LPARAM>(pid));
        if (WaitForSingleObject(pi.hProcess, 10000) != WAIT_OBJECT_0) {
            std::printf("idle_probe: no exit after WM_QUIT, terminating\n");
            TerminateProcess(pi.hProcess, 1);
        }
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        std::printf("idle_probe: exit code %lu\n", code);
    }
    if (pi.hThread) CloseHandle(pi.hThread);
    if (process) CloseHandle(process);
    return 0;
}
