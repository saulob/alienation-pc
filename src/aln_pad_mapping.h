#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The player's gamepad mapping file, beside the settings file; 0 when it cannot be named. */
static inline int aln_gamepad_db_path(const char *settings_file, char *out, size_t size) {
    static const char name[]="gamecontrollerdb.txt";
    if (!out || !size) return 0;
    out[0]=0;
    if (!settings_file || !*settings_file) return 0;
    const char *end=strrchr(settings_file,'/'), *back=strrchr(settings_file,'\\');
    if (back && (!end || back>end)) end=back;
    const size_t folder=end ? (size_t)(end-settings_file)+1 : 0;
    if (folder+sizeof(name)>size) return 0;
    memcpy(out,settings_file,folder);
    memcpy(out+folder,name,sizeof(name));
    return 1;
}

/* The joysticks already reported as having no gamepad mapping; bounded, the oldest is forgotten first. */
#define ALN_UNMAPPED_SEEN 16
typedef struct { uint32_t ids[ALN_UNMAPPED_SEEN]; size_t count; } AlnUnmappedSeen;

static inline int aln_unmapped_first_sight(AlnUnmappedSeen *seen, uint32_t id) {
    for (size_t i=0;i<seen->count;++i) if (seen->ids[i]==id) return 0;
    if (seen->count==ALN_UNMAPPED_SEEN) {
        memmove(seen->ids,seen->ids+1,(ALN_UNMAPPED_SEEN-1)*sizeof(*seen->ids));
        --seen->count;
    }
    seen->ids[seen->count++]=id;
    return 1;
}

/* A joystick that went away is forgotten, so one that comes back is reported again. */
static inline void aln_unmapped_forget_absent(AlnUnmappedSeen *seen, const uint32_t *present, size_t count) {
    size_t kept=0;
    for (size_t i=0;i<seen->count;++i) {
        int found=0;
        for (size_t j=0;j<count && !found;++j) found=present[j]==seen->ids[i];
        if (found) seen->ids[kept++]=seen->ids[i];
    }
    seen->count=kept;
}

/* Name (when SDL has one), GUID and VID:PID (when SDL knows them); no serial number, address or path. */
static inline void aln_unmapped_line(const char *name, const char *guid, unsigned vendor, unsigned product, char *line, size_t size) {
    char clean[96]="";
    if (name) {
        size_t k=0;
        for (const char *c=name;*c && k+1<sizeof(clean);++c) clean[k++]=(unsigned char)*c<0x20 || *c==0x7f ? ' ' : *c;
        clean[k]=0;
    }
    char ids[16]="";
    if (vendor || product) snprintf(ids,sizeof(ids),", %04x:%04x",vendor&0xffffu,product&0xffffu);
    snprintf(line,size,"Runtime: joystick without a gamepad mapping: %s%s(guid %s%s); add a line for this GUID to gamecontrollerdb.txt in the user folder",
             clean,clean[0] ? " " : "",guid ? guid : "",ids);
}
