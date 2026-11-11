/* Save data on host folders: a writable mount is committed only at unmount, interrupted commits recover, and two
 * profiles (user folders) never see each other's saves. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#else
#include <ftw.h>
#include <unistd.h>
#define make_dir(path) mkdir(path, 0777)
#endif

static int checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

static int32_t error_slot;
int32_t *runtime_errno(void) { return &error_slot; }
int32_t runtime_guest_errno(int host_errno) { return host_errno; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(table[i].name, nid)) return (uintptr_t)table[i].function;
    return 0;
}

#define ERR_PARAMETER ((int32_t)0x809F0000)
#define ERR_EXISTS ((int32_t)0x809F0007)
#define ERR_NOT_FOUND ((int32_t)0x809F0008)
#define ERR_BAD_MOUNTED ((int32_t)0x809F000D)
#define MODE_RDONLY 1
#define MODE_RDWR 2
#define MODE_CREATE 4
#define GUEST_WRONLY 1
#define GUEST_CREAT 0x200
#define GUEST_TRUNC 0x400

typedef struct { char data[10]; char pad[6]; } TitleId;
typedef struct { char data[32]; } DirName;
typedef struct { char data[16]; } MountPoint;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint64_t blocks; uint32_t mode; uint8_t reserved[32]; } Mount1;
typedef struct { MountPoint point; uint64_t required_blocks; uint32_t unused, status; uint8_t reserved[28]; int32_t pad; } MountResult;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t unused;
                 uint8_t reserved[32]; int32_t pad2; } Delete;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t key, order;
                 uint8_t reserved[32]; } SearchCond;
typedef struct { uint32_t hits; int32_t pad; DirName *names; uint32_t names_capacity, set_count;
                 void *params; void *infos; uint8_t reserved[12]; int32_t pad2; } SearchResult;
_Static_assert(sizeof(Mount1) == 80 && sizeof(MountResult) == 64, "save data mount layouts");
_Static_assert(sizeof(SearchCond) == 64 && sizeof(SearchResult) == 56, "dir name search layouts");

typedef int32_t (ABI *InitFn)(const void *);
typedef int32_t (ABI *MountFn)(const Mount1 *, MountResult *);
typedef int32_t (ABI *UmountFn)(const MountPoint *);
typedef int32_t (ABI *SetParamFn)(const MountPoint *, uint32_t, const void *, uint64_t);
typedef int32_t (ABI *DeleteFn)(const Delete *);
typedef int32_t (ABI *SearchFn)(const SearchCond *, SearchResult *);
static MountFn save_mount;
static UmountFn save_umount;
static SetParamFn save_set_param;
static DeleteFn save_delete;
static SearchFn save_search;

static char root[512], user_a[600], user_b[600];

static int exists(const char *path) { struct stat st; return !stat(path, &st); }
static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(text, 1, strlen(text), f); fclose(f); }
}
static void read_text(const char *path, char *out, size_t size) {
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t n = fread(out, 1, size - 1, f);
    out[n] = 0;
    fclose(f);
}
static void use_profile(const char *user) {
    runtime_file_configure(root, user);
    runtime_savedata_configure("CUSA00062");
}
static void save_path(const char *user, const char *leaf, char *out, size_t size) {
    snprintf(out, size, "%s/savedata/1/CUSA00062/%s", user, leaf);
}
static int32_t mount_save(const char *name, uint32_t mode, MountResult *result) {
    DirName dir; memset(&dir, 0, sizeof(dir)); snprintf(dir.data, sizeof(dir.data), "%s", name);
    Mount1 m; memset(&m, 0, sizeof(m));
    m.user = 1; m.dir = &dir; m.blocks = 1024; m.mode = mode;
    memset(result, 0, sizeof(*result));
    return save_mount(&m, result);
}
static void guest_write(const char *point, const char *text) {
    char guest[64]; snprintf(guest, sizeof(guest), "%s/savedata.dat", point);
    const int64_t fd = runtime_file_open(guest, GUEST_WRONLY | GUEST_CREAT | GUEST_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(runtime_file_write((int)fd, text, strlen(text)) == (int64_t)strlen(text));
    runtime_file_close((int)fd);
}
static void guest_read(const char *point, char *out, size_t size) {
    char guest[64]; snprintf(guest, sizeof(guest), "%s/savedata.dat", point);
    out[0] = 0;
    const int64_t fd = runtime_file_open(guest, 0, 0);
    if (fd < 0) return;
    const int64_t n = runtime_file_read((int)fd, out, size - 1);
    out[n > 0 ? n : 0] = 0;
    runtime_file_close((int)fd);
}
static int no_leftovers(const char *user) {
    char path[700];
    const char *suffixes[] = {".aln-work", ".aln-old", ".aln-commit"};
    for (int i = 0; i < 3; ++i) {
        char leaf[64]; snprintf(leaf, sizeof(leaf), "savedata%s", suffixes[i]);
        save_path(user, leaf, path, sizeof(path));
        if (exists(path)) return 0;
    }
    return 1;
}

static void test_create_commit_overwrite(void) {
    char data[700], text[64];
    MountResult r;
    use_profile(user_a);
    save_path(user_a, "savedata/savedata.dat", data, sizeof(data));
    CHECK(mount_save("savedata", MODE_RDWR, &r) == ERR_NOT_FOUND);
    CHECK(mount_save("savedata", MODE_RDWR | MODE_CREATE, &r) == 0 && r.status == 1);
    guest_write(r.point.data, "first");
    CHECK(!exists(data));                       /* nothing is visible before the unmount commits */
    CHECK(save_set_param(&r.point, 1, "ALIENATION", 10) == 0);
    CHECK(save_umount(&r.point) == 0);
    read_text(data, text, sizeof(text));
    CHECK(!strcmp(text, "first"));
    CHECK(no_leftovers(user_a));

    unsigned char param[RUNTIME_SAVE_PARAM_SIZE];
    CHECK(runtime_savedata_param(1, "savedata", param) == 0 && !strcmp((const char *)param, "ALIENATION"));
    char temp[700]; save_path(user_a, "savedata.sce_sys/param.bin.aln-tmp", temp, sizeof(temp));
    CHECK(!exists(temp));

    CHECK(mount_save("savedata", MODE_RDWR | MODE_CREATE, &r) == ERR_EXISTS);
    CHECK(mount_save("savedata", MODE_RDWR, &r) == 0 && r.status == 0);
    MountResult again;
    CHECK(mount_save("savedata", MODE_RDONLY, &again) == ERR_BAD_MOUNTED);
    guest_read(r.point.data, text, sizeof(text));
    CHECK(!strcmp(text, "first"));
    guest_write(r.point.data, "second");
    guest_read(r.point.data, text, sizeof(text));
    CHECK(!strcmp(text, "second"));
    read_text(data, text, sizeof(text));
    CHECK(!strcmp(text, "first"));              /* the committed save is untouched while mounted */
    CHECK(save_umount(&r.point) == 0);
    read_text(data, text, sizeof(text));
    CHECK(!strcmp(text, "second"));
    CHECK(save_umount(&r.point) == ERR_NOT_FOUND);

    CHECK(mount_save("savedata", MODE_RDONLY, &r) == 0);
    guest_read(r.point.data, text, sizeof(text));
    CHECK(!strcmp(text, "second"));
    CHECK(save_umount(&r.point) == 0 && no_leftovers(user_a));
}

/* On-disk states a stopped process can leave behind, as the next mount finds them. */
static void test_recovery(void) {
    char host[700], work[700], old[700], marker[700], file[700], inner[760], text[64];
    MountResult r;
    use_profile(user_a);
    save_path(user_a, "savedata", host, sizeof(host));
    save_path(user_a, "savedata.aln-work", work, sizeof(work));
    save_path(user_a, "savedata.aln-old", old, sizeof(old));
    save_path(user_a, "savedata.aln-commit", marker, sizeof(marker));
    save_path(user_a, "savedata/savedata.dat", file, sizeof(file));

    /* Stopped before unmounting: the half-written work copy is dropped. */
    make_dir(work);
    snprintf(inner, sizeof(inner), "%s/savedata.dat", work);
    write_text(inner, "partial");
    CHECK(mount_save("savedata", MODE_RDONLY, &r) == 0);
    guest_read(r.point.data, text, sizeof(text));
    CHECK(!strcmp(text, "second"));
    CHECK(save_umount(&r.point) == 0 && no_leftovers(user_a));

    /* Stopped after marking the work copy complete: the swap is finished. */
    make_dir(work);
    snprintf(inner, sizeof(inner), "%s/savedata.dat", work);
    write_text(inner, "third");
    write_text(marker, "");
    CHECK(mount_save("savedata", MODE_RDONLY, &r) == 0);
    guest_read(r.point.data, text, sizeof(text));
    CHECK(!strcmp(text, "third"));
    CHECK(save_umount(&r.point) == 0 && no_leftovers(user_a));

    /* Stopped between the two renames: the save folder is already the old one. */
    CHECK(rename(host, old) == 0);
    make_dir(work);
    snprintf(inner, sizeof(inner), "%s/savedata.dat", work);
    write_text(inner, "fourth");
    write_text(marker, "");
    CHECK(mount_save("savedata", MODE_RDONLY, &r) == 0);
    CHECK(save_umount(&r.point) == 0);
    read_text(file, text, sizeof(text));
    CHECK(!strcmp(text, "fourth") && no_leftovers(user_a));

#ifdef _WIN32
    /* A marked swap that cannot finish (the save folder is in use) blocks the save instead of overwriting the work copy. */
    make_dir(work);
    snprintf(inner, sizeof(inner), "%s/savedata.dat", work);
    write_text(inner, "pending");
    write_text(marker, "");
    FILE *held = fopen(file, "rb");
    CHECK(held != NULL);
    CHECK(mount_save("savedata", MODE_RDWR, &r) != 0);
    snprintf(inner, sizeof(inner), "%s/savedata.dat", work);
    read_text(inner, text, sizeof(text));
    CHECK(!strcmp(text, "pending") && exists(marker));
    if (held) fclose(held);
    CHECK(mount_save("savedata", MODE_RDONLY, &r) == 0);
    guest_read(r.point.data, text, sizeof(text));
    CHECK(!strcmp(text, "pending"));
    CHECK(save_umount(&r.point) == 0 && no_leftovers(user_a));
    /* Back to the content the later checks expect. */
    CHECK(mount_save("savedata", MODE_RDWR, &r) == 0);
    guest_write(r.point.data, "fourth");
    CHECK(save_umount(&r.point) == 0);
#endif

    /* An old folder without a commit mark is the save itself. */
    CHECK(rename(host, old) == 0);
    CHECK(mount_save("savedata", MODE_RDONLY, &r) == 0);
    CHECK(save_umount(&r.point) == 0);
    read_text(file, text, sizeof(text));
    CHECK(!strcmp(text, "fourth") && no_leftovers(user_a));
}

static void test_profiles_and_names(void) {
    char a_file[700], b_file[700], text[64];
    MountResult r;
    use_profile(user_b);
    CHECK(mount_save("savedata", MODE_RDONLY, &r) == ERR_NOT_FOUND);
    CHECK(mount_save("savedata", MODE_RDWR | MODE_CREATE, &r) == 0);
    guest_write(r.point.data, "profile b");
    CHECK(save_umount(&r.point) == 0);
    save_path(user_a, "savedata/savedata.dat", a_file, sizeof(a_file));
    save_path(user_b, "savedata/savedata.dat", b_file, sizeof(b_file));
    read_text(a_file, text, sizeof(text));
    CHECK(!strcmp(text, "fourth"));
    read_text(b_file, text, sizeof(text));
    CHECK(!strcmp(text, "profile b"));

    CHECK(mount_save("../savedata", MODE_RDONLY, &r) == ERR_PARAMETER);
    CHECK(mount_save(".hidden", MODE_RDONLY, &r) == ERR_PARAMETER);
    CHECK(mount_save("savedata.aln-work", MODE_RDWR | MODE_CREATE, &r) == ERR_PARAMETER);
    CHECK(mount_save("a/b", MODE_RDONLY, &r) == ERR_PARAMETER);

    /* An unfinished work copy is not a save of its own. */
    char work[700]; save_path(user_b, "savedata.aln-work", work, sizeof(work));
    make_dir(work);
    DirName names[8]; memset(names, 0, sizeof(names));
    SearchCond cond; memset(&cond, 0, sizeof(cond)); cond.user = 1;
    SearchResult found; memset(&found, 0, sizeof(found));
    found.names = names; found.names_capacity = 8;
    CHECK(save_search(&cond, &found) == 0 && found.hits == 1 && !strcmp(names[0].data, "savedata"));

    DirName dir; memset(&dir, 0, sizeof(dir)); snprintf(dir.data, sizeof(dir.data), "savedata");
    Delete del; memset(&del, 0, sizeof(del)); del.user = 1; del.dir = &dir;
    CHECK(save_delete(&del) == 0);
    CHECK(!exists(b_file) && !exists(work));
    CHECK(save_delete(&del) == ERR_NOT_FOUND);
    read_text(a_file, text, sizeof(text));
    CHECK(!strcmp(text, "fourth"));
}

int main(void) {
    snprintf(root, sizeof(root), "%s", ALN_SAVEDATA_TEST_ROOT);
#ifdef _WIN32
    compat_remove_tree(root);
#else
    char command[700]; snprintf(command, sizeof(command), "rm -rf '%s'", root);
    if (system(command)) return 2;
#endif
    make_dir(root);
    snprintf(user_a, sizeof(user_a), "%s/user", root);
    snprintf(user_b, sizeof(user_b), "%s/user/test-client-b", root);
    use_profile(user_a);
    InitFn init = (InitFn)runtime_savedata_resolve("sceSaveDataInitialize3");
    save_mount = (MountFn)runtime_savedata_resolve("sceSaveDataMount");
    save_umount = (UmountFn)runtime_savedata_resolve("sceSaveDataUmount");
    save_set_param = (SetParamFn)runtime_savedata_resolve("sceSaveDataSetParam");
    save_delete = (DeleteFn)runtime_savedata_resolve("sceSaveDataDelete");
    save_search = (SearchFn)runtime_savedata_resolve("sceSaveDataDirNameSearch");
    if (!init || !save_mount || !save_umount || !save_set_param || !save_delete || !save_search) {
        printf("FAIL: save data exports missing\n");
        return 1;
    }
    CHECK(init(NULL) == 0);
    test_create_commit_overwrite();
    test_recovery();
    test_profiles_and_names();
    printf("savedata tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
