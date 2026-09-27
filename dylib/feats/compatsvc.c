// CompatManager WebUI service.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#include "../util/file.h"

#include "compatsvc.h"
#include "compat.h"
#include "webui.h"
#include "../resolver/anchor.h"
#include "../util/log.h"

// Route names carry the interface version the page asks for.
#define ROUTE_GET_COMPAT_TOOLS     "CompatManager.GetCompatTools#1"
#define ROUTE_SPECIFY_COMPAT_TOOL  "CompatManager.SpecifyCompatTool#1"
#define ROUTE_NOTIFY_STATE_CHANGED "CompatManager.NotifyStateChanged#1"

// Mangled RTTI names of the message types. A client that builds the CompatManager
// page compiles these without registering the service, so the vtables are there to
// be found. A client whose page specifies the tool directly carries none of them.
#define TYPE_GET_REQUEST      "37CCompatManager_GetCompatTools_Request"
#define TYPE_GET_RESPONSE     "38CCompatManager_GetCompatTools_Response"
#define TYPE_SPECIFY_REQUEST  "40CCompatManager_SpecifyCompatTool_Request"
#define TYPE_SPECIFY_RESPONSE "41CCompatManager_SpecifyCompatTool_Response"
#define TYPE_STATE_CHANGED    "40CCompatManager_StateChanged_Notification"

// CCompatManager_GetCompatTools_Request.
#define REQ_HAS_BITS_OFF 16
#define REQ_APPID_OFF    24
#define REQ_HAS_APPID    1

// CCompatManager_SpecifyCompatTool_Request. An unset name field points at the
// shared empty string, which clears the mapping back to its default.
#define SPECIFY_TOOL_NAME_OFF 24
#define SPECIFY_APPID_OFF     32
#define SPECIFY_HAS_APPID     2

// Result the page reads as success, and the one it reads as failure.
#define RESULT_OK   1
#define RESULT_FAIL 2

// Field numbers of the two messages, from the interface the page is built against.
#define TOOL_NAME            1
#define TOOL_DISPLAY_NAME    2
#define TOOL_IS_DEFAULT      3
#define TOOL_IS_ACTIVE       4
#define TOOL_IS_LEGACY       5
#define TOOL_IS_INCOMPATIBLE 6

#define RESP_TOOLS    1
#define RESP_SELECTED 3
#define RESP_DEFAULT  4

// Wire types this writes.
#define WIRE_VARINT 0
#define WIRE_BYTES  2

// Per-tool and per-reply size caps, for safety.
#define TOOL_MAX  512
#define BODY_MAX  (TOOL_MAX * 8)

// No appid equals the global Steam Play setting, answered with windows.
#define PLATFORMS_UNSCOPED COMPAT_PLATFORM_WINDOWS

// A tool's own appid gets this result instead of a list.
#define RESULT_IS_A_TOOL 128

// Display label for the "no tool" entry.
#define TOOL_NONE_LABEL "No Compatibility Tool"

#define NP_ENV_PREFIX "notproton-env-v1:"
#define NP_ENV_FILE  "notproton-environment"
#define NP_ENV_MAX   (256 * 1024)

static int mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (!len || len >= sizeof tmp) return -1;
    memcpy(tmp, path, len + 1);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = '\0';
        if (tmp[0] && mkdir(tmp, 0755) != 0 && errno != EEXIST)
            return -1;
        tmp[i] = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int find_library_root(uint32_t appid, char *out, size_t out_size) {
    const char *home = np_home_dir();
    if (!home) return -1;

    char vdf[PATH_MAX];
    int n = snprintf(vdf, sizeof vdf,
                     "%s/Library/Application Support/Steam/steamapps/libraryfolders.vdf",
                     home);
    if (n < 0 || (size_t)n >= sizeof vdf) return -1;

    FILE *f = fopen(vdf, "r");
    if (!f) {
        n = snprintf(out, out_size, "%s/Library/Application Support/Steam", home);
        return n >= 0 && (size_t)n < out_size ? 0 : -1;
    }

    char file[1024 * 1024];
    size_t used = fread(file, 1, sizeof file - 1, f);
    fclose(f);
    file[used] = '\0';

    char app_key[32];
    n = snprintf(app_key, sizeof app_key, "\"%u\"", appid);
    if (n < 0 || (size_t)n >= sizeof app_key) return -1;

    const char *scan = file;
    while ((scan = strstr(scan, "\"path\"")) != NULL) {
        const char *value = strchr(scan, '"');
        value = value ? strchr(value + 1, '"') : NULL;
        value = value ? strchr(value + 1, '"') : NULL;
        if (!value) break;
        value++;
        const char *end = strchr(value, '"');
        if (!end) break;

        char library[PATH_MAX];
        size_t lib_len = (size_t)(end - value);
        if (lib_len == 0 || lib_len >= sizeof library) {
            scan = end + 1;
            continue;
        }
        memcpy(library, value, lib_len);
        library[lib_len] = '\0';

        const char *next_path = strstr(end + 1, "\"path\"");
        const char *app = strstr(end + 1, app_key);
        if (app && (!next_path || app < next_path)) {
            n = snprintf(out, out_size, "%s", library);
            return n >= 0 && (size_t)n < out_size ? 0 : -1;
        }
        scan = end + 1;
    }

    return -1;
}
static int save_environment_file(uint32_t appid, const char *text) {
    if (!appid || !text || strlen(text) > NP_ENV_MAX)
        return -1;

    char library[PATH_MAX];
    if (find_library_root(appid, library, sizeof library) != 0)
        return -1;

    char prefix[PATH_MAX];
    int n = snprintf(prefix, sizeof prefix, "%s/steamapps/compatdata/%u", library, appid);
    if (n < 0 || (size_t)n >= sizeof prefix || mkdir_p(prefix) != 0)
        return -1;

    char file[PATH_MAX];
    char temp[PATH_MAX];
    n = snprintf(file, sizeof file, "%s/%s", prefix, NP_ENV_FILE);
    if (n < 0 || (size_t)n >= sizeof file) return -1;
    n = snprintf(temp, sizeof temp, "%s.new.%d", file, getpid());
    if (n < 0 || (size_t)n >= sizeof temp) return -1;

    if (!text[0]) {
        return unlink(file) == 0 || errno == ENOENT ? 0 : -1;
    }

    FILE *f = fopen(temp, "w");
    if (!f) return -1;
    if (fwrite(text, 1, strlen(text), f) != strlen(text) || fflush(f) != 0) {
        fclose(f);
        unlink(temp);
        return -1;
    }
    if (fclose(f) != 0) {
        unlink(temp);
        return -1;
    }
    if (rename(temp, file) != 0) {
        unlink(temp);
        return -1;
    }
    return 0;
}

// Protobuf writer.
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    int      ok;
} np_pb_t;

static void pb_byte(np_pb_t *w, uint8_t b) {
    if (w->len >= w->cap) { w->ok = 0; return; }
    w->buf[w->len++] = b;
}

static void pb_varint(np_pb_t *w, uint64_t v) {
    do {
        uint8_t b = (uint8_t)(v & 0x7f);
        v >>= 7;
        pb_byte(w, v ? (uint8_t)(b | 0x80) : b);
    } while (v && w->ok);
}

static void pb_tag(np_pb_t *w, uint32_t field, uint32_t wire) {
    pb_varint(w, ((uint64_t)field << 3) | wire);
}

static void pb_blob(np_pb_t *w, uint32_t field, const uint8_t *bytes, size_t len) {
    pb_tag(w, field, WIRE_BYTES);
    pb_varint(w, len);
    for (size_t i = 0; i < len && w->ok; i++)
        pb_byte(w, bytes[i]);
}

static void pb_string(np_pb_t *w, uint32_t field, const char *str) {
    if (!str) return;
    pb_blob(w, field, (const uint8_t *)str, strlen(str));
}

static void pb_bool(np_pb_t *w, uint32_t field, int set) {
    pb_tag(w, field, WIRE_VARINT);
    pb_varint(w, set ? 1 : 0);
}

static void pb_tool(np_pb_t *w, uint32_t resp_field,
                    const char *name, const char *display,
                    int is_active, int is_incompatible) {
    uint8_t item[TOOL_MAX];
    np_pb_t t = { item, sizeof item, 0, 1 };
    pb_string(&t, TOOL_NAME, name);
    pb_string(&t, TOOL_DISPLAY_NAME, display);
    pb_bool(&t, TOOL_IS_DEFAULT, 0);
    pb_bool(&t, TOOL_IS_ACTIVE, is_active);
    pb_bool(&t, TOOL_IS_LEGACY, 0);
    pb_bool(&t, TOOL_IS_INCOMPATIBLE, is_incompatible);
    if (t.ok)
        pb_blob(w, resp_field, item, t.len);
    else
        w->ok = 0;
}

static int offered(const uint8_t *tool, uint32_t platforms, int bypass) {
    uint32_t converts_from =
        *(const uint32_t *)(tool + np_compat_tool_off(COMPAT_TOOL_PLATFORM_OFF));
    uint32_t gate = *(const uint32_t *)(tool + COMPAT_TOOL_GATE_FLAGS_OFF);

    if (!(platforms & converts_from)) return 0;
    if ((gate & COMPAT_TOOL_GATE_HIDDEN) && !bypass) return 0;
    return 1;
}

static const char *tool_name(const void *tool) {
    if (!tool) return NULL;
    return *(const char *const *)((const uint8_t *)tool + COMPAT_TOOL_NAME_OFF);
}

static int get_compat_tools(uintptr_t request, uintptr_t response) {
    uint32_t has_bits = *(const uint32_t *)(request + REQ_HAS_BITS_OFF);
    uint32_t appid    = (has_bits & REQ_HAS_APPID)
                      ? *(const uint32_t *)(request + REQ_APPID_OFF) : 0;

    uint8_t *mgr = (uint8_t *)np_compat_manager();
    if (!mgr) {
        // An empty list is a valid answer, and the page renders it as no dropdown
        // rather than as an error.
        NP_WARN("compatsvc: app %u asked for tools before any manager was seen, so "
                "the page gets an empty list", appid);
        return RESULT_OK;
    }

    const uint8_t *array = *(const uint8_t **)(mgr + COMPAT_MANAGER_TOOL_ARRAY_OFF);
    uint32_t count       = *(const uint32_t *)(mgr + COMPAT_MANAGER_TOOL_COUNT_OFF);
    if (!array || count > COMPAT_MANAGER_TOOLS_MAX) {
        NP_ERR("compatsvc: the manager reports %u tool(s) at %p, which is not a list "
               "this reads", count, (const void *)array);
        return RESULT_FAIL;
    }

    int bypass = mgr[np_compat_enabled_off() + 2] != 0;
    if (appid && !bypass) {
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *tool = array + (size_t)i * np_compat_tool_stride();
            if (*(const uint32_t *)(tool + np_compat_tool_off(COMPAT_TOOL_APPID_OFF)) != appid)
                continue;
            NP_LOG("compatsvc: app %u is itself a compatibility tool, so it is "
                   "offered none", appid);
            return RESULT_IS_A_TOOL;
        }
    }

    uint32_t platforms = appid ? np_compat_valid_platforms(mgr, appid)
                               : PLATFORMS_UNSCOPED;
    // The resolved tool and the raw mapping.
    const char *active = tool_name(np_compat_tool_for_app(mgr, appid));
    // Raw mapping, including names that resolve to no installed tool.
    const char *chosen = appid ? np_compat_app_mapping(mgr, appid)
                               : tool_name(np_compat_chosen_tool(mgr, 0));
    const bool  holds_none = chosen && strcmp(chosen, NP_COMPAT_TOOL_NONE) == 0;
    // The fallback tool for when no choice is made. The wildcard covers apps the
    // client will not run natively (Windows stuff). Left unset where nothing carries the app,
    // so the "Default" entry shows no name.
    const char *fallback = np_compat_wildcard_mapping(mgr, appid);
    if (!fallback && (appid == 0 || np_compat_would_force(mgr, appid)))
        fallback = tool_name(np_compat_registered_tool(mgr));

    int32_t best = 0;
    for (uint32_t i = 0; i < count; i++) {
        int32_t prio = *(const int32_t *)(array + (size_t)i * np_compat_tool_stride()
                                          + COMPAT_TOOL_PRIORITY_OFF);
        if (prio > best) best = prio;
    }

    uint8_t  body[BODY_MAX];
    np_pb_t  w      = { body, sizeof body, 0, 1 };
    uint32_t listed = 0;

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *tool = array + (size_t)i * np_compat_tool_stride();
        if (!offered(tool, platforms, bypass)) continue;

        const char *name = tool_name(tool);
        if (!name) continue;

        int32_t prio = *(const int32_t *)(tool + COMPAT_TOOL_PRIORITY_OFF);
        pb_tool(&w, RESP_TOOLS, name,
                *(const char *const *)(tool + COMPAT_TOOL_DISPLAY_OFF),
                active && strcmp(active, name) == 0,
                best != 0 && prio != best && prio > 0);
        if (!w.ok) {
            NP_ERR("compatsvc: tool %s does not fit the reply", name);
            return RESULT_FAIL;
        }
        listed++;
    }

    // "No tool" entry.
    if (appid && (fallback || holds_none)) {
        pb_tool(&w, RESP_TOOLS, NP_COMPAT_TOOL_NONE, TOOL_NONE_LABEL,
                holds_none, 0);
        if (!w.ok) {
            NP_ERR("compatsvc: the entry for using no tool does not fit the reply");
            return RESULT_FAIL;
        }
        listed++;
    }

    pb_string(&w, RESP_SELECTED, chosen ? chosen : "");

    // The page renders this as "Default".
    if (fallback)
        pb_string(&w, RESP_DEFAULT, fallback);

    if (!w.ok) {
        NP_ERR("compatsvc: %u tool(s) do not fit the reply to app %u", listed, appid);
        return RESULT_FAIL;
    }
    if (!np_webui_parse_into(response, body, (uint32_t)w.len)) {
        NP_ERR("compatsvc: the client rejected the %zu byte reply built for app %u",
               w.len, appid);
        return RESULT_FAIL;
    }

    NP_LOG("compatsvc: app %u is offered %u entr(ies) from %u tool(s) on platforms "
           "0x%x, mapped to %s, running under %s, defaulting to %s", appid, listed,
           count, platforms, chosen ? chosen : "no tool",
           active ? active : "no tool", fallback ? fallback : "no tool");
    return RESULT_OK;
}

// Reads a protobuf string field.
static const char *string_field(uintptr_t field) {
    uintptr_t str = *(const uintptr_t *)field & ~(uintptr_t)1;
    if (!str) return "";

    const int8_t *bytes = (const int8_t *)str;
    return bytes[23] < 0 ? *(const char *const *)str : (const char *)str;
}

// The page takes the result code as the answer and refetches the tool list.
static int specify_compat_tool(uintptr_t request, uintptr_t response) {
    uint32_t has_bits = *(const uint32_t *)(request + REQ_HAS_BITS_OFF);
    uint32_t appid    = (has_bits & SPECIFY_HAS_APPID)
                      ? *(const uint32_t *)(request + SPECIFY_APPID_OFF) : 0;
    const char *tool  = string_field(request + SPECIFY_TOOL_NAME_OFF);
    (void)response;

    if (strncmp(tool, NP_ENV_PREFIX, strlen(NP_ENV_PREFIX)) == 0) {
        const char *text = tool + strlen(NP_ENV_PREFIX);
        if (save_environment_file(appid, text) != 0) {
            NP_ERR("compatsvc: failed to save environment settings for app %u", appid);
            return RESULT_FAIL;
        }
        NP_LOG("compatsvc: saved environment settings for app %u", appid);
        return RESULT_OK;
    }

    uint8_t *mgr = (uint8_t *)np_compat_manager();
    if (!mgr) {
        NP_ERR("compatsvc: app %u cannot be mapped to %s before any manager was seen",
               appid, *tool ? tool : "its default");
        return RESULT_FAIL;
    }

    np_compat_map_tool(mgr, appid, tool);
    NP_LOG("compatsvc: app %u now maps to %s", appid, *tool ? tool : "its default");
    return RESULT_OK;
}

static uintptr_t g_state_changed;
static int g_routes = NP_COMPATSVC_UNTRIED;

void np_compatsvc_state_changed(void) {
    if (!g_state_changed) return;
    np_webui_notify(ROUTE_NOTIFY_STATE_CHANGED, g_state_changed);
}

void np_compatsvc_register(const struct mach_header_64 *mh, intptr_t slide,
                           uintptr_t text_base, size_t text_size) {
    // Every path below settles g_routes, so it doubles as the once-only guard. A registry
    // that is not up yet leaves it untried, which is what brings the next lookup back.
    if (g_routes != NP_COMPATSVC_UNTRIED || !np_webui_registry()) return;

    uintptr_t request  = np_rtti_vptr(mh, slide, text_base, text_size, TYPE_GET_REQUEST);
    uintptr_t response = np_rtti_vptr(mh, slide, text_base, text_size, TYPE_GET_RESPONSE);
    if (!request || !response) {
        // A fact about the build, not a fault: every client whose page specifies the tool
        // directly lands here. hook_webpatch owns the error, where the cost is known.
        g_routes = NP_COMPATSVC_ABSENT;
        NP_LOG("compatsvc: the client has no message types for %s",
               ROUTE_GET_COMPAT_TOOLS);
        return;
    }

    np_webui_register_route(ROUTE_GET_COMPAT_TOOLS, request, response, get_compat_tools);

    uintptr_t specify_request  = np_rtti_vptr(mh, slide, text_base, text_size,
                                              TYPE_SPECIFY_REQUEST);
    uintptr_t specify_response = np_rtti_vptr(mh, slide, text_base, text_size,
                                              TYPE_SPECIFY_RESPONSE);
    if (!specify_request || !specify_response) {
        // Half a CompatManager. Unlike the case above this is not a shape any client
        // ships: the list can be read and nothing can be set from it.
        g_routes = NP_COMPATSVC_ABSENT;
        NP_ERR("compatsvc: the client answers %s but has no message types for %s",
               ROUTE_GET_COMPAT_TOOLS, ROUTE_SPECIFY_COMPAT_TOOL);
        return;
    }

    np_webui_register_route(ROUTE_SPECIFY_COMPAT_TOOL, specify_request,
                            specify_response, specify_compat_tool);

    g_routes = NP_COMPATSVC_READY;

    uintptr_t notification = np_rtti_vptr(mh, slide, text_base, text_size,
                                          TYPE_STATE_CHANGED);
    g_state_changed = notification ? np_webui_new_message(notification) : 0;
    if (!g_state_changed)
        NP_WARN("compatsvc: the page will show a stale tool until it is reopened");
}

int np_compatsvc_routes(void) {
    return g_routes;
}
