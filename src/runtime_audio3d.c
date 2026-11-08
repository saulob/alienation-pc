/* libSceAudio3d as a stereo renderer over the runtime's AudioOut (port type 126). The bed and each object's PCM are
 * queued per granule; PortAdvance mixes one granule to stereo (bed folded, objects at their GAIN, position not
 * rendered) and PortPush plays the mixed queue through a paced AudioOut port. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define ERR_INVALID_PORT ((int32_t)0x80EA0002)
#define ERR_INVALID_OBJECT ((int32_t)0x80EA0003)
#define ERR_INVALID_PARAMETER ((int32_t)0x80EA0004)
#define ERR_OUT_OF_MEMORY ((int32_t)0x80EA0005)
#define ERR_OUT_OF_RESOURCES ((int32_t)0x80EA0006)
#define ERR_NOT_READY ((int32_t)0x80EA0007)
#define ERR_NOT_SUPPORTED ((int32_t)0x80EA0008)
#define PORTS 4
#define MAX_OBJECTS 512
#define MAX_DEPTH 64
#define AUDIO_OUT_AUDIO3D 126
#define AUDIO_OUT_S16_STEREO 1
enum { FORMAT_S16=0, FORMAT_FLOAT=1 };
enum { MODE_NO_ADVANCE=0, MODE_ADVANCE_NO_PUSH=1, MODE_ADVANCE_AND_PUSH=2 };
enum { ATTR_PCM=1, ATTR_GAIN=5, ATTR_RESET_STATE=7 };

typedef struct { uint64_t size_this; uint32_t granularity, rate, max_objects, queue_depth, buffer_mode, pad, num_beds; } OpenParameters;
_Static_assert(sizeof(OpenParameters)==40,"OrbisAudio3dOpenParameters layout");
typedef struct { uint32_t format; void *buffer; uint32_t samples; } Pcm;
typedef struct { uint32_t id, pad; const void *value; uint64_t size; } Attribute;
_Static_assert(sizeof(Pcm)==24 && sizeof(Attribute)==24,"Audio3d attribute layouts");

/* A ring of granules; each granule is `granularity` frames of `channels` floats. */
typedef struct { float *granules[MAX_DEPTH]; unsigned head, count; } Queue;
typedef struct { int reserved, release_when_empty; float gain; Queue pcm; } Object;
typedef struct {
    int used, audio_out;
    uint32_t granularity, depth, mode, max_objects;
    Queue bed;
    int16_t *mixed[MAX_DEPTH]; unsigned mixed_head, mixed_count;
    Object *objects;
    pthread_mutex_t lock;
} Port;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized;
static Port ports[PORTS];
static size_t granules_mixed, granules_played;

static Port *port_of(uint32_t id) { return id<PORTS && ports[id].used ? &ports[id] : NULL; }
static void queue_clear(Queue *q) {
    for (unsigned i=0;i<q->count;++i) free(q->granules[(q->head+i)%MAX_DEPTH]);
    q->head=q->count=0;
}
static float *queue_pop(Queue *q) {
    if (!q->count) return NULL;
    float *g=q->granules[q->head]; q->head=(q->head+1)%MAX_DEPTH; --q->count;
    return g;
}
static float sample(const void *buffer, uint32_t format, size_t index) {
    if (format==FORMAT_FLOAT) { float v; memcpy(&v,(const char *)buffer+index*4,4); return v; }
    int16_t v; memcpy(&v,(const char *)buffer+index*2,2); return (float)v/32768.0f;
}
/* Fronts at full level, centre and surrounds at -3 dB, LFE dropped. Channel order FL FR C LFE SL SR BL BR. */
static int32_t enqueue(const Port *p, Queue *q, const void *buffer, uint32_t format, uint32_t channels, uint32_t frames, int stereo) {
    if (!buffer || !frames || format>FORMAT_FLOAT) return ERR_INVALID_PARAMETER;
    if ((uintptr_t)buffer & (format==FORMAT_FLOAT ? 3 : 1)) return ERR_INVALID_PARAMETER;
    if (q->count>=p->depth) return ERR_NOT_READY;
    const int out_channels=stereo ? 2 : 1;
    float *g=calloc((size_t)p->granularity*(size_t)out_channels,sizeof(float));
    if (!g) return ERR_OUT_OF_MEMORY;
    const float k=0.70710678f;
    for (uint32_t f=0;f<frames && f<p->granularity;++f) {
        const size_t at=(size_t)f*channels;
        if (!stereo) { g[f]=sample(buffer,format,at); continue; }
        float l=sample(buffer,format,at), r=sample(buffer,format,at+1);
        if (channels>=6) { const float c=k*sample(buffer,format,at+2); l+=c+k*sample(buffer,format,at+4); r+=c+k*sample(buffer,format,at+5); }
        if (channels>=8) { l+=k*sample(buffer,format,at+6); r+=k*sample(buffer,format,at+7); }
        g[f*2]=l; g[f*2+1]=r;
    }
    q->granules[(q->head+q->count)%MAX_DEPTH]=g; ++q->count;
    return 0;
}

static ABI int32_t audio3d_initialize(int64_t reserved) {
    if (reserved) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&lock);
    const int was=initialized;
    initialized=1;
    pthread_mutex_unlock(&lock);
    if (was) return ERR_NOT_READY;
    runtime_audio_out_init();
    puts("Runtime: Audio3d initialized (stereo renderer over AudioOut)");
    return 0;
}
static void port_release(Port *p) {
    if (p->audio_out>=0) runtime_audio_out_close(p->audio_out);
    queue_clear(&p->bed);
    for (unsigned i=0;i<p->mixed_count;++i) free(p->mixed[(p->mixed_head+i)%MAX_DEPTH]);
    for (uint32_t i=0;p->objects && i<p->max_objects;++i) queue_clear(&p->objects[i].pcm);
    free(p->objects);
    pthread_mutex_destroy(&p->lock);
    memset(p,0,sizeof(*p));
}
static ABI int32_t audio3d_terminate(void) {
    pthread_mutex_lock(&lock);
    const int was=initialized;
    for (int i=0;i<PORTS;++i) if (ports[i].used) port_release(&ports[i]);
    initialized=0;
    pthread_mutex_unlock(&lock);
    return was ? 0 : ERR_NOT_READY;
}
static ABI int32_t audio3d_default_parameters(OpenParameters *out) {
    if (!out) return ERR_INVALID_PARAMETER;
    const OpenParameters defaults={0x20,0x100,0,512,2,MODE_ADVANCE_AND_PUSH,0,0};
    memcpy(out,&defaults,0x20);
    return 0;
}
static ABI int32_t audio3d_port_open(int32_t user, const OpenParameters *in, uint32_t *id) {
    if (id) *id=0xFFFFFFFFu;
    if (user!=0xFF || !in || !id) return ERR_INVALID_PARAMETER;
    if (!initialized) return ERR_NOT_READY;
    OpenParameters e={0x28,in->granularity,in->rate,512,2,MODE_NO_ADVANCE,0,2};
    switch (in->size_this & ~UINT64_C(7)) {
    case 0x10: break;
    case 0x18: e.max_objects=in->max_objects; e.queue_depth=in->queue_depth; e.buffer_mode=MODE_ADVANCE_NO_PUSH; break;
    case 0x20: e.max_objects=in->max_objects; e.queue_depth=in->queue_depth; e.buffer_mode=in->buffer_mode; break;
    case 0x28: e.max_objects=in->max_objects; e.queue_depth=in->queue_depth; e.buffer_mode=in->buffer_mode; e.num_beds=in->num_beds; break;
    default: return ERR_INVALID_PARAMETER;
    }
    if (e.rate!=0 || e.granularity<0x100 || (e.granularity&0xFF) || !e.max_objects || !e.queue_depth ||
        e.queue_depth>MAX_DEPTH || e.buffer_mode>MODE_ADVANCE_AND_PUSH || (e.num_beds&~1u)!=2)
        return ERR_INVALID_PARAMETER;
    if (e.max_objects>MAX_OBJECTS) e.max_objects=MAX_OBJECTS;
    pthread_mutex_lock(&lock);
    int slot=-1;
    for (int i=0;i<PORTS && slot<0;++i) if (!ports[i].used) slot=i;
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_OUT_OF_RESOURCES; }
    Port *p=&ports[slot];
    p->objects=calloc(e.max_objects,sizeof(Object));
    if (!p->objects) { pthread_mutex_unlock(&lock); return ERR_OUT_OF_MEMORY; }
    p->used=1; p->audio_out=-1; p->granularity=e.granularity; p->depth=e.queue_depth; p->mode=e.buffer_mode;
    p->max_objects=e.max_objects;
    pthread_mutex_init(&p->lock,NULL);
    pthread_mutex_unlock(&lock);
    printf("Runtime: Audio3d port %d open (granularity %u, queue depth %u, mode %u, %u objects)\n",slot,e.granularity,
           e.queue_depth,e.buffer_mode,e.max_objects);
    *id=(uint32_t)slot;
    return 0;
}
static ABI int32_t audio3d_port_close(uint32_t id) {
    pthread_mutex_lock(&lock);
    Port *p=port_of(id);
    if (p) port_release(p);
    pthread_mutex_unlock(&lock);
    return p ? 0 : ERR_INVALID_PORT;
}
static ABI int32_t audio3d_port_set_attribute(uint32_t id, uint32_t attribute, const void *value, uint64_t size) {
    (void)attribute;
    if (!port_of(id)) return ERR_INVALID_PORT;
    return value && size ? 0 : ERR_INVALID_PARAMETER; /* reverb and spread settings have no stereo counterpart */
}
static ABI int32_t audio3d_bed_write(uint32_t id, uint32_t channels, uint32_t format, const void *buffer, uint32_t frames) {
    Port *p=port_of(id);
    if (!p) return ERR_INVALID_PORT;
    if (channels!=2 && channels!=6 && channels!=8) return ERR_INVALID_PARAMETER;
    static int reported;
    if (!reported++) printf("DEBUG_SAULO: Audio3d first bed write: %u channels, format %u, %u frames\n",channels,format,frames);
    pthread_mutex_lock(&p->lock);
    const int32_t r=enqueue(p,&p->bed,buffer,format,channels,frames,1);
    pthread_mutex_unlock(&p->lock);
    return r;
}
static ABI int32_t audio3d_object_reserve(uint32_t id, uint32_t *object) {
    if (!object) return ERR_INVALID_PARAMETER;
    Port *p=port_of(id);
    if (!p) return ERR_INVALID_PORT;
    pthread_mutex_lock(&p->lock);
    int32_t r=ERR_OUT_OF_RESOURCES;
    for (uint32_t i=0;i<p->max_objects;++i) if (!p->objects[i].reserved) {
        p->objects[i]=(Object){.reserved=1};
        *object=i; r=0; break;
    }
    pthread_mutex_unlock(&p->lock);
    return r;
}
static Object *object_of(Port *p, uint32_t object) {
    return object<p->max_objects && p->objects[object].reserved && !p->objects[object].release_when_empty ? &p->objects[object] : NULL;
}
static ABI int32_t audio3d_object_unreserve(uint32_t id, uint32_t object) {
    Port *p=port_of(id);
    if (!p) return ERR_INVALID_PORT;
    pthread_mutex_lock(&p->lock);
    Object *o=object_of(p,object);
    if (o) {
        if (o->pcm.count) o->release_when_empty=1;
        else o->reserved=0;
    }
    pthread_mutex_unlock(&p->lock);
    return o ? 0 : ERR_INVALID_OBJECT;
}
static ABI int32_t audio3d_object_set_attributes(uint32_t id, uint32_t object, uint64_t count, const Attribute *attributes) {
    Port *p=port_of(id);
    if (!p) return ERR_INVALID_PORT;
    if (!count || !attributes) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&p->lock);
    Object *o=object_of(p,object);
    int32_t r=o ? 0 : ERR_INVALID_OBJECT;
    for (uint64_t i=0;o && i<count;++i)
        if (attributes[i].id==ATTR_RESET_STATE) { queue_clear(&o->pcm); o->gain=0.0f; break; }
    for (uint64_t i=0;o && !r && i<count;++i) {
        const Attribute *a=&attributes[i];
        if (a->id==ATTR_GAIN && a->value && a->size>=sizeof(float)) memcpy(&o->gain,a->value,sizeof(float));
        else if (a->id==ATTR_PCM && a->value && a->size>=sizeof(Pcm)) {
            const Pcm *pcm=a->value;
            r=enqueue(p,&o->pcm,pcm->buffer,pcm->format,1,pcm->samples,0);
        }
    }
    pthread_mutex_unlock(&p->lock);
    return r;
}
/* One granule: the bed at unity and every object at its GAIN (0 until the game sets one), as stereo S16. */
static int32_t mix(Port *p) {
    const size_t samples=(size_t)p->granularity*2;
    float *sum=calloc(samples,sizeof(float));
    int16_t *out=malloc(samples*sizeof(int16_t));
    if (!sum || !out) { free(sum); free(out); return ERR_OUT_OF_MEMORY; }
    float *bed=queue_pop(&p->bed);
    for (size_t i=0;bed && i<samples;++i) sum[i]+=bed[i];
    free(bed);
    for (uint32_t i=0;i<p->max_objects;++i) {
        Object *o=&p->objects[i];
        float *g=o->reserved ? queue_pop(&o->pcm) : NULL;
        for (uint32_t f=0;g && f<p->granularity;++f) { sum[f*2]+=g[f]*o->gain; sum[f*2+1]+=g[f]*o->gain; }
        free(g);
        if (o->release_when_empty && !o->pcm.count) memset(o,0,sizeof(*o));
    }
    for (size_t i=0;i<samples;++i) {
        const float v=sum[i]<-1.0f ? -1.0f : sum[i]>1.0f ? 1.0f : sum[i];
        out[i]=(int16_t)(v*32767.0f);
    }
    free(sum);
    p->mixed[(p->mixed_head+p->mixed_count)%MAX_DEPTH]=out; ++p->mixed_count;
    ++granules_mixed;
    return 0;
}
static ABI int32_t audio3d_port_advance(uint32_t id) {
    Port *p=port_of(id);
    if (!p) return ERR_INVALID_PORT;
    if (p->mode==MODE_NO_ADVANCE) return ERR_NOT_SUPPORTED;
    pthread_mutex_lock(&p->lock);
    const int32_t r=p->mixed_count>=p->depth ? ERR_NOT_READY : mix(p);
    pthread_mutex_unlock(&p->lock);
    return r;
}
static ABI int32_t audio3d_port_queue_level(uint32_t id, uint32_t *level, uint32_t *available) {
    Port *p=port_of(id);
    if (!p) return ERR_INVALID_PORT;
    if (!level && !available) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&p->lock);
    if (level) *level=p->mixed_count;
    if (available) *available=p->mixed_count<p->depth ? p->depth-p->mixed_count : 0;
    pthread_mutex_unlock(&p->lock);
    return 0;
}
/* Plays the oldest mixed granule; AudioOut returns once per granule period, which paces the game's audio thread. */
static int32_t play_one(Port *p, int *played) {
    pthread_mutex_lock(&p->lock);
    int16_t *granule=NULL;
    if (p->mixed_count) { granule=p->mixed[p->mixed_head]; p->mixed_head=(p->mixed_head+1)%MAX_DEPTH; --p->mixed_count; }
    pthread_mutex_unlock(&p->lock);
    *played=granule!=NULL;
    if (!granule) return 0;
    const int32_t r=runtime_audio_out_output(p->audio_out,granule);
    free(granule);
    ++granules_played;
    return r<0 ? r : 0;
}
static int32_t ensure_output(Port *p) {
    if (p->audio_out>=0) return 0;
    const int32_t handle=runtime_audio_out_open(AUDIO_OUT_AUDIO3D,p->granularity,AUDIO_OUT_S16_STEREO);
    if (handle<0) return handle;
    p->audio_out=handle;
    return 0;
}
static ABI int32_t audio3d_port_push(uint32_t id, uint32_t blocking) {
    Port *p=port_of(id);
    if (!p) return ERR_INVALID_PORT;
    if (p->mode!=MODE_ADVANCE_AND_PUSH) return ERR_NOT_SUPPORTED;
    int32_t r=ensure_output(p);
    if (r) return r;
    for (;;) {
        pthread_mutex_lock(&p->lock);
        const int full=p->mixed_count>=p->depth;
        pthread_mutex_unlock(&p->lock);
        if (!full) return 0;
        int played=0;
        if ((r=play_one(p,&played)) || !played || !blocking) return r;
    }
}
static ABI int32_t audio3d_port_flush(uint32_t id) {
    Port *p=port_of(id);
    if (!p) return ERR_INVALID_PORT;
    int32_t r=ensure_output(p);
    for (int played=1; !r && played;) r=play_one(p,&played);
    return r;
}

static const RuntimeExport exports[]={
    {"sceAudio3dInitialize",audio3d_initialize}, {"sceAudio3dTerminate",audio3d_terminate},
    {"sceAudio3dGetDefaultOpenParameters",audio3d_default_parameters},
    {"sceAudio3dPortOpen",audio3d_port_open}, {"sceAudio3dPortClose",audio3d_port_close},
    {"sceAudio3dPortSetAttribute",audio3d_port_set_attribute}, {"sceAudio3dBedWrite",audio3d_bed_write},
    {"sceAudio3dObjectReserve",audio3d_object_reserve}, {"sceAudio3dObjectUnreserve",audio3d_object_unreserve},
    {"sceAudio3dObjectSetAttributes",audio3d_object_set_attributes},
    {"sceAudio3dPortAdvance",audio3d_port_advance}, {"sceAudio3dPortPush",audio3d_port_push},
    {"sceAudio3dPortFlush",audio3d_port_flush}, {"sceAudio3dPortGetQueueLevel",audio3d_port_queue_level},
};
uintptr_t runtime_audio3d_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_audio3d_report(void) {
    printf("Runtime: Audio3d granules mixed=%zu, played=%zu\n",granules_mixed,granules_played);
}
