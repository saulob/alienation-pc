#include "crash_guard.h"
#include "runtime_log.h"
#include "startup_phase.h"

#include <dbghelp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(condition) do { if (!(condition)) { printf("FAILED: %s (line %d)\n", #condition, __LINE__); ++failures; } } while (0)

static LONG CALLBACK production_vectored(EXCEPTION_POINTERS *info) { return aln_crash_guard_vectored(info); }

static LONG CALLBACK recovering_vectored(EXCEPTION_POINTERS *info) {
    return info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
                   !(info->ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE)
               ? EXCEPTION_CONTINUE_EXECUTION
               : EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI swallowing_filter(EXCEPTION_POINTERS *info) {
    (void)info;
    return EXCEPTION_CONTINUE_SEARCH;
}

static void faulting_detail(EXCEPTION_POINTERS *info) {
    (void)info;
    *(volatile int *)0x30 = 1;
}

static void plain_detail(EXCEPTION_POINTERS *info) {
    (void)info;
    runtime_log_write_direct("Detail: the detailed report ran\n");
}

static int describe_module(char *out, size_t size, uintptr_t address) {
    MEMORY_BASIC_INFORMATION info;
    char path[MAX_PATH];
    if (!VirtualQuery((LPCVOID)address, &info, sizeof(info)) || info.Type != MEM_IMAGE ||
        !GetModuleFileNameA((HMODULE)info.AllocationBase, path, sizeof(path)))
        return 0;
    const char *name = strrchr(path, '\\');
    snprintf(out, size, "%s+0x%llx", name ? name + 1 : path, (unsigned long long)(address - (uintptr_t)info.AllocationBase));
    return 1;
}

static size_t memory_context(char *out, size_t size, uintptr_t address) {
    return (size_t)snprintf(out, size, "\nRuntime: memory op 1: unmap 0x1000+0x1000%s", address ? " (focus)" : "");
}

static DWORD WINAPI read_worker(LPVOID unused) {
    (void)unused;
    aln_thread_label_set("test:worker");
    return (DWORD)*(volatile int *)0x40;
}

typedef void(NTAPI *MoveMemory)(void *, const void *, size_t);
typedef void(NTAPI *FillMemory)(void *, size_t, int);

static void foreign_read(uintptr_t address) {
    static volatile size_t length = 16;
    char buffer[16];
    ((MoveMemory)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlMoveMemory"))(buffer, (const void *)address, length);
}

static void foreign_write(uintptr_t address) {
    static volatile size_t length = 4096;
    ((FillMemory)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlFillMemory"))((void *)address, length, 0);
}

/* A frame with no unwind data between the fault and the thread start, as guest code is. */
static unsigned char *unwind_less_stub;

static void make_stub(void *target) {
    unwind_less_stub = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    static const unsigned char head[] = {0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0xb8};
    static const unsigned char tail[] = {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3};
    memcpy(unwind_less_stub, head, sizeof(head));
    memcpy(unwind_less_stub + sizeof(head), &target, sizeof(target));
    memcpy(unwind_less_stub + sizeof(head) + sizeof(target), tail, sizeof(tail));
}

static DWORD WINAPI stub_worker(LPVOID unused) {
    (void)unused;
    aln_thread_label_set("test:worker");
    ((void (*)(void *, const void *, size_t))(void *)unwind_less_stub)(NULL, (const void *)0x90, 16);
    return 0;
}

static DWORD WINAPI stub_write_worker(LPVOID unused) {
    (void)unused;
    aln_thread_label_set("test:worker");
    ((void (*)(void *, size_t, int))(void *)unwind_less_stub)((void *)0x0, 4096, 0);
    return 0;
}

static volatile int stop_recursion;

static int recurse(int depth) {
    volatile char pad[1024];
    if (stop_recursion) return 0;
    pad[0] = (char)depth;
    return recurse(depth + 1) + pad[0];
}

static void on_abort(int signal_number) {
    (void)signal_number;
    aln_crash_guard_report_here("abort() was called", 0x40000015u, 3);
}

static void run_thread(LPTHREAD_START_ROUTINE routine) {
    HANDLE thread = CreateThread(NULL, 0, routine, NULL, 0, NULL);
    WaitForSingleObject(thread, INFINITE);
}

static int run_child(const char *mode, const char *directory) {
    char path[4200], log[4200];
    const int production_name = !strcmp(mode, "production_name");
    snprintf(log, sizeof(log), "%s\\crash_guard_test.log", directory);
    if (!runtime_log_start(directory, production_name ? NULL : log, path, sizeof(path))) return 90;
    AlnCrashHooks hooks = {plain_detail, describe_module, memory_context};
    if (!strcmp(mode, "detail_fault")) hooks.detail = faulting_detail;
    aln_crash_guard_install(&hooks);
    AddVectoredExceptionHandler(1, production_vectored);
    aln_startup_phase("avplayer: intro closed");
    aln_crash_context_set_video("/app0/video/intro.mp4");
    aln_crash_context_set_display(3360, 1440, "1440p", "21:9");
    aln_checkpoint_note("AvPlayer closed");
    aln_thread_label_set("test:main");
    signal(SIGABRT, on_abort);

    if (!strcmp(mode, "read") || !strcmp(mode, "detail_fault") || production_name) return *(volatile int *)0x10;
    if (!strcmp(mode, "write")) { *(volatile int *)0x20 = 7; return 0; }
    if (!strcmp(mode, "worker")) { run_thread(read_worker); return 0; }
    if (!strcmp(mode, "foreign_read")) { foreign_read(0x50); return 0; }
    if (!strcmp(mode, "foreign_write")) { foreign_write(0x0); return 0; }
    if (!strcmp(mode, "recovered")) {
        AddVectoredExceptionHandler(0, recovering_vectored);
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
        puts("Test: execution continued after a recoverable fault");
        return 0;
    }
    if (!strcmp(mode, "unwind_less") || !strcmp(mode, "unwind_less_write")) {
        const int write = !strcmp(mode, "unwind_less_write");
        make_stub(GetProcAddress(GetModuleHandleA("ntdll.dll"), write ? "RtlFillMemory" : "RtlMoveMemory"));
        run_thread(write ? stub_write_worker : stub_worker);
        return 0;
    }
    if (!strcmp(mode, "guest_frame")) {
        make_stub(GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlFillMemory"));
        aln_crash_guard_set_guest((uintptr_t)unwind_less_stub, 4096);
        run_thread(stub_write_worker);
        return 0;
    }
    if (!strcmp(mode, "replaced")) {
        SetUnhandledExceptionFilter(swallowing_filter);
        CHECK(aln_crash_guard_verify() == 1);
        CHECK(aln_crash_guard_verify() == 0);
        if (failures) return 91;
        foreign_read(0x60);
        return 0;
    }
    if (!strcmp(mode, "replaced_silently")) {
        SetUnhandledExceptionFilter(swallowing_filter);
        Sleep(2500);
        foreign_read(0x70);
        return 0;
    }
    if (!strcmp(mode, "stack")) return recurse(0);
    if (!strcmp(mode, "abort")) { abort(); }
    return 92;
}

typedef struct {
    const char *mode;
    DWORD exit_code;
    int terminal;
    const char *const *expect;
    const char *const *reject;
} Case;

static char directory[MAX_PATH];

static void remove_files(void) {
    char pattern[MAX_PATH + 4];
    snprintf(pattern, sizeof(pattern), "%s\\*", directory);
    WIN32_FIND_DATAA found;
    HANDLE find = FindFirstFileA(pattern, &found);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        char file[MAX_PATH + 300];
        snprintf(file, sizeof(file), "%s\\%s", directory, found.cFileName);
        DeleteFileA(file);
    } while (FindNextFileA(find, &found));
    FindClose(find);
}

static char *read_file(const char *name, DWORD *size) {
    char path[MAX_PATH + 300];
    snprintf(path, sizeof(path), "%s\\%s", directory, name);
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    *size = GetFileSize(file, NULL);
    char *data = calloc(*size + 1, 1);
    DWORD read = 0;
    if (data) ReadFile(file, data, *size, &read, NULL);
    CloseHandle(file);
    return data;
}

static int count_of(const char *text, const char *needle) {
    int count = 0;
    for (const char *at = text; (at = strstr(at, needle)); at += strlen(needle)) ++count;
    return count;
}

typedef BOOL(WINAPI *ReadStream)(PVOID, MINIDUMP_STREAM_TYPE, PMINIDUMP_DIRECTORY *, PVOID *, ULONG *);

static void check_dump(const char *name, DWORD pid, DWORD exit_code, const char *log, const char *own_exe) {
    char path[MAX_PATH + 300];
    snprintf(path, sizeof(path), "%s\\%s", directory, name);
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);
    if (file == INVALID_HANDLE_VALUE) return;
    const DWORD size = GetFileSize(file, NULL);
    CHECK(size > 1024 && size < 256u * 1024 * 1024);
    HANDLE mapping = CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);
    void *view = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : NULL;
    ReadStream read_stream = (ReadStream)(void *)GetProcAddress(LoadLibraryA("dbghelp.dll"), "MiniDumpReadDumpStream");
    if (view && read_stream) {
        PMINIDUMP_DIRECTORY directory_entry;
        PVOID stream;
        ULONG stream_size;
        CHECK(read_stream(view, ExceptionStream, &directory_entry, &stream, &stream_size));
        const MINIDUMP_EXCEPTION_STREAM *exception = stream;
        CHECK(exception->ExceptionRecord.ExceptionCode == (exit_code == 3 ? 0x40000015u : exit_code));
        const char *thread_text = strstr(log, "; thread ");
        CHECK(thread_text && exception->ThreadId == (DWORD)strtoul(thread_text + 9, NULL, 10));
        CHECK(read_stream(view, CommentStreamA, &directory_entry, &stream, &stream_size));
        const char *comment = stream;
        char expected[64];
        snprintf(expected, sizeof(expected), "process %lu,", pid);
        CHECK(strstr(comment, expected) != NULL);
        CHECK(strstr(comment, own_exe) != NULL);
        CHECK(strstr(comment, "crash_guard_test") != NULL || strstr(comment, "alienation_001.log") != NULL);
    } else {
        CHECK(0);
    }
    if (view) UnmapViewOfFile(view);
    if (mapping) CloseHandle(mapping);
    CloseHandle(file);
}

static void run_case(const char *self, const Case *test) {
    remove_files();
    char command[2 * MAX_PATH + 64];
    snprintf(command, sizeof(command), "\"%s\" --child %s \"%s\"", self, test->mode, directory);
    STARTUPINFOA startup;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process;
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
        printf("FAILED: %s: cannot start the child\n", test->mode);
        ++failures;
        return;
    }
    if (WaitForSingleObject(process.hProcess, 90000) != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        printf("FAILED: %s: the child did not end\n", test->mode);
        ++failures;
    }
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    const DWORD pid = process.dwProcessId;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    const int production_name = !strcmp(test->mode, "production_name");
    const char *log_name = production_name ? "alienation_001.log" : "crash_guard_test.log";
    const char *dump_name = production_name ? "alienation_001.dmp" : "crash_guard_test.dmp";
    DWORD size = 0;
    char *log = read_file(log_name, &size);
    if (!log) {
        printf("FAILED: %s: no log\n", test->mode);
        ++failures;
        return;
    }
    const int before = failures;
    if (code != test->exit_code) printf("exit code of %s: %lx\n", test->mode, code);
    CHECK(code == test->exit_code);
    if (!test->terminal) {
        CHECK(count_of(log, "CRASH: ") == 0);
    } else {
        CHECK(count_of(log, "CRASH: the process ends with exit code") == 1);
        CHECK(strstr(log, "phase=avplayer: intro closed") != NULL);
        CHECK(strstr(log, "render 3360x1440 (1440p) 21:9") != NULL);
        CHECK(strstr(log, "video=/app0/video/intro.mp4") != NULL);
        CHECK(strstr(log, "last checkpoint=[thread ") != NULL);
        CHECK(strstr(log, "AvPlayer closed") != NULL);
        CHECK(strstr(log, "(test:") != NULL);
        CHECK(strstr(log, "registers: rax=0x") != NULL);
        CHECK(strstr(log, "code addresses on the stack:") != NULL);
    }
    for (const char *const *text = test->expect; text && *text; ++text)
        if (!strstr(log, *text)) { printf("FAILED: %s: log lacks \"%s\"\n", test->mode, *text); ++failures; }
    for (const char *const *text = test->reject; text && *text; ++text)
        if (strstr(log, *text)) { printf("FAILED: %s: log has \"%s\"\n", test->mode, *text); ++failures; }

    DWORD dump_size = 0;
    char *dump = read_file(dump_name, &dump_size);
    CHECK((dump != NULL) == (test->terminal != 0));
    free(dump);
    if (test->terminal && dump_size) {
        char own[MAX_PATH];
        const char *base = strrchr(self, '\\');
        snprintf(own, sizeof(own), "%s", base ? base + 1 : self);
        check_dump(dump_name, pid, test->exit_code, log, own);
    }
    if (failures != before) printf("---- %s log ----\n%s\n", test->mode, log);
    free(log);
}

static void classification(void) {
    CHECK(aln_crash_classify(0xC0000005u, 1, 0) == ALN_FAULT_CRASH);
    CHECK(aln_crash_classify(0xC0000005u, 0, 1) == ALN_FAULT_CRASH);
    CHECK(aln_crash_classify(0xC0000005u, 0, 0) == ALN_FAULT_IGNORE);
    CHECK(aln_crash_classify(0xC00000FDu, 0, 0) == ALN_FAULT_CRASH);
    CHECK(aln_crash_classify(0x80000003u, 1, 1) == ALN_FAULT_IGNORE);
    CHECK(aln_crash_classify(0xE06D7363u, 1, 1) == ALN_FAULT_IGNORE);
    char path[100];
    CHECK(aln_crash_dump_path(path, sizeof(path), "C:\\logs\\alienation_017.log", 1) && !strcmp(path, "C:\\logs\\alienation_017.dmp"));
    CHECK(aln_crash_dump_path(path, sizeof(path), "C:\\logs\\alienation_017.log", 3) && !strcmp(path, "C:\\logs\\alienation_017_3.dmp"));
    CHECK(!aln_crash_dump_path(path, 8, "C:\\logs\\alienation_017.log", 1));
    CHECK(!aln_crash_dump_path(path, sizeof(path), "C:\\logs.d\\noextension", 1));
    CHECK(!strcmp(aln_crash_code_name(0xC0000005u), "access violation"));
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--child")) return run_child(argv[2], argv[3]);
    char temp[MAX_PATH];
    GetTempPathA(sizeof(temp), temp);
    snprintf(directory, sizeof(directory), "%saln-crash-guard-tests-%lu", temp, GetCurrentProcessId());
    CreateDirectoryA(directory, NULL);
    classification();

    static const char *const read_text[] = {"CRASH: access violation (0xc0000005) read at 0x10", "Detail: the detailed report ran",
                                            "minidump: crash_guard_test.dmp", NULL};
    static const char *const write_text[] = {"write at 0x20", NULL};
    static const char *const worker_text[] = {"read at 0x40", NULL};
    static const char *const foreign_text[] = {"first-chance access violation", "UNCONFIRMED candidate (a handler can be reached)",
                                               "no handler took this exception", "read at 0x50",
                                               "ntdll.dll+", "Runtime: memory op 1", NULL};
    static const char *const foreign_write_text[] = {"write at 0x0", "first-chance access violation", "no handler took this exception", NULL};
    static const char *const recovered_text[] = {"UNCONFIRMED candidate (a handler can be reached)",
                                                 "Test: execution continued after a recoverable fault", NULL};
    static const char *const unwind_text[] = {"UNCONFIRMED candidate (no handler can be reached from this stack)",
                                              "no handler can be reached from this stack", "read at 0x90", NULL};
    static const char *const unwind_write_text[] = {"write at 0x0", "no handler can be reached from this stack", NULL};
    static const char *const guest_text[] = {"no handler can be reached from this stack", "guest offset 0x", NULL};
    static const char *const replaced_text[] = {"the unhandled-exception filter had been replaced", "read at 0x60", NULL};
    static const char *const silent_text[] = {"the unhandled-exception filter had been replaced", "read at 0x70", NULL};
    static const char *const second_text[] = {"a second fault on the crashing thread", "read at 0x10", NULL};
    static const char *const production_text[] = {"minidump: alienation_001.dmp", NULL};
    static const char *const no_unreachable[] = {"no handler can be reached", NULL};

    static const Case cases[] = {
        {"read", 0xC0000005u, 1, read_text, NULL},
        {"write", 0xC0000005u, 1, write_text, NULL},
        {"worker", 0xC0000005u, 1, worker_text, NULL},
        {"foreign_read", 0xC0000005u, 1, foreign_text, no_unreachable},
        {"foreign_write", 0xC0000005u, 1, foreign_write_text, no_unreachable},
        {"recovered", 0, 0, recovered_text, NULL},
        {"unwind_less", 0xC0000005u, 1, unwind_text, NULL},
        {"unwind_less_write", 0xC0000005u, 1, unwind_write_text, NULL},
        {"guest_frame", 0xC0000005u, 1, guest_text, NULL},
        {"replaced", 0xC0000005u, 1, replaced_text, NULL},
        {"replaced_silently", 0xC0000005u, 1, silent_text, NULL},
        {"stack", 0xC00000FDu, 1, NULL, NULL},
        {"abort", 3, 1, NULL, NULL},
        {"detail_fault", 0xC0000005u, 1, second_text, NULL},
        {"production_name", 0xC0000005u, 1, production_text, NULL},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) run_case(argv[0], &cases[i]);

    remove_files();
    RemoveDirectoryA(directory);
    if (!failures) puts("crash guard tests passed");
    return failures ? 1 : 0;
}
