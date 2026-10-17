#pragma once
// ct/core/assert.h
// CT_ASSERT (Debug only), CT_VERIFY (always evaluated), CT_FATAL (dump + terminate).
// ct_core cannot depend on ct_platform, so CT_FATAL goes through a handler that
// ct::platform::CrashHandler::Install() registers (it writes a minidump). Without one it calls __fastfail.
// Thread model: everything here is safe from any thread.

namespace ct {

using FatalHandler = void (*)(const char* reason);   // must not return
void SetFatalHandler(FatalHandler handler);

void AssertFailed(const char* expr, const char* file, int line);       // logs at Error and flushes
[[noreturn]] void Fatal(const char* msg, const char* file, int line);   // logs, flushes, calls the fatal handler

} // namespace ct

// _DEBUG is set by /MTd, so this also works for targets that do not get CT_DEBUG (third-party code like ImGui).
#if defined(_DEBUG)
#define CT_ASSERT(cond) do { if (!(cond)) { ::ct::AssertFailed(#cond, __FILE__, __LINE__); __debugbreak(); } } while (0)
#define CT_VERIFY(cond) do { if (!(cond)) { ::ct::AssertFailed(#cond, __FILE__, __LINE__); __debugbreak(); } } while (0)
#else
// sizeof keeps variables used only in asserts "referenced" (no C4189 at /W4) without evaluating anything.
#define CT_ASSERT(cond) ((void)sizeof(!(cond)))
#define CT_VERIFY(cond) do { if (!(cond)) { ::ct::AssertFailed(#cond, __FILE__, __LINE__); } } while (0)
#endif

#define CT_FATAL(msg) ::ct::Fatal((msg), __FILE__, __LINE__)
