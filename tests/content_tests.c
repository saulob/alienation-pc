#define _GNU_SOURCE
#include "runtime.h"
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define make_dir(p) _mkdir(p)
#define change_dir(p) _chdir(p)
#define current_dir(p, n) _getcwd(p, n)
#define remove_dir(p) _rmdir(p)
#else
#include <unistd.h>
#define make_dir(p) mkdir(p, 0700)
#define change_dir(p) chdir(p)
#define current_dir(p, n) getcwd(p, n)
#define remove_dir(p) rmdir(p)
#endif
static char test_directory[512], original_directory[4096];
static int event_queue_full;
static void cleanup(void) {
    remove("content-test-data/probe.txt");
    remove_dir("content-test-data");
    remove("content-test-user/data/rename-source.txt");
    remove_dir("content-test-user/temp0");
    remove_dir("content-test-user/download0");
    remove_dir("content-test-user/data");
    remove_dir("content-test-user");
    change_dir(original_directory);
    remove_dir(test_directory);
}
static int checks, failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(c)) {                                                                                                    \
            ++failures;                                                                                                \
            printf("FAIL %d: %s\n", __LINE__, #c);                                                                     \
        }                                                                                                              \
    } while (0)
static int32_t error_slot;
static int entitlement_events;
int32_t runtime_user_initial_id(void) { return 1; }
int runtime_system_event_push(const void *event, size_t size) {
    uint32_t type, user, service;
    CHECK(size == 0xc0);
    memcpy(&type, event, 4);
    memcpy(&user, (const char *)event + 4, 4);
    memcpy(&service, (const char *)event + 8, 4);
    CHECK(type == 0x10000003 && user == 1 && service == 0);
    ++entitlement_events;
    return !event_queue_full;
}
int32_t *runtime_errno(void) { return &error_slot; }
int32_t runtime_guest_errno(int host) { return host; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(table[i].name, name))
            return (uintptr_t)table[i].function;
    return 0;
}
typedef struct {
    char label[17], padding[3];
    uint32_t status;
} Info;
typedef int32_t(ABI *Load)(uint16_t);
typedef int32_t(ABI *Init)(const void *, void *);
typedef int32_t(ABI *List)(uint32_t, void *, uint32_t, uint32_t *);
typedef int32_t(ABI *Get)(uint32_t, const void *, void *);
typedef int32_t(ABI *Mount)(uint32_t, const void *, void *);
typedef int32_t(ABI *Enqueue)(uint32_t, const void *);
int main(int argc, char **argv) {
    const int empty = argc == 2 && !strcmp(argv[1], "--empty");
    const int limit = argc == 2 && !strcmp(argv[1], "--limit");
    event_queue_full = argc == 2 && !strcmp(argv[1], "--queue-full");
    if (!current_dir(original_directory, sizeof(original_directory)))
        return 1;
#ifdef _WIN32
    char temporary[512];
    if (!GetTempPathA(sizeof(temporary), temporary) || !GetTempFileNameA(temporary, "aln", 0, test_directory))
        return 1;
    if (remove(test_directory) || make_dir(test_directory))
        return 1;
#else
    strcpy(test_directory, "/tmp/aln-content-XXXXXX");
    if (!mkdtemp(test_directory))
        return 1;
#endif
    if (change_dir(test_directory))
        return 1;
    atexit(cleanup);
    make_dir("content-test-data");
    FILE *f = fopen("content-test-data/probe.txt", "wb");
    CHECK(f != NULL);
    if (f) {
        fputs("DLC payload", f);
        fclose(f);
    }
    runtime_file_configure("content-test-data", "content-test-user");
    uint32_t parameters[5] = {3, 0, 0, 0, 0};
    runtime_content_configure(parameters);
    CHECK(runtime_content_addon("../INVALIDLABEL00", "content-test-data") != 0);
    if (!empty) {
        for (unsigned i = 0; i < (limit ? 256u : 19u); ++i) {
            char label[17];
            snprintf(label, sizeof(label), "TESTDLC%09u", i);
            CHECK(!runtime_content_addon(label, "content-test-data"));
        }
        CHECK(runtime_content_addon("TESTDLC000000000", "content-test-data") != 0);
        if (limit)
            CHECK(runtime_content_addon("TESTDLC000000256", "content-test-data") != 0);
    }
    Load load = (Load)runtime_content_resolve("g8cM39EUZ6o#libSceSysmodule");
    Init init = (Init)runtime_content_resolve("R9lA82OraNs#libSceAppContent");
    List list = (List)runtime_content_resolve("xnd8BJzAxmk#libSceAppContent");
    Get get = (Get)runtime_content_resolve("m47juOmH0VE#libSceAppContent");
    Mount mount = (Mount)runtime_content_resolve("VANhIWcqYak#libSceAppContent");
    Enqueue enqueue = (Enqueue)runtime_content_resolve("7gxh+5QubhY#libSceAppContent");
    CHECK(load && init && list && get && mount && enqueue);
    unsigned char reserved[40] = {0}, boot[40];
    CHECK(!load(0xb4));
    CHECK(!init(reserved, boot));
    CHECK(entitlement_events == (empty ? 0 : 1));
    if (limit) {
        uint32_t hits;
        char point[16];
        CHECK(!list(0, NULL, 0, &hits) && hits == 256);
        for (unsigned i = 0; i < 256; ++i) {
            char label[17];
            snprintf(label, sizeof(label), "TESTDLC%09u", i);
            CHECK(!mount(0, label, point));
        }
        CHECK(!strcmp(point, "/addcont255"));
        printf("%d checks, %d failures (256 DLC limit)\n", checks, failures);
        return failures ? 1 : 0;
    }
    if (empty) {
        uint32_t hits = 999;
        Info info;
        CHECK(!list(0, NULL, 0, &hits) && hits == 0);
        CHECK(get(0, "UNKNOWN000000000", &info) != 0);
        printf("%d checks, %d failures (no DLC installed)\n", checks, failures);
        return failures ? 1 : 0;
    }
    CHECK(runtime_content_addon("TESTDLC999999999", "content-test-data") != 0);
    uint32_t hits = 999;
    CHECK(!list(0, NULL, 0, &hits) && hits == 19);
    CHECK(!list(1, NULL, 0, &hits) && hits == 0);
    CHECK(list(8, NULL, 0, &hits) != 0);
    CHECK(list(0, NULL, 0, NULL) != 0);
    struct {
        Info entries[2];
        unsigned char guard[24];
    } bounded;
    memset(&bounded, 0xaa, sizeof(bounded));
    CHECK(!list(0, bounded.entries, 2, &hits) && hits == 2);
    Info many[24];
    memset(many, 0xaa, sizeof(many));
    CHECK(!list(0, many, 24, &hits) && hits == 19);
    CHECK((unsigned char)many[19].label[0] == 0xaa);
    CHECK(!strcmp(bounded.entries[0].label, "TESTDLC000000000") && bounded.entries[0].status == 4);
    for (unsigned i = 0; i < sizeof(bounded.guard); ++i)
        CHECK(bounded.guard[i] == 0xaa);
    Info info;
    CHECK(!get(0, "TESTDLC000000018", &info) && info.status == 4);
    CHECK(get(1, "TESTDLC000000018", &info) != 0);
    CHECK(get(0, "UNKNOWN000000000", &info) != 0);
    CHECK(!enqueue(0, "TESTDLC000000018"));
    CHECK(enqueue(0, "UNKNOWN000000000") != 0);
    for (unsigned i = 0; i < 19; ++i) {
        char label[17], point[16], guest[80], expected[16], host[512];
        snprintf(label, sizeof(label), "TESTDLC%09u", i);
        CHECK(!mount(0, label, point));
        snprintf(expected, sizeof(expected), "/addcont%u", i);
        CHECK(!strcmp(point, expected));
        snprintf(guest, sizeof(guest), "%s/probe.txt", point);
        CHECK(!runtime_file_translate(guest, host, sizeof(host)));
        int fd = (int)runtime_file_open(guest, 0, 0);
        CHECK(fd >= 0);
        if (fd >= 0) {
            char data[12] = {0};
            CHECK(runtime_file_read(fd, data, 11) == 11);
            CHECK(!strcmp(data, "DLC payload"));
            runtime_file_close(fd);
        }
        CHECK(runtime_file_open(guest, 1, 0) == -EROFS);
    }
    char point[16];
    CHECK(mount(0, "UNKNOWN000000000", point) != 0);
    char host[512];
    CHECK(runtime_file_translate("/addcont0/../probe.txt", host, sizeof(host)) != 0);
    CHECK(runtime_file_translate("/addcont0/..\\outside.txt", host, sizeof(host)) != 0);
    CHECK(runtime_file_translate("/addcont0/probe.txt:stream", host, sizeof(host)) != 0);
    CHECK(!mount(0, "TESTDLC000000018", point)); /* remount is idempotent */
    typedef int64_t(ABI * PathOp)(const char *);
    typedef int64_t(ABI * Make)(const char *, int);
    typedef int64_t(ABI * Rename)(const char *, const char *);
    typedef int64_t(ABI * Truncate)(const char *, int64_t);
    CHECK(((PathOp)runtime_file_resolve("unlink"))("/addcont0/probe.txt") == -1 && error_slot == EROFS);
    CHECK(((PathOp)runtime_file_resolve("rmdir"))("/addcont0") == -1 && error_slot == EROFS);
    CHECK(((Make)runtime_file_resolve("mkdir"))("/addcont0/new", 0700) == -1 && error_slot == EROFS);
    CHECK(((Rename)runtime_file_resolve("rename"))("/addcont0/probe.txt", "/data/stolen.txt") == -1 &&
          error_slot == EROFS);
    f = fopen("content-test-user/data/rename-source.txt", "wb");
    CHECK(f != NULL);
    if (f) {
        fputs("preserve", f);
        fclose(f);
    }
    CHECK(((Rename)runtime_file_resolve("rename"))("/data/rename-source.txt", "/addcont0/probe.txt") == -1 &&
          error_slot == EROFS);
    CHECK(((Truncate)runtime_file_resolve("truncate"))("/addcont0/probe.txt", 0) == -1 && error_slot == EROFS);
    CHECK(runtime_file_open("/addcont0/new.txt", 0x200, 0600) == -EROFS);
    Load unload = (Load)runtime_content_resolve("eR2bZFAAU0Q#libSceSysmodule");
    CHECK(!unload(0xb4));
    CHECK(!load(0xb4));
    CHECK(!init(reserved, boot));
    CHECK(entitlement_events == 1);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
