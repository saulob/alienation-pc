/* Offline AppContent provider for the base game and add-ons installed from local packages.
 * No network downloads or store purchases. Only explicitly installed add-ons are reported. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#include <unistd.h>
#endif
#define INVALID_ID ((int32_t)0x805a1000)
#define NOT_LOADED ((int32_t)0x805a1001)
#define PARAMETER ((int32_t)0x80d90002)
#define BUSY ((int32_t)0x80d90003)
#define NOT_FOUND ((int32_t)0x80d90005)
#define NO_ENTITLEMENT ((int32_t)0x80d90007)
static unsigned refs, initialized, configured;
static uint32_t parameters[5];
static unsigned queries, lists;
#define MAX_ADDONS 256
typedef struct { char label[17], padding[3]; uint32_t status; } AddonInfo;
_Static_assert(sizeof(AddonInfo)==24,"PS4 AppContent add-on info ABI");
typedef struct { AddonInfo info; char host[512]; } Addon;
static Addon addons[MAX_ADDONS];
static unsigned addon_count;
static int entitlement_notified;
int runtime_content_addon(const char *label,const char *host) {
    if(initialized || !label || strlen(label)!=16 || !host || strlen(host)>=sizeof(addons[0].host) || addon_count==MAX_ADDONS) return -1;
    for(unsigned i=0;i<16;++i) if(!((label[i]>='A' && label[i]<='Z') || (label[i]>='0' && label[i]<='9'))) return -1;
    for(unsigned i=0;i<addon_count;++i) if(!strcmp(addons[i].info.label,label)) return -1;
    Addon *a=&addons[addon_count++];
    memcpy(a->info.label,label,17); a->info.status=4; /* installed */
    strcpy(a->host,host);
    printf("Runtime: DLC registered %s\n",label);
    return 0;
}
void runtime_content_configure(const uint32_t values[5]) {
    if (refs || initialized || (values[0]!=1 && values[0]!=3)) {
        fputs("ERROR: invalid or late content profile\n",stderr); runtime_exit(1);
    }
    memcpy(parameters,values,sizeof(parameters)); configured=1;
}
static void require_provider(void) {
    if (!configured) { fputs("STOP: AppContent needs --content-profile\n",stderr); runtime_exit(21); }
}
/* System modules: every library this eboot imports is provided by the host
 * runtime, so loading one only tracks a reference count. AppContent (0xb4)
 * additionally requires the configured content profile. */
static unsigned module_refs[0x10000];
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (id==0xb4) require_provider();
    if (module_refs[id]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); runtime_exit(21); }
    ++module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; printf("Runtime: AppContent host provider loaded; references=%u\n",refs); }
    else printf("Runtime: system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
/* Internal modules (libSceFios2 asks for its helpers): nothing to load on the host. */
static ABI int32_t module_load_internal_arg(uint32_t id, uint64_t argc, const void *argv, uint32_t flags, int32_t *result) {
    (void)argc; (void)argv; (void)flags;
    if (result) *result=0;
    printf("Runtime: internal system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
static ABI int32_t module_loaded(uint16_t id) {
    if (!id) return INVALID_ID;
    return module_refs[id] ? 0 : NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t result=module_loaded(id);
    if (result) return result;
    --module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; if (!refs) initialized=0; }
    return 0;
}
static void require_loaded(void) {
    require_provider();
    if (!refs) { fputs("STOP: AppContent called before module load\n",stderr); runtime_exit(21); }
}
static ABI int32_t content_init(const unsigned char *init,unsigned char *boot) {
    require_loaded();
    if (!init || !boot) return PARAMETER;
    if (initialized) return BUSY;
    /* Only the reserved-zero init profile is implemented. */
    for (unsigned i=0;i<32;++i) if (init[i]) return PARAMETER;
    memset(boot,0,40); initialized=1;
    printf("Runtime: AppContent initialized; %u installed add-ons\n",addon_count);
    if(addon_count && !entitlement_notified) {
        entitlement_notified=1; /* One best-effort notification per process, including unload/re-init. */
        /* PS4 SystemService EntitlementUpdate (0x10000003), local user 1, service 0.
         * The runtime queues it until the game's user manager is ready. */
        const RuntimeEntitlementEvent event={.type=0x10000003,.user=runtime_user_initial_id(),.service=0};
        _Static_assert(sizeof(event)==RUNTIME_SYSTEM_EVENT_BYTES,"SystemService event contract");
        if(!runtime_system_event_push(&event,sizeof(event))) {
            fputs("Runtime: DLC refresh event queue full; installed entitlements remain available\n",stderr);
        }
    }
    return 0;
}
static void require_initialized(void) {
    require_loaded();
    if (!initialized) { fputs("STOP: AppContent query before initialization\n",stderr); runtime_exit(21); }
}
static ABI int32_t param_int(uint32_t id,int32_t *out) {
    require_initialized();
    if (id>4 || !out) return PARAMETER;
    memcpy(out,&parameters[id],4); ++queries;
    printf("Runtime: AppContent parameter %u = %d\n",id,*out);
    return 0;
}
static ABI int32_t addon_list(uint32_t service,void *list,uint32_t capacity,uint32_t *hits) {
    require_initialized();
    if (service>7) return PARAMETER;
    if ((!capacity || !list) && !hits) return PARAMETER;
    unsigned count=service ? 0 : addon_count;
    if(list && capacity) {
        if(count>capacity) count=capacity;
        for(unsigned i=0;i<count;++i) memcpy((unsigned char *)list+i*sizeof(AddonInfo),&addons[i].info,sizeof(AddonInfo));
    }
    if (hits) *hits=count;
    printf("Runtime: DLC list service=%u, capacity=%u, returned=%u\n",service,capacity,count);
    ++lists; return 0;
}
/* /download0 lives in the user directory: report the host's free space there. */
static ABI int32_t download_space_kb(const char *mount_point,uint64_t *kb) {
    require_initialized();
    if (!mount_point || !kb) return PARAMETER;
#ifdef _WIN32
    ULARGE_INTEGER free_bytes;
    if (!GetDiskFreeSpaceExA(runtime_file_user_dir(),&free_bytes,NULL,NULL)) free_bytes.QuadPart=0;
    *kb=free_bytes.QuadPart/1024;
#else
    struct statvfs space;
    *kb=statvfs(runtime_file_user_dir(),&space) ? 0 : (uint64_t)space.f_bavail*space.f_frsize/1024;
#endif
    return 0;
}
static ABI int32_t addon_mount(uint32_t service,const void *label,void *mount_point) {
    require_initialized();
    if(service>7 || !label || !mount_point) return PARAMETER;
    if(!service) for(unsigned i=0;i<addon_count;++i) if(!memcmp(label,addons[i].info.label,16)) {
        char point[16]={0}; snprintf(point,sizeof(point),"/addcont%u",i);
        if(runtime_file_mount(point,addons[i].host)) return BUSY;
        memcpy(mount_point,point,sizeof(point));
        printf("Runtime: DLC mounted %s at %s\n",addons[i].info.label,point);
        return 0;
    }
    return NOT_FOUND;
}
static ABI int32_t addon_enqueue_download(uint32_t service,const void *label) {
    require_initialized();
    if(service>7 || !label) return PARAMETER;
    if(!service) for(unsigned i=0;i<addon_count;++i) if(!memcmp(label,addons[i].info.label,16)) return 0;
    return NO_ENTITLEMENT;
}
static ABI int32_t addon_info(uint32_t service,const void *label,void *info) {
    require_initialized();
    if(service>7 || !label || !info) return PARAMETER;
    if(!service) for(unsigned i=0;i<addon_count;++i) if(!memcmp(label,addons[i].info.label,16)) {
        memcpy(info,&addons[i].info,sizeof(AddonInfo));
        printf("Runtime: DLC found %s (installed)\n",addons[i].info.label);
        return 0;
    }
    return NO_ENTITLEMENT;
}
uintptr_t runtime_content_resolve(const char *name) {
    if (!strcmp(name,"g8cM39EUZ6o#libSceSysmodule")) return (uintptr_t)module_load;
    if (!strcmp(name,"fMP5NHUOaMk#libSceSysmodule")) return (uintptr_t)module_loaded;
    if (!strcmp(name,"eR2bZFAAU0Q#libSceSysmodule")) return (uintptr_t)module_unload;
    if (!strcmp(name,"hHrGoGoNf+s#libSceSysmodule")) return (uintptr_t)module_load_internal_arg;
    if (!strcmp(name,"R9lA82OraNs#libSceAppContent")) return (uintptr_t)content_init;
    if (!strcmp(name,"99b82IKXpH4#libSceAppContent")) return (uintptr_t)param_int;
    if (!strcmp(name,"xnd8BJzAxmk#libSceAppContent")) return (uintptr_t)addon_list;
    if (!strcmp(name,"Gl6w5i0JokY#libSceAppContent")) return (uintptr_t)download_space_kb;
    if (!strcmp(name,"VANhIWcqYak#libSceAppContent")) return (uintptr_t)addon_mount;
    if (!strcmp(name,"7gxh+5QubhY#libSceAppContent")) return (uintptr_t)addon_enqueue_download;
    if (!strcmp(name,"m47juOmH0VE#libSceAppContent")) return (uintptr_t)addon_info;
    return 0;
}
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u\n",refs,initialized,queries,lists);
}
