/* Local co-op input without a window: SDL3 virtual gamepads (every physical joystick backend off) driven through the
 * port's libScePad and libSceUserService, the way the game's user manager and pad binder use them (docs/ONLINE.md
 * "Local co-op"): user 1 from the start, a LOGIN event when a controller first takes a slot, scePadOpen per logged-in
 * user, OPTIONS on another controller to join, up to four players. */
#include "runtime.h"
#include "aln_pad_axes.h"
#include "aln_pad_diag.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

#define BTN_OPTIONS 0x8u
#define BTN_CROSS 0x4000u
#define USER_NO_EVENT ((int32_t)0x80960007)
#define PAD_USER_NOT_LOGIN ((int32_t)0x809b0081)
#define PAD_ALREADY_OPENED ((int32_t)0x80920004)

/* What the runtime gets from the loader and the video core. */
const char *runtime_symbol(const char *nid) { return nid; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    for (size_t i=0;i<count;++i) if (!strcmp(nid,table[i].name)) return (uintptr_t)table[i].function;
    return 0;
}
int alngpu_overlay_captures_input(void) { return 0; }
void alngpu_choice_set_input(uint32_t (*buttons)(void)) { (void)buttons; }
int alngpu_latency_trace_enabled(void) { return 0; }
uint64_t alngpu_latency_press_ns(void) { return 0; }

typedef int32_t (ABI *Init)(void);
typedef int32_t (ABI *Open)(int32_t, int32_t, int32_t, const void *);
typedef int32_t (ABI *Read)(int32_t, uint8_t *);
typedef int32_t (ABI *Handle)(int32_t);
typedef int32_t (ABI *HandleOf)(int32_t, int32_t, int32_t);
typedef int32_t (ABI *Light)(int32_t, const uint8_t *);
typedef int32_t (ABI *Info)(int32_t, uint8_t *);
typedef int32_t (ABI *Ids)(int32_t *);
typedef int32_t (ABI *Name)(int32_t, char *, uint64_t);
typedef int32_t (ABI *Color)(int32_t, int32_t *);

static void *pad(const char *name) { void *f=(void *)runtime_pad_resolve(name); CHECK(f!=NULL); return f; }
static void *user(const char *name) { void *f=(void *)runtime_users_resolve(name); CHECK(f!=NULL); return f; }

typedef struct { SDL_JoystickID id; SDL_Joystick *joystick; } Virtual;

static Virtual attach(int n) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.vendor_id=0x054c;
    desc.product_id=(Uint16)(0x09cc+n);
    char name[32];
    snprintf(name,sizeof(name),"Virtual pad %d",n);
    desc.name=name;
    Virtual v={SDL_AttachVirtualJoystick(&desc),NULL};
    CHECK(v.id!=0);
    v.joystick=SDL_OpenJoystick(v.id);
    CHECK(v.joystick!=NULL);
    SDL_Delay(300); /* the runtime looks for new gamepads every 250 ms */
    return v;
}

static void press(Virtual v, SDL_GamepadButton button, bool down) {
    CHECK(SDL_SetJoystickVirtualButton(v.joystick,button,down));
    SDL_UpdateJoysticks();
}

static uint32_t buttons(int32_t handle, uint8_t *connected) {
    uint8_t state[120];
    memset(state,0xcc,sizeof(state));
    CHECK(((Read)pad("scePadReadState"))(handle,state)==0);
    if (connected) *connected=state[0x4c];
    uint32_t b;
    memcpy(&b,state,4);
    return b;
}

typedef struct { uint8_t lx, ly, rx, ry; } Sticks;

static Sticks sticks(int32_t handle) {
    uint8_t state[120];
    memset(state,0xcc,sizeof(state));
    CHECK(((Read)pad("scePadReadState"))(handle,state)==0);
    return (Sticks){state[4],state[5],state[6],state[7]};
}

static bool same_sticks(Sticks a, Sticks b) { return a.lx==b.lx && a.ly==b.ly && a.rx==b.rx && a.ry==b.ry; }

static void left_stick(Virtual v, int x, int y) {
    CHECK(SDL_SetJoystickVirtualAxis(v.joystick,SDL_GAMEPAD_AXIS_LEFTX,(Sint16)x));
    CHECK(SDL_SetJoystickVirtualAxis(v.joystick,SDL_GAMEPAD_AXIS_LEFTY,(Sint16)y));
    SDL_UpdateJoysticks();
}

static int sign(int v) { return v>0 ? 1 : v<0 ? -1 : 0; }
static int host_of(double fraction) { return (int)lround(fraction*32767.0); }

/* Issue #2: both left stick axes reach the game together; each guest axis depends on its own host axis only. */
static void left_stick_tests(Virtual v, int32_t handle) {
    static const int centre=128;
    left_stick(v,0,0);
    CHECK(same_sticks(sticks(handle),(Sticks){centre,centre,centre,centre}));

    static const struct { const char *name; int x, y; } full[]={
        {"up",0,-32768}, {"down",0,32767}, {"left",-32768,0}, {"right",32767,0},
        {"up-left",-32768,-32768}, {"up-right",32767,-32768}, {"down-left",-32768,32767}, {"down-right",32767,32767},
    };
    for (size_t i=0;i<sizeof(full)/sizeof(*full);++i) {
        left_stick(v,full[i].x,full[i].y);
        const Sticks s=sticks(handle);
        CHECK(s.lx==(full[i].x<0 ? 0 : full[i].x>0 ? 255 : centre));
        CHECK(s.ly==(full[i].y<0 ? 0 : full[i].y>0 ? 255 : centre));
        CHECK(s.rx==centre && s.ry==centre);
    }

    /* Half, asymmetric and every other diagonal: each axis equals what the same host axis gives alone. */
    static const double fractions[]={0.25,0.5,0.75,1.0};
    for (size_t i=0;i<sizeof(fractions)/sizeof(*fractions);++i) for (size_t j=0;j<sizeof(fractions)/sizeof(*fractions);++j)
        for (int sx=-1;sx<=1;sx+=2) for (int sy=-1;sy<=1;sy+=2) {
            const int hx=sx*host_of(fractions[i]), hy=sy*host_of(fractions[j]);
            left_stick(v,hx,0);
            const uint8_t alone_x=sticks(handle).lx;
            left_stick(v,0,hy);
            const uint8_t alone_y=sticks(handle).ly;
            left_stick(v,hx,hy);
            const Sticks s=sticks(handle);
            CHECK(s.lx==alone_x && s.ly==alone_y);
            CHECK(sign(s.lx-centre)==sx && sign(s.ly-centre)==sy);
        }
    /* 25 % left + 75 % up keeps its proportions, mirrored 75 % left + 25 % up. */
    left_stick(v,-host_of(0.25),-host_of(0.75));
    const Sticks a=sticks(handle);
    left_stick(v,-host_of(0.75),-host_of(0.25));
    const Sticks b=sticks(handle);
    CHECK(a.lx<centre && a.ly<centre && (centre-a.ly)>2*(centre-a.lx));
    CHECK(b.lx==a.ly && b.ly==a.lx);

    /* Dead zone: inside it both axes read centre together; just outside, a diagonal still moves both. */
    const int edge=(int)(ALN_STICK_DEADZONE*32767.f);
    left_stick(v,-edge,-edge);
    CHECK(same_sticks(sticks(handle),(Sticks){centre,centre,centre,centre}));
    left_stick(v,edge+700,-(edge+700));
    CHECK(sticks(handle).lx>centre && sticks(handle).ly<centre);
    left_stick(v,-32768,-(edge+700));
    CHECK(sticks(handle).lx==0 && sticks(handle).ly<centre);
    left_stick(v,-edge,-32768);
    CHECK(sticks(handle).lx==centre && sticks(handle).ly==0);

    /* The same diagonal held for 10,000 samples never changes either axis. */
    left_stick(v,-host_of(0.7071),-host_of(0.7071));
    const Sticks held=sticks(handle);
    CHECK(held.lx<centre && held.ly<centre);
    int unstable=0;
    for (int n=0;n<10000;++n) unstable+=!same_sticks(sticks(handle),held);
    CHECK(unstable==0);
    SDL_UpdateJoysticks();
    CHECK(same_sticks(sticks(handle),held));

    /* Host events arrive per axis, in any order: the published state keeps the latest value of both. */
    left_stick(v,-20000,-12000);
    const Sticks reference=sticks(handle);
    int event_failures=0;
    for (int n=0;n<400;++n) {
        const bool x_first=n%2==0;
        left_stick(v,0,0);
        CHECK(SDL_SetJoystickVirtualAxis(v.joystick,x_first ? SDL_GAMEPAD_AXIS_LEFTX : SDL_GAMEPAD_AXIS_LEFTY,(Sint16)(x_first ? -20000 : -12000)));
        SDL_UpdateJoysticks();
        const Sticks first=sticks(handle);
        event_failures+=x_first ? !(first.lx==reference.lx && first.ly==centre) : !(first.ly==reference.ly && first.lx==centre);
        CHECK(SDL_SetJoystickVirtualAxis(v.joystick,x_first ? SDL_GAMEPAD_AXIS_LEFTY : SDL_GAMEPAD_AXIS_LEFTX,(Sint16)(x_first ? -12000 : -20000)));
        SDL_UpdateJoysticks();
        event_failures+=!same_sticks(sticks(handle),reference);
    }
    CHECK(event_failures==0);
    left_stick(v,0,0);

    /* A stick circle in 45 degree steps: no sector collapses to a cardinal direction. */
    static const double cosine[8]={1,0.70710678,0,-0.70710678,-1,-0.70710678,0,0.70710678};
    static const double sine[8]={0,0.70710678,1,0.70710678,0,-0.70710678,-1,-0.70710678};
    for (int n=0;n<8;++n) {
        const int hx=host_of(cosine[n]), hy=host_of(sine[n]);
        left_stick(v,hx,hy);
        const Sticks s=sticks(handle);
        CHECK(sign(s.lx-centre)==sign(hx) && sign(s.ly-centre)==sign(hy));
        left_stick(v,-hx,-hy);
        const Sticks opposite=sticks(handle);
        CHECK(abs((s.lx-centre)+(opposite.lx-centre))<=1 && abs((s.ly-centre)+(opposite.ly-centre))<=1);
        if (hx && hy) CHECK(abs(abs(s.lx-centre)-abs(s.ly-centre))<=1);
    }

    left_stick(v,0,0);
}

/* Keyboard stick keys: simultaneous keys on both axes give the same diagonals, and a stick follows them only while held. */
static void key_stick_tests(void) {
    static const struct { bool left, right, up, down; int x, y; } keys[]={
        {0,0,0,0,128,128}, {0,0,1,0,128,0}, {0,0,0,1,128,255}, {1,0,0,0,0,128}, {0,1,0,0,255,128},
        {1,0,1,0,0,0}, {0,1,1,0,255,0}, {1,0,0,1,0,255}, {0,1,0,1,255,255}, {1,1,1,1,127,127},
    };
    for (size_t i=0;i<sizeof(keys)/sizeof(*keys);++i) {
        uint8_t x=128, y=128;
        aln_key_axis(&x,keys[i].left,keys[i].right);
        aln_key_axis(&y,keys[i].up,keys[i].down);
        CHECK(x==keys[i].x && y==keys[i].y);
    }
    uint8_t held=71;
    aln_key_axis(&held,false,false);
    CHECK(held==71);
}

/* Issue #2 diagnostics: the lines the runtime logs, and the real classifier fed synthetic pad reads. */
static char diag_lines[512][400];
static int diag_count;
static void diag_sink(const char *line) { if (diag_count<512) snprintf(diag_lines[diag_count++],sizeof(diag_lines[0]),"%s",line); }

static int diag_lines_with(const char *player, const char *text) {
    int n=0;
    for (int i=0;i<diag_count;++i) n+=strstr(diag_lines[i],player) && strstr(diag_lines[i],text);
    return n;
}

typedef struct { int reports, stable, source, guest, lost; } DiagCount;

static void diag_feed(AlnPadDiag *d, DiagCount *c, int hx, int hy, int gx, int gy) {
    switch (aln_pad_diag_sample(d,(int16_t)hx,(int16_t)hy,(uint8_t)gx,(uint8_t)gy)) {
    case ALN_PAD_DIAG_STABLE: ++c->stable; ++c->reports; break;
    case ALN_PAD_DIAG_SOURCE_ALTERNATED: ++c->source; ++c->reports; break;
    case ALN_PAD_DIAG_GUEST_ALTERNATED: ++c->guest; ++c->reports; break;
    case ALN_PAD_DIAG_GUEST_LOST_AXIS: ++c->lost; ++c->reports; break;
    default: break;
    }
}

static DiagCount diag_run(AlnPadDiag *d, int samples, int hx, int hy) {
    DiagCount c={0};
    for (int n=0;n<samples;++n) diag_feed(d,&c,hx,hy,aln_stick_axis((int16_t)hx),aln_stick_axis((int16_t)hy));
    return c;
}

static void diag_tests(void) {
    static const int corner[4][2]={{-30000,-30000},{30000,-30000},{-30000,30000},{30000,30000}};
    for (int i=0;i<4;++i) {
        AlnPadDiag d; aln_pad_diag_reset(&d);
        const DiagCount c=diag_run(&d,ALN_PAD_DIAG_WINDOW*3,corner[i][0],corner[i][1]);
        CHECK(c.stable==1 && c.reports==1);
    }

    /* The host alternates X-only and Y-only: the source, not the conversion. */
    AlnPadDiag d; aln_pad_diag_reset(&d);
    DiagCount c={0};
    for (int n=0;n<ALN_PAD_DIAG_WINDOW*2;++n) {
        const int hx=n%2 ? 0 : -30000, hy=n%2 ? -30000 : 0;
        diag_feed(&d,&c,hx,hy,aln_stick_axis((int16_t)hx),aln_stick_axis((int16_t)hy));
    }
    CHECK(c.source==1 && c.reports==1);

    /* A stable host diagonal while the guest alternates left only and up only: the exact reported failure. */
    aln_pad_diag_reset(&d); c=(DiagCount){0};
    for (int n=0;n<ALN_PAD_DIAG_WINDOW*3;++n) diag_feed(&d,&c,-30000,-30000,n%2 ? 128 : 0,n%2 ? 0 : 128);
    CHECK(c.guest==1 && c.reports==1);
    char line[384];
    aln_pad_diag_verdict_line(ALN_PAD_DIAG_GUEST_ALTERNATED,1,d.samples,line,sizeof(line));
    CHECK(strstr(line,"player 1 left-stick diagonal anomaly: host stable, guest alternated X/Y"));
    aln_pad_diag_summary(&d,1,line,sizeof(line));
    CHECK(strstr(line,"host_alternated=no guest_alternated=yes") && strstr(line,"samples=48"));

    /* A guest that keeps one axis at centre without alternating is also flagged. */
    aln_pad_diag_reset(&d); c=(DiagCount){0};
    for (int n=0;n<ALN_PAD_DIAG_WINDOW;++n) diag_feed(&d,&c,-30000,-30000,0,128);
    CHECK(c.lost==1 && c.reports==1);

    /* Noise inside the dead zone, and steady cardinal movement, capture nothing. */
    aln_pad_diag_reset(&d); c=(DiagCount){0};
    for (int n=0;n<2000;++n) {
        const int noise=n%2 ? 2000 : -2000;
        diag_feed(&d,&c,noise,-noise,aln_stick_axis((int16_t)noise),aln_stick_axis((int16_t)-noise));
    }
    CHECK(c.reports==0 && !d.capturing && d.samples==0);
    c=diag_run(&d,2000,-32768,0);
    CHECK(c.reports==0 && !d.capturing && d.samples==0);
    aln_pad_diag_reset(&d);
    c=diag_run(&d,2000,0,32767);
    CHECK(c.reports==0 && !d.capturing && d.samples==0);

    /* A stable diagonal reports once however long it is held; a later anomaly is allowed once more. */
    aln_pad_diag_reset(&d);
    c=diag_run(&d,10000,-30000,-30000);
    CHECK(c.stable==1 && c.reports==1);
    c=(DiagCount){0};
    for (int n=0;n<10000;++n) diag_feed(&d,&c,-30000,-30000,n%2 ? 128 : 0,n%2 ? 0 : 128);
    CHECK(c.guest==1 && c.reports==1);

    /* A disconnect resets that controller only. */
    AlnPadDiag other; aln_pad_diag_reset(&other);
    aln_pad_diag_reset(&d);
    c=diag_run(&d,100,-30000,-30000);
    DiagCount o=diag_run(&other,100,30000,30000);
    CHECK(c.stable==1 && o.stable==1);
    aln_pad_diag_reset(&d);
    c=diag_run(&d,100,-30000,-30000);
    o=diag_run(&other,100,30000,30000);
    CHECK(c.stable==1 && o.stable==0);

    /* Metadata lines: name, type and VID:PID when known; nothing invented when not. */
    AlnPadDiagInfo info={.player=2,.name="Xbox Wireless Controller",.type="xboxone",.connection="wireless",.vendor=0x045e,.product=0x0b13,.version=0x0509};
    aln_pad_diag_connect_line(&info,line,sizeof(line));
    CHECK(!strcmp(line,"PadDiag: player 2 connected: name=Xbox Wireless Controller, type=xboxone, vid=0x045e, pid=0x0b13, version=0x0509, connection=wireless"));
    info=(AlnPadDiagInfo){.player=1};
    aln_pad_diag_connect_line(&info,line,sizeof(line));
    CHECK(!strcmp(line,"PadDiag: player 1 connected:"));
    info=(AlnPadDiagInfo){.player=3,.name="Pad, with	tab",.vendor=0x28de,.product=0x11ff,.steam_input="yes"};
    aln_pad_diag_connect_line(&info,line,sizeof(line));
    CHECK(!strcmp(line,"PadDiag: player 3 connected: name=Pad  with tab, vid=0x28de, pid=0x11ff, steam_input=yes"));
    char tiny[20];
    aln_pad_diag_connect_line(&info,tiny,sizeof(tiny));
    CHECK(strlen(tiny)<sizeof(tiny));
}

static int logins(int32_t *users, int max) {
    int n=0;
    int32_t event[2];
    while (((Ids)user("sceUserServiceGetEvent"))(event)==0) {
        CHECK(event[0]==0);
        if (n<max) users[n++]=event[1];
    }
    return n;
}

static int logged_in(void) {
    int32_t ids[4];
    CHECK(((Ids)user("sceUserServiceGetLoginUserIdList"))(ids)==0);
    int n=0;
    for (int i=0;i<4;++i) if (ids[i]!=-1) CHECK(ids[i]==++n);
    return n;
}

int main(void) {
    static const char *const backends[]={"SDL_JOYSTICK_HIDAPI","SDL_JOYSTICK_RAWINPUT","SDL_JOYSTICK_DIRECTINPUT",
                                         "SDL_XINPUT_ENABLED","SDL_JOYSTICK_WGI","SDL_JOYSTICK_GAMEINPUT","SDL_JOYSTICK_MFI",
                                         "SDL_JOYSTICK_LINUX_CLASSIC","SDL_JOYSTICK_LINUX_DEADZONES"};
    for (size_t i=0;i<sizeof(backends)/sizeof(*backends);++i) SDL_SetHint(backends[i],"0");
    SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS","1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        printf("local pad tests: SDL gamepad subsystem unavailable (%s)\n",SDL_GetError());
        return 1;
    }
    runtime_pad_set_diag_sink(diag_sink);
    CHECK(((Init)pad("scePadInit"))()==0);

    /* Enumeration at boot: the initial user only, with its LOGIN; the keyboard stands in for player 1's pad. */
    int32_t initial=0;
    CHECK(((Ids)user("sceUserServiceGetInitialUser"))(&initial)==0 && initial==1);
    CHECK(logged_in()==1);
    int32_t users[8];
    CHECK(logins(users,8)==1 && users[0]==1);
    const int32_t p1=((Open)pad("scePadOpen"))(1,0,0,NULL);
    CHECK(p1==1);
    CHECK(((Open)pad("scePadOpen"))(1,0,0,NULL)==PAD_ALREADY_OPENED);
    CHECK(((Open)pad("scePadOpen"))(2,0,0,NULL)==PAD_USER_NOT_LOGIN);
    uint8_t connected=0;
    buttons(p1,&connected);
    CHECK(connected==1);

    /* The first gamepad is player 1's: no new user. */
    Virtual a=attach(1);
    buttons(p1,&connected);
    CHECK(connected==1 && logins(users,8)==0 && logged_in()==1);
    press(a,SDL_GAMEPAD_BUTTON_SOUTH,true);
    CHECK(buttons(p1,NULL)&BTN_CROSS);
    press(a,SDL_GAMEPAD_BUTTON_SOUTH,false);

    /* Two local controllers: user 2 logs in, the game opens its pad, and OPTIONS on it is the second player's join. */
    Virtual b=attach(2);
    buttons(p1,NULL);
    CHECK(logins(users,8)==1 && users[0]==2 && logged_in()==2);
    const int32_t p2=((Open)pad("scePadOpen"))(2,0,0,NULL);
    CHECK(p2==2);
    press(b,SDL_GAMEPAD_BUTTON_START,true);
    CHECK((buttons(p2,&connected)&BTN_OPTIONS) && connected==1);
    CHECK(!(buttons(p1,NULL)&BTN_OPTIONS));
    press(b,SDL_GAMEPAD_BUTTON_START,false);
    char name[32];
    int32_t color=-1;
    CHECK(((Name)user("sceUserServiceGetUserName"))(2,name,sizeof(name))==0 && !strcmp(name,"Player 2"));
    CHECK(((Color)user("sceUserServiceGetUserColor"))(2,&color)==0 && color==1);
    CHECK(runtime_pad_slot_human(1) && !runtime_pad_slot_human(2));

    /* Three and four local controllers. */
    Virtual c=attach(3);
    buttons(p1,NULL);
    CHECK(logins(users,8)==1 && users[0]==3 && logged_in()==3);
    const int32_t p3=((Open)pad("scePadOpen"))(3,0,0,NULL);
    CHECK(p3==3);
    Virtual d=attach(4);
    buttons(p1,NULL);
    CHECK(logins(users,8)==1 && users[0]==4 && logged_in()==4);
    const int32_t p4=((Open)pad("scePadOpen"))(4,0,0,NULL);
    CHECK(p4==4);
    press(d,SDL_GAMEPAD_BUTTON_START,true);
    CHECK((buttons(p4,NULL)&BTN_OPTIONS) && !(buttons(p3,NULL)&BTN_OPTIONS) && !(buttons(p2,NULL)&BTN_OPTIONS));
    press(d,SDL_GAMEPAD_BUTTON_START,false);
    CHECK(((Color)user("sceUserServiceGetUserColor"))(4,&color)==0 && color==3);
    CHECK(runtime_pad_slot_human(3));

    /* Pad N is user N's, and reads only controller N: a button on one shows on its pad and on no other. */
    {
        const Virtual devices[4]={a,b,c,d};
        const int32_t handles[4]={p1,p2,p3,p4};
        CHECK(a.id!=b.id && a.id!=c.id && a.id!=d.id && b.id!=c.id && b.id!=d.id && c.id!=d.id);
        for (int n=0;n<4;++n) {
            CHECK(((HandleOf)pad("scePadGetHandle"))(n+1,0,0)==handles[n] && handles[n]==n+1);
            press(devices[n],SDL_GAMEPAD_BUTTON_SOUTH,true);
            for (int m=0;m<4;++m) CHECK(((buttons(handles[m],NULL)&BTN_CROSS)!=0)==(m==n));
            press(devices[n],SDL_GAMEPAD_BUTTON_SOUTH,false);
        }
    }
    /* Issue #2: left stick diagonals on player 1 and on another pad, each pad's stick its own. */
    left_stick_tests(a,p1);
    left_stick(b,-host_of(0.7071),-host_of(0.7071));
    CHECK(sticks(p1).lx==128 && sticks(p1).ly==128 && sticks(p2).lx<128 && sticks(p2).ly<128);
    left_stick(b,0,0);
    left_stick_tests(b,p2);
    key_stick_tests();
    diag_tests();

    /* The runtime's own lines: a 10,000-read diagonal logs one capture per pad, and the pads stay apart. */
    CHECK(diag_lines_with("player 1 connected","vid=0x054c")==1 && diag_lines_with("player 2 connected","name=Virtual pad 2")==1);
    CHECK(diag_lines_with("platform=","sdl=")==1 && diag_lines_with("player 1 connected","virtual=yes")==1);
    CHECK(diag_lines_with("player 1","anomaly")==0 && diag_lines_with("player 2","anomaly")==0);
    CHECK(diag_lines_with("player 1","stable diagonal captured")<=1);
    CHECK(diag_lines_with("player 1","")<=5);
    {
        const int before=diag_count;
        left_stick(c,-30000,-30000);
        left_stick(d,30000,30000);
        for (int n=0;n<10000;++n) { sticks(p3); sticks(p4); }
        CHECK(diag_count-before==4);
        CHECK(diag_lines_with("player 3","stable diagonal captured")==1 && diag_lines_with("player 4","stable diagonal captured")==1);
        CHECK(diag_lines_with("player 3","guest_flips=0")==1 && diag_lines_with("player 4","host_flips=0")==1);
        left_stick(c,0,0);
        left_stick(d,0,0);
    }
    /* Later scans never give a controller that has a slot a second one. */
    SDL_Delay(600);
    buttons(p1,NULL);
    CHECK(logins(users,8)==0 && logged_in()==4);

    /* A fifth controller: the game supports four local players, so nobody new logs in. */
    Virtual e=attach(5);
    buttons(p1,NULL);
    CHECK(logins(users,8)==0 && logged_in()==4);
    CHECK(((Open)pad("scePadOpen"))(5,0,0,NULL)!=5);

    /* A controller that disconnects leaves its user logged in, reported disconnected; reconnected, the same user. (A
     * spare fifth controller would take the free slot at once, so it goes first.) */
    CHECK(SDL_DetachVirtualJoystick(e.id));
    SDL_UpdateJoysticks();
    CHECK(SDL_DetachVirtualJoystick(c.id));
    SDL_UpdateJoysticks();
    buttons(p3,&connected);
    CHECK(connected==0 && logged_in()==4 && logins(users,8)==0);
    uint8_t info[28];
    CHECK(((Info)pad("scePadGetControllerInformation"))(p3,info)==0 && info[12]==0);
    Virtual c2=attach(6);
    buttons(p3,&connected);
    CHECK(connected==1 && logins(users,8)==0);
    CHECK(((Info)pad("scePadGetControllerInformation"))(p3,info)==0 && info[12]==1);
    left_stick(c2,30000,-30000);
    for (int n=0;n<200;++n) sticks(p3);
    CHECK(diag_lines_with("player 3","stable diagonal captured")==2 && diag_lines_with("player 4","stable diagonal captured")==1);
    CHECK(diag_lines_with("player 3 connected","name=Virtual pad 6")==1);
    left_stick(c2,0,0);

    /* The light bar follows the game's per-player colour; a reset gives the user's own. */
    const uint8_t red[4]={255,0,0,0};
    CHECK(((Light)pad("scePadSetLightBar"))(p2,red)==0);
    CHECK(((Handle)pad("scePadResetLightBar"))(p2)==0);
    CHECK(((Light)pad("scePadSetLightBar"))(9,red)!=0);
    CHECK(((Handle)pad("scePadClose"))(p4)==0);
    CHECK(((Open)pad("scePadOpen"))(4,0,0,NULL)==4);

    SDL_CloseJoystick(a.joystick); SDL_CloseJoystick(b.joystick); SDL_CloseJoystick(d.joystick);
    SDL_CloseJoystick(e.joystick); SDL_CloseJoystick(c2.joystick); SDL_CloseJoystick(c.joystick);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    printf("%s (%d checks, %d failures)\n",failures ? "local pad tests FAILED" : "local pad tests OK",checks,failures);
    return failures ? 1 : 0;
}
