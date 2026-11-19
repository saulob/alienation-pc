#pragma once
#include "aln_pad_axes.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Left-stick diagnostics for the log: one capture window of consecutive pad reads, summarized in two compact lines. */
#define ALN_PAD_DIAG_WINDOW 48
#define ALN_PAD_DIAG_MIN_FLIPS 4
#define ALN_PAD_DIAG_HOST_ACTIVE 8192

typedef enum {
    ALN_PAD_DIAG_NONE, ALN_PAD_DIAG_STABLE, ALN_PAD_DIAG_SOURCE_ALTERNATED, ALN_PAD_DIAG_GUEST_ALTERNATED,
    ALN_PAD_DIAG_GUEST_LOST_AXIS,
} AlnPadDiagVerdict;

typedef enum { STICK_NEUTRAL, STICK_X_ONLY, STICK_Y_ONLY, STICK_BOTH } StickShape;

typedef struct {
    bool capturing, stable_reported, anomaly_reported;
    int samples, host_both, guest_both, x_collapsed, y_collapsed, host_flips, guest_flips;
    int host_min[2], host_max[2], guest_min[2], guest_max[2];
    StickShape host_last, guest_last;
} AlnPadDiag;

static inline void aln_pad_diag_reset(AlnPadDiag *d) { memset(d,0,sizeof(*d)); }

static inline StickShape aln_pad_diag_shape(bool x, bool y) { return x && y ? STICK_BOTH : x ? STICK_X_ONLY : y ? STICK_Y_ONLY : STICK_NEUTRAL; }
static inline bool aln_pad_diag_flip(StickShape from, StickShape to) {
    return (from==STICK_X_ONLY && to==STICK_Y_ONLY) || (from==STICK_Y_ONLY && to==STICK_X_ONLY);
}
static inline void aln_pad_diag_span(int *low, int *high, int v, bool first) {
    if (first || v<*low) *low=v;
    if (first || v>*high) *high=v;
}

static inline AlnPadDiagVerdict aln_pad_diag_verdict(const AlnPadDiag *d) {
    if (d->host_flips>=ALN_PAD_DIAG_MIN_FLIPS) return ALN_PAD_DIAG_SOURCE_ALTERNATED;
    const bool host_stable=d->host_both*10>=d->samples*9;
    if (!host_stable) return ALN_PAD_DIAG_NONE;
    if (d->guest_flips>=ALN_PAD_DIAG_MIN_FLIPS) return ALN_PAD_DIAG_GUEST_ALTERNATED;
    return d->guest_both*10>=d->samples*9 ? ALN_PAD_DIAG_STABLE : ALN_PAD_DIAG_GUEST_LOST_AXIS;
}

/* One pad read: host axes as SDL gave them, guest bytes as the game gets them. Returns the verdict to log when a
 * window ends with something not yet logged for this controller, else NONE. */
static inline AlnPadDiagVerdict aln_pad_diag_sample(AlnPadDiag *d, int16_t host_x, int16_t host_y, uint8_t guest_x, uint8_t guest_y) {
    const int guest_threshold=aln_stick_axis(ALN_PAD_DIAG_HOST_ACTIVE)-128;
    const bool hx=abs(host_x)>=ALN_PAD_DIAG_HOST_ACTIVE, hy=abs(host_y)>=ALN_PAD_DIAG_HOST_ACTIVE;
    const bool gx=abs(guest_x-128)>=guest_threshold, gy=abs(guest_y-128)>=guest_threshold;
    const StickShape host=aln_pad_diag_shape(hx,hy), guest=aln_pad_diag_shape(gx,gy);
    if (!d->capturing) {
        if (host!=STICK_BOTH && !aln_pad_diag_flip(d->host_last,host)) { d->host_last=host; return ALN_PAD_DIAG_NONE; }
        const bool reported=d->stable_reported, anomaly=d->anomaly_reported;
        aln_pad_diag_reset(d);
        d->stable_reported=reported; d->anomaly_reported=anomaly;
        d->capturing=true;
        d->host_last=d->guest_last=STICK_NEUTRAL;
    }
    const bool first=d->samples==0;
    aln_pad_diag_span(&d->host_min[0],&d->host_max[0],host_x,first);
    aln_pad_diag_span(&d->host_min[1],&d->host_max[1],host_y,first);
    aln_pad_diag_span(&d->guest_min[0],&d->guest_max[0],guest_x,first);
    aln_pad_diag_span(&d->guest_min[1],&d->guest_max[1],guest_y,first);
    d->host_both+=host==STICK_BOTH;
    d->guest_both+=guest==STICK_BOTH;
    d->x_collapsed+=hx && !gx && gy;
    d->y_collapsed+=hy && !gy && gx;
    d->host_flips+=aln_pad_diag_flip(d->host_last,host);
    d->guest_flips+=aln_pad_diag_flip(d->guest_last,guest);
    d->host_last=host; d->guest_last=guest;
    if (++d->samples<ALN_PAD_DIAG_WINDOW) return ALN_PAD_DIAG_NONE;
    d->capturing=false;
    const AlnPadDiagVerdict verdict=aln_pad_diag_verdict(d);
    if (verdict==ALN_PAD_DIAG_NONE) return ALN_PAD_DIAG_NONE;
    if (verdict==ALN_PAD_DIAG_STABLE) {
        if (d->stable_reported) return ALN_PAD_DIAG_NONE;
        d->stable_reported=true;
        return verdict;
    }
    if (d->anomaly_reported) return ALN_PAD_DIAG_NONE;
    d->anomaly_reported=true;
    return verdict;
}

static inline void aln_pad_diag_summary(const AlnPadDiag *d, int player, char *line, size_t size) {
    snprintf(line,size,"PadDiag: player %d left-stick capture: samples=%d host_x=%d..%d host_y=%d..%d guest_x=%d..%d guest_y=%d..%d "
             "both_active=%d x_collapsed=%d y_collapsed=%d host_flips=%d guest_flips=%d host_alternated=%s guest_alternated=%s",
             player,d->samples,d->host_min[0],d->host_max[0],d->host_min[1],d->host_max[1],
             d->guest_min[0],d->guest_max[0],d->guest_min[1],d->guest_max[1],
             d->guest_both,d->x_collapsed,d->y_collapsed,d->host_flips,d->guest_flips,
             d->host_flips>=ALN_PAD_DIAG_MIN_FLIPS ? "yes" : "no",d->guest_flips>=ALN_PAD_DIAG_MIN_FLIPS ? "yes" : "no");
}

static inline void aln_pad_diag_verdict_line(AlnPadDiagVerdict verdict, int player, int samples, char *line, size_t size) {
    switch (verdict) {
    case ALN_PAD_DIAG_STABLE: snprintf(line,size,"PadDiag: player %d stable diagonal captured: host and guest both stable, samples=%d",player,samples); break;
    case ALN_PAD_DIAG_SOURCE_ALTERNATED: snprintf(line,size,"PadDiag: player %d left-stick source alternated before guest conversion, samples=%d",player,samples); break;
    case ALN_PAD_DIAG_GUEST_ALTERNATED: snprintf(line,size,"PadDiag: player %d left-stick diagonal anomaly: host stable, guest alternated X/Y, samples=%d",player,samples); break;
    case ALN_PAD_DIAG_GUEST_LOST_AXIS: snprintf(line,size,"PadDiag: player %d left-stick diagonal anomaly: host stable, guest lost an axis, samples=%d",player,samples); break;
    default: line[0]=0;
    }
}

/* Connection line from what SDL exposes; unknown fields (NULL, 0, -1) are left out. */
typedef struct {
    int player;
    const char *name, *type, *connection;
    unsigned vendor, product, version;
    int virtual_device;
    const char *steam_input;
} AlnPadDiagInfo;

static inline void aln_pad_diag_connect_line(const AlnPadDiagInfo *i, char *line, size_t size) {
    size_t n=(size_t)snprintf(line,size,"PadDiag: player %d connected:",i->player);
    char name[96]="";
    if (i->name) {
        size_t k=0;
        for (const char *c=i->name;*c && k+1<sizeof(name);++c) name[k++]=(unsigned char)*c<0x20 || *c==',' ? ' ' : *c;
        name[k]=0;
    }
    const char *sep=" ";
    #define PAD_DIAG_FIELD(...) do { if (n<size) n+=(size_t)snprintf(line+n,size-n,__VA_ARGS__); sep=", "; } while (0)
    if (name[0]) { PAD_DIAG_FIELD("%sname=%s",sep,name); }
    if (i->type && i->type[0]) PAD_DIAG_FIELD("%stype=%s",sep,i->type);
    if (i->vendor || i->product) { PAD_DIAG_FIELD("%svid=0x%04x, pid=0x%04x",sep,i->vendor,i->product); }
    if (i->version) PAD_DIAG_FIELD("%sversion=0x%04x",sep,i->version);
    if (i->connection && i->connection[0]) PAD_DIAG_FIELD("%sconnection=%s",sep,i->connection);
    if (i->virtual_device) PAD_DIAG_FIELD("%svirtual=yes",sep);
    if (i->steam_input) PAD_DIAG_FIELD("%ssteam_input=%s",sep,i->steam_input);
    #undef PAD_DIAG_FIELD
}
