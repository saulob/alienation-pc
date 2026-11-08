/* libSceAjm: contexts, codec modules, instances and batches in the SDK chunk format; batches run synchronously at
 * submission. This port decodes no codec: the game's sound bank holds only Wwise Vorbis and ADPCM media, which the
 * game decodes itself, so a job that hands encoded input to an instance stops explicitly. */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define ERR_INVALID_CONTEXT ((int32_t)0x80930002)
#define ERR_INVALID_INSTANCE ((int32_t)0x80930003)
#define ERR_INVALID_BATCH ((int32_t)0x80930004)
#define ERR_INVALID_PARAMETER ((int32_t)0x80930005)
#define ERR_OUT_OF_RESOURCES ((int32_t)0x80930007)
#define ERR_ALREADY_REGISTERED ((int32_t)0x80930009)
#define ERR_NOT_REGISTERED ((int32_t)0x8093000A)
#define ERR_WRONG_REVISION ((int32_t)0x8093000B)
#define ERR_MALFORMED_BATCH ((int32_t)0x80930011)
#define RESULT_INVALID_PARAMETER 0x4
#define RESULT_FATAL 0x80000000u
#define STATISTICS_INSTANCE 0x80000u
#define MAX_INSTANCES 0x3fff
#define MAX_CONTEXTS 8
#define MAX_BATCHES 256
#define CODECS 24
enum { IDENT_JOB=0, IDENT_INPUT_RUN=1, IDENT_INPUT_CONTROL=2, IDENT_CONTROL_FLAGS=3, IDENT_RUN_FLAGS=4,
       IDENT_RETURN_ADDRESS=6, IDENT_INLINE=7, IDENT_OUTPUT_RUN=17, IDENT_OUTPUT_CONTROL=18 };

typedef struct { uint32_t word; uint32_t size; } Chunk;          /* ident:6 payload:20 reserved:6 */
typedef struct { uint32_t word; uint32_t size; void *address; } ChunkBuffer;
typedef struct { int32_t result, internal_result; } SidebandResult;
typedef struct { int error_code; const void *job_address; uint32_t command_offset; const void *job_return_address; } BatchError;
typedef struct { void *address; uint64_t size; } AjmBuffer;
_Static_assert(sizeof(ChunkBuffer)==16,"Ajm buffer chunk");

typedef struct { int used; uint32_t codec; } Instance;
typedef struct { int registered[CODECS]; Instance instances[MAX_INSTANCES+1]; } Context;

static pthread_mutex_t lock=PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static Context *contexts[MAX_CONTEXTS+1];
static int batches[MAX_BATCHES];
static size_t jobs_run, batches_run, instances_created;

static uint32_t ident(uint32_t word) { return word & 0x3f; }
static uint32_t payload(uint32_t word) { return (word>>6) & 0xfffff; }
static uint32_t header(uint32_t id, uint32_t value) { return (id & 0x3f) | ((value & 0xfffff)<<6); }

static unsigned char *put_buffer(unsigned char *p, uint32_t id, const void *address, uint64_t size) {
    ChunkBuffer c={header(id,0),(uint32_t)size,(void *)address}; memcpy(p,&c,sizeof(c)); return p+sizeof(c);
}
static unsigned char *put_flags(unsigned char *p, uint32_t id, uint64_t flags) {
    Chunk c={header(id,(uint32_t)(flags>>32)),(uint32_t)flags}; memcpy(p,&c,sizeof(c)); return p+sizeof(c);
}
static void *finish_job(unsigned char *start, unsigned char *end, uint32_t instance) {
    Chunk job={header(IDENT_JOB,instance),(uint32_t)(end-start-sizeof(Chunk))};
    memcpy(start,&job,sizeof(job));
    return end;
}
static ABI void *job_control(void *buffer, uint32_t instance, uint64_t flags, void *in, uint64_t in_size,
                             void *out, uint64_t out_size, void *return_address) {
    unsigned char *p=(unsigned char *)buffer+sizeof(Chunk);
    if (return_address) p=put_buffer(p,IDENT_RETURN_ADDRESS,return_address,0);
    p=put_buffer(p,IDENT_INPUT_CONTROL,in,in_size);
    flags &= instance==STATISTICS_INSTANCE ? UINT64_C(0xC0018007) : UINT64_C(0x60000000E7FF);
    p=put_flags(p,IDENT_CONTROL_FLAGS,flags);
    p=put_buffer(p,IDENT_OUTPUT_CONTROL,out,out_size);
    return finish_job(buffer,p,instance);
}
static ABI void *job_run_split(void *buffer, uint32_t instance, uint64_t flags, const AjmBuffer *in, uint64_t in_count,
                               const AjmBuffer *out, uint64_t out_count, void *sideband, uint64_t sideband_size,
                               void *return_address) {
    unsigned char *p=(unsigned char *)buffer+sizeof(Chunk);
    if (return_address) p=put_buffer(p,IDENT_RETURN_ADDRESS,return_address,0);
    for (uint64_t i=0;i<in_count;++i) p=put_buffer(p,IDENT_INPUT_RUN,in[i].address,in[i].size);
    p=put_flags(p,IDENT_RUN_FLAGS,flags & UINT64_C(0xE00000001FFF));
    for (uint64_t i=0;i<out_count;++i) p=put_buffer(p,IDENT_OUTPUT_RUN,out[i].address,out[i].size);
    p=put_buffer(p,IDENT_OUTPUT_CONTROL,sideband,sideband_size);
    return finish_job(buffer,p,instance);
}

typedef struct { uint64_t input_bytes; int have_flags; ChunkBuffer output_control; } Job;
static int parse_job(const unsigned char *p, uint32_t size, Job *job) {
    memset(job,0,sizeof(*job));
    for (const unsigned char *end=p+size; p<end;) {
        uint32_t word; memcpy(&word,p,4);
        switch (ident(word)) {
        case IDENT_CONTROL_FLAGS: case IDENT_RUN_FLAGS: job->have_flags=1; p+=8; break;
        case IDENT_INPUT_RUN: { ChunkBuffer c; memcpy(&c,p,16); job->input_bytes+=c.size; p+=16; break; }
        case IDENT_OUTPUT_CONTROL: memcpy(&job->output_control,p,16); p+=16; break;
        case IDENT_OUTPUT_RUN: case IDENT_INPUT_CONTROL: case IDENT_RETURN_ADDRESS: p+=16; break;
        default: return -1;
        }
    }
    return job->have_flags ? 0 : -1;
}
static __attribute__((noreturn)) void stop_decode(uint32_t codec, uint64_t bytes) {
    fprintf(stderr,"STOP: Ajm decode requested for codec %u (%llu input bytes); no Ajm codec is implemented\n",codec,
            (unsigned long long)bytes);
    runtime_report();
    runtime_log_flush();
    _exit(24); /* GPU, audio and guest threads are running */
}
/* Control jobs (reset, initialize, gapless, statistics) need no decoder: their sideband reports success. */
static void run_job(Context *ctx, uint32_t id, const Job *job) {
    ++jobs_run;
    unsigned char *side=job->output_control.size ? job->output_control.address : NULL;
    SidebandResult *result=side && job->output_control.size>=sizeof(SidebandResult) ? (SidebandResult *)side : NULL;
    if (side) memset(side,0,job->output_control.size);
    if (id==STATISTICS_INSTANCE) return;
    const Instance *in=&ctx->instances[id & MAX_INSTANCES];
    if ((id & MAX_INSTANCES)==0 || !in->used) {
        if (result) result->result=(int32_t)(RESULT_FATAL|RESULT_INVALID_PARAMETER);
        return;
    }
    if (job->input_bytes) stop_decode(in->codec,job->input_bytes);
}

static Context *context(uint32_t id) { return id && id<=MAX_CONTEXTS ? contexts[id] : NULL; }
static ABI int32_t ajm_initialize(int64_t reserved, uint32_t *out) {
    if (!out || reserved) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&lock);
    for (uint32_t i=1;i<=MAX_CONTEXTS;++i) if (!contexts[i]) {
        contexts[i]=calloc(1,sizeof(Context));
        pthread_mutex_unlock(&lock);
        if (!contexts[i]) return ERR_OUT_OF_RESOURCES;
        *out=i;
        puts("Runtime: Ajm context initialized (no codec decodes)");
        return 0;
    }
    pthread_mutex_unlock(&lock);
    return ERR_OUT_OF_RESOURCES;
}
static ABI int32_t ajm_finalize(uint32_t id) {
    pthread_mutex_lock(&lock);
    Context *c=context(id);
    if (c) { free(c); contexts[id]=NULL; }
    pthread_mutex_unlock(&lock);
    return c ? 0 : ERR_INVALID_CONTEXT;
}
static ABI int32_t ajm_module_register(uint32_t id, uint32_t codec, int64_t reserved) {
    if (reserved || codec>=CODECS) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&lock);
    Context *c=context(id);
    const int32_t r=!c ? ERR_INVALID_CONTEXT : c->registered[codec] ? ERR_ALREADY_REGISTERED : 0;
    if (!r) c->registered[codec]=1;
    pthread_mutex_unlock(&lock);
    if (!r) printf("Runtime: Ajm codec %u registered\n",codec);
    return r;
}
static ABI int32_t ajm_module_unregister(uint32_t id, uint32_t codec) {
    pthread_mutex_lock(&lock);
    Context *c=context(id);
    const int32_t r=!c ? ERR_INVALID_CONTEXT : codec>=CODECS ? ERR_INVALID_PARAMETER : !c->registered[codec] ? ERR_NOT_REGISTERED : 0;
    if (!r) c->registered[codec]=0;
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t ajm_instance_create(uint32_t id, uint32_t codec, uint64_t flags, uint32_t *out) {
    if (!out || codec>=CODECS || ((flags>>7)&7)>2) return ERR_INVALID_PARAMETER;
    if (!(flags & 7)) return ERR_WRONG_REVISION;
    pthread_mutex_lock(&lock);
    Context *c=context(id);
    int32_t r=!c ? ERR_INVALID_CONTEXT : !c->registered[codec] ? ERR_NOT_REGISTERED : ERR_OUT_OF_RESOURCES;
    if (c && c->registered[codec]) for (uint32_t i=1;i<=MAX_INSTANCES;++i) if (!c->instances[i].used) {
        c->instances[i]=(Instance){1,codec};
        *out=i|(codec<<14); r=0; ++instances_created;
        if (instances_created<=8) printf("DEBUG_SAULO: Ajm instance codec %u flags %#llx\n",codec,(unsigned long long)flags);
        break;
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t ajm_instance_destroy(uint32_t id, uint32_t instance) {
    pthread_mutex_lock(&lock);
    Context *c=context(id);
    Instance *in=c ? &c->instances[instance & MAX_INSTANCES] : NULL;
    const int32_t r=!c ? ERR_INVALID_CONTEXT : !(instance & MAX_INSTANCES) || !in->used ? ERR_INVALID_INSTANCE : 0;
    if (!r) memset(in,0,sizeof(*in));
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t ajm_batch_start(uint32_t id, unsigned char *buffer, uint32_t size, int priority,
                                   BatchError *error, uint32_t *out) {
    (void)priority;
    if (size & 7 || !out) return ERR_MALFORMED_BATCH;
    pthread_mutex_lock(&lock);
    Context *c=context(id);
    int slot=-1;
    for (int i=0;c && i<MAX_BATCHES && slot<0;++i) if (!batches[i]) slot=i;
    if (!c || slot<0) { pthread_mutex_unlock(&lock); return !c ? ERR_INVALID_CONTEXT : ERR_OUT_OF_RESOURCES; }
    for (unsigned char *p=buffer, *end=buffer+size; p<end;) {
        Chunk chunk; memcpy(&chunk,p,8);
        unsigned char *body=p+8;
        p=body+chunk.size;
        if (ident(chunk.word)==IDENT_INLINE) continue;
        Job job;
        if (ident(chunk.word)!=IDENT_JOB || p>end || parse_job(body,chunk.size,&job)) {
            if (error) *error=(BatchError){ERR_MALFORMED_BATCH,body-8,(uint32_t)(body-8-buffer),NULL};
            pthread_mutex_unlock(&lock);
            return ERR_MALFORMED_BATCH;
        }
        run_job(c,payload(chunk.word),&job);
    }
    batches[slot]=(int)id;
    ++batches_run;
    *out=(uint32_t)slot+1;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t ajm_batch_wait(uint32_t id, uint32_t batch, uint32_t timeout, BatchError *error) {
    (void)timeout; (void)error;
    pthread_mutex_lock(&lock);
    const int32_t r=!context(id) ? ERR_INVALID_CONTEXT : !batch || batch>MAX_BATCHES || !batches[batch-1] ? ERR_INVALID_BATCH : 0;
    if (!r) batches[batch-1]=0;
    pthread_mutex_unlock(&lock);
    return r;
}

static const RuntimeExport exports[]={
    {"sceAjmInitialize",ajm_initialize}, {"sceAjmFinalize",ajm_finalize},
    {"sceAjmModuleRegister",ajm_module_register}, {"sceAjmModuleUnregister",ajm_module_unregister},
    {"sceAjmInstanceCreate",ajm_instance_create}, {"sceAjmInstanceDestroy",ajm_instance_destroy},
    {"sceAjmBatchJobControlBufferRa",job_control}, {"sceAjmBatchJobRunSplitBufferRa",job_run_split},
    {"sceAjmBatchStartBuffer",ajm_batch_start}, {"sceAjmBatchWait",ajm_batch_wait},
};
uintptr_t runtime_ajm_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_ajm_report(void) {
    printf("Runtime: Ajm batches=%zu, jobs=%zu, instances created=%zu\n",batches_run,jobs_run,instances_created);
}
