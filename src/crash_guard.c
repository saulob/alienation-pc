#include "crash_guard.h"

#include "runtime_log.h"
#include "startup_phase.h"

#include <dbghelp.h>
#include <stdio.h>
#include <string.h>

#define DETAIL_TIMEOUT_MS 20000
#define DUMP_TIMEOUT_MS 60000
#define FILTER_CHECK_MS 1000
#define FIRST_CHANCE_LIMIT 8

enum {
    CODE_ACCESS_VIOLATION = 0xC0000005u,
    CODE_IN_PAGE_ERROR = 0xC0000006u,
    CODE_INVALID_HANDLE = 0xC0000008u,
    CODE_ILLEGAL_INSTRUCTION = 0xC000001Du,
    CODE_ARRAY_BOUNDS = 0xC000008Cu,
    CODE_INT_DIVIDE_BY_ZERO = 0xC0000094u,
    CODE_INT_OVERFLOW = 0xC0000095u,
    CODE_PRIV_INSTRUCTION = 0xC0000096u,
    CODE_STACK_OVERFLOW = 0xC00000FDu,
    CODE_HEAP_CORRUPTION = 0xC0000374u,
    CODE_STACK_CHECK = 0xC0000409u,
    CODE_DATATYPE_MISALIGNMENT = 0x80000002u,
};

typedef BOOL(WINAPI *WriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

static AlnCrashHooks hooks;
static ULONGLONG start_tick;
static uintptr_t exe_base, exe_end, guest_image;
static uint64_t guest_size;
static WriteDump write_minidump;
static HANDLE watchdog, begin_event, detail_done, dump_done;
static volatile LONG reporting, first_chance_lines;
static volatile DWORD owner_thread, exit_override;
static volatile LONG overflow;
static const char *volatile request_note;
static EXCEPTION_POINTERS *volatile request;
static char line[4096];
static char watchdog_line[512], dump_line[512], filter_line[700], chance_line[6144];
static char exe_name[260];
static volatile LONG chance_busy;

typedef struct {
    char *data;
    size_t size, used;
} Text;

static void text_init(Text *text, char *buffer, size_t size) {
    text->data = buffer;
    text->size = size;
    text->used = 0;
    if (size) buffer[0] = 0;
}

static void text_char(Text *text, char c) {
    if (text->used + 1 >= text->size) return;
    text->data[text->used++] = c;
    text->data[text->used] = 0;
}

static void text_str(Text *text, const char *s) {
    while (s && *s) text_char(text, *s++);
}

static void text_hex(Text *text, uint64_t value) {
    char digits[16];
    int n = 0;
    do {
        digits[n++] = "0123456789abcdef"[value & 15];
        value >>= 4;
    } while (value && n < 16);
    text_str(text, "0x");
    while (n) text_char(text, digits[--n]);
}

static void text_dec(Text *text, uint64_t value) {
    char digits[20];
    int n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n) text_char(text, digits[--n]);
}

const char *aln_crash_code_name(uint32_t code) {
    switch (code) {
    case CODE_ACCESS_VIOLATION: return "access violation";
    case CODE_IN_PAGE_ERROR: return "in-page error";
    case CODE_INVALID_HANDLE: return "invalid handle";
    case CODE_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case CODE_ARRAY_BOUNDS: return "array bounds exceeded";
    case CODE_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case CODE_INT_OVERFLOW: return "integer overflow";
    case CODE_PRIV_INSTRUCTION: return "privileged instruction";
    case CODE_STACK_OVERFLOW: return "stack overflow";
    case CODE_HEAP_CORRUPTION: return "heap corruption";
    case CODE_STACK_CHECK: return "stack check failure";
    case CODE_DATATYPE_MISALIGNMENT: return "datatype misalignment";
    case 0x40000015u: return "abort";
    default: return "exception";
    }
}

AlnFaultAction aln_crash_classify(uint32_t code, int in_guest, int ours) {
    switch (code) {
    case CODE_STACK_OVERFLOW:
    case CODE_HEAP_CORRUPTION:
    case CODE_STACK_CHECK:
    case CODE_INVALID_HANDLE:
        return ALN_FAULT_CRASH;
    case CODE_ACCESS_VIOLATION:
    case CODE_IN_PAGE_ERROR:
    case CODE_ILLEGAL_INSTRUCTION:
    case CODE_PRIV_INSTRUCTION:
    case CODE_INT_DIVIDE_BY_ZERO:
    case CODE_INT_OVERFLOW:
    case CODE_ARRAY_BOUNDS:
    case CODE_DATATYPE_MISALIGNMENT:
        return in_guest || ours ? ALN_FAULT_CRASH : ALN_FAULT_IGNORE;
    default:
        return ALN_FAULT_IGNORE;
    }
}

static int in_guest(uintptr_t address) { return guest_image && address >= guest_image && address - guest_image < guest_size; }

/* This executable or code outside any module; a VirtualQuery takes no loader lock, unlike a module lookup. */
static int port_owned(uintptr_t address) {
    MEMORY_BASIC_INFORMATION info;
    if (address - exe_base < exe_end - exe_base) return 1;
    return !VirtualQuery((LPCVOID)address, &info, sizeof(info)) || info.Type != MEM_IMAGE;
}

static void describe(Text *text, uintptr_t address) {
    char buffer[300];
    if (in_guest(address)) {
        text_str(text, "guest offset ");
        text_hex(text, address - guest_image);
        return;
    }
    if (hooks.describe && hooks.describe(buffer, sizeof(buffer), address)) {
        text_str(text, buffer);
        return;
    }
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery((LPCVOID)address, &info, sizeof(info)) && info.Type == MEM_IMAGE) {
        text_str(text, address - exe_base < exe_end - exe_base ? "alienation.exe+" : "module+");
        text_hex(text, address - (address - exe_base < exe_end - exe_base ? exe_base : (uintptr_t)info.AllocationBase));
    } else {
        text_str(text, "no module");
    }
}

#define PS4_RANGE_MIN UINT64_C(0x1000000000)
#define PS4_RANGE_MAX UINT64_C(0xfc00000000)

static void describe_memory(Text *text, uintptr_t address) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((LPCVOID)address, &info, sizeof(info))) {
        text_str(text, "unknown memory");
        return;
    }
    text_str(text, info.State == MEM_COMMIT ? "committed" : info.State == MEM_RESERVE ? "reserved" : "free");
    text_str(text, " memory, protection ");
    text_hex(text, info.Protect);
    text_str(text, ", region ");
    text_hex(text, (uintptr_t)info.BaseAddress);
    text_char(text, '+');
    text_hex(text, info.RegionSize);
    if (address >= PS4_RANGE_MIN && address < PS4_RANGE_MAX) text_str(text, ", inside the PS4 address range");
}

static int code_address(uintptr_t address) {
    MEMORY_BASIC_INFORMATION info;
    return address >= 0x10000 && VirtualQuery((LPCVOID)address, &info, sizeof(info)) && info.State == MEM_COMMIT &&
           (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

static int read_word(uintptr_t address, uint64_t *word) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)address, word, sizeof(*word), &got) && got == sizeof(*word);
}

static void put_thread_label(Text *text, DWORD thread) {
    const char *label = aln_thread_label(thread);
    if (!*label) return;
    text_str(text, " (");
    text_str(text, label);
    text_char(text, ')');
}

static void put_registers(Text *text, const CONTEXT *c) {
    const struct { const char *name; DWORD64 value; } regs[] = {
        {"rax", c->Rax}, {"rbx", c->Rbx}, {"rcx", c->Rcx}, {"rdx", c->Rdx}, {"rsi", c->Rsi}, {"rdi", c->Rdi},
        {"rbp", c->Rbp}, {"rsp", c->Rsp}, {"r8", c->R8},   {"r9", c->R9},   {"r10", c->R10}, {"r11", c->R11},
        {"r12", c->R12}, {"r13", c->R13}, {"r14", c->R14}, {"r15", c->R15}};
    text_str(text, "\n  registers:");
    for (size_t i = 0; i < sizeof(regs) / sizeof(*regs); ++i) {
        text_char(text, ' ');
        text_str(text, regs[i].name);
        text_char(text, '=');
        text_hex(text, regs[i].value);
    }
}

/* Code addresses among the first stack words: for a fault inside a library, its caller is among them. */
static void put_stack_code(Text *text, uintptr_t rsp) {
    int found = 0;
    text_str(text, "\n  code addresses on the stack:");
    for (int i = 0; i < 64 && found < 10; ++i) {
        uint64_t word;
        if (!read_word(rsp + (uintptr_t)i * 8, &word)) break;
        if (!in_guest((uintptr_t)word) && !code_address((uintptr_t)word)) continue;
        text_str(text, "\n    rsp+");
        text_hex(text, (uint64_t)i * 8);
        text_str(text, " = ");
        describe(text, (uintptr_t)word);
        ++found;
    }
    if (!found) text_str(text, " none");
}

static const char *access_name(ULONG_PTR kind) { return kind == 1 ? "write" : kind == 8 ? "execute" : "read"; }

static void put_fault(Text *text, const EXCEPTION_RECORD *record) {
    text_str(text, aln_crash_code_name(record->ExceptionCode));
    text_str(text, " (");
    text_hex(text, record->ExceptionCode);
    text_str(text, ")");
    if ((record->ExceptionCode == CODE_ACCESS_VIOLATION || record->ExceptionCode == CODE_IN_PAGE_ERROR) &&
        record->NumberParameters >= 2) {
        text_char(text, ' ');
        text_str(text, access_name(record->ExceptionInformation[0]));
        text_str(text, " at ");
        text_hex(text, record->ExceptionInformation[1]);
    }
    text_str(text, ", flags ");
    text_hex(text, record->ExceptionFlags);
}

static void write_text(const Text *text) {
    runtime_log_write_direct(text->data);
}

static void write_emergency_line(const EXCEPTION_POINTERS *info, const char *note, DWORD thread) {
    const CONTEXT *c = info->ContextRecord;
    Text text;
    char context[320];
    text_init(&text, line, sizeof(line));
    text_str(&text, "CRASH: ");
    put_fault(&text, info->ExceptionRecord);
    if (note) {
        text_str(&text, " [");
        text_str(&text, note);
        text_char(&text, ']');
    }
    text_str(&text, "\n  rip ");
    text_hex(&text, c->Rip);
    text_str(&text, " = ");
    describe(&text, (uintptr_t)c->Rip);
    if ((info->ExceptionRecord->ExceptionCode == CODE_ACCESS_VIOLATION ||
         info->ExceptionRecord->ExceptionCode == CODE_IN_PAGE_ERROR) &&
        info->ExceptionRecord->NumberParameters >= 2) {
        text_str(&text, "\n  fault address ");
        text_hex(&text, info->ExceptionRecord->ExceptionInformation[1]);
        text_str(&text, ": ");
        describe_memory(&text, (uintptr_t)info->ExceptionRecord->ExceptionInformation[1]);
    }
    text_str(&text, "\n  rsp ");
    text_hex(&text, c->Rsp);
    text_str(&text, ", rbp ");
    text_hex(&text, c->Rbp);
    text_str(&text, "; thread ");
    text_dec(&text, thread);
    put_thread_label(&text, thread);
    text_str(&text, ", ");
    text_dec(&text, (GetTickCount64() - start_tick) / 1000);
    text_str(&text, " s after start\n  ");
    aln_crash_context_describe(context, sizeof(context));
    text_str(&text, context);
    put_registers(&text, c);
    put_stack_code(&text, (uintptr_t)c->Rsp);
    text_char(&text, '\n');
    write_text(&text);
}

static void write_exit_line(DWORD code) {
    Text text;
    text_init(&text, line, sizeof(line));
    text_str(&text, "CRASH: the process ends with exit code ");
    text_hex(&text, code);
    text_char(&text, '\n');
    write_text(&text);
}

int aln_crash_dump_path(char *out, size_t size, const char *log_path, int attempt) {
    const char *dot = log_path ? strrchr(log_path, '.') : NULL;
    const char *slash = log_path ? strrchr(log_path, '\\') : NULL;
    if (!dot || (slash && dot < slash)) return 0;
    const size_t stem = (size_t)(dot - log_path);
    const int written = attempt >= 2 ? snprintf(out, size, "%.*s_%d.dmp", (int)stem, log_path, attempt)
                                     : snprintf(out, size, "%.*s.dmp", (int)stem, log_path);
    return written > 0 && (size_t)written < size;
}

static void write_dump(void) {
    static char path[4300];
    Text text;
    text_init(&text, dump_line, sizeof(dump_line));
    if (!write_minidump || !runtime_log_session_path()[0]) {
        text_str(&text, "  minidump: not written (no dbghelp or no log)\n");
        write_text(&text);
        return;
    }
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 1; attempt < 20 && file == INVALID_HANDLE_VALUE; ++attempt) {
        if (!aln_crash_dump_path(path, sizeof(path), runtime_log_session_path(), attempt)) break;
        file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (file == INVALID_HANDLE_VALUE) {
        text_str(&text, "  minidump: not written (the file could not be created)\n");
        write_text(&text);
        return;
    }
    static char comment[900];
    Text note;
    text_init(&note, comment, sizeof(comment));
    text_str(&note, "Alienation PC crash dump: ");
    text_str(&note, exe_name);
    text_str(&note, " process ");
    text_dec(&note, GetCurrentProcessId());
    text_str(&note, ", exception ");
    text_hex(&note, request ? request->ExceptionRecord->ExceptionCode : 0);
    text_str(&note, " on thread ");
    text_dec(&note, owner_thread);
    text_str(&note, ", log ");
    text_str(&note, runtime_log_session_path());
    MINIDUMP_USER_STREAM stream = {CommentStreamA, (ULONG)note.used + 1, comment};
    MINIDUMP_USER_STREAM_INFORMATION streams = {1, &stream};
    MINIDUMP_EXCEPTION_INFORMATION exception = {owner_thread, request, FALSE};
    const MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                               MiniDumpWithProcessThreadData | MiniDumpWithFullMemoryInfo);
    const BOOL ok = write_minidump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &exception, &streams, NULL);
    const DWORD error = ok ? 0 : GetLastError();
    CloseHandle(file);
    const char *name = strrchr(path, '\\');
    text_str(&text, "  minidump: ");
    text_str(&text, name ? name + 1 : path);
    if (ok) {
        text_str(&text, " (next to this log)\n");
    } else {
        text_str(&text, " failed, error ");
        text_hex(&text, error);
        text_char(&text, '\n');
        DeleteFileA(path);
    }
    write_text(&text);
}

static __attribute__((noreturn)) void end_process(DWORD code) {
    write_exit_line(code);
    runtime_log_flush();
    TerminateProcess(GetCurrentProcess(), code);
    for (;;) Sleep(INFINITE);
}

static DWORD request_code(void) {
    return exit_override ? exit_override : request ? request->ExceptionRecord->ExceptionCode : 0;
}

static DWORD WINAPI watchdog_main(LPVOID unused) {
    (void)unused;
    for (;;) {
        if (WaitForSingleObject(begin_event, FILTER_CHECK_MS) == WAIT_TIMEOUT) {
            aln_crash_guard_verify();
            continue;
        }
        if (overflow) write_emergency_line(request, request_note, owner_thread);
        const int detail_finished = overflow || WaitForSingleObject(detail_done, DETAIL_TIMEOUT_MS) == WAIT_OBJECT_0;
        if (!detail_finished) {
            Text text;
            text_init(&text, watchdog_line, sizeof(watchdog_line));
            text_str(&text, "CRASH: the detailed report did not finish in 20 s; the lines above are what was recorded\n");
            write_text(&text);
        }
        write_dump();
        SetEvent(dump_done);
        if (!detail_finished || overflow) end_process(request_code());
        Sleep(INFINITE);
    }
}

__attribute__((noreturn)) void aln_crash_guard_report(EXCEPTION_POINTERS *info, const char *note) {
    const DWORD self = GetCurrentThreadId();
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (InterlockedCompareExchange(&reporting, 1, 0)) {
        if (owner_thread == self) {
            Text text;
            text_init(&text, line, sizeof(line));
            text_str(&text, "CRASH: a second fault on the crashing thread while it was reported, ");
            put_fault(&text, info->ExceptionRecord);
            text_str(&text, ", rip ");
            text_hex(&text, info->ContextRecord->Rip);
            text_char(&text, '\n');
            write_text(&text);
            if (watchdog) SetEvent(detail_done);
            else end_process(request_code());
            WaitForSingleObject(dump_done, DUMP_TIMEOUT_MS);
            end_process(request_code());
        }
        for (;;) Sleep(INFINITE);
    }
    owner_thread = self;
    request = info;
    runtime_log_mark_crash();
    if (code == CODE_STACK_OVERFLOW && watchdog) {
        /* Almost no stack is left: the crash thread writes everything. */
        request_note = note;
        overflow = 1;
        SetEvent(begin_event);
        for (;;) Sleep(INFINITE);
    }
    write_emergency_line(info, note, self);
    if (watchdog) SetEvent(begin_event);
    if (hooks.detail) hooks.detail(info);
    if (watchdog) {
        SetEvent(detail_done);
        WaitForSingleObject(dump_done, DUMP_TIMEOUT_MS);
    }
    end_process(request_code());
}

void aln_crash_guard_report_here(const char *what, uint32_t code, uint32_t exit_code) {
    CONTEXT context;
    EXCEPTION_RECORD record;
    EXCEPTION_POINTERS pointers = {&record, &context};
    memset(&record, 0, sizeof(record));
    RtlCaptureContext(&context);
    record.ExceptionCode = code;
    record.ExceptionAddress = (PVOID)(uintptr_t)context.Rip;
    exit_override = exit_code;
    aln_crash_guard_report(&pointers, what);
}

/* The dispatcher walks the stack for a handler with the frames' unwind data. Guest code has none, so a fault below
 * a guest frame never reaches the thread's own handler or the unhandled-exception filter: the process just ends. */
static int handler_reachable(const CONTEXT *start) {
    CONTEXT c = *start;
    for (int depth = 0; depth < 64; ++depth) {
        const uintptr_t rip = (uintptr_t)c.Rip;
        if (!rip) return 1;
        if (in_guest(rip)) return 0;
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(rip, &image_base, NULL);
        if (!function) {
            uint64_t return_address;
            if (depth || !read_word((uintptr_t)c.Rsp, &return_address)) return 0;
            c.Rip = return_address;
            c.Rsp += 8;
            continue;
        }
        PVOID handler_data = NULL;
        DWORD64 establisher = 0;
        if (RtlVirtualUnwind(UNW_FLAG_EHANDLER, image_base, rip, function, &c, &handler_data, &establisher, NULL) &&
            !port_owned(rip))
            return 1;
    }
    return 1;
}

static void write_first_chance(const EXCEPTION_POINTERS *info, int reachable) {
    const EXCEPTION_RECORD *record = info->ExceptionRecord;
    const CONTEXT *c = info->ContextRecord;
    char context[320];
    Text text;
    text_init(&text, chance_line, sizeof(chance_line));
    text_str(&text, "Diagnostics: first-chance ");
    put_fault(&text, record);
    text_str(&text, " in ");
    describe(&text, (uintptr_t)c->Rip);
    text_str(&text, " on thread ");
    text_dec(&text, GetCurrentThreadId());
    put_thread_label(&text, GetCurrentThreadId());
    text_str(&text, reachable ? ", UNCONFIRMED candidate (a handler can be reached)"
                              : ", UNCONFIRMED candidate (no handler can be reached from this stack)");
    if (record->NumberParameters >= 2 && (record->ExceptionCode == CODE_ACCESS_VIOLATION || record->ExceptionCode == CODE_IN_PAGE_ERROR)) {
        text_str(&text, "\n  fault address ");
        text_hex(&text, record->ExceptionInformation[1]);
        text_str(&text, ": ");
        describe_memory(&text, (uintptr_t)record->ExceptionInformation[1]);
    }
    text_str(&text, "\n  rip ");
    text_hex(&text, c->Rip);
    text_str(&text, ", rsp ");
    text_hex(&text, c->Rsp);
    text_str(&text, ", rbp ");
    text_hex(&text, c->Rbp);
    text_str(&text, "\n  ");
    aln_crash_context_describe(context, sizeof(context));
    text_str(&text, context);
    put_registers(&text, c);
    put_stack_code(&text, (uintptr_t)c->Rsp);
    if (hooks.memory_context && text.size - text.used > 1024) {
        const uintptr_t focus = record->NumberParameters >= 2 ? (uintptr_t)record->ExceptionInformation[1] : 0;
        text.used += hooks.memory_context(text.data + text.used, text.size - text.used, focus);
    }
    text_char(&text, '\n');
    write_text(&text);
}

LONG aln_crash_guard_vectored(EXCEPTION_POINTERS *info) {
    const uint32_t code = info->ExceptionRecord->ExceptionCode;
    const uintptr_t rip = (uintptr_t)info->ContextRecord->Rip;
    const int guest = in_guest(rip);
    const int ours = guest || port_owned(rip);
    if (aln_crash_classify(code, guest, ours) == ALN_FAULT_CRASH) {
        if (IsDebuggerPresent()) return EXCEPTION_CONTINUE_SEARCH;
        aln_crash_guard_report(info, NULL);
    }
    if (aln_crash_classify(code, 0, 1) == ALN_FAULT_IGNORE) return EXCEPTION_CONTINUE_SEARCH;
    const int reachable = handler_reachable(info->ContextRecord);
    if (InterlockedIncrement(&first_chance_lines) <= FIRST_CHANCE_LIMIT && !InterlockedExchange(&chance_busy, 1)) {
        write_first_chance(info, reachable);
        InterlockedExchange(&chance_busy, 0);
    }
    if (reachable || IsDebuggerPresent()) return EXCEPTION_CONTINUE_SEARCH;
    aln_crash_guard_report(info, "no handler can be reached from this stack: the unwinder stops at guest or unwind-less code");
}

static LONG WINAPI unhandled(EXCEPTION_POINTERS *info) {
    aln_crash_guard_report(info, "no handler took this exception");
}

int aln_crash_guard_verify(void) {
    const LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(unhandled);
    if (previous == unhandled) return 0;
    Text text;
    char where[300];
    text_init(&text, filter_line, sizeof(filter_line));
    text_str(&text, "Crash diagnostics: the unhandled-exception filter had been replaced");
    if (previous) {
        text_str(&text, " by ");
        if (hooks.describe && hooks.describe(where, sizeof(where), (uintptr_t)previous)) text_str(&text, where);
        else describe(&text, (uintptr_t)previous);
    }
    text_str(&text, "; restored\n");
    write_text(&text);
    return 1;
}

void aln_crash_guard_set_guest(uintptr_t image, uint64_t size) {
    guest_size = size;
    guest_image = image;
}

void aln_crash_guard_install(const AlnCrashHooks *installed) {
    if (installed) hooks = *installed;
    start_tick = GetTickCount64();
    const HMODULE exe = GetModuleHandleW(NULL);
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)exe + ((const IMAGE_DOS_HEADER *)exe)->e_lfanew);
    exe_base = (uintptr_t)exe;
    exe_end = exe_base + nt->OptionalHeader.SizeOfImage;
    char exe_path[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe_path, sizeof(exe_path))) {
        const char *name = strrchr(exe_path, '\\');
        snprintf(exe_name, sizeof(exe_name), "%s", name ? name + 1 : exe_path);
    }
    const HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    write_minidump = dbghelp ? (WriteDump)(void *)GetProcAddress(dbghelp, "MiniDumpWriteDump") : NULL;
    begin_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    detail_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    dump_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (begin_event && detail_done && dump_done)
        watchdog = CreateThread(NULL, 256 * 1024, watchdog_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    SetUnhandledExceptionFilter(unhandled);
    printf("Crash diagnostics: %s, minidump %s\n", watchdog ? "crash thread ready" : "no crash thread",
           write_minidump ? "next to this log" : "unavailable");
}
