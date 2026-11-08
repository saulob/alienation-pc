/* Focused tests of libSceIme's keyboard functions, resolved the way the loader binds the game's imports. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

static int checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

#define IME_BUSY ((int32_t)0x80BC0001)
#define IME_NOT_OPENED ((int32_t)0x80BC0002)
#define IME_INVALID_USER_ID ((int32_t)0x80BC0010)
#define IME_INVALID_OPTION ((int32_t)0x80BC0015)
#define IME_INVALID_HANDLER ((int32_t)0x80BC0022)
#define IME_INVALID_ADDRESS ((int32_t)0x80BC0031)
#define IME_INVALID_RESERVED ((int32_t)0x80BC0032)

typedef struct { uint32_t option; uint8_t reserved1[4]; void *arg; void *handler; uint8_t reserved2[8]; } KeyboardParam;
typedef int32_t (ABI *OpenFn)(int32_t user, const KeyboardParam *param);
typedef int32_t (ABI *CloseFn)(int32_t user);
typedef int32_t (ABI *UpdateFn)(const void *handler);

static void ABI never_called(void *arg, const void *event) { (void)arg; (void)event; ++failures; }

static void *resolve(const char *nid, const char *name) {
    void *function = (void *)runtime_resolve(nid, 0);
    CHECK(function != NULL && !strcmp(runtime_import_name(nid), name));
    return function;
}

int main(void) {
    runtime_start(1);
    OpenFn open_keyboard = (OpenFn)resolve("eaFXjfJv3xs#libSceIme", "sceImeKeyboardOpen");
    CloseFn close_keyboard = (CloseFn)resolve("PMVehSlfZ94#libSceIme", "sceImeKeyboardClose");
    UpdateFn update = (UpdateFn)resolve("-4GCfYdNF1s#libSceIme", "sceImeUpdate");
    if (!open_keyboard || !close_keyboard || !update) return 1;

    KeyboardParam param = {0};
    param.handler = (void *)never_called;

    CHECK(update(never_called) == IME_NOT_OPENED);
    CHECK(close_keyboard(1) == IME_NOT_OPENED);
    CHECK(open_keyboard(1, NULL) == IME_INVALID_ADDRESS);

    KeyboardParam no_handler = {0};
    CHECK(open_keyboard(1, &no_handler) == IME_INVALID_HANDLER);

    KeyboardParam bad_option = param;
    bad_option.option = 0x40;
    CHECK(open_keyboard(1, &bad_option) == IME_INVALID_OPTION);

    KeyboardParam bad_reserved = param;
    bad_reserved.reserved2[7] = 1;
    CHECK(open_keyboard(1, &bad_reserved) == IME_INVALID_RESERVED);
    bad_reserved = param;
    bad_reserved.reserved1[0] = 1;
    CHECK(open_keyboard(1, &bad_reserved) == IME_INVALID_RESERVED);

    CHECK(open_keyboard(-1, &param) == IME_INVALID_USER_ID);
    CHECK(open_keyboard(2, &param) == IME_INVALID_USER_ID);
    CHECK(close_keyboard(-1) == IME_INVALID_USER_ID);
    CHECK(update(never_called) == IME_NOT_OPENED);

    param.option = 0x3F;
    CHECK(open_keyboard(1, &param) == 0);
    CHECK(open_keyboard(1, &param) == IME_BUSY);
    CHECK(update(never_called) == 0);
    CHECK(update(NULL) == 0);
    CHECK(close_keyboard(1) == 0);
    CHECK(close_keyboard(1) == IME_NOT_OPENED);
    CHECK(update(never_called) == IME_NOT_OPENED);
    CHECK(open_keyboard(1, &param) == 0);
    CHECK(close_keyboard(1) == 0);

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
