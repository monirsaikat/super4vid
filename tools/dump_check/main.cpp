// dump_check: prints the exception and the faulting thread's call stack of a minidump written by the crash handler,
// resolved with the PDBs next to the modules (or on --symbols), the way WinDbg's `.ecxr; kn` shows it (P00 0.7).
//
//   dump_check crash.dmp [--symbols dir] [--expect text]
//       --expect: exit 0 only when a frame of the faulting stack (function or file:line) contains `text`.
// Exit codes: 0 ok, 1 usage / unreadable dump, 2 no exception stream, 3 --expect not found.

#include <ct/platform/win.h>

#include <dbghelp.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

struct Dump {
    const unsigned char* base = nullptr;
    uint64_t size = 0;
    struct Range {
        uint64_t start;
        uint64_t size;
        uint64_t rva;
    };
    std::vector<Range> memory;
    struct Module {
        uint64_t base;
        uint64_t size;
        std::wstring path;
        const unsigned char* image = nullptr;   // the file mapped as an image (sections at their RVAs), on demand
        bool tried = false;
    };
    std::vector<Module> modules;
};
Dump g_dump;

template <class T>
const T* At(RVA64 rva) {
    return rva + sizeof(T) <= g_dump.size ? reinterpret_cast<const T*>(g_dump.base + rva) : nullptr;
}

const unsigned char* MappedImage(Dump::Module& m);

BOOL CALLBACK ReadMemory(HANDLE, DWORD64 address, PVOID buffer, DWORD size, LPDWORD read) {
    for (const Dump::Range& r : g_dump.memory) {
        if (address >= r.start && address + size <= r.start + r.size) {
            std::memcpy(buffer, g_dump.base + r.rva + (address - r.start), size);
            if (read) *read = size;
            return TRUE;
        }
    }
    // Code and unwind data are not in the dump (as in WinDbg, they come from the module files on disk): map the
    // module as an image and read at the same RVA. Relocations are not applied, which unwinding does not need.
    for (Dump::Module& m : g_dump.modules) {
        if (address < m.base || address + size > m.base + m.size) continue;
        if (!MappedImage(m)) break;
        std::memcpy(buffer, m.image + (address - m.base), size);
        if (read) *read = size;
        return TRUE;
    }
    if (read) *read = 0;
    return FALSE;
}

std::wstring DumpString(RVA rva) {
    const auto* s = At<MINIDUMP_STRING>(rva);
    if (!s) return {};
    return std::wstring(s->Buffer, s->Length / sizeof(wchar_t));
}

const unsigned char* MappedImage(Dump::Module& m) {
    if (!m.tried) {
        m.tried = true;
        HMODULE h = LoadLibraryExW(m.path.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
        // The returned handle carries flag bits in its low two bits; the mapping starts at the aligned address.
        if (h) m.image = reinterpret_cast<const unsigned char*>(reinterpret_cast<uintptr_t>(h) & ~uintptr_t{3});
    }
    return m.image;
}

// x64 unwinding needs the RUNTIME_FUNCTION covering a pc: binary search of the module's .pdata in the mapped image
// (SymFunctionTableAccess64 finds nothing for a pseudo process handle). dbghelp then reads the UNWIND_INFO through
// ReadMemory, which serves image addresses from the same mapping.
PVOID CALLBACK FunctionTableAccess(HANDLE, DWORD64 pc) {
    static IMAGE_RUNTIME_FUNCTION_ENTRY found;
    for (Dump::Module& m : g_dump.modules) {
        if (pc < m.base || pc >= m.base + m.size) continue;
        const unsigned char* img = MappedImage(m);
        if (!img) return nullptr;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(img);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(img + dos->e_lfanew);
        if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        const auto* table = reinterpret_cast<const IMAGE_RUNTIME_FUNCTION_ENTRY*>(img + dir.VirtualAddress);
        size_t lo = 0, hi = dir.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
        const auto rva = static_cast<DWORD>(pc - m.base);
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (rva < table[mid].BeginAddress) hi = mid;
            else if (rva >= table[mid].EndAddress) lo = mid + 1;
            else {
                found = table[mid];
                return &found;
            }
        }
        return nullptr;
    }
    return nullptr;
}

std::string Narrow(const wchar_t* w) {
    char b[1024];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof(b), nullptr, nullptr);
    return b;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: dump_check crash.dmp [--symbols dir] [--expect text]\n");
        return 1;
    }
    std::wstring symbols;
    std::string expect;
    for (int i = 2; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--symbols") && i + 1 < argc) symbols = argv[++i];
        else if (!wcscmp(argv[i], L"--expect") && i + 1 < argc) expect = Narrow(argv[++i]);
    }
    HANDLE file = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "dump_check: cannot open %ls\n", argv[1]);
        return 1;
    }
    LARGE_INTEGER sz;
    GetFileSizeEx(file, &sz);
    HANDLE map = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    void* view = map ? MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0) : nullptr;
    if (!view) return 1;
    g_dump.base = static_cast<const unsigned char*>(view);
    g_dump.size = static_cast<uint64_t>(sz.QuadPart);

    void* stream = nullptr;
    ULONG stream_size = 0;
    if (MiniDumpReadDumpStream(view, MemoryListStream, nullptr, &stream, &stream_size) && stream) {
        const auto* list = static_cast<const MINIDUMP_MEMORY_LIST*>(stream);
        for (ULONG i = 0; i < list->NumberOfMemoryRanges; ++i) {
            const MINIDUMP_MEMORY_DESCRIPTOR& d = list->MemoryRanges[i];
            g_dump.memory.push_back({d.StartOfMemoryRange, d.Memory.DataSize, d.Memory.Rva});
        }
    }
    if (MiniDumpReadDumpStream(view, Memory64ListStream, nullptr, &stream, &stream_size) && stream) {
        const auto* list = static_cast<const MINIDUMP_MEMORY64_LIST*>(stream);
        uint64_t rva = list->BaseRva;
        for (ULONG64 i = 0; i < list->NumberOfMemoryRanges; ++i) {
            g_dump.memory.push_back({list->MemoryRanges[i].StartOfMemoryRange, list->MemoryRanges[i].DataSize, rva});
            rva += list->MemoryRanges[i].DataSize;
        }
    }

    if (!MiniDumpReadDumpStream(view, ExceptionStream, nullptr, &stream, &stream_size) || !stream) {
        std::printf("dump_check: no exception stream\n");
        return 2;
    }
    const auto* ex = static_cast<const MINIDUMP_EXCEPTION_STREAM*>(stream);
    const auto* ctx_in_dump = At<CONTEXT>(ex->ThreadContext.Rva);
    if (!ctx_in_dump) return 2;
    CONTEXT ctx = *ctx_in_dump;   // StackWalk64 modifies it
    std::printf("exception 0x%08lX at 0x%llx, thread %lu, %zu memory ranges\n", ex->ExceptionRecord.ExceptionCode,
                static_cast<unsigned long long>(ex->ExceptionRecord.ExceptionAddress), ex->ThreadId, g_dump.memory.size());

    // A pseudo process handle: every dbghelp call below passes explicit module bases and our ReadMemory.
    const HANDLE process = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0xC7D0));
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    if (!SymInitializeW(process, symbols.empty() ? nullptr : symbols.c_str(), FALSE)) return 1;
    if (MiniDumpReadDumpStream(view, ModuleListStream, nullptr, &stream, &stream_size) && stream) {
        const auto* mods = static_cast<const MINIDUMP_MODULE_LIST*>(stream);
        for (ULONG i = 0; i < mods->NumberOfModules; ++i) {
            const MINIDUMP_MODULE& m = mods->Modules[i];
            // The image is read from the module's original path (unwind data); its PDB is found through the image's
            // CodeView record (the build tree), else on the symbol path.
            const std::wstring path = DumpString(m.ModuleNameRva);
            g_dump.modules.push_back({m.BaseOfImage, m.SizeOfImage, path});
            SymLoadModuleExW(process, nullptr, path.c_str(), nullptr, m.BaseOfImage, m.SizeOfImage, nullptr, 0);
        }
    }

    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    bool found = expect.empty();
    for (int n = 0; n < 64; ++n) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, nullptr, &frame, &ctx, &ReadMemory, &FunctionTableAccess,
                         &SymGetModuleBase64, nullptr) ||
            frame.AddrPC.Offset == 0)
            break;
        const DWORD64 pc = frame.AddrPC.Offset;
        // Return addresses point after the call; look up pc - 1 for frames above the faulting one.
        const DWORD64 lookup = n == 0 ? pc : pc - 1;
        char line_buf[1024];
        IMAGEHLP_MODULEW64 mi{sizeof(mi)};
        const std::string mod = SymGetModuleInfoW64(process, lookup, &mi) ? Narrow(mi.ModuleName) : std::string("?");
        alignas(SYMBOL_INFO) char sym_buf[sizeof(SYMBOL_INFO) + 512];
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        const char* fn = SymFromAddr(process, lookup, &disp, sym) ? sym->Name : "?";
        IMAGEHLP_LINE64 line{sizeof(line)};
        DWORD line_disp = 0;
        if (SymGetLineFromAddr64(process, lookup, &line_disp, &line))
            std::snprintf(line_buf, sizeof(line_buf), "%02d %s!%s+0x%llx  %s:%lu", n, mod.c_str(), fn,
                          static_cast<unsigned long long>(disp), line.FileName, line.LineNumber);
        else
            std::snprintf(line_buf, sizeof(line_buf), "%02d %s!%s+0x%llx", n, mod.c_str(), fn, static_cast<unsigned long long>(disp));
        std::printf("%s\n", line_buf);
        if (!found && std::strstr(line_buf, expect.c_str())) found = true;
    }
    SymCleanup(process);
    UnmapViewOfFile(view);
    CloseHandle(map);
    CloseHandle(file);
    if (!found) {
        std::printf("dump_check: \"%s\" not on the faulting stack\n", expect.c_str());
        return 3;
    }
    return 0;
}
