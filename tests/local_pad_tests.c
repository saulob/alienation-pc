/* Local co-op input without a window: SDL3 virtual gamepads (every physical joystick backend off) driven through the
 * port's libScePad and libSceUserService, the way the game's user manager and pad binder use them (docs/ONLINE.md
 * "Local co-op"): user 1 from the start, a LOGIN event when a controller first takes a slot, scePadOpen per logged-in
 * user, OPTIONS on another controller to join, up to four players. */
#include "runtime.h"
#include <SDL3/SDL.h>
#include <stdio.h>
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
