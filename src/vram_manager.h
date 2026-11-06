// VRAM Manager for X-Plane 12
// Dynamic VRAM management: adaptive render distance + VRAM purge, built to
// coexist with other plugins (never owns the datarefs it touches).
#pragma once

// Version scheme: x.x.xx (the last two digits are a build/patch counter).
#define VRAMMGR_VERSION "1.9.18"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMDataAccess.h"
#include "XPLMUtilities.h"
#include "XPLMMenus.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPWidgets.h"
#include "XPStandardWidgets.h"

// ---------------------------------------------------------------------------
// logging / time
// ---------------------------------------------------------------------------
void  Log(const char* fmt, ...);
double NowSec();

// ---------------------------------------------------------------------------
// localization
// ---------------------------------------------------------------------------
enum StrId {
    S_TITLE = 0,
    S_AUTO, S_AUTOPURGE, S_TARGET, S_MINLOD,
    S_PURGE, S_RESET, S_SAVE, S_HIDE, S_LANG,
    S_ABOUT, S_RELOADSC, S_LIVE, S_GPU,
    S_MENU_SHOW, S_MENU_AUTO, S_MENU_PURGE, S_MENU_RESET, S_MENU_RELOAD, S_MENU_UI,
    S_MENU_THEME, S_MENU_ABOUT,
    S_COUNT
};
const char* T(int id);
int  GetLang();
void SetLang(int lang);

// status line identifiers (text is localised in the UI layer)
enum StatusId {
    ST_OFF = 0, ST_ACTIVE, ST_REDUCE, ST_HOLD, ST_RECOVER, ST_YIELD,
    ST_UNAVAIL, ST_PURGE_LOD, ST_PURGE_TEX, ST_LOADING, ST_SIMLOCK, ST_EXT,
    ST_WAITREF
};

// ---------------------------------------------------------------------------
// settings (persisted to Output/preferences/VRAMManager.prf)
// ---------------------------------------------------------------------------
struct Settings {
    int   autoManage    = 1;
  int   confVer       = 0;     // config schema version (migrations)
    int   autoPurge     = 1;   // emergency render-distance purge when stuck at min
    int   showOnStart   = 1;
    int   darkTheme     = 0;   // 0 = light/classic window (readable), 1 = dark window
    int   uiStyle       = 2;   // 2 = modern self-drawn panel, 1 = classic XPWidgets
    int   debug         = 0;
    int   lang          = -1;    // -1 = auto, 0 = EN, 1 = 中文
    float uiScale       = 0.0f;  // 0 = auto
    float targetPct     = 82.0f; // reduce render distance above this
    float restorePct    = 68.0f; // restore render distance below this
    float minFactor     = 0.55f; // lowest render distance (fraction of X-Plane's)
    float maxFactor     = 1.00f;
    float stepPct       = 0.02f; // small steps = the change is not noticeable
    float purgeDepth    = 0.18f; // a purge dips by this fraction of the current value
    float purgeHold     = 2.0f;  // ...and holds it only this long (seconds)
    int   cloudTrim     = 0;     // 0 off, 1 light, 2 max, 3 auto
    float fpsTarget     = 0.0f;  // 0 = off, otherwise the FPS to hold
    float pagingM       = 6000.0f; // texture paging distance in metres, 0 = X-Plane default
    float texBudget     = 1.0f;  // multiplier on X-Plane's texture pool budget
    int   texBudgetAuto = 0;     // 1 = enlarge X-Plane's texture pool automatically
    int   compTex       = 0;     // 1 = keep scenery textures compressed in VRAM
    int   texGuard      = 1;     // 0 off, 1 gentle (-2%), 2 strong (-6%)
    int   microSweep    = 0;     // periodic paging dips (can hitch - opt in)
    int   priority      = 0;     // 1 = raise the sim process to HIGH priority
    int   timeres       = 0;     // 1 = ask for a 1 ms system timer
    int   affinity      = 0;     // 1 = pin to the first half of the logical CPUs
    int   winX          = -1;
    int   winY          = -1;
};
extern Settings gSet;
void Config_Load();
void Config_Save();
void Config_Path(char* out, int n);

// ---------------------------------------------------------------------------
// VRAM sampling (PDH per-process GPU memory + DXGI budget)
// ---------------------------------------------------------------------------
struct VramInfo {
    bool     valid       = false;
    bool     fromPdh     = false;
    int      source      = 0; // 0 none, 1 PDH per-process, 2 DXGI, 3 PDH adapter (system)
    uint64_t processBytes = 0; // this process (X-Plane) dedicated VRAM
    uint64_t totalBytes   = 0; // all processes
    uint64_t budgetBytes  = 0; // OS budget for this process
    uint64_t vramBytes    = 0; // adapter dedicated VRAM
    char     adapter[128] = {0};
    float    pct          = 0.0f;
};
bool Vram_Init();
void Vram_Shutdown();
bool Vram_Sample(VramInfo* out);

// ---------------------------------------------------------------------------
// sim datarefs
// ---------------------------------------------------------------------------
enum RefId { REF_LOD = 0, REF_TEXRES, REF_ASYNC, REF_FRAMETIME, REF_COUNT };

struct SimRef {
    XPLMDataRef ref      = nullptr;
    int         type     = 0;
    bool        writable = false;
    char        name[128] = {0};
};
extern SimRef gRef[REF_COUNT];

bool  Refs_Init();   // returns true if the render-distance dataref was found
void  Refs_Retry(double now);  // late registration retry / discovery
void  Refs_Probe();  // dumps every related dataref found in the sim to Log.txt
float RefFloat(int id, float dflt = 0.0f);
int   RefInt(int id, int dflt = 0);
void  RefSetFloat(int id, float v);
bool  AsyncIdle();

// ---------------------------------------------------------------------------
// runtime state shared with the UI
// ---------------------------------------------------------------------------
enum Mode { MODE_OFF = 0, MODE_IDLE, MODE_REDUCE, MODE_HOLD, MODE_RECOVER,
            MODE_PURGE, MODE_YIELD, MODE_UNAVAIL };

struct Runtime {
    VramInfo vram;
    float    fps       = 0.0f;

    // measured effect of a purge / of the reduced render distance
    double   vramBeforeGB = 0.0, vramAfterGB = 0.0;   // last purge, GB
    float    fpsBefore    = 0.0f, fpsAfter   = 0.0f;  // last purge
    double   purgeDoneSec = -1.0;                     // when the result was taken
    float    fpsFull      = 0.0f;                     // EMA of FPS at 100% distance
    float    fpsReduced   = 0.0f;                     // EMA of FPS while reduced
  int      cloudTrimmed  = 0;                        // cloud/shadow knobs currently trimmed
  int      cloudAuto     = 0;                        // level picked by the auto manager
  float    fpsEwma       = 0.0f;                     // smoothed FPS for the governor
  bool     texDropped    = false;                    // X-Plane lowered its texture level
  int      limiter       = 0;                        // 0 none, 1 VRAM, 2 FPS, 3 textures
  float    pagingNow     = 0.0f;                     // texture paging distance in use
  float    texBudget     = 1.0f;
    float    lodNow    = 0.0f;
    float    lodBase   = 0.0f;
    int      texNow    = -1;
    int      texBase   = -1;   // X-Plane's own texture level when we started
    float    factor    = 1.0f;
    int      mode      = MODE_OFF;
    int      statusId  = ST_OFF;
    float    statusArg = 0.0f;
    bool     autoOn    = false;
    bool     lodOk     = false;
    bool     texOk     = false;
    bool     simOverrides = false;
    bool     purging   = false;
    double   lastPurgeSec = -1.0;
    char     conflict[256] = {0};
};
extern Runtime gRt;

// ---------------------------------------------------------------------------
// control engine
// ---------------------------------------------------------------------------
void Ctrl_Init();
void Ctrl_Tick(double now, double dt);
void Ctrl_SetAuto(bool on);
void Ctrl_Reset();             // restore X-Plane's own values
void Ctrl_Purge();             // one-shot VRAM purge (render distance only)
void Ctrl_ReloadScenery();
void Ctrl_ToggleAuto();
void Ctrl_OnSceneryLoaded();

// Texture paging distance (sim/private/controls/tex/paging/max_distance): the
// distance out to which textures stay resident.  Lower = far less VRAM.
void Ctrl_SetPagingDistance(float metres);
void Ctrl_SetTextureBudget(float mult);
void Ctrl_SetTextureCompression(bool on);

// Logs which private cloud/shadow knobs exist in this X-Plane build.
void Ctrl_ProbeVramKnobs();

// Cloud / shadow VRAM trimming.  These knobs are discovered at runtime: only
// datarefs that exist, are writable and are integers are touched, and every
// original value is restored when the trim is switched off.
bool Ctrl_ApplyCloudTrim(int level);
void Ctrl_RestoreCloudTrim();
void Ctrl_DumpVramCandidates();

// ---------------------------------------------------------------------------
// UI (modern self-drawn panel lives in panel.h / panel.cpp)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// menu
// ---------------------------------------------------------------------------
void Menu_Init();
void Menu_Shutdown();
void Menu_Refresh();
bool Menu_Ready();

// conflict scan of other loaded plugins / FlyWithLua scripts
void ScanOtherPlugins();
