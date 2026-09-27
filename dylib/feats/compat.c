// Compatibility tool support
#include "compat.h"
#include "../util/log.h"
#include "../util/file.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// The client maps Cloud's Windows path roots into the compat prefix only when
// the tool's name contains "proton" so without a match every AutoCloud rule silently
// skips the app.
#define TOOL_DIR_NAME "notproton"
#define TOOL_DISPLAY_NAME "CrossOver Preview (Rosetta)"
#define TOOL_DIR_NAME_FEX "notproton-fex"
#define TOOL_DISPLAY_NAME_FEX "CrossOver Preview (FEX)"

#define COMPAT_MANAGER_ENABLED_OFF  0x7B0
#define COMPAT_TOOL_STRIDE          0x130
#define COMPAT_TOOL_FLAGS_OFF       0x28  // bit 0: tool needs no downloaded app
#define COMPAT_TOOL_FLAG_NO_APP     0x1
#define COMPAT_TOOL_FROM_OSLIST_OFF 0x70
#define COMPAT_TOOL_TO_OSLIST_OFF   0x78

#define COMPAT_MAPPING_PRIORITY_GLOBAL 75
#define COMPAT_MAPPING_PRIORITY_APP    250

static const char TOOL_MANIFEST[] =
    "\"manifest\"\n"
    "{\n"
    "  \"version\" \"2\"\n"
    "  \"commandline\" \"/run %verb%\"\n"
    "}\n";

// Steam's compatibilitytools.d scanner registers a tool when this file is present
// in the tool subdirectory. install_path is "." because the manifest lives inside
// the tool directory. to_oslist is macos
static void tool_declaration(char *out, size_t size,
                            const char *name, const char *display) {
    snprintf(out, size,
        "\"compatibilitytools\"\n"
        "{\n"
        "  \"compat_tools\"\n"
        "  {\n"
        "    \"%s\"\n"
        "    {\n"
        "      \"install_path\" \".\"\n"
        "      \"display_name\" \"%s\"\n"
        "      \"from_oslist\" \"windows\"\n"
        "      \"to_oslist\" \"macos\"\n"
        "    }\n"
        "  }\n"
        "}\n",
        name, display);
}

#include "compat_run.h"  // RUN_SCRIPT, generated from compat_run.sh

static int write_file(const char *path, const char *content, int executable) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(content, f);
    int err = fclose(f);
    if (executable)
        chmod(path, 0755);
    return err ? -1 : 0;
}

static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode) ? 0 : -1;
    return mkdir(path, 0755);
}

static int32_t g_tool_shift;

#define COMPAT_SELECTION_MAX 128
#define COMPAT_SELECTION_NAME_MAX 32
#define COMPAT_SELECTION_DIR "compat-selections"

typedef struct {
    uint32_t appid;
    char name[COMPAT_SELECTION_NAME_MAX];
} compat_selection_t;

static compat_selection_t g_selections[COMPAT_SELECTION_MAX];
static size_t g_selection_count;

static int valid_selection_name(const char *name) {
    return name && (
        strcmp(name, TOOL_DIR_NAME) == 0 ||
        strcmp(name, TOOL_DIR_NAME_FEX) == 0 ||
        strcmp(name, NP_COMPAT_TOOL_NONE) == 0
    );
}

static int selection_path(char *out, size_t out_size, uint32_t appid) {
    const char *home = np_home_dir();
    if (!home) return -1;

    int n = snprintf(out, out_size,
                     "%s/Library/Application Support/notproton/%s/%u",
                     home, COMPAT_SELECTION_DIR, appid);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

static int ensure_selection_dir(void) {
    const char *home = np_home_dir();
    if (!home) return -1;

    char path[512];
    int n = snprintf(path, sizeof path, "%s/Library", home);
    if (n < 0 || (size_t)n >= sizeof path || mkdir(path, 0755) != 0) {
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    }

    n = snprintf(path, sizeof path, "%s/Library/Application Support", home);
    if (n < 0 || (size_t)n >= sizeof path || mkdir(path, 0755) != 0) {
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    }

    n = snprintf(path, sizeof path, "%s/Library/Application Support/notproton", home);
    if (n < 0 || (size_t)n >= sizeof path || mkdir(path, 0755) != 0) {
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    }

    n = snprintf(path, sizeof path, "%s/Library/Application Support/notproton/%s",
                 home, COMPAT_SELECTION_DIR);
    if (n < 0 || (size_t)n >= sizeof path || mkdir(path, 0755) != 0) {
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    }

    return 0;
}

static void persist_selection(uint32_t appid, const char *name) {
    char path[512];
    if (selection_path(path, sizeof path, appid) != 0)
        return;

    if (!name || !name[0]) {
        unlink(path);
        return;
    }

    if (ensure_selection_dir() != 0)
        return;

    char temp[512];
    int n = snprintf(temp, sizeof temp, "%s.tmp.%u", path, (unsigned)getpid());
    if (n < 0 || (size_t)n >= sizeof temp)
        return;

    FILE *f = fopen(temp, "w");
    if (!f) return;

    int ok = fprintf(f, "%s\n", name) > 0 && fclose(f) == 0;
    if (!ok) {
        fclose(f);
        unlink(temp);
        return;
    }

    if (rename(temp, path) != 0)
        unlink(temp);
}

static int load_persisted_selection(uint32_t appid,
                                    char *name, size_t name_size) {
    char path[512];
    if (!name || name_size == 0 || selection_path(path, sizeof path, appid) != 0)
        return 0;

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    if (!fgets(name, (int)name_size, f)) {
        fclose(f);
        return 0;
    }
    fclose(f);

    name[strcspn(name, "\r\n")] = '\0';
    return valid_selection_name(name);
}

static void remember_selection(uint32_t appid, const char *name, int persist) {
    for (size_t i = 0; i < g_selection_count; i++) {
        if (g_selections[i].appid != appid) continue;

        if (!valid_selection_name(name) || !name[0]) {
            g_selections[i] = g_selections[g_selection_count - 1];
            g_selection_count--;
            if (persist) persist_selection(appid, NULL);
            return;
        }

        snprintf(g_selections[i].name, sizeof g_selections[i].name, "%s", name);
        if (persist) persist_selection(appid, g_selections[i].name);
        return;
    }

    if (!valid_selection_name(name) || !name[0]) {
        if (persist) persist_selection(appid, NULL);
        return;
    }

    if (g_selection_count >= COMPAT_SELECTION_MAX) {
        if (persist) persist_selection(appid, name);
        return;
    }

    g_selections[g_selection_count].appid = appid;
    snprintf(g_selections[g_selection_count].name,
             sizeof g_selections[g_selection_count].name, "%s", name);
    g_selection_count++;
    if (persist) persist_selection(appid, name);
}

void np_compat_record_selection(uint32_t appid, const char *name) {
    remember_selection(appid, name, 1);
}

const char *np_compat_recorded_selection(uint32_t appid) {
    for (size_t i = 0; i < g_selection_count; i++) {
        if (g_selections[i].appid == appid)
            return g_selections[i].name;
    }

    char persisted[COMPAT_SELECTION_NAME_MAX];
    if (!load_persisted_selection(appid, persisted, sizeof persisted))
        return NULL;

    remember_selection(appid, persisted, 0);
    for (size_t i = 0; i < g_selection_count; i++) {
        if (g_selections[i].appid == appid)
            return g_selections[i].name;
    }
    return NULL;
}

// LDR Wt,[X0,#imm]. Writes the byte offset and destination register.
static int ldr32_from_x0(uint32_t w, uint32_t *off, uint32_t *rt) {
    if ((w & 0xFFC003E0u) != 0xB9400000u) return 0;
    *rt  = w & 0x1Fu;
    *off = ((w >> 10) & 0xFFFu) * 4u;
    return 1;
}

// LDR Xt,[Xn,#imm] for a named base register.
static int ldr64_from(uint32_t w, uint32_t base, uint32_t *off) {
    if ((w & 0xFFC00000u) != 0xF9400000u) return 0;
    if (((w >> 5) & 0x1Fu) != base) return 0;
    *off = ((w >> 10) & 0xFFFu) * 8u;
    return 1;
}

static int ldr64_from_x0(uint32_t w, uint32_t *off) {
    return ldr64_from(w, 0, off);
}

// LDRB Wt,[Xn,#imm] for a named base register.
static int ldrb_from(uint32_t w, uint32_t base, uint32_t *off) {
    if ((w & 0xFFC00000u) != 0x39400000u) return 0;
    if (((w >> 5) & 0x1Fu) != base) return 0;
    *off = (w >> 10) & 0xFFFu;
    return 1;
}

// MOV Xd,X0, which assembles as ORR Xd,XZR,X0. Writes the destination register.
static int mov_from_x0(uint32_t w, uint32_t *rd) {
    if ((w & 0xFFFFFFE0u) != 0xAA0003E0u) return 0;
    *rd = w & 0x1Fu;
    return 1;
}

// CBZ Wt. Writes the branch target.
static int cbz32(uintptr_t pc, uint32_t w, uint32_t rt, uintptr_t *target) {
    if ((w & 0xFF000000u) != 0x34000000u) return 0;
    if ((w & 0x1Fu) != rt) return 0;
    int32_t imm = (int32_t)((w >> 5) & 0x7FFFFu);
    imm = (imm << 13) >> 13;
    *target = pc + (uintptr_t)(imm * 4);
    return 1;
}

// The oslist gate compares both oslist strings, so its two loads sit at the far
// end of the struct and corroborate a shift derived at the near end. The base
// register is whatever the compiler picked, so a build that stops using the one
// seen here yields nothing and the shift stands on the near-end pair alone.
// Returns the number of offsets written.
static int probe_oslist_offsets(uintptr_t oslist_gate, uint32_t base,
                                uint32_t *from_off, uint32_t *to_off) {
    if (!oslist_gate) return 0;

    uint32_t seen[2];
    int n = 0;
    for (uintptr_t pc = oslist_gate - 0x40; pc <= oslist_gate + 0x20; pc += 4) {
        uint32_t off;
        if (!ldr64_from(*(const uint32_t *)pc, base, &off)) continue;
        if (n > 0 && seen[0] == off) continue;
        if (n > 1 && seen[1] == off) continue;
        if (n == 2) return 0;
        seen[n++] = off;
    }
    if (n != 2) return 0;

    // to_oslist sits above from_oslist in the struct, whichever order the
    // comparison happens to load them in.
    *to_off   = seen[0] > seen[1] ? seen[0] : seen[1];
    *from_off = seen[0] > seen[1] ? seen[1] : seen[0];
    return 2;
}

int np_compat_probe_tool_layout(uintptr_t appid_gate, uintptr_t oslist_gate) {
    if (!appid_gate) return 0;

    uint32_t appid_off, rt;
    if (!ldr32_from_x0(*(const uint32_t *)appid_gate, &appid_off, &rt)) {
        NP_WARN("np_compat_probe_tool_layout: 0x%lx is not the appid load",
                (unsigned long)appid_gate);
        return 0;
    }

    uintptr_t local_path;
    if (!cbz32(appid_gate + 4, *(const uint32_t *)(appid_gate + 4), rt, &local_path)) {
        NP_WARN("np_compat_probe_tool_layout: no appid branch after 0x%lx",
                (unsigned long)appid_gate);
        return 0;
    }

    // The install field is the first thing the local path wants, within a few
    // instructions of the branch landing.
    uint32_t install_off = 0;
    for (uintptr_t pc = local_path; pc < local_path + 0x20; pc += 4) {
        if (ldr64_from_x0(*(const uint32_t *)pc, &install_off)) break;
    }
    if (!install_off) {
        NP_WARN("np_compat_probe_tool_layout: no install load at 0x%lx",
                (unsigned long)local_path);
        return 0;
    }

    int32_t by_appid   = (int32_t)appid_off   - COMPAT_TOOL_APPID_OFF;
    int32_t by_install = (int32_t)install_off - COMPAT_TOOL_INSTALL_OFF;
    if (by_appid != by_install) {
        NP_ERR("np_compat_probe_tool_layout: appid moved %+d but install moved %+d; "
               "the tool layout no longer shifts as one block",
               by_appid, by_install);
        return 0;
    }

    // COMPAT_TOOL_TO_OSLIST_OFF is the highest field written into a registration,
    // so agreement there means the shift holds across every field in use rather
    // than only the two it was read from.
    uint32_t from_off = 0, to_off = 0;
    if (probe_oslist_offsets(oslist_gate, 21, &from_off, &to_off) == 2) {
        int32_t by_from = (int32_t)from_off - COMPAT_TOOL_FROM_OSLIST_OFF;
        int32_t by_to   = (int32_t)to_off   - COMPAT_TOOL_TO_OSLIST_OFF;
        if (by_from != by_appid || by_to != by_appid) {
            NP_ERR("np_compat_probe_tool_layout: near fields moved %+d but oslist "
                   "moved %+d/%+d; the tool layout no longer shifts as one block",
                   by_appid, by_from, by_to);
            return 0;
        }
        NP_DBG("np_compat_probe_tool_layout: oslist offsets 0x%x/0x%x agree on %+d",
               from_off, to_off, by_appid);
    }

    g_tool_shift = by_appid;
    if (g_tool_shift)
        NP_LOG("np_compat_probe_tool_layout: tool fields from install onward are "
               "%+d from the baseline (appid 0x%x, install 0x%x)",
               g_tool_shift, appid_off, install_off);
    return 1;
}

// The manager is a much larger object than a tool entry and grows in the middle,
// so a single distance does not describe it the way one does for the tool tail:
// the tool array and its count sit below the growth and do not move, while the
// enabled flag sits above it and does. Reading the flag's own offset back keeps
// the two independent.
static uint32_t g_enabled_off = COMPAT_MANAGER_ENABLED_OFF;

int np_compat_probe_enabled_off(uintptr_t bis_enabled) {
    if (!bis_enabled) return 0;

    // this arrives in X0 and is parked in a callee-saved register before the
    // profiling call clobbers it, so the flag is read through that register.
    uint32_t base = 0;
    int have_base = 0;
    for (uintptr_t pc = bis_enabled; pc < bis_enabled + 0x40; pc += 4) {
        if (mov_from_x0(*(const uint32_t *)pc, &base)) { have_base = 1; break; }
    }
    if (!have_base) {
        NP_WARN("np_compat_probe_enabled_off: this is never parked in a register "
                "at 0x%lx", (unsigned long)bis_enabled);
        return 0;
    }

    // The flag is the first thing the body tests, ahead of any other byte read
    // through that register.
    uint32_t off = 0;
    int found = 0;
    for (uintptr_t pc = bis_enabled; pc < bis_enabled + 0x80; pc += 4) {
        if (ldrb_from(*(const uint32_t *)pc, base, &off)) { found = 1; break; }
    }
    if (!found) {
        NP_WARN("np_compat_probe_enabled_off: no flag load through x%u at 0x%lx",
                base, (unsigned long)bis_enabled);
        return 0;
    }

    // A tiny offset would mean the match landed on some unrelated early byte
    // read rather than on the flag, which sits far into the object.
    if (off < 0x100) {
        NP_ERR("np_compat_probe_enabled_off: 0x%x is too low to be the enabled "
               "flag; refusing to write it", off);
        return 0;
    }

    if (off != g_enabled_off)
        NP_LOG("np_compat_probe_enabled_off: enabled flag at 0x%x, %+d from the "
               "baseline", off, (int32_t)off - COMPAT_MANAGER_ENABLED_OFF);
    g_enabled_off = off;
    return 1;
}

uint32_t np_compat_enabled_off(void) {
    return g_enabled_off;
}

uint32_t np_compat_tool_off(uint32_t reference_off) {
    if (reference_off < COMPAT_TOOL_INSTALL_OFF) return reference_off;
    return (uint32_t)((int32_t)reference_off + g_tool_shift);
}

size_t np_compat_tool_stride(void) {
    return (size_t)((int32_t)COMPAT_TOOL_STRIDE + g_tool_shift);
}

// The compatibility tool needs to go in the folder Steam normally (on Linux)
// scans for compatibility tools. So the tool is shoved into
// ~/Library/Application Support/Steam/compatibilitytools.d and
// STEAM_EXTRA_COMPAT_TOOLS_PATHS points there.
void np_compat_export_tools_path(void) {
    const char *home = np_home_dir();
    if (!home) return;
    if (getenv("STEAM_EXTRA_COMPAT_TOOLS_PATHS")) return;

    char tools_dir[512];
    snprintf(tools_dir, sizeof(tools_dir),
             "%s/Library/Application Support/Steam/compatibilitytools.d", home);

    setenv("STEAM_EXTRA_COMPAT_TOOLS_PATHS", tools_dir, 1);
    NP_LOG("np_compat_export_tools_path: set STEAM_EXTRA_COMPAT_TOOLS_PATHS=%s",
           tools_dir);
}

static int write_tool_manifest(const char *tools_dir,
                                  const char *name, const char *display) {
    char tool_dir[512];
    char path[512];
    char declaration[1024];
    int wrote = 0;

    snprintf(tool_dir, sizeof(tool_dir), "%s/%s", tools_dir, name);
    if (ensure_dir(tool_dir) != 0) return -1;

    tool_declaration(declaration, sizeof(declaration), name, display);

    snprintf(path, sizeof(path), "%s/compatibilitytool.vdf", tool_dir);
    if (write_file(path, declaration, 0) == 0) wrote++;

    snprintf(path, sizeof(path), "%s/toolmanifest.vdf", tool_dir);
    if (access(path, F_OK) != 0 && write_file(path, TOOL_MANIFEST, 0) == 0)
        wrote++;

    snprintf(path, sizeof(path), "%s/run", tool_dir);
    FILE *run = fopen(path, "w");
    if (run) {
        fprintf(run, "#!/bin/sh\\nNOTPROTON_COMPAT_TOOL='%s'\\nexport NOTPROTON_COMPAT_TOOL\\n", name);
        fputs(RUN_SCRIPT, run);
        if (fclose(run) == 0) {
            chmod(path, 0755);
            wrote++;
        }
    }

    return wrote;
}

int np_compat_ensure_tool_manifest(void) {
    const char *home = np_home_dir();
    if (!home) return -1;

    char tools_dir[512];
    snprintf(tools_dir, sizeof(tools_dir),
             "%s/Library/Application Support/Steam/compatibilitytools.d", home);

    if (ensure_dir(tools_dir) != 0) {
        NP_WARN("np_compat_ensure_tool_manifest: cannot create %s", tools_dir);
        return -1;
    }

    int wrote = 0;
    int result = write_tool_manifest(tools_dir, TOOL_DIR_NAME, TOOL_DISPLAY_NAME);
    if (result > 0) wrote += result;
    result = write_tool_manifest(tools_dir, TOOL_DIR_NAME_FEX, TOOL_DISPLAY_NAME_FEX);
    if (result > 0) wrote += result;

    if (wrote > 0)
        NP_LOG("np_compat_ensure_tool_manifest: wrote %d file(s) to %s", wrote, tools_dir);

    return 0;
}

static const char *np_compat_tool_dir_named(const char *name) {
    static char dirs[2][512];
    const char *home = np_home_dir();
    if (!home || !name) return NULL;

    size_t slot;
    if (strcmp(name, TOOL_DIR_NAME) == 0)
        slot = 0;
    else if (strcmp(name, TOOL_DIR_NAME_FEX) == 0)
        slot = 1;
    else
        return NULL;

    snprintf(dirs[slot], sizeof(dirs[slot]),
             "%s/Library/Application Support/Steam/compatibilitytools.d/%s",
             home, name);
    return dirs[slot];
}

const char *np_compat_tool_dir(void) {
    return np_compat_tool_dir_named(TOOL_DIR_NAME);
}

const char *np_compat_tool_install_for_name(const char *name) {
    return np_compat_tool_dir_named(name);
}

const char *np_compat_tool_commandline(void) {
    return "/run %verb%";
}

void np_compat_force_enable(void *compat_mgr) {
    if (!compat_mgr) return;

    uint8_t *base = (uint8_t *)compat_mgr;
    uint32_t off = np_compat_enabled_off();
    uint8_t prev = base[off];
    base[off] = 1;

    // Steam polls compat state frequently. Only log the one transition.
    if (!prev)
        NP_LOG("np_compat_force_enable: compat layer force-enabled (this+0x%X: 0 -> 1)",
               off);
}

typedef uint32_t (*fn_GetValidPlatforms)(void *compat_mgr, uint32_t appid);
static fn_GetValidPlatforms g_get_valid_platforms;

void np_compat_set_valid_platforms_fn(uintptr_t get_valid_platforms) {
    g_get_valid_platforms = (fn_GetValidPlatforms)get_valid_platforms;
}

uint32_t np_compat_valid_platforms(void *compat_mgr, uint32_t appid) {
    if (!g_get_valid_platforms || !compat_mgr)
        return 0;
    return g_get_valid_platforms(compat_mgr, appid);
}

typedef void *(*fn_FindToolForTargetApp)(void *compat_mgr, uint32_t appid);
static fn_FindToolForTargetApp g_find_tool_for_app;

void np_compat_set_find_tool_fn(uintptr_t find_tool_for_target_app) {
    g_find_tool_for_app = (fn_FindToolForTargetApp)find_tool_for_target_app;
}

void *np_compat_tool_for_app(void *compat_mgr, uint32_t appid) {
    if (!g_find_tool_for_app || !compat_mgr)
        return NULL;
    return g_find_tool_for_app(compat_mgr, appid);
}

// The config argument is a tool-specific string the dropdown has no way to set, and
// the client passes it empty from its own handler.
static fn_FindToolForTargetApp g_chosen_tool;

void np_compat_set_chosen_tool_fn(uintptr_t find_tool_for_target_app) {
    g_chosen_tool = (fn_FindToolForTargetApp)find_tool_for_target_app;
}

void *np_compat_chosen_tool(void *compat_mgr, uint32_t appid) {
    if (!g_chosen_tool || !compat_mgr)
        return NULL;
    return g_chosen_tool(compat_mgr, appid);
}

bool np_compat_would_force(void *compat_mgr, uint32_t appid) {
    if (!compat_mgr || !appid)
        return false;
    uint32_t platforms = np_compat_valid_platforms(compat_mgr, appid);
    return (platforms & COMPAT_PLATFORM_WINDOWS) && !(platforms & COMPAT_PLATFORM_MACOS);
}

typedef void *(*fn_FindMapping)(void *compat_mgr, uint32_t appid);
static fn_FindMapping g_find_mapping;

void np_compat_set_app_mapping_fn(uintptr_t find_mapping) {
    g_find_mapping = (fn_FindMapping)find_mapping;
}

// { const char *name; const char *config; int32_t priority }, owned by the client.
static const char *mapping_name(const void *mapping) {
    const char *const *entry = (const char *const *)mapping;
    if (!entry || !entry[0] || !entry[0][0])
        return NULL;
    return entry[0];
}

const char *np_compat_app_mapping(void *compat_mgr, uint32_t appid) {
    if (!g_find_mapping || !compat_mgr || !appid)
        return NULL;
    return mapping_name(g_find_mapping(compat_mgr, appid));
}

static fn_FindMapping g_get_wildcard;

void np_compat_set_wildcard_fn(uintptr_t get_wildcard_mapping) {
    g_get_wildcard = (fn_FindMapping)get_wildcard_mapping;
}

const char *np_compat_wildcard_mapping(void *compat_mgr, uint32_t appid) {
    if (!g_get_wildcard || !compat_mgr || !appid)
        return NULL;
    return mapping_name(g_get_wildcard(compat_mgr, appid));
}

typedef void (*fn_SetCompatToolMapping)(void *compat_mgr, uint32_t appid,
                                        const char *tool_name, const char *config,
                                        uint32_t priority);
static fn_SetCompatToolMapping g_set_mapping;

void np_compat_set_mapping_fn(uintptr_t set_compat_tool_mapping) {
    g_set_mapping = (fn_SetCompatToolMapping)set_compat_tool_mapping;
}

void np_compat_map_tool(void *compat_mgr, uint32_t appid, const char *tool_name) {
    if (!g_set_mapping || !compat_mgr || !tool_name) {
        NP_ERR("np_compat_map_tool: app %u cannot be mapped to %s", appid,
               tool_name ? tool_name : "no tool");
        return;
    }
    g_set_mapping(compat_mgr, appid, tool_name, "",
                  appid ? COMPAT_MAPPING_PRIORITY_APP
                        : COMPAT_MAPPING_PRIORITY_GLOBAL);
}

typedef void (*fn_YldRegisterTool)(void *compat_mgr, void *tool);
static fn_YldRegisterTool g_yld_register_tool;

void np_compat_set_register_fn(uintptr_t yld_register_tool) {
    g_yld_register_tool = (fn_YldRegisterTool)yld_register_tool;
}

static void *g_manager;

static void *np_compat_registered_tool_named(void *compat_mgr, const char *wanted);

void *np_compat_registered_tool_for_name(void *compat_mgr, const char *name) {
    return np_compat_registered_tool_named(compat_mgr, name);
}

void *np_compat_manager(void) {
    return g_manager;
}

static void register_tool_entry(void *compat_mgr,
                                    const char *name, const char *display) {
    static uint8_t rosetta_tool[COMPAT_TOOL_STRIDE + 64];
    static uint8_t fex_tool[COMPAT_TOOL_STRIDE + 64];

    uint8_t *tool = strcmp(name, TOOL_DIR_NAME_FEX) == 0 ? fex_tool : rosetta_tool;
    memset(tool, 0, COMPAT_TOOL_STRIDE + 64);

    static const char from_oslist[] = "windows";
    static const char to_oslist[] = "macos";

    *(uint32_t *)(tool + COMPAT_TOOL_FLAGS_OFF) = COMPAT_TOOL_FLAG_NO_APP;
    *(const char **)(tool + COMPAT_TOOL_NAME_OFF) = name;
    *(const char **)(tool + COMPAT_TOOL_DISPLAY_OFF) = display;
    *(const char **)(tool + np_compat_tool_off(COMPAT_TOOL_INSTALL_OFF)) =
        np_compat_tool_dir_named(name);
    *(uint32_t *)(tool + np_compat_tool_off(COMPAT_TOOL_APPID_OFF)) = 0;
    *(uint32_t *)(tool + np_compat_tool_off(COMPAT_TOOL_PLATFORM_OFF)) = COMPAT_PLATFORM_WINDOWS;
    *(int32_t *)(tool + COMPAT_TOOL_PRIORITY_OFF) = 0;
    *(const char **)(tool + np_compat_tool_off(COMPAT_TOOL_FROM_OSLIST_OFF)) = from_oslist;
    *(const char **)(tool + np_compat_tool_off(COMPAT_TOOL_TO_OSLIST_OFF)) = to_oslist;

    g_yld_register_tool(compat_mgr, tool);
    NP_LOG("np_compat_register_crossover: registered %s into manager 0x%llx",
           name, (unsigned long long)(uintptr_t)compat_mgr);
}

void np_compat_register_crossover(void *compat_mgr) {
    static int registered_rosetta = 0;
    static int registered_fex = 0;

    if (compat_mgr)
        g_manager = compat_mgr;

    if (!compat_mgr || !g_yld_register_tool)
        return;

    if (!registered_rosetta &&
        !np_compat_registered_tool_named(compat_mgr, TOOL_DIR_NAME)) {
        register_tool_entry(compat_mgr, TOOL_DIR_NAME, TOOL_DISPLAY_NAME);
        registered_rosetta = 1;
    }

    if (!registered_fex &&
        !np_compat_registered_tool_named(compat_mgr, TOOL_DIR_NAME_FEX)) {
        register_tool_entry(compat_mgr, TOOL_DIR_NAME_FEX, TOOL_DISPLAY_NAME_FEX);
        registered_fex = 1;
    }
}

static void *np_compat_registered_tool_named(void *compat_mgr, const char *wanted) {
    if (!compat_mgr || !wanted)
        return NULL;

    uint8_t *base = (uint8_t *)compat_mgr;
    uint8_t *array = *(uint8_t **)(base + COMPAT_MANAGER_TOOL_ARRAY_OFF);
    uint32_t count = *(uint32_t *)(base + COMPAT_MANAGER_TOOL_COUNT_OFF);
    if (!array || count > COMPAT_MANAGER_TOOLS_MAX)
        return NULL;

    for (uint32_t i = 0; i < count; i++) {
        uint8_t *entry = array + (size_t)i * np_compat_tool_stride();
        const char *name = *(const char **)(entry + COMPAT_TOOL_NAME_OFF);
        if (name && strcmp(name, wanted) == 0)
            return entry;
    }
    return NULL;
}

void *np_compat_registered_tool(void *compat_mgr) {
    void *entry = np_compat_registered_tool_named(compat_mgr, TOOL_DIR_NAME);
    if (entry) return entry;
    return np_compat_registered_tool_named(compat_mgr, TOOL_DIR_NAME_FEX);
}
