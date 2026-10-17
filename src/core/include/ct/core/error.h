#pragma once
// ct/core/error.h
// HRESULT formatting and logging, the Err result type for logic-level functions, and the CT_RETURN_IF_* macros.
// Thread model: all functions are thread-safe. The macros need <windows.h> (FAILED/HRESULT) at the use site.

#include <string>
#include <string_view>

namespace ct {

// Describes why a logic-level function (`bool Foo(Err* err)`) returned false. `message` is user-presentable.
struct Err {
    std::string message;
    long hr = 0;
    void Set(std::string_view m, long h = 0);
    bool Ok() const { return message.empty(); }
};

// "0x887A0026 (DXGI_ERROR_ACCESS_LOST)". Known DXGI/D3D/MF/WASAPI codes by name, otherwise FormatMessage text.
std::string HrToString(long hr);

// Logs "<expr> failed: <HrToString(hr)>" at Error level with the caller's file and line.
void LogHr(long hr, const char* expr, const char* file, int line);

} // namespace ct

#define CT_RETURN_IF_FAILED(expr)  do { HRESULT _hr = (expr); if (FAILED(_hr)) { ::ct::LogHr(_hr, #expr, __FILE__, __LINE__); return _hr; } } while (0)
#define CT_LOG_IF_FAILED(expr)     do { HRESULT _hr = (expr); if (FAILED(_hr)) ::ct::LogHr(_hr, #expr, __FILE__, __LINE__); } while (0)
#define CT_RETURN_IF_NULL(ptr, hr) do { if (!(ptr)) { ::ct::LogHr((hr), #ptr " was null", __FILE__, __LINE__); return (hr); } } while (0)
#define CT_RETURN_FALSE_IF(cond, err, msg) do { if (cond) { if (err) (err)->Set(msg); return false; } } while (0)
