#ifndef MUTEGUARD_H
#define MUTEGUARD_H
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define CONST_VTABLE
#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0A00
#pragma warning(push)
/* Windows SDK 10.0.19041 winbase.h uses a legacy defined() macro. */
#pragma warning(disable:5105)
#include <windows.h>
#pragma warning(pop)
#include <objbase.h>
#include <stddef.h>
#include <wchar.h>

#define MG_LIMIT 128
#define MG_NAME 260
#define MG_ID 1024
#define MG_CHANGED (WM_APP + 1)
#define MG_SETTINGS (WM_APP + 2)

typedef enum OutputMode { OUTPUT_IGNORE = 0, OUTPUT_MUTE = 1, OUTPUT_PROTECT = 2 } OutputMode;
typedef struct OutputRule { wchar_t *id, *name; OutputMode mode; } OutputRule;
typedef struct Config {
    BOOL enabled;
    size_t app_count, output_count;
    wchar_t **apps;
    OutputRule *outputs;
    BOOL all_outputs;
} Config;
typedef struct OutputInfo { wchar_t *id, *name; DWORD state; } OutputInfo;
typedef struct OutputList { OutputInfo *items; size_t count; } OutputList;
typedef struct Engine Engine;

_Ret_writes_bytes_maybenull_(bytes) void *mg_alloc(size_t bytes);
void mg_free(void *p);
wchar_t *mg_string(const wchar_t *s);
void config_init(Config *c);
void config_free(Config *c);
BOOL config_copy(Config *dst, const Config *src);
BOOL config_add_app(Config *c, const wchar_t *name);
void config_remove_app(Config *c, size_t index);
BOOL config_set_output(Config *c, const wchar_t *id, const wchar_t *name, OutputMode mode);
OutputMode config_output_mode(const Config *c, const wchar_t *id);
OutputMode config_saved_output_mode(const Config *c, const wchar_t *id);
BOOL config_matches_app(const Config *c, const wchar_t *name);
BOOL config_valid_app(const wchar_t *name);
BOOL config_path(wchar_t *path, size_t count);
/* Missing = S_FALSE, malformed = HRESULT_FROM_WIN32(ERROR_INVALID_DATA). */
HRESULT config_load(Config *c, const wchar_t *path);
HRESULT config_save(const Config *c, const wchar_t *path);

HRESULT outputs_enumerate(OutputList *list);
void outputs_free(OutputList *list);
Engine *engine_start(const Config *c, HWND notify);
BOOL engine_update(Engine *e, const Config *c);
HRESULT engine_status(Engine *e);
void engine_stop(Engine *e);
#ifdef MG_TESTING
/* Test build only: reveal a real render endpoint through the device-added path. */
Engine *engine_test_start(const Config *c, const wchar_t *held_endpoint);
BOOL engine_test_reveal(Engine *e);
#endif

extern HINSTANCE g_instance;
extern HWND g_owner;
extern Config g_config;
extern Engine *g_engine;
extern wchar_t g_config_path[MAX_PATH];
BOOL app_commit(const Config *c);
void settings_open(void);
void settings_changed(BOOL devices, BOOL enabled);
void settings_close(void);
BOOL settings_message(MSG *m);

#endif
