/* Native x86-64 loader: maps the prepared PS4 image and runs the original code on the host CPU. */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include "aln_sdl_compat.h"
#include "runtime.h"
#include "trophies/trophy_runtime.h"
#include "alngpu.h"
#include "aln_launcher.h"
#include "startup_phase.h"
#include "boot_diagnostic.h"
#include "aln_frame_rate.h"
#include "aln_menu_buttons.h"
#include "aln_resolution.h"
#include "skip_intro.h"
#include "exit_game.h"
#include "exit_game_stream.h"
#include "text_fix.h"
#include "controller_prompts.h"
#include "runtime_log.h"
#include "aln_image_format.h"
#include "isa_emulate.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error Alienation PC requires x86-64 GCC or Clang (including MinGW).
#endif
#ifdef _WIN32
#include <windows.h>
#include <signal.h>
#include "crash_guard.h"
#include "module_diagnostics.h"
#else
#include <sys/mman.h>
#include <malloc.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static const AlnResolution *render_resolution=&aln_resolutions[0];
static const AlnAspectRatio *aspect_ratio=&aln_aspect_ratios[0];
static AlnResolution render_target;
static size_t page_size;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;

static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }
/* For failures after the GPU threads started: exit() would run destructors under them. */
static void fail_running(const char *message) { fprintf(stderr, "ERROR: %s\n", message); runtime_log_flush(); _exit(1); }
static void read_content_profile(const char *path, uint32_t values[ALN_CONTENT_VALUES]) {
    char message[1200];
    unsigned char data[ALN_CONTENT_PROFILE_SIZE+1];
    FILE *profile=fopen(path,"rb");
    if (!profile) {
        snprintf(message,sizeof(message),"content profile not found or unreadable: %s; install the game again from the launcher",path);
        fail(message);
    }
    const size_t size=fread(data,1,sizeof(data),profile);
    fclose(profile);
    switch (aln_content_profile_parse(data,size,values)) {
    case ALN_CONTENT_OK: return;
    case ALN_CONTENT_BAD_SIZE:
        snprintf(message,sizeof(message),"content profile %s has %s%zu bytes, expected %d; rebuild the boot image",path,
                 size>ALN_CONTENT_PROFILE_SIZE ? "more than " : "",size>ALN_CONTENT_PROFILE_SIZE ? (size_t)ALN_CONTENT_PROFILE_SIZE : size,
                 ALN_CONTENT_PROFILE_SIZE);
        break;
    case ALN_CONTENT_BAD_SKU:
        snprintf(message,sizeof(message),"content profile %s has SKU %u, expected %u (full) or %u (trial); rebuild the boot image",
                 path,values[0],ALN_CONTENT_SKU_FULL,ALN_CONTENT_SKU_TRIAL);
        break;
    default:
        snprintf(message,sizeof(message),"content profile %s has signature %.8s, expected %s; rebuild the boot image",path,
                 (const char *)data,ALN_CONTENT_MAGIC);
    }
    fail(message);
}
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
static void *allocate(size_t size) {
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) return low;
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("VirtualAlloc failed");
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fail("mmap failed");
#endif
    return p;
}
static void protect(void *p, size_t size, unsigned flags) {
#ifdef _WIN32
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
#else
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p, size, mode)) fail("mprotect failed");
#endif
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", runtime_import_name(names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           (uintptr_t)__builtin_return_address(0) - (uintptr_t)image, argument);
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-(uintptr_t)image;
    for (uint64_t m=0;m<module_count;++m)
        if (caller>=modules[m].base && caller-modules[m].base<modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,m==0 ? "libc.prx" : "system module",caller-modules[m].base);
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    runtime_log_flush();
    _exit(20); /* no destructors: GPU, audio and guest threads are still running */
}
#ifndef _WIN32
/* enter_on_stack(entry, arg0, arg1, stack_top): call entry(arg0,arg1) on a new stack. */
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(".text\n.globl enter_on_stack\nenter_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
#endif
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
static void log_process_exit(void) { printf("Process: exit() reached in phase '%s'\n",aln_startup_phase_current()); }
static void log_cpu(void) {
    char name[64], sets[96];
    const IsaHostFeatures host=isa_host_features();
    isa_host_name(name,sizeof(name));
    printf("CPU: %s\n",name);
    if (isa_missing_sets(host,1,sets,sizeof(sets))) printf("CPU: emulating %s\n",sets);
    if (isa_missing_sets(host,0,sets,sizeof(sets))) printf("CPU: missing %s, which cannot be emulated\n",sets);
}
/* A AlnGraphicsCheck for the processor, which runs the game's PS4 (Jaguar) code directly. */
static int cpu_usable(char *cause, size_t cause_size, char *advice, size_t advice_size) {
    char missing[96];
    if (!isa_missing_sets(isa_host_features(),0,missing,sizeof(missing))) return 1;
    snprintf(cause,cause_size,"This processor lacks %s, which the game's PS4 code needs.",missing);
    snprintf(advice,advice_size,"The game needs an Intel Core 4th generation (Haswell), AMD FX or newer processor.");
    return 0;
}
#ifndef _WIN32
static const char *renderer_settings_file;
static int system_usable(char *cause, size_t cause_size, char *advice, size_t advice_size) {
    return cpu_usable(cause,cause_size,advice,advice_size) && aln_check_renderer(renderer_settings_file,cause,cause_size,advice,advice_size);
}
#endif
#ifdef _WIN32
static int annotate_address(char *out, size_t size, uintptr_t address) {
    if (image && address-(uintptr_t)image<0x20000000) { snprintf(out,size,"guest offset 0x%" PRIxPTR,address-(uintptr_t)image); return 1; }
    return module_diagnostics_describe(out,size,address);
}
static void describe_address(char *out, size_t size, uintptr_t address) {
    if (!annotate_address(out,size,address)) snprintf(out,size,"%p",(void *)address);
}
static void crash_detail(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    const CONTEXT *c=info->ContextRecord;
    char where[MAX_PATH+64];
    describe_address(where,sizeof(where),(uintptr_t)c->Rip);
    if (record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2)
        fprintf(stderr,"Fault: %s access to %p at %s (thread %lu)\n",
                record->ExceptionInformation[0]==1 ? "write" : record->ExceptionInformation[0]==8 ? "execute" : "read",
                (void *)record->ExceptionInformation[1],where,GetCurrentThreadId());
    else
        fprintf(stderr,"Fault: exception 0x%08lx at %s (thread %lu)\n",record->ExceptionCode,where,GetCurrentThreadId());
    module_diagnostics_report_crash((uintptr_t)c->Rip,
        record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2 ? (uintptr_t)record->ExceptionInformation[1] : (uintptr_t)c->Rip,
        (uintptr_t)image,0x20000000);
    module_diagnostics_report_thread_name();
    module_diagnostics_report_backtrace(c,(uintptr_t)image,0x20000000);
    fprintf(stderr,"  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n",
            c->Rax,c->Rbx,c->Rcx,c->Rdx,c->Rsi,c->Rdi,c->Rbp,c->Rsp);
    {
        static char operations[3072];
        const int fault_address=record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2;
        const size_t length=runtime_memory_recent(operations,sizeof(operations),fault_address ? (uintptr_t)record->ExceptionInformation[1] : 0);
        if (length) fputs(operations,stderr);
    }
    if (gpu_enabled) alngpu_dump_guest_writes(info);
    /* Top stack words: after a call through a null pointer [rsp] is the caller's return address. */
    uintptr_t words[8];
    SIZE_T read=0;
    if (ReadProcessMemory(GetCurrentProcess(),(void *)c->Rsp,words,sizeof(words),&read) && read==sizeof(words))
        for (int i=0;i<8;++i) {
            if (annotate_address(where,sizeof(where),words[i])) fprintf(stderr,"  [rsp+%02x] %016llx %s\n",i*8,(unsigned long long)words[i],where);
            else fprintf(stderr,"  [rsp+%02x] %016llx\n",i*8,(unsigned long long)words[i]);
        }
    /* rbp chain (guest and host code keep frame pointers). */
    uintptr_t rbp=(uintptr_t)c->Rbp;
    for (int depth=0; depth<24 && rbp; ++depth) {
        uintptr_t frame[2];
        SIZE_T got=0;
        if (!ReadProcessMemory(GetCurrentProcess(),(void *)rbp,frame,sizeof(frame),&got) || got!=sizeof(frame) || frame[0]<=rbp) break;
        describe_address(where,sizeof(where),frame[1]);
        fprintf(stderr,"  #%d %s\n",depth,where);
        rbp=frame[0];
    }
    runtime_log_flush();
}
static void abort_signal(int sig) {
    (void)sig;
    aln_crash_guard_report_here("abort() was called: an uncaught C++ exception, a pure virtual call or a failed library check",
                                0x40000015u,3);
}
static void purecall(void) { aln_crash_guard_report_here("pure virtual call",0x40000015u,3); }
/* MinGW makes a failed C runtime parameter check non-fatal (the call fails with EINVAL); the first ones are logged. */
static void invalid_parameter(const wchar_t *e, const wchar_t *f, const wchar_t *fl, unsigned line, uintptr_t r) {
    static volatile LONG count;
    (void)e; (void)f; (void)fl; (void)line; (void)r;
    if (InterlockedIncrement(&count)>8) return;
    void *frames[4];
    char text[512], where[160];
    int length=snprintf(text,sizeof(text),"Diagnostics: an invalid C runtime parameter (non-fatal), called from:");
    const USHORT n=RtlCaptureStackBackTrace(1,4,frames,NULL);
    for (USHORT i=0;i<n && length>0 && (size_t)length<sizeof(text)-170;++i) {
        describe_address(where,sizeof(where),(uintptr_t)frames[i]);
        length+=snprintf(text+length,sizeof(text)-(size_t)length,"%s%s",i ? ", " : " ",where);
    }
    snprintf(text+length,sizeof(text)-(size_t)length,"\n");
    runtime_log_write_direct(text);
}
static LONG CALLBACK vectored_fault(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    DWORD code=record->ExceptionCode;
    if (code==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2) {
        /* GPU page tracking (write-protected guest pages) is resolved first. */
        if (gpu_enabled && alngpu_handle_fault(info,(void *)record->ExceptionInformation[1])) return EXCEPTION_CONTINUE_EXECUTION;
        if (runtime_fault_recover) {
            /* A speculative guest memory read (GPU draw preparation) faulted: the thread resumes
             * in aln_longjmp, outside the exception dispatcher, back at its recovery point. */
            unsigned long long *buffer=*runtime_fault_recover;
            runtime_fault_recover=NULL;
            CONTEXT *c=info->ContextRecord;
            c->Rsp=((c->Rsp-128)&~(DWORD64)15)-40;
            c->Rcx=(DWORD64)(uintptr_t)buffer;
            c->Rdx=1;
            c->Rip=(DWORD64)(uintptr_t)aln_longjmp;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code==EXCEPTION_ILLEGAL_INSTRUCTION && image && (uintptr_t)info->ContextRecord->Rip-(uintptr_t)image<0x20000000) {
        /* Jaguar's MOVBE and BMI1 trap on hosts without them; LZCNT does not, so it is rewritten at load. */
        CONTEXT *c=info->ContextRecord;
        _Static_assert(offsetof(CONTEXT,R15)-offsetof(CONTEXT,Rax)==15*sizeof(DWORD64),"CONTEXT register order");
        uint64_t flags=c->EFlags;
        size_t length=isa_emulate((const uint8_t *)(uintptr_t)c->Rip,c->Rip,&c->Rax,&flags);
        if (length) {
            c->EFlags=(DWORD)flags;
            c->Rip+=length;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return aln_crash_guard_vectored(info);
}
/* Guest code reads its TCB with `mov rax, gs:[0]` (the boot image builder rewrote fs:[0]); on Windows
 * the displacement becomes the TEB TLS slot that holds the guest TCB (runtime_thread.c). */
uint32_t runtime_thread_tcb_offset(void);
static uint64_t patch_tcb_loads(const Segment *segments, uint64_t count) {
    static const unsigned char load[]={0x65,0x48,0x8b,0x04,0x25,0,0,0,0};
    uint32_t displacement=runtime_thread_tcb_offset();
    uint64_t patched=0;
    for (uint64_t s=0;s<count;++s) {
        if (!(segments[s].flags&1) || segments[s].size<sizeof(load)) continue;
        unsigned char *at=image+segments[s].address, *end=at+segments[s].size-sizeof(load);
        for (; at<=end; ++at)
            if (at[0]==0x65 && !memcmp(at,load,sizeof(load))) { memcpy(at+5,&displacement,4); ++patched; at+=sizeof(load)-1; }
    }
    return patched;
}
#endif
#ifndef _WIN32
static void fault(int sig, siginfo_t *info, void *context) {
    /* GPU page tracking (write-protected guest pages) is resolved first. */
    if (gpu_enabled && sig == SIGSEGV && alngpu_handle_fault(context, info->si_addr)) return;
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    if (sig == SIGILL) {
        static const int order[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                                      REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};
        ucontext_t *state = context;
        uint64_t gpr[16];
        for (int i = 0; i < 16; ++i) gpr[i] = (uint64_t)state->uc_mcontext.gregs[order[i]];
        uint64_t at = (uint64_t)state->uc_mcontext.gregs[REG_RIP], flags = (uint64_t)state->uc_mcontext.gregs[REG_EFL];
        size_t length = at - (uintptr_t)image < 0x20000000 ? isa_emulate((const uint8_t *)(uintptr_t)at, at, gpr, &flags) : 0;
        if (length) {
            for (int i = 0; i < 16; ++i) state->uc_mcontext.gregs[order[i]] = (greg_t)gpr[i];
            state->uc_mcontext.gregs[REG_EFL] = (greg_t)flags;
            state->uc_mcontext.gregs[REG_RIP] = (greg_t)(at + length);
            return;
        }
    }
    runtime_log_mark_crash();
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    ucontext_t *uc = context;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    char line[512];
    Dl_info where;
    snprintf(line, sizeof(line), "Crash: startup phase: %s\n", aln_startup_phase_current());
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    if (rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%lx, address %p\n",
                 sig, (unsigned long)(rip - (uintptr_t)image), info->si_addr);
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(line, sizeof(line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        snprintf(line, sizeof(line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, info->si_addr);
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    if (gpu_enabled) alngpu_dump_guest_writes(context);
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            snprintf(line, sizeof(line), "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(line, sizeof(line), "  #%d %p\n", i, frames[i]);
        ssize_t written_=write(2, line, strlen(line)); (void)written_;
    }
    runtime_log_flush();
    _exit(128 + sig);
}
#endif
#ifndef _WIN32
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets
 * when inside the image). Reads use process_vm_readv so bad frames cannot fault. */
static uintptr_t exe_base;
static void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *uc) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip=(uintptr_t)uc->uc_mcontext.gregs[REG_RIP], rbp=(uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
    uint64_t tid=(uint64_t)gettid();
    /* First argument register: the lock address when a thread waits on a futex. */
    char arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg+6,tid); write_hex(arg+27,(uint64_t)uc->uc_mcontext.gregs[REG_RDI]);
    { ssize_t written_=write(2,arg,sizeof(arg)-1); (void)written_; }
    for (int depth=0; depth<24; ++depth) {
        write_hex(line+6,tid); write_hex(line+27,rip); write_hex(line+59,rip-(uintptr_t)image); write_hex(line+90,rip-exe_base);
        { ssize_t written_=write(2,line,sizeof(line)-1); (void)written_; }
        uintptr_t frame[2];
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
}
static void thread_dump(int sig, siginfo_t *info, void *context) { (void)sig; (void)info; dump_frames(context); }
static void watchdog(int sig, siginfo_t *info, void *context) {
    (void)info;
    runtime_log_mark_crash();
    const char head[]="STOP: watchdog timeout; thread stacks:\n";
    { ssize_t written_=write(2,head,sizeof(head)-1); (void)written_; }
    dump_frames(context);
    int dir=open("/proc/self/task",O_RDONLY|O_DIRECTORY);
    char buffer[4096];
    long n;
    pid_t self=gettid();
    while (dir>=0 && (n=syscall(SYS_getdents64,dir,buffer,sizeof(buffer)))>0)
        for (long at=0; at<n;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *d=(void *)(buffer+at);
            pid_t tid=(pid_t)strtol(d->name,NULL,10);
            if (tid>0 && tid!=self) { syscall(SYS_tgkill,getpid(),tid,SIGUSR2); usleep(20000); }
            at+=d->reclen;
        }
    usleep(100000);
    runtime_log_flush();
    _exit(128 + sig);
}
#endif
/* param.sfo lookup: string or integer value of key, 0 when absent. */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
/* The Options row and its hooks go in together: the bytecode is patched only when the hooked code is the known one. */
static void install_exit_game(const char *app0, const char *build_key, uint8_t *code, uint64_t size, uint8_t *stubs,
                              size_t stubs_size) {
    const ExitGameSite *site = exit_game_site(build_key);
    const char *why = "";
    char detail[160] = "";
    if (!exit_game_verify(code, size, site, &why)) {
        printf("Exit Game: unavailable: %s\n", why);
        return;
    }
    if (aln_exit_game_stream_install(app0, build_key, detail, sizeof(detail)) != 0) {
        printf("Exit Game: unavailable: %s\n", detail);
        return;
    }
    exit_game_set_handler(alngpu_request_close);
    if (exit_game_install(code, size, site, stubs, stubs_size)) printf("Exit Game: %s\n", detail);
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}
/* Title, versions and executable fingerprint, so a log identifies the exact build it ran. */
static void log_game_identity(const char *app0, const AlnGameBuild *build) {
    char sfo[4096], title_id[16]="?", app_ver[16]="?", version[16]="?", category[8]="?";
    uint32_t system_ver=0;
    snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
    sfo_value(sfo,"TITLE_ID",title_id,sizeof(title_id),NULL);
    sfo_value(sfo,"APP_VER",app_ver,sizeof(app_ver),NULL);
    sfo_value(sfo,"VERSION",version,sizeof(version),NULL);
    sfo_value(sfo,"CATEGORY",category,sizeof(category),NULL);
    sfo_value(sfo,"SYSTEM_VER",NULL,0,&system_ver);
    printf("Game: %s APP_VER %s VERSION %s CATEGORY %s SYSTEM_VER 0x%08x\n",title_id,app_ver,version,category,(unsigned)system_ver);
    if (build) printf("Game: build %s, eboot sha256 %s\n",build->label,build->eboot_sha256);
    else puts("Game: not a supported build");
}
static const char *command_line(int argc, char **argv) {
#ifdef _WIN32
    (void)argc; (void)argv;
    return GetCommandLineA();
#else
    static char line[4096];
    size_t used=0;
    for (int i=0;i<argc && used<sizeof(line)-1;++i)
        used+=(size_t)snprintf(line+used,sizeof(line)-used,"%s%s",i ? " " : "",argv[i]);
    return line;
#endif
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    /* tools\build.ps1: the launcher's boot image builder on a prepared game folder, without starting anything. */
    if (argc == 4 && !strcmp(argv[1], "--prepare-image")) {
        char error[1024];
        if (aln_build_boot_image(argv[2], argv[3], error, sizeof(error))) {
            fprintf(stderr, "Boot image failed: %s\n", error);
            return 1;
        }
        printf("Boot image written to %s\n", argv[3]);
        return 0;
    }
#ifndef _WIN32
    /* Keep host heap objects handed to the guest (thread handles, TLS) in the
       non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits. */
    mallopt(M_ARENA_MAX,1);
    mallopt(M_MMAP_THRESHOLD,32*1024*1024);
#endif
    /* The PS4 user range must stay free of host allocations (GPU driver, heaps). */
    runtime_memory_reserve();
#ifdef _WIN32
    /* The guest TCB needs one of the 64 TEB TLS slots before DLLs (Vulkan drivers) take them. */
    runtime_thread_tcb_offset();
    module_diagnostics_baseline();
#endif
    int cpu_only = 0, strict_imports = 0, check_imports = 0;
    unsigned timeout_seconds = 0;
    int skip_launcher = 0, force_launcher = 0, game_child = 0;
    const char *boot=NULL, *content_profile=NULL, *app0=NULL, *user_dir=NULL, *logs_dir=NULL, *log_file=NULL;
    int first_option = 1;
    if (argc > 1 && strncmp(argv[1], "--", 2)) { boot = argv[1]; first_option = 2; }
    for (int i = first_option; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--check-imports")) check_imports = 1;
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--logs") && i+1<argc) logs_dir=argv[++i];
        else if (!strcmp(argv[i], "--log") && i+1<argc) log_file=argv[++i];
        else if (!strcmp(argv[i], "--aln-skip-launcher")) skip_launcher = 1;
        else if (!strcmp(argv[i], "--launcher")) force_launcher = 1;
        else if (!strcmp(argv[i], "--aln-game")) game_child = 1;
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else {
            fprintf(stderr, "Unknown option: %s\nUsage: %s [boot.bin] [--cpu-only] [--strict-imports] [--check-imports] [--content-profile file] [--app0 dir] [--user dir] [--logs dir] [--log file] [--aln-skip-launcher] [--launcher] [--timeout seconds]\n", argv[i], argv[0]);
            return 1;
        }
    }
    static AlnPaths paths;
    const AlnPathRequest request={boot,content_profile,app0,user_dir,logs_dir};
    if (!aln_resolve_paths(&request,&paths)) fail(paths.error[0] ? paths.error : "cannot locate the game folders");
    boot=paths.boot; content_profile=paths.content_profile; app0=paths.app0; user_dir=paths.user_dir;
    static char settings_file[4200];
    snprintf(settings_file,sizeof(settings_file),"%s/alienation.ini",user_dir);
    char log_path[4200];
    static char log_folder[4200];
    snprintf(log_folder,sizeof(log_folder),"%s",paths.logs_dir);
    int log_started=0;
#ifdef _WIN32
    /* An existing --log file is continued: the game child's, or the session tools\build.ps1 -Run started. */
    if (log_file) log_started=runtime_log_continue(log_file,log_path,sizeof(log_path));
#endif
    if (!log_started) log_started=runtime_log_start(paths.logs_dir,log_file,log_path,sizeof(log_path));
    if (log_started) {
        time_t now=time(NULL);
        char stamp[32];
        strftime(stamp,sizeof(stamp),"%Y-%m-%dT%H:%M:%S",localtime(&now));
        printf("[run] %s\n[run] %s\n[run] log %s\n[run] prepared root %s\n",stamp,command_line(argc,argv),log_path,app0);
        atexit(runtime_log_flush);
        atexit(log_process_exit);
        runtime_log_set_detailed(aln_detailed_logging(settings_file));
        runtime_log_install_cleanup();
        if (!aln_logs_directory(paths.logs_dir,log_path,log_folder,sizeof(log_folder)))
            snprintf(log_folder,sizeof(log_folder),"%s",paths.logs_dir);
    } else fprintf(stderr,"Cannot create the log %s\n",log_file ? log_file : paths.logs_dir);
#ifdef _WIN32
    {
        static const AlnCrashHooks crash_hooks={crash_detail,module_diagnostics_describe,runtime_memory_recent};
        aln_crash_guard_install(&crash_hooks);
    }
    AddVectoredExceptionHandler(1, vectored_fault);
    signal(SIGABRT,abort_signal);
    _set_invalid_parameter_handler(invalid_parameter);
    _set_purecall_handler(purecall);
#endif
    if (game_child) {
        puts("Startup: game-only child process");
#ifdef _WIN32
        module_diagnostics_report_startup();
#endif
    } else log_cpu();
    if (!cpu_only && !check_imports && !game_child) {
        AlnGraphicsCheck check_graphics=cpu_usable;
        int game_process=0;
#ifdef _WIN32
        game_process=1;
#else
        renderer_settings_file=settings_file;
        check_graphics=system_usable;
#endif
        AlnLauncherConfig launcher={app0,paths.image_dir,user_dir,settings_file,log_folder,skip_launcher,force_launcher,check_graphics,
                                    game_process,log_started ? log_path : NULL};
        aln_startup_phase("launcher handoff");
        AlnLauncherResult launched=aln_launcher_run(&launcher);
        aln_startup_phase("launcher returned");
        runtime_log_set_detailed(aln_detailed_logging(settings_file));
        if (launched==ALN_LAUNCHER_QUIT) { runtime_log_mark_clean_exit(); exit(0); }
        if (launched==ALN_LAUNCHER_BLOCKED) exit(1);
#ifdef _WIN32
        if (launched==ALN_LAUNCHER_GAME_STARTED) {
            /* The game process appends to this log: nothing here may write to it once it runs. */
            puts("Launcher: the game process continues this log");
            runtime_log_flush();
            runtime_log_release();
            if (!aln_resume_game_process()) {
                runtime_log_write_direct("Launcher: could not start the game process\n");
                exit(1);
            }
            exit(0);
        }
#endif
    }
    if (!check_imports) {
        char cause[160], advice[160], message[330];
        if (!cpu_usable(cause,sizeof(cause),advice,sizeof(advice))) {
            snprintf(message,sizeof(message),"%s %s",cause,advice);
            fail(message);
        }
    }
    if (!aln_resolve_paths(&request,&paths)) fail(paths.error[0] ? paths.error : "cannot locate the game folders");
    boot=paths.boot; content_profile=paths.content_profile; app0=paths.app0;
    aln_prepare_sdl_gamepad();
    AlnGameBuild game_build;
    const int known_build=aln_game_build(paths.image_dir,&game_build);
    char build_key[64]="";
    if (known_build) snprintf(build_key,sizeof(build_key),"%s-%s",game_build.title_id,game_build.version);
    const char *pinned_build=getenv("ALN_GAME_BUILD");
    if (pinned_build && *pinned_build) {
        if (strcmp(pinned_build,build_key)) {
            char message[256];
            snprintf(message,sizeof(message),"ALN_GAME_BUILD is %s but the game image is %s",pinned_build,
                     known_build ? build_key : "not a supported build");
            fail(message);
        }
        printf("Game: build pinned by ALN_GAME_BUILD\n");
    }
    log_game_identity(app0,known_build ? &game_build : NULL);
    printf("ALN: game build %s\n",known_build ? game_build.label : "not a supported build");
    render_resolution=&aln_resolutions[aln_render_resolution(settings_file,paths.image_dir)];
    aspect_ratio=&aln_aspect_ratios[aln_aspect_ratio(settings_file,paths.image_dir)];
    render_target=aln_render_target(render_resolution,aspect_ratio);
    runtime_aln_set_neo_mode(render_target.pro_memory);
    if (known_build)
        runtime_memory_set_direct_extra(
            aln_budget_growth(game_build.resolution_sites,game_build.resolution_site_count,&render_target));
    aln_crash_context_set_display(render_target.width,render_target.height,render_resolution->key,aspect_ratio->key);
    runtime_pad_set_settings(settings_file);
    printf("Overlay compatibility: %s game process\n",game_child ? "separate" : "original");
    if (content_profile) {
        uint32_t values[ALN_CONTENT_VALUES];
        read_content_profile(content_profile,values);
        runtime_content_configure(values);
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        if (known_build) {
            char prompts_detail[160]="";
            if (aln_controller_prompts_install(settings_file,app0,build_key,prompts_detail,sizeof(prompts_detail))==0)
                printf("Controller prompts: %s\n",prompts_detail[0] ? prompts_detail : "PlayStation (original)");
            else
                printf("Controller prompts: Xbox unavailable (%s)\n",prompts_detail);
            char text_detail[160]="";
            const int fixed=aln_text_fix_install(app0,build_key,text_detail,sizeof(text_detail));
            if (fixed>0) printf("Text fixes: %s\n",text_detail);
            else if (fixed<0) printf("Text fixes: unavailable (%s)\n",text_detail);
        }
        char sfo[4096], id[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
        trophy_runtime_configure(app0, user_dir ? user_dir : "user");
    }
    if (!check_imports) runtime_online_configure(settings_file, user_dir ? user_dir : "user", known_build ? build_key : NULL);
    alngpu_register_kernel();
#ifdef _WIN32
    SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize;
    if (getenv("ALN_STRICT_HANDLES")) {
        PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY strict={0};
        strict.RaiseExceptionOnInvalidHandleReference=1;
        strict.HandleExceptionsPermanentlyEnabled=1;
        if (!SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy,&strict,sizeof(strict)))
            fprintf(stderr,"ALN_STRICT_HANDLES: policy not set (%lu)\n",GetLastError());
    }
    (void)timeout_seconds; /* no watchdog on Windows */
#else
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    Dl_info self_info;
    if (dladdr((void *)main,&self_info)) exe_base=(uintptr_t)self_info.dli_fbase;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    alarm(timeout_seconds); /* 0 disables the watchdog */
#endif
    FILE *f = fopen(boot, "rb");
    if (!f) fail("cannot open boot file; install the game from the launcher first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, ALN_BOOT_MAGIC, ALN_MAGIC_SIZE)) fail("bad boot file signature; rebuild the boot image");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = read64(f);
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    aln_startup_phase("runtime initialization");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int native_libc=!strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    procparam=read64(f);
    for (int i=0;i<4;++i) main_tls[i]=read64(f);
    module_count=read64(f);
    if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
    for (uint64_t m=0;m<module_count;++m) {
        LinkedModule *x=&modules[m];
        x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
        x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
    }
    nb=read64(f);
    if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
        main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
        fail("invalid eboot TLS metadata");
    for (uint64_t m=0;m<module_count;++m) {
        LinkedModule *x=&modules[m];
        if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
            (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                               x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>7)))
            fail("invalid linked module metadata");
    }
    if (nb>import_count) fail("invalid binding count");
    for (uint64_t i=0;i<nb;++i) {
        uint64_t index=read64(f),address=read64(f),kind=read64(f);
        int inside=0;
        for (uint64_t m=0;m<module_count;++m)
            if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
        if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
        bindings[index]=address; binding_kinds[index]=kind;
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    const int skip_intro_requested = aln_skip_intro(settings_file);
    const size_t skip_intro_bytes = known_build && skip_intro_requested ? skip_intro_stub_bytes() : 0;
    const size_t exit_game_bytes = known_build ? exit_game_stub_bytes() : 0;
    const size_t hook_bytes = skip_intro_bytes + exit_game_bytes ? round_page(skip_intro_bytes + exit_game_bytes) : 0;
    /* The hook stubs follow the image, within a jump of it. */
    image = allocate(round_page(size) + hook_bytes);
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    printf("Loader: image %" PRIu64 " bytes, entry 0x%" PRIx64 ", %" PRIu64 " segments, %" PRIu64 " relocations, %" PRIu64
           " imports (%" PRIu64 " bound natively), %" PRIu64 " linked modules\n",size,entry,ns,nr,import_count,nb,module_count);
    if (main_tls[2])
        printf("Loader: eboot static TLS %" PRIu64 " bytes (%" PRIu64 " initialized), alignment %" PRIu64 "\n",
               main_tls[2],main_tls[1],main_tls[3]);
    if (runtime_memory_prepare()) {
#ifdef _WIN32
        MessageBoxW(NULL,L"Alienation could not reserve or commit guest memory. Freeing virtual memory or disk space for the page file may help. Your saved data has not been opened.",
                    L"Alienation - memory unavailable",MB_OK|MB_ICONERROR);
#endif
        fail("guest memory unavailable before starting GPU or game threads");
    }
    if (!cpu_only && !alngpu_available()) puts("GPU: no video core in this build; running CPU-side only");
    else if (!cpu_only) {
        char title[128]="Alienation", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        uint32_t features=0;
        if (known_build && game_build.resolution_site_count) features|=ALN_GPU_FEATURE_RENDER_RESOLUTION;
        AlnGpuConfig gpu={title,serial,user_dir,settings_file,(uint32_t)sdk,attributes,1920,1080,features,
                          render_target.width,render_target.height};
        aln_startup_phase("graphics initialization");
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        if (alngpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,alngpu_symbol_count());
#ifdef _WIN32
        module_diagnostics_report();
        aln_crash_guard_verify();
#endif
    }
    unsigned char *traps = allocate(round_page((import_count + 1) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        /* SysV: mov edi, index; movabs rax, handler; jmp rax. No fake return values. */
        uint32_t index = (uint32_t)i;
        uintptr_t handler = (uintptr_t)unresolved;
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe; /* mov rsi,rdi: preserve arg0 */
        t[3] = 0xbf; memcpy(t + 4, &index, 4);
        t[8] = 0x48; t[9] = 0xb8; memcpy(t + 10, &handler, 8);
        t[18] = 0xff; t[19] = 0xe0;
    }
    uint64_t runtime_bound=0, native_bound=0, unbound=0;
    unsigned char *counted=calloc(import_count ? import_count : 1,1);
    if (!counted) fail("allocation failed");
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2 ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                        : relocs[i].kind == 1 ? (uintptr_t)(traps + 32 * relocs[i].value)
                        : (uintptr_t)image + relocs[i].value;
        if (relocs[i].kind) {
            uintptr_t resolved = runtime_resolve(names[relocs[i].value], relocs[i].kind == 2);
            int how=0;
            if (resolved) { value = resolved + relocs[i].addend; how=1; }
            else if (native_libc && bindings[relocs[i].value]) {
                uint64_t address=bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value]!=relocs[i].kind || !mapped(segments,ns,address,relocs[i].addend+1)) fail("native export kind/range mismatch");
                value=(uintptr_t)image+address+relocs[i].addend;
                how=2;
            }
            if (!counted[relocs[i].value]) {
                counted[relocs[i].value]=1;
                if (how==1) ++runtime_bound; else if (how==2) ++native_bound; else ++unbound;
            }
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    free(counted);
    printf("Imports: %" PRIu64 " referenced; %" PRIu64 " by the runtime, %" PRIu64 " by linked modules, %" PRIu64
           " unbound (stop with STOP when called)\n",runtime_bound+native_bound+unbound,runtime_bound,native_bound,unbound);
    aln_startup_phase("imports resolved");
    if (check_imports) {
        /* Lists every import the game would stop at, without running it (needs the GPU
         * library: without --cpu-only its symbols are registered above). */
        unsigned char *checked=calloc(import_count ? import_count : 1,1);
        if (!checked) fail("allocation failed");
        uint64_t missing=0;
        for (uint64_t i=0;i<nr;++i) {
            uint64_t index=relocs[i].value;
            if (!relocs[i].kind || checked[index]) continue;
            checked[index]=1;
            if (runtime_resolve(names[index],relocs[i].kind==2) || (native_libc && bindings[index])) continue;
            printf("MISSING: %s (%s)\n",names[index],runtime_import_name(names[index]));
            ++missing;
        }
        printf("Imports: %" PRIu64 " missing\n",missing);
        runtime_log_flush();
        _exit(missing ? 1 : 0); /* no destructors: GPU threads are running */
    }
    if (render_resolution!=&aln_resolutions[0] || aspect_ratio!=&aln_aspect_ratios[0]) {
        char error[320];
        if (!known_build) fail_running("render resolution and aspect ratio need a supported game build");
        if (!game_build.resolution_site_count)
            fail_running("render resolution and aspect ratio are not available for this game build yet");
        if (!aln_apply_resolution(image,size,game_build.resolution_sites,game_build.resolution_site_count,
                                  &render_target,error,sizeof(error))) {
            char message[400];
            snprintf(message,sizeof(message),"%s, game build %s",error,game_build.label);
            fail_running(message);
        }
        uint32_t patched_width=0,patched_height=0,heap_bytes=0;
        for (size_t i=0;i<game_build.resolution_site_count;++i) {
            const AlnResolutionSite *site=&game_build.resolution_sites[i];
            uint32_t value;
            memcpy(&value,image+site->offset+site->opcode_length,sizeof(value));
            if (site->pixel_budget) heap_bytes=value;
            else if (site->height) patched_height=value;
            else patched_width=value;
        }
        printf("Patch: game display size sites hold %ux%u (%zu sites)\n",patched_width,patched_height,
               game_build.resolution_site_count);
        if (heap_bytes)
            printf("Patch: GPU heap holds %u MiB (retail 1024 MiB), direct memory pool +%llu MiB\n",heap_bytes>>20,
                   (unsigned long long)(aln_budget_growth(game_build.resolution_sites,game_build.resolution_site_count,
                                                          &render_target)>>20));
    }
    if (known_build && game_build.frame_rate) {
        char error[320];
        if (!aln_apply_frame_rate(image,size,game_build.frame_rate,error,sizeof(error))) {
            char message[400];
            snprintf(message,sizeof(message),"%s, game build %s",error,game_build.label);
            fail_running(message);
        }
        printf("Frame rate: the game's engine_set_fps script calls are ignored; the engine's startup target stays 59.94\n");
    }
    if (known_build) {
        char message[160];
        aln_menu_buttons_apply(image,size,build_key,aln_menu_buttons_wanted(aln_asian_menu_buttons(settings_file)),
                               message,sizeof(message));
        printf("Asian menu buttons: %s\n",message);
    }
    if (skip_intro_requested) {
        if (known_build) skip_intro_install(image,size,build_key,image+round_page(size),skip_intro_bytes);
        else puts("Skip Intro: requested but unavailable for an unknown game build: not a supported build");
    }
    if (known_build)
        install_exit_game(app0,build_key,image,size,image+round_page(size)+skip_intro_bytes,exit_game_bytes);
    printf("Display: render resolution %dx%d (%s), aspect ratio %s\n",render_target.width,render_target.height,
           render_resolution->key,aspect_ratio->name);
    const AlnRunSettings run_settings={known_build ? game_build.label : NULL,render_target.width,render_target.height,
                                       render_resolution->key,aspect_ratio->name};
    aln_startup_phase("patches installed");
    aln_log_settings_summary(settings_file,paths.image_dir,app0,&run_settings);
#ifdef _WIN32
    printf("Guest TCB loads redirected to the TEB TLS slot: %" PRIu64 "\n", patch_tcb_loads(segments, ns));
#endif
    protect(traps, round_page((import_count + 1) * 32), 5);
    if (hook_bytes) protect(image + round_page(size), hook_bytes, 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
    aln_startup_phase("guest image mapped");
#ifdef _WIN32
    aln_crash_guard_set_guest((uintptr_t)image,size);
#endif
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image+procparam);
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        aln_startup_phase("linked modules initialization");
        for (uint64_t m=0;m<module_count;++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            typedef int (ABI *ModuleInit)(uint64_t,void *,void *);
            int result=((ModuleInit)(image+modules[m].init))(0,NULL,NULL);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
    aln_startup_phase("guest entry");
    aln_boot_diag_start();
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
    struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
#ifdef _WIN32
    typedef void (ABI *Entry)(void *, void (ABI *)(void));
    ((Entry)(image + entry))(&params, guest_exit);
#else
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks. */
    enum { MAIN_STACK=8*1024*1024 };
    unsigned char *stack=runtime_low_map(MAIN_STACK,PROT_READ|PROT_WRITE);
    if (!stack) fail("cannot allocate guest main stack");
    enter_on_stack(image+entry,&params,(void *)guest_exit,stack+MAIN_STACK-64);
#endif
    fail("entry unexpectedly returned");
}
