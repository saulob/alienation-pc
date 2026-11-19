#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define ALN_STICK_DEADZONE 0.10f

/* An SDL stick axis (signed 16 bit) as a PS4 stick byte: 128 is centre, the dead zone is rescaled away. */
static inline uint8_t aln_stick_axis(int16_t v) {
    float f=(float)v/32767.f;
    if (f>1) f=1;
    if (f<-1) f=-1;
    f=fabsf(f)<=ALN_STICK_DEADZONE ? 0.f : (f>0 ? f-ALN_STICK_DEADZONE : f+ALN_STICK_DEADZONE)/(1.f-ALN_STICK_DEADZONE);
    int x=(int)lroundf(f*127.5f+127.5f);
    return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x);
}

/* A stick axis from two keys: it follows the keys only while one is held. */
static inline void aln_key_axis(uint8_t *axis, bool negative, bool positive) {
    if (negative || positive) *axis=(uint8_t)(128-(negative ? 128 : 0)+(positive ? 127 : 0));
}
