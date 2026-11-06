// Plugin entry points, config file, flight loop.
#include "vram_manager.h"
#include "panel.h"
#include <mmsystem.h>

#include <cwchar>

Settings gSet;

// ---------------------------------------------------------------------------
void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char line[1152];
    _snprintf(line, sizeof(line), "[VRAMManager] %s\n", buf);
    line[sizeof(line) - 1] = 0;
    XPLMDebugString(line);
}

double NowSec() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

// ---------------------------------------------------------------------------
// config
// ---------------------------------------------------------------------------
void Config_Path(char* out, int n) {
    // XPLMGetPrefsPath() gives a *file* path on X-Plane 12 (.../Output/preferences/
    // Set X-Plane.prf), so build the folder from the system path instead.
    char sys[512] = {0};
    XPLMGetSystemPath(sys);
    size_t len = strlen(sys);
    while (len > 0 && (sys[len - 1] == '\\' || sys[len - 1] == '/')) sys[--len] = 0;
    _snprintf(out, n, "%s%cOutput%cpreferences%cVRAMManager.prf", sys, '/', '/', '/');
    out[n - 1] = 0;
}

static std::wstring Utf8ToWide(const char* s) {
    if (!s || !*s) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring w((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], len);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

void Config_Load() {
    char path[600];
    Config_Path(path, sizeof(path));
    std::wstring wp = Utf8ToWide(path);
    FILE* f = _wfopen(wp.c_str(), L"rb");
    if (!f) {
        Log("VRAM: no config file yet (%s), using defaults", path);
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == ';' || *p == '\r' || *p == '\n' || *p == 0) continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = p;
        char* val = eq + 1;
        char* e = key + strlen(key);
        while (e > key && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        while (*val == ' ' || *val == '\t') ++val;
        e = val + strlen(val);
        while (e > val && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;

        if (!strcmp(key, "auto"))           gSet.autoManage = atoi(val);
        else if (!strcmp(key, "autopurge")) gSet.autoPurge = atoi(val);
        else if (!strcmp(key, "texpurge")) { /* removed in 1.1: textures are never touched */ }
        else if (!strcmp(key, "show"))      gSet.showOnStart = atoi(val);
        else if (!strcmp(key, "debug"))     gSet.debug = atoi(val);
        else if (!strcmp(key, "lang"))      gSet.lang = atoi(val);
        else if (!strcmp(key, "uiscale"))   gSet.uiScale = (float)atof(val);
        else if (!strcmp(key, "dark"))      gSet.darkTheme = atoi(val);
        else if (!strcmp(key, "ui"))        gSet.uiStyle = atoi(val);
        else if (!strcmp(key, "target"))    gSet.targetPct = (float)atof(val);
        else if (!strcmp(key, "restore"))   gSet.restorePct = (float)atof(val);
        else if (!strcmp(key, "min"))       gSet.minFactor = (float)atof(val);
        else if (!strcmp(key, "max"))       gSet.maxFactor = (float)atof(val);
        else if (!strcmp(key, "step"))      gSet.stepPct = (float)atof(val);
        else if (!strcmp(key, "pdepth"))    gSet.purgeDepth = (float)atof(val);
        else if (!strcmp(key, "phold"))     gSet.purgeHold = (float)atof(val);
        else if (!strcmp(key, "cloudtrim")) gSet.cloudTrim = atoi(val);
        else if (!strcmp(key, "fpstarget")) gSet.fpsTarget = (float)atof(val);
        else if (!strcmp(key, "texguard"))  gSet.texGuard = atoi(val);
        else if (!strcmp(key, "microsweep")) gSet.microSweep = atoi(val);
        else if (!strcmp(key, "paging"))    gSet.pagingM = (float)atof(val);
        else if (!strcmp(key, "texbudget")) gSet.texBudget = (float)atof(val);
        else if (!strcmp(key, "texbudgetauto")) gSet.texBudgetAuto = atoi(val);
        else if (!strcmp(key, "comptex"))    gSet.compTex = atoi(val);
        else if (!strcmp(key, "priority"))  gSet.priority = atoi(val);
        else if (!strcmp(key, "timeres"))   gSet.timeres = atoi(val);
        else if (!strcmp(key, "affinity"))  gSet.affinity = atoi(val);
        else if (!strcmp(key, "winx"))      gSet.winX = atoi(val);
        else if (!strcmp(key, "winy"))      gSet.winY = atoi(val);
    }

    // ---- one-time config migrations -------------------------------------
    bool migrated = false;
    if (gSet.confVer < 2) {
        // v2: auto management on by default
        gSet.confVer    = 2;
        gSet.autoManage = 1;
        migrated = true;
    }
    if (gSet.confVer < 3) {
        // v3: the aggressive options added in 1.9.x can cost frame rate on some
        // systems, so they ship off again.
        gSet.confVer       = 3;
        gSet.priority      = 0;
        gSet.timeres       = 0;
        gSet.microSweep    = 0;
        gSet.texBudgetAuto = 0;
        gSet.texBudget     = 1.0f;
        migrated = true;
        Log("VRAM: config migrated to v3 - aggressive options reset to safe defaults");
    }
    if (gSet.confVer < 4) {
        // v4: keep scenery textures resident out to 6 km instead of 10 km.  It
        // is the single biggest VRAM saving that stays hard to notice.
        gSet.confVer = 4;
        if (gSet.pagingM <= 0.0f) {
            gSet.pagingM = 6000.0f;
            migrated = true;
            Log("VRAM: config migrated to v4 - texture paging distance defaults to 6000 m");
        }
    }
    // "Unnoticeable" limits: small steps, short and shallow purges.
    if (gSet.stepPct > 0.025f) gSet.stepPct = 0.025f;
    if (gSet.stepPct < 0.005f) gSet.stepPct = 0.005f;
    if (gSet.purgeDepth < 0.05f) gSet.purgeDepth = 0.05f;
    if (gSet.purgeDepth > 0.50f) gSet.purgeDepth = 0.50f;
    if (gSet.purgeHold < 1.0f)  gSet.purgeHold = 1.0f;
    if (gSet.purgeHold > 5.0f)  gSet.purgeHold = 5.0f;
    fclose(f);

    if (gSet.targetPct < 50) gSet.targetPct = 50;
    if (gSet.targetPct > 98) gSet.targetPct = 98;
    if (gSet.restorePct >= gSet.targetPct) gSet.restorePct = gSet.targetPct - 6.0f;
    if (gSet.restorePct < 30) gSet.restorePct = 30;
    if (gSet.minFactor < 0.2f) gSet.minFactor = 0.2f;
    if (gSet.minFactor > 1.0f) gSet.minFactor = 1.0f;
    if (gSet.maxFactor < gSet.minFactor) gSet.maxFactor = 1.0f;
    if (gSet.stepPct < 0.005f) gSet.stepPct = 0.005f;
    Log("VRAM: config loaded (auto=%d cloudtrim=%d fps=%.0f show=%d target=%.1f min=%.3f confver=%d)",
        gSet.autoManage, gSet.cloudTrim, (double)gSet.fpsTarget, gSet.showOnStart,
        (double)gSet.targetPct, (double)gSet.minFactor, gSet.confVer);
    if (migrated) {
        Log("VRAM: config migrated to schema v2 - auto management forced ON");
        Config_Save();
    }
}

void Config_Save() {
    char path[600];
    Config_Path(path, sizeof(path));
    std::wstring wp = Utf8ToWide(path);
    FILE* f = _wfopen(wp.c_str(), L"wb");
    if (!f) {
        Log("VRAM: cannot write config %s", path);
        return;
    }
    fprintf(f, "# VRAM Manager for X-Plane 12 (render distance only - textures are never changed)\n");
    fprintf(f, "confver=%d\n", gSet.confVer);
    fprintf(f, "auto=%d\n", gSet.autoManage);
    fprintf(f, "autopurge=%d\n", gSet.autoPurge);
    fprintf(f, "show=%d\n", gSet.showOnStart);
    fprintf(f, "debug=%d\n", gSet.debug);
    fprintf(f, "lang=%d\n", gSet.lang);
    fprintf(f, "uiscale=%.2f\n", gSet.uiScale);
    fprintf(f, "dark=%d\n", gSet.darkTheme);
    fprintf(f, "ui=%d\n", gSet.uiStyle);
    fprintf(f, "target=%.1f\n", gSet.targetPct);
    fprintf(f, "restore=%.1f\n", gSet.restorePct);
    fprintf(f, "min=%.3f\n", gSet.minFactor);
    fprintf(f, "max=%.3f\n", gSet.maxFactor);
    fprintf(f, "step=%.3f\n", gSet.stepPct);
    fprintf(f, "pdepth=%.3f\n", gSet.purgeDepth);
    fprintf(f, "phold=%.2f\n", gSet.purgeHold);
    fprintf(f, "cloudtrim=%d\n", gSet.cloudTrim);
    fprintf(f, "fpstarget=%.1f\n", gSet.fpsTarget);
    fprintf(f, "texguard=%d\n", gSet.texGuard);
    fprintf(f, "microsweep=%d\n", gSet.microSweep);
    fprintf(f, "paging=%.0f\n", (double)gSet.pagingM);
    fprintf(f, "texbudget=%.2f\n", (double)gSet.texBudget);
    fprintf(f, "texbudgetauto=%d\n", gSet.texBudgetAuto);
    fprintf(f, "comptex=%d\n", gSet.compTex);
    fprintf(f, "priority=%d\n", gSet.priority);
    fprintf(f, "timeres=%d\n", gSet.timeres);
    fprintf(f, "affinity=%d\n", gSet.affinity);
    fprintf(f, "winx=%d\n", gSet.winX);
    fprintf(f, "winy=%d\n", gSet.winY);
    fclose(f);
}

// ---------------------------------------------------------------------------
// flight loop
// ---------------------------------------------------------------------------
static bool  sEnabled = false;
static double sLastTick = 0.0;
static double sLastHeartbeat = 0.0;
static double sLastMenuTry   = 0.0;
static double sLastPanelTry  = 0.0;

static float FlightLoopCB(float sinceLastCall, float sinceLastLoop, int counter, void* refcon) {
    double now = NowSec();
    double dt = (sLastTick > 0.0) ? (now - sLastTick) : (double)sinceLastCall;
    if (dt <= 0.0 || dt > 5.0) dt = 1.0;
    sLastTick = now;

    // Sampling VRAM costs a PDH/DXGI query (~1 ms).  Only pay for it at full
    // rate when the numbers are actually being watched or acted on.
    static double sLastSample = 0.0;
    double every = (gSet.autoManage || Panel_Visible()) ? 0.5 : 2.0;
    if (sLastSample <= 0.0 || now - sLastSample >= every) {
        sLastSample = now;
        Vram_Sample(&gRt.vram);
    }
    Ctrl_Tick(now, dt);

    // X-Plane drops plugin menus when a plugin is disabled (and the menu bar may
    // not exist yet while starting), so make sure ours exists.  Same for the
    // panel window.
    if (!Menu_Ready() && now - sLastMenuTry > 3.0) {
        sLastMenuTry = now;
        Menu_Init();
    }
    if (!Panel_Active() && now - sLastPanelTry > 3.0) {
        sLastPanelTry = now;
        Panel_Init();
    }

    if (gSet.debug && now - sLastHeartbeat > 30.0) {
        sLastHeartbeat = now;
        Log("VRAM: heartbeat used=%.2fGB pct=%.0f%% factor=%.2f lod=%.3f tex=%d mode=%d fps=%.0f",
            (double)gRt.vram.processBytes / 1073741824.0, gRt.vram.pct, gRt.factor,
            gRt.lodNow, gRt.texNow, gRt.mode, gRt.fps);
    }
    return 0.5f;
}

// ---------------------------------------------------------------------------
// plugin entry points
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Windows-level tuning for the sim process.
//
//   1. HIGH priority class     - background work cannot steal frame time
//   2. power throttling off    - Windows will not lower the CPU state of the
//                                sim threads when it thinks they are idle
//   3. 1 ms timer resolution   - finer scheduling quanta for frame pacing
//   4. affinity (opt-in)       - keeps the sim on the first half of the CPUs,
//                                which are the P-cores on hybrid parts
//
// All of it is reversible from the config (priority / timeres / affinity).
// ---------------------------------------------------------------------------
#ifndef PROCESS_POWER_THROTTLING_CURRENT_VERSION
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#endif
#ifndef PROCESS_POWER_THROTTLING_EXECUTION_SPEED
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#endif

struct PowerThrottleState { unsigned long Version, ControlMask, StateMask; };

static void ApplyProcessTuning() {
    if (gSet.priority > 0) {
        if (SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
            Log("VRAM: process priority raised to HIGH");
        else
            Log("VRAM: could not raise priority (%lu)", (unsigned long)GetLastError());

        typedef BOOL (WINAPI *SetProcInfoFn)(HANDLE, int, void*, unsigned long);
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        SetProcInfoFn fn = k32 ? (SetProcInfoFn)GetProcAddress(k32, "SetProcessInformation") : nullptr;
        if (fn) {
            PowerThrottleState ts = {};
            ts.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            ts.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
            ts.StateMask = 0;                       // 0 = throttling off
            BOOL ok = fn(GetCurrentProcess(), 4 /*ProcessPowerThrottling*/, &ts, sizeof(ts));
            Log("VRAM: power throttling %s", ok ? "disabled" : "unchanged");
        }
    }
    if (gSet.timeres > 0) {
        TIMECAPS tc = {};
        if (timeGetDevCaps(&tc, sizeof(tc)) == MMSYSERR_NOERROR) {
            UINT want = 1;
            if (want < tc.wPeriodMin) want = tc.wPeriodMin;
            if (timeBeginPeriod(want) == TIMERR_NOERROR)
                Log("VRAM: timer resolution set to %u ms", want);
        }
    }
    if (gSet.affinity > 0) {
        SYSTEM_INFO si = {};
        GetSystemInfo(&si);
        DWORD total = si.dwNumberOfProcessors;
        if (total >= 12) {
            DWORD keep = total / 2;
            DWORD_PTR mask = 0;
            for (DWORD i = 0; i < keep; ++i) mask |= ((DWORD_PTR)1 << i);
            if (SetProcessAffinityMask(GetCurrentProcess(), mask))
                Log("VRAM: affinity limited to the first %lu logical CPUs", (unsigned long)keep);
            else
                Log("VRAM: affinity change failed (%lu)", (unsigned long)GetLastError());
        } else {
            Log("VRAM: affinity left alone (%lu logical CPUs)", (unsigned long)total);
        }
    }
}
PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc) {
    strcpy(outName, "VRAM Manager");
    strcpy(outSig, "dsh.vrammanager");
    strcpy(outDesc, "Dynamic VRAM management: adaptive render distance and VRAM "
                    "purge that restores X-Plane's own settings and backs off when "
                    "another add-on changes them.");

    Config_Load();
    Menu_Init();   // early: keeps the entry near the top of the Plugins list
    Log("VRAM: ===== VRAM Manager v%s loaded (X-Plane 12) =====", VRAMMGR_VERSION);
    return 1;
}

PLUGIN_API void XPluginStop(void) {
    Log("VRAM: plugin stopping");
    sEnabled = false;
    if (gRt.lodOk) Ctrl_Reset();
    if (Panel_Active()) Panel_Shutdown();
    Menu_Shutdown();
    Vram_Shutdown();
    Config_Save();
}

PLUGIN_API void XPluginEnable(void) {
    if (sEnabled) return;
    sEnabled = true;

    Vram_Init();
    Refs_Init();
    if (gSet.debug) Refs_Probe();
    Ctrl_Init();
    ScanOtherPlugins();
    if (gSet.cloudTrim > 0) Ctrl_ApplyCloudTrim(gSet.cloudTrim);
    if (gSet.pagingM > 0.0f) Ctrl_SetPagingDistance(gSet.pagingM);
    if (gSet.texBudget > 1.001f) Ctrl_SetTextureBudget(gSet.texBudget);
    if (gSet.compTex) Ctrl_SetTextureCompression(true);
    ApplyProcessTuning();
    if (!Panel_Init())
        Log("VRAM: panel window could not be created yet - will retry");
    Menu_Init();        // also (re)created here: X-Plane drops menus on disable
    Menu_Refresh();

    XPLMRegisterFlightLoopCallback(FlightLoopCB, 0.5f, nullptr);
    Log("VRAM: enabled (auto=%d, lod dataref=%s)", gSet.autoManage,
        gRt.lodOk ? gRef[REF_LOD].name : "missing");
}

PLUGIN_API void XPluginDisable(void) {
    if (!sEnabled) return;
    sEnabled = false;
    XPLMUnregisterFlightLoopCallback(FlightLoopCB, nullptr);

    // Put X-Plane's own values back so a disabled plugin leaves no trace.
    Ctrl_RestoreCloudTrim();
    Ctrl_SetPagingDistance(0.0f);      // put X-Plane's paging distance back
    Ctrl_SetTextureBudget(1.0f);       // and its texture budget
    Ctrl_SetTextureCompression(false);
    if (gRt.lodOk) {
        Ctrl_Reset();
    }
    if (Panel_Active()) Panel_Shutdown();
    Menu_Shutdown();    // X-Plane removes the menu anyway; drop our handle too
    Config_Save();
    Log("VRAM: disabled, X-Plane settings restored");
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID inFromWho, int inMessage, void* inParam) {
    (void)inFromWho;
    (void)inParam;
    switch (inMessage) {
        case XPLM_MSG_SCENERY_LOADED:
            if (gSet.debug) Log("VRAM: scenery loaded");
            Ctrl_OnSceneryLoaded();
            break;
        case XPLM_MSG_PLANE_LOADED:
            if (gSet.debug) Log("VRAM: plane loaded");
            Ctrl_OnSceneryLoaded();
            break;
        case XPLM_MSG_WILL_WRITE_PREFS:
            // Never let our temporary render distance be saved as the user's.
            if (gRt.lodOk) {
                RefSetFloat(REF_LOD, gRt.lodBase);
                Log("VRAM: prefs about to be written, render distance restored");
            }
            break;
        default:
            break;
    }
}


