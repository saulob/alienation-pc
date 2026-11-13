/* The launcher, called by the loader before the guest and the game window exist. */
#ifndef ALN_LAUNCHER_H
#define ALN_LAUNCHER_H

#include <stddef.h>
#include <stdint.h>

#include "aln_frame_rate.h"
#include "aln_resolution.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the processor and graphics system can run the game. Otherwise 0, with what is wrong and what to do about it in
 * the buffers. */
typedef int (*AlnGraphicsCheck)(char *cause, size_t cause_size, char *advice, size_t advice_size);

typedef struct {
    const char *app0;          /* prepared game folder (/app0) */
    const char *image_dir;     /* folder with boot.bin, content.bin and image.json */
    const char *user_dir;      /* user data; alienation.ini lives here unless settings_file is set */
    const char *settings_file; /* NULL: <user_dir>/alienation.ini */
    const char *logs_dir;      /* NULL when unknown */
    int skip_once;             /* --aln-skip-launcher: do not show it unless the game files are not ready */
    int force_show;            /* --launcher: show it even when "Show this launcher" is off, like holding Shift */
    AlnGraphicsCheck check_graphics; /* NULL: Play does not check; see aln_check_vulkan */
    int game_process;          /* Windows: with Allow overlay programs on, Play starts the game as a process of its own */
    const char *log_file;      /* this run's log, continued by the game process; NULL when there is none */
} AlnLauncherConfig;

/* NULL asks for the default, which is relative to the executable (launcher/paths.h). */
typedef struct {
    const char *boot;
    const char *content_profile;
    const char *app0;
    const char *user_dir;
    const char *logs_dir;
} AlnPathRequest;

typedef struct {
    char boot[4096];
    char content_profile[4096];
    char app0[4096];
    char image_dir[4096];
    char user_dir[4096];
    char logs_dir[4096];
    char error[512]; /* why the paths cannot be used, when it is known (ALN_GAME_BUILD) */
} AlnPaths;

/* 0 when the executable's folder is unknown, a path does not fit or ALN_GAME_BUILD cannot be used. */
int aln_resolve_paths(const AlnPathRequest *request, AlnPaths *paths);

/* The folder holding this run's log: the log_file's parent when one is given, else logs_dir. 0 when
 * neither yields a path. */
int aln_logs_directory(const char *logs_dir, const char *log_file, char *out, size_t size);

/* Index into aln_resolutions (aln_resolution.h) of the render_resolution in the settings file, among the resolutions the
 * build image_dir was built from: 0 (native) when the file or the value is missing or the build has no verified render
 * resolution sites. */
int aln_render_resolution(const char *settings_file, const char *image_dir);

/* Index into aln_aspect_ratios of the aspect_ratio in the settings file, among the shapes the build image_dir was built
 * from: 0 (16:9 Original) when the file or the value is missing or the build has no verified render resolution sites. */
int aln_aspect_ratio(const char *settings_file, const char *image_dir);

/* 1 when the settings file keeps the logs of normal sessions (detailed_logging; 1 by default). */
int aln_detailed_logging(const char *settings_file);

/* 1 when the settings file skips the intro video (skip_intro; 0 by default). */
int aln_skip_intro(const char *settings_file);
/* Asian Menu Buttons: 1 when Circle confirms in the menus. */
int aln_asian_menu_buttons(const char *settings_file);

/* What the loader decided for this run: the parts of the "Settings:" log lines the settings file alone does not say. */
typedef struct AlnRunSettings {
    const char *game_build;        /* label; NULL for an unknown build */
    int render_width, render_height;
    const char *render_resolution; /* aln_resolutions key */
    const char *aspect_ratio;      /* aln_aspect_ratios name */
} AlnRunSettings;

/* Prints the startup state support needs as "Settings:" lines: port version, platform, game build and the display and
 * gameplay settings in effect. Never the Online name, Friend Code or account.
 * `run` may be NULL: the settings file's values then. */
void aln_log_settings_summary(const char *settings_file, const char *image_dir, const char *app0,
                              const AlnRunSettings *run);

/* Starts the game process aln_launcher_run created suspended. Call it once nothing writes to the log any more; 1 when
 * it runs. A process that could not be started is ended. */
int aln_resume_game_process(void);

/* Fills `names` with the distinct names of the Vulkan devices the renderer can use and returns how many (none without a
 * usable Vulkan on Linux); enumerates only, no device is created. */
int aln_gpu_names(char (*names)[256], int max);

/* A AlnGraphicsCheck: 1 when Vulkan can run the renderer. Otherwise logs why and returns 0. It asks again on every call
 * until it succeeds, so a driver installed meanwhile is found. */
int aln_check_vulkan(char *cause, size_t cause_size, char *advice, size_t advice_size);

/* Builds boot.bin, content.bin and image.json in `out` from the prepared game folder `app0` (UTF-8 paths). 0 on
 * success; otherwise 1 and the reason in `error`. */
int aln_build_boot_image(const char *app0, const char *out, char *error, size_t error_size);

typedef struct {
    const char *label;
    const AlnResolutionSite *resolution_sites; /* empty: the build runs only at its native size */
    size_t resolution_site_count;
    const AlnFrameRatePatch *frame_rate;
    const char *title_id;
    const char *version;         /* the build's profile version */
    const char *eboot_sha256;
} AlnGameBuild;

/* The supported game build a boot image folder was built from (its image.json); 0 when it is not one. */
int aln_game_build(const char *image_dir, AlnGameBuild *build);

typedef enum {
    ALN_LAUNCHER_PLAY = 0,
    ALN_LAUNCHER_QUIT = 1,
    ALN_LAUNCHER_BLOCKED = 2,
    ALN_LAUNCHER_GAME_STARTED = 3 /* game_process: the game process exists, suspended; this process ends */
} AlnLauncherResult;

/* Shows the launcher when its rules say so (game files not ready, "Show this launcher" on, or Shift held) and returns
 * once the player pressed Play or quit. Returns ALN_LAUNCHER_PLAY at once when it is not shown.
 * While check_graphics fails, Play is refused: the launcher says why and stays open. A launcher that is not shown
 * explains in a window of its own and returns ALN_LAUNCHER_BLOCKED.
 * With game_process and Allow overlay programs on, Play creates the game process instead and returns
 * ALN_LAUNCHER_GAME_STARTED; when it cannot be created, the launcher stays open with the reason (or explains in a window
 * of its own). */
AlnLauncherResult aln_launcher_run(const AlnLauncherConfig *config);

#ifdef __cplusplus
}
#endif

#endif
