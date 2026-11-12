/* Windows guest memory: sparse backing, on-demand commitment, aliases, protection and release.
 * Includes the implementation so commitment can be inspected and failed on demand. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
void runtime_log_flush(void) {}
int32_t *runtime_errno(void) { static int32_t value; return &value; }
#include <windows.h>
#include <psapi.h>
static int fail_section_creation, fail_commit;
static DWORD commit_error=ERROR_NOT_ENOUGH_MEMORY;
static LPVOID WINAPI test_virtual_alloc(LPVOID address, SIZE_T size, DWORD type, DWORD protection) {
    if (fail_commit && (type & MEM_COMMIT)) { SetLastError(commit_error); return NULL; }
    return VirtualAlloc(address,size,type,protection);
}
static HANDLE WINAPI test_create_section(HANDLE file, SECURITY_ATTRIBUTES *security, ULONG access, ULONG protection,
                                         ULONG attributes, ULONG64 size, PCWSTR name,
                                         MEM_EXTENDED_PARAMETER *parameters, ULONG count) {
    if (fail_section_creation) { SetLastError(ERROR_COMMITMENT_LIMIT); return NULL; }
    return CreateFileMapping2(file,security,access,protection,attributes,size,name,parameters,count);
}
#define CreateFileMapping2 test_create_section
#define VirtualAlloc test_virtual_alloc
#include "../src/runtime_memory.c"
#undef CreateFileMapping2
#undef VirtualAlloc
static int checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %d: %s (WinError %lu)\n",__LINE__,#x,GetLastError()); } } while(0)
static uint64_t system_commit(void) {
    PERFORMANCE_INFORMATION info;
    memset(&info,0,sizeof(info)); info.cb=sizeof(info);
    K32GetPerformanceInfo(&info,sizeof(info));
    return (uint64_t)info.CommitTotal*info.PageSize;
}
static DWORD state_at(const void *address) {
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery(address,&info,sizeof(info)) ? info.State : 0;
}
static DWORD protect_at(const void *address) {
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery(address,&info,sizeof(info)) ? info.Protect : 0;
}
/* A direct mapping made while Windows refuses its commit with ERROR_COMMITMENT_LIMIT. */
static int32_t map_deferred(void **out, uint64_t size, int prot, int64_t phys) {
    commit_error=ERROR_COMMITMENT_LIMIT; fail_commit=1;
    const int32_t result=direct_map(out,size,prot,0,phys,0);
    fail_commit=0; commit_error=ERROR_NOT_ENOUGH_MEMORY;
    return result;
}
/* Exactly the commit this process holds for guest memory: the committed pages of the section. */
static uint64_t section_committed(void) {
    uint64_t total=0;
    for (unsigned char *at=backing_base, *end=backing_base+POOL_SIZE+FLEX_SPAN; at<end;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery(at,&m,sizeof(m))) break;
        unsigned char *next=(unsigned char *)m.BaseAddress+m.RegionSize;
        if (next>end) next=end;
        if (m.State==MEM_COMMIT) total+=(uint64_t)(next-at);
        at=next;
    }
    return total;
}
static LONG CALLBACK commit_on_fault(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    if (record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2 &&
        runtime_memory_commit_fault((uintptr_t)record->ExceptionInformation[1],(int)record->ExceptionInformation[0]))
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}
#define MIB (UINT64_C(1)<<20)
/* The direct allocations Alienation makes at startup, mapped at once (runtime log of CUSA04430 1.00). */
static const uint64_t kStartupLayout[]={1792*MIB,1024*MIB,1120*MIB,320*MIB,352*MIB};
#define LAYOUT_COUNT (sizeof(kStartupLayout)/sizeof(*kStartupLayout))
static uint64_t layout_total(void) {
    uint64_t total=0;
    for (size_t i=0;i<LAYOUT_COUNT;++i) total+=kStartupLayout[i];
    return total;
}
/* Maps the startup layout and touches one page every 64 MiB; returns the system commit it added, or UINT64_MAX. */
static uint64_t map_startup_layout(void **views, int64_t *physical, uint64_t *touched) {
    const uint64_t before=section_committed(), system_before=system_commit();
    *touched=0;
    for (size_t i=0;i<LAYOUT_COUNT;++i) {
        views[i]=NULL;
        if (direct_allocate(0,INT64_MAX,kStartupLayout[i],0,i==0 ? 0 : 3,&physical[i]) ||
            map_deferred(&views[i],kStartupLayout[i],3,physical[i])) return UINT64_MAX;
    }
    const uint64_t mapped=section_committed();
    for (size_t i=0;i<LAYOUT_COUNT;++i)
        for (uint64_t at=0; at<kStartupLayout[i]; at+=64*MIB) { ((volatile unsigned char *)views[i])[at]=(unsigned char)(i+1); *touched+=PAGE; }
    const uint64_t after=section_committed();
    printf("startup layout: %" PRIu64 " MiB mapped; guest memory commit +%" PRIu64 " KiB once mapped, +%" PRIu64
           " KiB after touching %" PRIu64 " KiB (system commit %+" PRId64 " MiB, other processes included)\n",
           layout_total()>>20,(mapped-before)>>10,(after-before)>>10,*touched>>10,
           ((int64_t)system_commit()-(int64_t)system_before)/(int64_t)MIB);
    return after-before;
}
static void unmap_startup_layout(void **views, const int64_t *physical) {
    for (size_t i=0;i<LAYOUT_COUNT;++i)
        if (views[i]) { CHECK(direct_unmap(views[i],kStartupLayout[i])==0); CHECK(direct_release(physical[i],kStartupLayout[i])==0); }
}
/* One of the two A/B processes: the pool prepared, then the layout mapped under the commit limit and touched, held
 * until the parent closes stdin. */
static int layout_child(void) {
    AddVectoredExceptionHandler(1,commit_on_fault);
    if (runtime_memory_prepare()) return 2;
    const uint64_t prepared=section_committed();
    void *views[LAYOUT_COUNT]; int64_t physical[LAYOUT_COUNT]; uint64_t touched;
    const uint64_t grew=map_startup_layout(views,physical,&touched);
    if (grew==UINT64_MAX) return 3;
    printf("ready %" PRIu64 " %" PRIu64 "\n",prepared>>10,grew>>10);
    char line[8];
    (void)fgets(line,sizeof(line),stdin);
    return ((volatile unsigned char *)views[0])[0]==1 ? 0 : 4;
}
typedef struct { unsigned char *base; uint64_t size; unsigned index; } RaceArgs;
static void *race_pages(void *arg) {
    const RaceArgs *r=arg;
    for (uint64_t at=0; at<r->size; at+=PAGE) ((volatile unsigned char *)r->base)[at+r->index]=(unsigned char)(r->index+1);
    return NULL;
}
typedef struct { PROCESS_INFORMATION process; HANDLE in, out; } Child;
static int start_child(Child *child) {
    SECURITY_ATTRIBUTES inherit={sizeof(inherit),NULL,TRUE};
    HANDLE in_read,out_write;
    if (!CreatePipe(&in_read,&child->in,&inherit,0) || !CreatePipe(&child->out,&out_write,&inherit,0)) return 0;
    SetHandleInformation(child->in,HANDLE_FLAG_INHERIT,0);
    SetHandleInformation(child->out,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOA startup; memset(&startup,0,sizeof(startup)); startup.cb=sizeof(startup);
    startup.dwFlags=STARTF_USESTDHANDLES; startup.hStdInput=in_read; startup.hStdOutput=out_write;
    startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
    char path[MAX_PATH], command[MAX_PATH+32];
    GetModuleFileNameA(NULL,path,sizeof(path));
    snprintf(command,sizeof(command),"\"%s\" --layout-child",path);
    const BOOL ok=CreateProcessA(NULL,command,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&startup,&child->process);
    CloseHandle(in_read); CloseHandle(out_write);
    return ok;
}
static int child_ready(Child *child, uint64_t *prepared_kib, uint64_t *grew_kib) {
    for (;;) {
        char text[256]; DWORD n=0, total=0;
        while (total<sizeof(text)-1 && ReadFile(child->out,text+total,1,&n,NULL) && n) { if (text[total++]=='\n') break; }
        if (!total) return 0;
        text[total]=0;
        if (sscanf(text,"ready %" SCNu64 " %" SCNu64,prepared_kib,grew_kib)==2) return 1;
    }
}
static void two_processes(void) {
    const uint64_t before=system_commit();
    Child children[2]; memset(children,0,sizeof(children));
    uint64_t grew[2]={0,0}, prepared[2]={0,0};
    int ready=1;
    for (int i=0;i<2;++i) ready&=start_child(&children[i]) && child_ready(&children[i],&prepared[i],&grew[i]);
    const uint64_t both=system_commit();
    CHECK(ready);
    CHECK(prepared[0]<=64 && prepared[1]<=64);
    printf("two processes: pools prepared with %" PRIu64 " / %" PRIu64 " KiB committed; each mapped %" PRIu64 " MiB and committed %" PRIu64 " / %" PRIu64 " KiB for it; system commit %+" PRId64
           " MiB with both alive\n",prepared[0],prepared[1],layout_total()>>20,grew[0],grew[1],((int64_t)both-(int64_t)before)/(int64_t)MIB);
    CHECK(grew[0]<layout_total()/1024/64 && grew[1]<layout_total()/1024/64);
    CHECK((int64_t)(both-before)<(int64_t)(layout_total()/4));
    for (int i=0;i<2;++i) {
        DWORD code=1;
        if (children[i].in) CloseHandle(children[i].in);
        if (children[i].process.hProcess) {
            WaitForSingleObject(children[i].process.hProcess,30000);
            GetExitCodeProcess(children[i].process.hProcess,&code);
            CloseHandle(children[i].process.hProcess); CloseHandle(children[i].process.hThread);
        }
        if (children[i].out) CloseHandle(children[i].out);
        CHECK(code==0);
    }
}
int main(int argc, char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    if (argc>1 && !strcmp(argv[1],"--layout-child")) return layout_child();
    AddVectoredExceptionHandler(1,commit_on_fault);
    MEMORY_BASIC_INFORMATION info;
    fail_section_creation=1;
    CHECK(runtime_memory_prepare()!=0);
    CHECK(backing_base==NULL && section==NULL);
    fail_section_creation=0;
    fail_commit=1;
    CHECK(runtime_memory_prepare()!=0);
    fail_commit=0;
    CHECK(backing_base!=NULL);
    if (!backing_base) return 1;
    CHECK(VirtualQuery(backing_base,&info,sizeof(info)) && info.State==MEM_RESERVE);
    CHECK(runtime_memory_prepare()==0);
    CHECK(VirtualQuery(backing_base+POOL_SIZE+FLEX_SPAN-PAGE,&info,sizeof(info)) && info.State==MEM_RESERVE);

    void *at=NULL;
    CHECK(map_flexible(&at,4*PAGE,3,0)==0);
    if (!at) return 1;
    unsigned char *code=at;
    code[0]=0xb8; code[1]=42; code[2]=0; code[3]=0; code[4]=0; code[5]=0xc3;
    CHECK(kernel_mprotect(at,4*PAGE,7)==0);
    CHECK(((int (*)(void))at)()==42);
    CHECK(kernel_mprotect(at,4*PAGE,1)==0);
    CHECK(VirtualQuery(at,&info,sizeof(info)) && info.Protect==PAGE_READONLY);
    CHECK(kernel_mprotect(at,4*PAGE,7)==0);
    CHECK(VirtualQuery(at,&info,sizeof(info)) && info.Protect==PAGE_EXECUTE_READWRITE);
    CHECK(kernel_mprotect(at,4*PAGE,5)==0);
    CHECK(protect_at(at)==PAGE_EXECUTE_READ);
    CHECK(((int (*)(void))at)()==42);
    CHECK(kernel_mprotect(at,4*PAGE,3)==0);
    CHECK(protect_at(at)==PAGE_READWRITE);
    CHECK(state_at(backing_base+POOL_SIZE+4*PAGE)==MEM_RESERVE);

    const unsigned char tracked=23;
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,0,0);
    CHECK(protect_at(at)==PAGE_NOACCESS);
    CHECK(runtime_memory_write_backing((uintptr_t)at+PAGE,&tracked,1));
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,1,0);
    CHECK(protect_at(at)==PAGE_READONLY);
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,1,1);
    CHECK(protect_at(at)==PAGE_READWRITE);
    CHECK(((unsigned char *)at)[PAGE]==tracked);
    runtime_memory_gpu_protect((uintptr_t)at+PAGE,PAGE,0,0);
    CHECK(protect_at(at)==PAGE_READWRITE && protect_at((char *)at+PAGE)==PAGE_NOACCESS && protect_at((char *)at+2*PAGE)==PAGE_READWRITE);
    runtime_memory_gpu_protect((uintptr_t)at+PAGE,PAGE,1,1);
    CHECK(protect_at((char *)at+PAGE)==PAGE_READWRITE);
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,0,0);
    CHECK(kernel_mprotect(at,4*PAGE,3)==0);
    CHECK(protect_at(at)==PAGE_NOACCESS);
    CHECK(kernel_mprotect(at,4*PAGE,1)==0);
    CHECK(kernel_mprotect(at,4*PAGE,3)==0);
    CHECK(protect_at(at)==PAGE_READWRITE);
    CHECK(kernel_mprotect(at,4*PAGE,1)==0);
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,1,1);
    CHECK(protect_at(at)==PAGE_READONLY);
    CHECK(kernel_mprotect(at,4*PAGE,5)==0);
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,0,0);
    CHECK(protect_at(at)==PAGE_EXECUTE);
    runtime_memory_gpu_protect((uintptr_t)at,4*PAGE,1,1);
    CHECK(protect_at(at)==PAGE_EXECUTE_READ);
    CHECK(kernel_mprotect(at,4*PAGE,1)==0);
    const unsigned char marker=17;
    CHECK(runtime_memory_write_backing((uintptr_t)at,&marker,1));
    CHECK(*(unsigned char *)at==marker);
    CHECK(direct_unmap(at,4*PAGE)==0);
    CHECK(VirtualQuery(at,&info,sizeof(info)) && info.State==MEM_RESERVE);
    at=NULL; CHECK(map_flexible(&at,4*PAGE,3,0)==0); CHECK(*(unsigned char *)at==0); CHECK(direct_unmap(at,4*PAGE)==0);

    void *executable=NULL;
    CHECK(map_flexible(&executable,PAGE,7,0)==0);
    if (executable) { *(unsigned char *)executable=0xc3; CHECK(VirtualQuery(executable,&info,sizeof(info)) && info.Protect==PAGE_EXECUTE_READWRITE); CHECK(direct_unmap(executable,PAGE)==0); }
    const unsigned char ret=0xc3;
    CHECK(map_flexible(&executable,PAGE,5,0)==0);
    if (executable) {
        CHECK(protect_at(executable)==PAGE_EXECUTE_READ);
        CHECK(runtime_memory_write_backing((uintptr_t)executable,&ret,1));
        ((void (*)(void))executable)();
        CHECK(kernel_mprotect(executable,PAGE,7)==0 && protect_at(executable)==PAGE_EXECUTE_READWRITE);
        CHECK(kernel_mprotect(executable,PAGE,3)==0 && protect_at(executable)==PAGE_READWRITE);
        CHECK(direct_unmap(executable,PAGE)==0);
    }
    int64_t phys=-1;
    void *direct_code=NULL;
    CHECK(direct_allocate(0,INT64_MAX,PAGE,0,3,&phys)==0);
    CHECK(direct_map(&direct_code,PAGE,7,0,phys,0)==0);
    if (direct_code) {
        *(unsigned char *)direct_code=ret;
        CHECK(protect_at(direct_code)==PAGE_EXECUTE_READWRITE);
        ((void (*)(void))direct_code)();
        CHECK(kernel_mprotect(direct_code,PAGE,5)==0 && protect_at(direct_code)==PAGE_EXECUTE_READ);
        CHECK(kernel_mprotect(direct_code,PAGE,3)==0 && protect_at(direct_code)==PAGE_READWRITE);
        CHECK(direct_unmap(direct_code,PAGE)==0);
    }
    direct_code=NULL;
    CHECK(direct_map(&direct_code,PAGE,5,0,phys,0)==0);
    if (direct_code) {
        CHECK(protect_at(direct_code)==PAGE_EXECUTE_READ);
        ((void (*)(void))direct_code)();
        CHECK(kernel_mprotect(direct_code,PAGE,7)==0 && protect_at(direct_code)==PAGE_EXECUTE_READWRITE);
        CHECK(direct_unmap(direct_code,PAGE)==0);
    }
    CHECK(direct_release(phys,PAGE)==0);

    unsigned char *split=NULL;
    CHECK(map_flexible((void **)&split,8*PAGE,3,0)==0);
    if (split) {
        split[0]=0x11; split[7*PAGE]=0x77;
        CHECK(direct_unmap(split+4*PAGE,4*PAGE)==0);
        CHECK(split[0]==0x11);
        void *other=NULL; CHECK(map_flexible(&other,4*PAGE,3,0)==0);
        if (other) { memset(other,0x22,4*PAGE); CHECK(split[0]==0x11 && split[3*PAGE]==0); CHECK(direct_unmap(other,4*PAGE)==0); }
        CHECK(direct_unmap(split,2*PAGE)==0);
        const unsigned char patch=0x33;
        CHECK(runtime_memory_write_backing((uintptr_t)split+3*PAGE,&patch,1));
        CHECK(split[3*PAGE]==patch && split[2*PAGE]==0);
        CHECK(direct_unmap(split+2*PAGE,2*PAGE)==0);
        CHECK(flexible_bytes==0);
    }
    unsigned char *middle=NULL;
    CHECK(map_flexible((void **)&middle,8*PAGE,3,0)==0);
    if (middle) {
        void *hole=middle+3*PAGE;
        CHECK(map_flexible(&hole,2*PAGE,3,MAP_FIXED_FLAG)==0);
        CHECK(flexible_bytes==8*PAGE);
        CHECK(direct_unmap(middle,3*PAGE)==0 && direct_unmap(middle+5*PAGE,3*PAGE)==0);
        CHECK(direct_unmap(hole,2*PAGE)==0);
        CHECK(flexible_bytes==0);
    }

    const uint64_t charged=128u<<20;
    CHECK(direct_allocate(0,INT64_MAX,charged,0,3,&phys)==0);
    void *first=NULL,*second=NULL;
    const uint64_t commit_before=system_commit();
    CHECK(direct_map(&first,charged,3,0,phys,0)==0);
    const uint64_t commit_first=system_commit();
    CHECK(direct_map(&second,charged,3,0,phys,0)==0);
    const uint64_t commit_second=system_commit();
    printf("Dragon accounting: a %" PRIu64 " MiB mapping commits %+" PRId64 " MiB, its alias %+" PRId64 " MiB (system commit)\n",
           charged>>20,((int64_t)commit_first-(int64_t)commit_before)/(int64_t)MIB,((int64_t)commit_second-(int64_t)commit_first)/(int64_t)MIB);
    CHECK((int64_t)(commit_first-commit_before)>=(int64_t)(charged*3/4));
    CHECK((int64_t)(commit_second-commit_first)<(int64_t)(charged/2));
    CHECK(direct_unmap(first,charged)==0); CHECK(direct_unmap(second,charged)==0); CHECK(direct_release(phys,charged)==0);

    CHECK(direct_allocate(0,INT64_MAX,8*PAGE,0,3,&phys)==0);
    first=NULL; second=NULL;
    CHECK(direct_map(&first,8*PAGE,3,0,phys,0)==0); CHECK(direct_map(&second,8*PAGE,3,0,phys,0)==0);
    CHECK(first && second && first!=second);
    ((uint64_t *)first)[3]=UINT64_C(0x1122334455667788);
    CHECK(((uint64_t *)second)[3]==UINT64_C(0x1122334455667788));
    ((uint64_t *)second)[5]=UINT64_C(0x8877665544332211);
    CHECK(((uint64_t *)first)[5]==UINT64_C(0x8877665544332211));

    void *failed=NULL; fail_commit=1;
    CHECK(map_flexible(&failed,PAGE,3,0)==NO_MEMORY);
    fail_commit=0;
    CHECK(((uint64_t *)first)[3]==UINT64_C(0x1122334455667788));
    void *replacement=first; fail_commit=1;
    CHECK(map_flexible(&replacement,PAGE,3,MAP_FIXED_FLAG)==NO_MEMORY);
    replacement=first;
    CHECK(direct_map(&replacement,PAGE,3,MAP_FIXED_FLAG,phys,0)==NO_MEMORY);
    fail_commit=0;
    CHECK(((uint64_t *)first)[3]==UINT64_C(0x1122334455667788));
    CHECK(((uint64_t *)second)[3]==UINT64_C(0x1122334455667788));
    void *start=NULL,*end=NULL; uint32_t prot=0;
    CHECK(query_protection(first,&start,&end,&prot)==0 && start==first && end>=(void *)((char *)first+8*PAGE) && prot==3);
    replacement=first;
    CHECK(direct_map(&replacement,PAGE,3,MAP_FIXED_FLAG,phys,0)==0 && replacement==first);
    CHECK(((uint64_t *)first)[3]==UINT64_C(0x1122334455667788) && ((uint64_t *)second)[5]==UINT64_C(0x8877665544332211));
    CHECK(map_flexible(&failed,PAGE,3,0)==0); CHECK(direct_unmap(failed,PAGE)==0);

    CHECK(kernel_mprotect(first,2*PAGE,1)==0);
    CHECK(direct_unmap((char *)first+2*PAGE,2*PAGE)==0);
    CHECK(VirtualQuery(first,&info,sizeof(info)) && info.Protect==PAGE_READONLY);
    CHECK(((uint64_t *)first)[3]==UINT64_C(0x1122334455667788));
    CHECK(direct_unmap(first,2*PAGE)==0); CHECK(direct_unmap((char *)first+4*PAGE,4*PAGE)==0);
    CHECK(direct_unmap(second,8*PAGE)==0); CHECK(direct_release(phys,8*PAGE)==0);
    void *reused=NULL; CHECK(direct_allocate(0,INT64_MAX,8*PAGE,0,3,&phys)==0);
    CHECK(direct_map(&reused,8*PAGE,3,0,phys,0)==0);
    CHECK(((uint64_t *)reused)[3]==0);
    CHECK(direct_unmap(reused,8*PAGE)==0); CHECK(direct_release(phys,8*PAGE)==0);

    const int64_t untouched=INT64_C(2)<<30;
    CHECK(direct_allocate(untouched,INT64_MAX,8*PAGE,0,3,&phys)==0 && phys==untouched);
    void *partial=NULL; CHECK(direct_map(&partial,2*PAGE,3,0,phys,0)==0);
    if (partial) *(unsigned char *)partial=0x77;
    CHECK(state_at(backing_base+phys+2*PAGE)==MEM_RESERVE);
    CHECK(direct_unmap(partial,2*PAGE)==0); CHECK(direct_release(phys,8*PAGE)==0);
    CHECK(state_at(backing_base+phys+2*PAGE)==MEM_RESERVE);
    partial=NULL; CHECK(direct_allocate(untouched,INT64_MAX,8*PAGE,0,3,&phys)==0 && phys==untouched);
    CHECK(direct_map(&partial,2*PAGE,3,0,phys,0)==0 && partial && *(unsigned char *)partial==0);
    CHECK(direct_unmap(partial,2*PAGE)==0); CHECK(direct_release(phys,8*PAGE)==0);

    unsigned char *whole=NULL,*alias=NULL;
    CHECK(direct_allocate(untouched,INT64_MAX,8*PAGE,0,3,&phys)==0 && phys==untouched);
    CHECK(direct_map((void **)&whole,8*PAGE,3,0,phys,0)==0 && direct_map((void **)&alias,8*PAGE,3,0,phys,0)==0);
    if (whole && alias) {
        whole[0]=0x10; whole[3*PAGE]=0x13; whole[7*PAGE]=0x17;
        CHECK(direct_release(phys+2*PAGE,3*PAGE)==0);
        CHECK(state_at(whole+2*PAGE)==MEM_RESERVE && state_at(alias+2*PAGE)==MEM_RESERVE);
        CHECK(state_at(whole)==MEM_COMMIT && state_at(whole+5*PAGE)==MEM_COMMIT);
        CHECK(whole[0]==0x10 && alias[0]==0x10 && whole[7*PAGE]==0x17 && alias[7*PAGE]==0x17);
        alias[PAGE]=0x21;
        CHECK(whole[PAGE]==0x21);
        CHECK(direct_release(phys,8*PAGE)==INVALID);
        void *refilled=NULL; int64_t again=-1;
        CHECK(direct_allocate(phys+2*PAGE,INT64_MAX,3*PAGE,0,3,&again)==0 && again==phys+(int64_t)(2*PAGE));
        CHECK(direct_map(&refilled,3*PAGE,3,0,again,0)==0);
        if (refilled) {
            CHECK(((unsigned char *)refilled)[PAGE]==0);
            CHECK(direct_unmap(refilled,3*PAGE)==0);
        }
        CHECK(direct_release(again,3*PAGE)==0);
        CHECK(whole[0]==0x10 && alias[7*PAGE]==0x17);
        CHECK(direct_unmap(whole,2*PAGE)==0 && direct_unmap(whole+5*PAGE,3*PAGE)==0);
        CHECK(direct_unmap(alias,2*PAGE)==0 && direct_unmap(alias+5*PAGE,3*PAGE)==0);
        CHECK(direct_release(phys,2*PAGE)==0 && direct_release(phys+5*PAGE,3*PAGE)==0);
    }

    int64_t high=(int64_t)(POOL_SIZE-65536); CHECK(direct_allocate(high,(int64_t)POOL_SIZE,65536,65536,3,&phys)==0);
    CHECK(direct_release(phys,65536)==0);
    CHECK(VirtualQuery(backing_base+high,&info,sizeof(info)) && info.State==MEM_RESERVE);

    CHECK(pool_expand(0,INT64_MAX,65536,65536,&phys)==0);
    void *reserved=NULL; CHECK(pool_reserve(NULL,65536,65536,0,&reserved)==0);
    CHECK(pool_commit(reserved,65536,3,3,0)==0); memset(reserved,0x5a,65536);
    CHECK(pool_decommit(reserved,65536,0)==0);
    CHECK(VirtualQuery(reserved,&info,sizeof(info)) && info.State==MEM_RESERVE);
    CHECK(pool_commit(reserved,65536,3,3,0)==0);
    CHECK(pool_decommit(reserved,65536,0)==0);
    /* Windows' commit limit refusing a mapping: it is placed anyway, and its pages are committed on first use. */
    const int64_t fresh=INT64_C(3)<<30;
    CHECK(!deferred_commit);
    int64_t lazy_phys=-1, kept_phys=-1;
    unsigned char *lazy_a=NULL,*lazy_b=NULL,*kept=NULL;
    CHECK(direct_allocate(0,INT64_MAX,PAGE,0,3,&kept_phys)==0 && direct_map((void **)&kept,PAGE,3,0,kept_phys,0)==0);
    if (kept) kept[1]=0x44;
    CHECK(direct_allocate(fresh,INT64_MAX,2*PAGE,0,3,&lazy_phys)==0 && lazy_phys==fresh);
    const uint64_t before_limit=section_committed();
    CHECK(map_deferred((void **)&lazy_a,2*PAGE,3,lazy_phys)==0 && map_deferred((void **)&lazy_b,2*PAGE,3,lazy_phys)==0);
    CHECK(deferred_commit && section_committed()==before_limit && lazy_a && lazy_b && kept && kept[1]==0x44);
    if (lazy_a && lazy_b) {
        CHECK(state_at(lazy_a)==MEM_RESERVE && state_at(lazy_b+PAGE)==MEM_RESERVE);
        fail_commit=1;
        CHECK(!runtime_memory_commit_fault((uintptr_t)lazy_b+PAGE,1));
        fail_commit=0;
        CHECK(state_at(lazy_a+PAGE)==MEM_RESERVE && state_at(backing_base+fresh+PAGE)==MEM_RESERVE);
        lazy_b[PAGE+3]=0x31;
        CHECK(lazy_a[PAGE+3]==0x31 && state_at(lazy_a)==MEM_RESERVE && section_committed()==before_limit+PAGE);
        lazy_a[7]=0x13;
        CHECK(lazy_b[7]==0x13);
        CHECK(runtime_memory_commit_fault((uintptr_t)lazy_b+PAGE,1));
        CHECK(direct_unmap(lazy_a,2*PAGE)==0 && direct_unmap(lazy_b,2*PAGE)==0);
    }
    CHECK(direct_release(lazy_phys,2*PAGE)==0);
    CHECK(direct_unmap(kept,PAGE)==0 && direct_release(kept_phys,PAGE)==0);

    /* Untouched pages: aliases with different protections, mprotect, GPU tracking and a remap after lazy backing. */
    unsigned char *rw=NULL,*ro=NULL;
    CHECK(direct_allocate(fresh+(int64_t)MIB,INT64_MAX,4*PAGE,0,3,&phys)==0 && phys==fresh+(int64_t)MIB);
    CHECK(map_deferred((void **)&rw,4*PAGE,3,phys)==0 && map_deferred((void **)&ro,4*PAGE,1,phys)==0);
    if (rw && ro) {
        CHECK(state_at(rw)==MEM_RESERVE && state_at(ro)==MEM_RESERVE);
        CHECK(ro[PAGE]==0);
        CHECK(protect_at(ro+PAGE)==PAGE_READONLY && protect_at(rw+PAGE)==PAGE_READWRITE);
        CHECK(runtime_memory_commit_fault((uintptr_t)ro+PAGE,0) && !runtime_memory_commit_fault((uintptr_t)ro+PAGE,1));
        rw[PAGE+5]=9;
        CHECK(ro[PAGE+5]==9);
        CHECK(kernel_mprotect(rw,4*PAGE,1)==0);
        CHECK(protect_at(rw+PAGE)==PAGE_READONLY && state_at(rw+2*PAGE)==MEM_RESERVE);
        CHECK(kernel_mprotect(rw,4*PAGE,3)==0);
        CHECK(rw[2*PAGE]==0 && protect_at(rw+2*PAGE)==PAGE_READWRITE && protect_at(ro+2*PAGE)==PAGE_READONLY);
        runtime_memory_gpu_protect((uintptr_t)rw+3*PAGE,PAGE,1,0);
        CHECK(state_at(rw+3*PAGE)==MEM_COMMIT && protect_at(rw+3*PAGE)==PAGE_READONLY);
        CHECK(kernel_mprotect(rw,4*PAGE,3)==0 && protect_at(rw+3*PAGE)==PAGE_READONLY);
        runtime_memory_gpu_protect((uintptr_t)rw+3*PAGE,PAGE,0,0);
        CHECK(protect_at(rw+3*PAGE)==PAGE_NOACCESS && protect_at(ro+3*PAGE)==PAGE_READONLY);
        CHECK(!runtime_memory_commit_fault((uintptr_t)rw+3*PAGE,0) && !runtime_memory_commit_fault((uintptr_t)rw+3*PAGE,1));
        runtime_memory_gpu_protect((uintptr_t)rw+3*PAGE,PAGE,1,1);
        CHECK(protect_at(rw+3*PAGE)==PAGE_READWRITE);
        CHECK(direct_unmap(rw,4*PAGE)==0);
        unsigned char *again=NULL;
        CHECK(map_deferred((void **)&again,4*PAGE,3,phys)==0);
        CHECK(again && state_at(again+PAGE)==MEM_COMMIT && protect_at(again+PAGE)==PAGE_READWRITE && again[PAGE+5]==9);
        CHECK(protect_at(ro+PAGE)==PAGE_READONLY);
        if (again) CHECK(direct_unmap(again,4*PAGE)==0);
        CHECK(direct_unmap(ro,4*PAGE)==0);
    }
    CHECK(direct_release(phys,4*PAGE)==0);

    /* Eight threads touch the same uncommitted pages at once through two aliases: one commit each, no lost write. */
    CHECK(direct_allocate(fresh+2*(int64_t)MIB,INT64_MAX,8*MIB,0,3,&phys)==0 && phys==fresh+2*(int64_t)MIB);
    unsigned char *shared_a=NULL,*shared_b=NULL;
    CHECK(map_deferred((void **)&shared_a,8*MIB,3,phys)==0 && map_deferred((void **)&shared_b,8*MIB,3,phys)==0);
    if (shared_a && shared_b) {
        const uint64_t racing_before=section_committed();
        pthread_t racers[8];
        static RaceArgs race[8];
        for (int i=0;i<8;++i) { race[i]=(RaceArgs){i&1 ? shared_b : shared_a,8*MIB,(unsigned)i}; pthread_create(&racers[i],NULL,race_pages,&race[i]); }
        for (int i=0;i<8;++i) pthread_join(racers[i],NULL);
        int intact=1;
        for (uint64_t at=0; at<8*MIB; at+=PAGE)
            for (unsigned i=0;i<8;++i) intact&=shared_a[at+i]==(unsigned char)(i+1) && shared_b[at+i]==(unsigned char)(i+1);
        CHECK(intact);
        CHECK(section_committed()-racing_before==8*MIB);
        CHECK(direct_unmap(shared_a,8*MIB)==0 && direct_unmap(shared_b,8*MIB)==0);
    }
    CHECK(direct_release(phys,8*MIB)==0);

    /* Under the commit limit, Alienation's startup allocations cost commit for the pages used, not the 4.5 GiB mapped. */
    void *views[LAYOUT_COUNT]; int64_t physical[LAYOUT_COUNT]; uint64_t touched=0;
    const uint64_t grew=map_startup_layout(views,physical,&touched);
    CHECK(grew!=UINT64_MAX && grew<layout_total()/16);
    unsigned char *alias_view=NULL;
    if (grew!=UINT64_MAX) {
        CHECK(map_deferred((void **)&alias_view,64*MIB,3,physical[1])==0 && alias_view && alias_view[0]==2);
        if (alias_view) {
            alias_view[3*PAGE]=0x5a;
            CHECK(((unsigned char *)views[1])[3*PAGE]==0x5a);
            CHECK(direct_unmap(alias_view,64*MIB)==0);
        }
    }
    unmap_startup_layout(views,physical);
    two_processes();
    printf("Windows memory: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
