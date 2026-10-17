#define CT_LOG_CATEGORY "crash"
#include <ct/platform/crash_handler.h>

#include <ct/core/assert.h>
#include <ct/core/log.h>
#include <ct/platform/win.h>

#include <ct/core/strings.h>

#include <dbghelp.h>
#include <eh.h>
#include <intrin.h>
#include <signal.h>
#include <stdlib.h>

#include <atomic>
#include <cwchar>

// Everything the filter needs is preallocated: it runs in a process that may have a corrupt heap.

namespace ct::platform {

namespace {

constexpr DWORD kFatalExceptionCode = 0xE0C70001;   // custom, raised by WriteDumpAndExit to get a context record
constexpr UINT kCrashExitCode = 0xC0000409;         // STATUS_STACK_BUFFER_OVERRUN, the conventional fail-fast code
constexpr int kMaxEmergency = 8;

struct Emergency {
    CrashHandler::EmergencyCallback fn;
    void* user;
};

wchar_t g_dump_dir[MAX_PATH] = {};
wchar_t g_dump_path[MAX_PATH + 64] = {};
Emergency g_emergency[kMaxEmergency] = {};
std::atomic<int> g_emergency_count{0};
std::atomic<bool> g_in_crash{false};
const char* g_fatal_reason = nullptr;

struct DumpRequest {
    EXCEPTION_POINTERS* ep;
    DWORD thread_id;
    BOOL ok;
};

DWORD WINAPI WriteDumpThread(void* param) {
    auto* req = static_cast<DumpRequest*>(param);
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf(g_dump_path, std::size(g_dump_path), L"%s\\crash-%04u%02u%02u-%02u%02u%02u-%lu.dmp", g_dump_dir, st.wYear,
             st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
    HANDLE file = CreateFileW(g_dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        g_dump_path[0] = L'\0';
        req->ok = FALSE;
        return 1;
    }
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = req->thread_id;
    info.ExceptionPointers = req->ep;
    info.ClientPointers = FALSE;
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                                 MiniDumpWithUnloadedModules | MiniDumpWithDataSegs);
    req->ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, req->ep ? &info : nullptr,
                                nullptr, nullptr);
    CloseHandle(file);
    if (!req->ok) {
        DeleteFileW(g_dump_path);
        g_dump_path[0] = L'\0';
    }
    return 0;
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
    if (g_in_crash.exchange(true)) {
        TerminateProcess(GetCurrentProcess(), kCrashExitCode);   // crashed while handling a crash
    }
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    const void* address = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    if (code == kFatalExceptionCode && g_fatal_reason)
        CT_LOG_ERROR("fatal error: %s", g_fatal_reason);
    else
        CT_LOG_ERROR("unhandled exception 0x%08lX at %p", static_cast<unsigned long>(code), address);
    Log::Flush();

    DumpRequest req{ep, GetCurrentThreadId(), FALSE};
    if (code == EXCEPTION_STACK_OVERFLOW) {
        // Almost no stack left on this thread: let a fresh thread write the dump.
        HANDLE thread = CreateThread(nullptr, 256 * 1024, &WriteDumpThread, &req, 0, nullptr);
        if (thread) {
            WaitForSingleObject(thread, 30'000);
            CloseHandle(thread);
        }
    } else {
        WriteDumpThread(&req);
    }
    if (req.ok)
        CT_LOG_ERROR("crash dump written: %ls", g_dump_path);
    else
        CT_LOG_ERROR("crash dump could not be written (error %lu)", GetLastError());

    const int count = g_emergency_count.load(std::memory_order_acquire);
    for (int i = 0; i < count && i < kMaxEmergency; ++i)
        if (g_emergency[i].fn) g_emergency[i].fn(g_emergency[i].user);

    Log::Flush();
    TerminateProcess(GetCurrentProcess(), kCrashExitCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

void InvalidParameterHandler(const wchar_t* expr, const wchar_t* func, const wchar_t* file, unsigned int line,
                             uintptr_t reserved) {
    // The release CRT passes nulls for all of these.
    (void)expr; (void)func; (void)file; (void)line; (void)reserved;
    CrashHandler::WriteDumpAndExit("invalid parameter passed to a CRT function");
}

void PureCallHandler() {
    CrashHandler::WriteDumpAndExit("pure virtual function call");
}

void TerminateHandler() {
    CrashHandler::WriteDumpAndExit("std::terminate");
}

void AbortSignalHandler(int sig) {
    (void)sig;
    CrashHandler::WriteDumpAndExit("abort()");
}

void FatalFromCore(const char* reason) {
    CrashHandler::WriteDumpAndExit(reason);
}

// Separate function: __try cannot share a frame with objects that need unwinding.
void RaiseFatal() {
    __try {
        RaiseException(kFatalExceptionCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    } __except (UnhandledFilter(GetExceptionInformation())) {
    }
}

} // namespace

void CrashHandler::Install() {
    if (g_dump_dir[0] == L'\0') {
        // Until SetDumpDir(): %TEMP%. GetTempPathW returns a trailing backslash; drop it.
        const DWORD n = GetTempPathW(MAX_PATH, g_dump_dir);
        if (n > 0 && n < MAX_PATH && g_dump_dir[n - 1] == L'\\') g_dump_dir[n - 1] = L'\0';
    }
    SetUnhandledExceptionFilter(&UnhandledFilter);
    _set_invalid_parameter_handler(&InvalidParameterHandler);
    _set_purecall_handler(&PureCallHandler);
    set_terminate(&TerminateHandler);
    signal(SIGABRT, &AbortSignalHandler);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetFatalHandler(&FatalFromCore);
    // Make sure dbghelp is loaded now, not inside a crashed process.
    LoadLibraryW(L"dbghelp.dll");
}

void CrashHandler::SetDumpDir(const char* utf8) {
    const std::wstring dir = Utf8ToWide(utf8 ? utf8 : "");
    if (dir.empty() || dir.size() >= MAX_PATH) return;
    wcsncpy_s(g_dump_dir, dir.c_str(), _TRUNCATE);
}

void CrashHandler::RegisterEmergency(EmergencyCallback fn, void* user) {
    const int index = g_emergency_count.load(std::memory_order_relaxed);
    if (index >= kMaxEmergency) {
        CT_LOG_ERROR("too many emergency callbacks (max %d)", kMaxEmergency);
        return;
    }
    g_emergency[index] = Emergency{fn, user};
    g_emergency_count.store(index + 1, std::memory_order_release);
}

void CrashHandler::WriteDumpAndExit(const char* reason) {
    g_fatal_reason = reason ? reason : "(no reason)";
    RaiseFatal();
    // UnhandledFilter terminates the process; this is only reached if it was already crashing.
    TerminateProcess(GetCurrentProcess(), kCrashExitCode);
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}

const wchar_t* CrashHandler::LastDumpPath() {
    return g_dump_path;
}

} // namespace ct::platform
