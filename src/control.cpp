// Dataref plumbing + the adaptive control engine.
//
// Design rules that keep this plugin out of other add-ons' way:
//   * only two sim values are ever written: the render-distance LOD bias and
//     (opt-in) the texture resolution level, for a purge;
//   * we always remember X-Plane's own value and put it back;
//   * if somebody else (user slider or another plugin) changes the value we
//     detect it, adopt it as the new baseline and back off for a while;
//   * if X-Plane itself overwrites our writes every frame, we detect that and
//     stop writing instead of fighting the sim.
#include "vram_manager.h"

#include <filesystem>

SimRef  gRef[REF_COUNT];
Runtime gRt;

// ---------------------------------------------------------------------------
// dataref candidates
// ---------------------------------------------------------------------------
struct RefCand {
    int         id;
    const char* names[4];
};

static const RefCand kCands[REF_COUNT] = {
    { REF_LOD,       { "sim/private/controls/reno/LOD_bias_rat", "sim/private/controls/reno/LOD_bias_rat_2", nullptr } },
    { REF_TEXRES,    { "sim/private/controls/reno/tex_res", "sim/private/controls/reno/tex_res_3d", nullptr } },
    { REF_ASYNC,     { "sim/graphics/scenery/async_scenery_load_in_progress", nullptr } },
    { REF_FRAMETIME, { "sim/operation/misc/frame_rate_period", nullptr } },
};

static bool ResolveRef(int id, bool required) {
    SimRef& r = gRef[id];
    for (int i = 0; i < 4 && kCands[id].names[i]; ++i) {
        XPLMDataRef ref = XPLMFindDataRef(kCands[id].names[i]);
        if (!ref) continue;
        XPLMDataRefInfo_t info = {};
        info.structSize = (int)sizeof(info);
        XPLMGetDataRefInfo(ref, &info);
        r.ref      = ref;
        r.type     = info.type;
        r.writable = info.writable != 0;
        strncpy(r.name, kCands[id].names[i], sizeof(r.name) - 1);
        Log("VRAM: dataref '%s' type=%d writable=%d", r.name, r.type, (int)r.writable);
        return true;
    }
    if (required) Log("VRAM: dataref for id %d NOT FOUND", id);
    return false;
}

bool Refs_Init() {
    for (int i = 0; i < REF_COUNT; ++i) gRef[i] = SimRef();
    bool lod  = ResolveRef(REF_LOD, true);
    bool tex  = ResolveRef(REF_TEXRES, false);
    ResolveRef(REF_ASYNC, false);
    ResolveRef(REF_FRAMETIME, false);
    gRt.lodOk = lod;
    gRt.texOk = tex && gRef[REF_TEXRES].ref != nullptr;
    if (!lod)
        Log("VRAM: render-distance dataref not registered yet - will retry while the sim "
            "finishes starting up");
    return lod;
}

// ---------------------------------------------------------------------------
// Late registration: X-Plane creates its rendering-settings datarefs *after*
// plugins are enabled, so the first lookup at XPLMEnable usually fails.  Retry
// a few times and, if the names changed in a new build, enumerate all datarefs
// once and look for anything that looks like a render-distance control.
// ---------------------------------------------------------------------------
static std::vector<std::string> sCandLod;
static std::vector<std::string> sCandTex;
static bool   sScanned    = false;
static double sLastRetry  = 0.0;
static double sEnableTime = 0.0;
static int    sRetryCount = 0;

// X-Plane's own values, captured at enable (or when a late lookup succeeds).
static float s_origLod = 0.0f;
static int   s_origTex = -1;

static void AssignFromInfo(int id, XPLMDataRef ref, const char* name) {
    XPLMDataRefInfo_t info = {};
    info.structSize = (int)sizeof(info);
    XPLMGetDataRefInfo(ref, &info);
    SimRef& r = gRef[id];
    r.ref      = ref;
    r.type     = info.type;
    r.writable = info.writable != 0;
    _snprintf(r.name, sizeof(r.name) - 1, "%s", name ? name : "?");
}

// One-time full dataref scan (XPLM400); only runs when the normal names fail.
static void ScanCandidates() {
    sScanned = true;
#if defined(XPLM400)
    int n = XPLMCountDataRefs();
    if (n <= 0) return;
    std::vector<XPLMDataRef> refs((size_t)n);
    XPLMGetDataRefsByIndex(0, n, refs.data());
    for (int i = 0; i < n; ++i) {
        if (!refs[i]) continue;
        XPLMDataRefInfo_t info = {};
        info.structSize = (int)sizeof(info);
        XPLMGetDataRefInfo(refs[i], &info);
        if (!info.name) continue;
        std::string nm = info.name;
        if (nm.find("lod_bias") != std::string::npos ||
            (nm.find("reno") != std::string::npos && nm.find("lod") != std::string::npos) ||
            nm.find("draw_dist") != std::string::npos) {
            sCandLod.push_back(nm);
        }
        if (nm.find("tex_res") != std::string::npos ||
            nm.find("texture_res") != std::string::npos) {
            sCandTex.push_back(nm);
        }
    }
    Log("VRAM: dataref scan found %d LOD-like and %d texture-like datarefs",
        (int)sCandLod.size(), (int)sCandTex.size());
    for (size_t i = 0; i < sCandLod.size() && i < 12; ++i)
        Log("VRAM:   lod candidate: %s", sCandLod[i].c_str());
    for (size_t i = 0; i < sCandTex.size() && i < 6; ++i)
        Log("VRAM:   tex candidate: %s", sCandTex[i].c_str());
#endif
}

static bool TryResolveLate(int id, const std::vector<std::string>& extra) {
    for (int i = 0; i < 4 && kCands[id].names[i]; ++i) {
        XPLMDataRef ref = XPLMFindDataRef(kCands[id].names[i]);
        if (ref) {
            AssignFromInfo(id, ref, kCands[id].names[i]);
            return true;
        }
    }
    for (size_t i = 0; i < extra.size(); ++i) {
        XPLMDataRef ref = XPLMFindDataRef(extra[i].c_str());
        if (!ref) continue;
        XPLMDataRefInfo_t info = {};
        info.structSize = (int)sizeof(info);
        XPLMGetDataRefInfo(ref, &info);
        // only accept things we can actually drive
        if (id == REF_LOD && !(info.type & xplmType_Float)) continue;
        AssignFromInfo(id, ref, extra[i].c_str());
        return true;
    }
    return false;
}

void Refs_Retry(double now) {
    if (gRt.lodOk && gRt.texOk) return;
    if (sEnableTime <= 0.0) sEnableTime = now;
    if (now - sLastRetry < 2.0) return;
    sLastRetry = now;
    ++sRetryCount;

    // give the sim ~6 s to finish registering its graphics datarefs, then scan
    if (!sScanned && now - sEnableTime > 6.0) ScanCandidates();

    if (!gRt.lodOk && TryResolveLate(REF_LOD, sCandLod)) {
        gRt.lodOk = true;
        s_origLod = RefFloat(REF_LOD, 1.0f);
        gRt.lodBase = s_origLod > 0.001f ? s_origLod : 1.0f;
        gRt.lodNow  = s_origLod;
        gRt.factor  = 1.0f;
        if (!gRef[REF_LOD].writable) {
            gRt.simOverrides = true;
            Log("VRAM: '%s' found but not writable - monitoring only", gRef[REF_LOD].name);
        } else {
            Log("VRAM: render-distance dataref resolved late: '%s' (type=%d) base=%.4f",
                gRef[REF_LOD].name, gRef[REF_LOD].type, s_origLod);
        }
    }
    if (!gRt.texOk && TryResolveLate(REF_TEXRES, sCandTex)) {
        gRt.texOk  = true;
        gRt.texNow = RefInt(REF_TEXRES, -1);
        gRt.texBase = gRt.texNow;
        Log("VRAM: texture-level dataref resolved late: '%s' = %d (display only)",
            gRef[REF_TEXRES].name, gRt.texNow);
    }
    if (!gRt.lodOk && sRetryCount % 30 == 0) {
        Log("VRAM: still no render-distance dataref after %d retries (%.0f s)",
            sRetryCount, now - sEnableTime);
    }
}

// Log every dataref in the running sim that looks render-distance / texture /
// VRAM related.  This is what makes the plugin adapt to a new X-Plane build
// instead of silently breaking.
void Refs_Probe() {
#if defined(XPLM400)
    int n = XPLMCountDataRefs();
    if (n <= 0) return;
    std::vector<XPLMDataRef> refs((size_t)n);
    XPLMGetDataRefsByIndex(0, n, refs.data());

    static const char* kKeys[] = { "lod_bias", "reno/", "tex_res", "vram",
                                   "rendering_res", "draw_objs", "resolution", nullptr };
    int logged = 0;
    for (int i = 0; i < n && logged < 160; ++i) {
        if (!refs[i]) continue;
        XPLMDataRefInfo_t info = {};
        info.structSize = (int)sizeof(info);
        XPLMGetDataRefInfo(refs[i], &info);
        if (!info.name) continue;
        bool hit = false;
        for (int k = 0; kKeys[k] && !hit; ++k)
            if (strstr(info.name, kKeys[k])) hit = true;
        if (!hit) continue;
        double v = 0.0;
        const char* kind = "?";
        if (info.type & xplmType_Float)       { v = XPLMGetDataf(refs[i]);  kind = "float"; }
        else if (info.type & xplmType_Double) { v = XPLMGetDatad(refs[i]);  kind = "double"; }
        else if (info.type & xplmType_Int)    { v = XPLMGetDatai(refs[i]);  kind = "int"; }
        else kind = "other";
        Log("PROBE %-58s %-7s writable=%d value=%.4f", info.name, kind, info.writable != 0, v);
        ++logged;
    }
    Log("PROBE done, %d matching datarefs of %d", logged, n);
#endif
}

float RefFloat(int id, float dflt) {
    SimRef& r = gRef[id];
    if (!r.ref) return dflt;
    if (r.type & xplmType_Float)  return XPLMGetDataf(r.ref);
    if (r.type & xplmType_Double) return (float)XPLMGetDatad(r.ref);
    if (r.type & xplmType_Int)    return (float)XPLMGetDatai(r.ref);
    return dflt;
}

int RefInt(int id, int dflt) {
    SimRef& r = gRef[id];
    if (!r.ref) return dflt;
    if (r.type & xplmType_Int)    return XPLMGetDatai(r.ref);
    if (r.type & xplmType_Float)  return (int)lroundf(XPLMGetDataf(r.ref));
    if (r.type & xplmType_Double) return (int)lround(XPLMGetDatad(r.ref));
    return dflt;
}

void RefSetFloat(int id, float v) {
    SimRef& r = gRef[id];
    if (!r.ref || !r.writable) return;
    if (r.type & xplmType_Float)       XPLMSetDataf(r.ref, v);
    else if (r.type & xplmType_Double) XPLMSetDatad(r.ref, (double)v);
    else if (r.type & xplmType_Int)    XPLMSetDatai(r.ref, (int)lroundf(v));
}   // note: there is deliberately no setter for int datarefs - the texture
    // level (reno/tex_res) must stay exactly as X-Plane set it.

bool AsyncIdle() {
    if (!gRef[REF_ASYNC].ref) return true;
    return RefInt(REF_ASYNC, 0) == 0;
}

// ---------------------------------------------------------------------------
// control engine state
// ---------------------------------------------------------------------------
static double s_lastDecision  = 0.0;
static float  s_ewma          = -1.0f;
static double s_yieldUntil    = 0.0;
static double s_lastConflictLog = -100.0;
static double s_lastBudgetChg  = -100.0;
static double s_lastWrite     = -1.0;
static float  s_written       = -1.0f;
static float  s_prevTarget    = -1.0f;   // value that would be there if our write was ignored
static bool   s_verifyPending = false;
static double s_verifyAt      = 0.0;
static int    s_simLockCount  = 0;

static int    s_purgePhase    = 0;
static int    s_autoCloud     = 0;      // cloud level chosen by the auto manager
static double s_lastCloudChg  = 0.0;    // rate limit for cloud level changes
static double s_texDropSince  = 0.0;    // when the texture level first dropped
static bool   s_probed        = false;  // private knobs probed after registration
static double s_lastTrimTry   = -1.0;   // retry timer for applying the trim
static int    s_trimTries     = 0;

static bool ApplyTrimLevel(int level);   // defined with the trim module below
static double s_purgeT0       = 0.0;
static float  s_purgeHoldUsed = 0.0f;   // hold time of the purge in flight
static float  s_pagingHold    = 2.0f;
static XPLMDataRef s_pagingRef = nullptr;   // tex/paging/max_distance
static float  s_pagingOrig    = 0.0f;
static bool   s_pagingActive  = false;
static double s_lastMicroSweep = -1.0;
static float  s_aglM          = 0.0f;   // height above ground, metres
static double s_lastAutoPurge = -1.0;
static double s_measureAt     = -1.0;   // when to read the post-purge result
static bool   s_reapplyPending = false;
static double s_reapplyAt      = 0.0;

// ---------------------------------------------------------------------------
static void WriteLod(float value, double now, float ignoredIfNotApplied) {
    s_prevTarget    = ignoredIfNotApplied;
    RefSetFloat(REF_LOD, value);
    s_written       = value;
    s_lastWrite     = now;
    s_verifyPending = true;
    s_verifyAt      = now + 0.5;
}

// Emergency purge = pull the render distance down hard for a few seconds so
// X-Plane evicts far-away scenery.  Textures are deliberately NOT touched:
// lowering the texture level is what makes the panel/cockpit go blurry.
static void PumpPurge(double now) {
    double age = now - s_purgeT0;
    float hold = s_purgeHoldUsed > 0.5f ? s_purgeHoldUsed : 0.5f;
    if (s_purgePhase == 1 && age > (double)hold) {
        if (s_pagingActive && s_pagingRef) {
            XPLMSetDataf(s_pagingRef, s_pagingOrig);
            s_pagingActive = false;
            s_purgePhase = 0;
            gRt.purging  = false;
            Log("VRAM: purge finished after %.1fs, paging restored to %.0f m, usage %.1f%%",
                age, (double)s_pagingOrig, gRt.vram.pct);
            return;
        }
        float before = gRt.lodNow > 0.001f ? gRt.lodNow : gRt.lodBase * gRt.factor;
        WriteLod(gRt.lodBase * gRt.factor, now, before);
        s_purgePhase = 0;
        gRt.purging  = false;
        Log("VRAM: purge finished after %.1fs, usage %.1f%%", age, gRt.vram.pct);
    }
}

void Ctrl_Init() {
    bool lodOk = gRef[REF_LOD].ref != nullptr;
    bool texOk = gRef[REF_TEXRES].ref != nullptr;
    gRt = Runtime();
    gRt.lodOk = lodOk;
    gRt.texOk = texOk;
    s_ewma          = -1.0f;
    s_written       = -1.0f;
    s_verifyPending = false;
    s_simLockCount  = 0;
    s_purgePhase    = 0;
    s_lastAutoPurge = -1.0;
    s_autoCloud     = 0;
    s_lastCloudChg  = 0.0;
    s_lastMicroSweep = -1.0;
    s_purgeHoldUsed  = 0.0f;
    s_probed         = false;
    s_lastTrimTry    = -1.0;
    s_trimTries      = 0;

    s_origLod = RefFloat(REF_LOD, 1.0f);
    s_origTex = RefInt(REF_TEXRES, -1);
    gRt.lodBase = s_origLod > 0.001f ? s_origLod : 1.0f;
    gRt.factor  = 1.0f;
    gRt.lodNow  = s_origLod;
    gRt.texNow  = s_origTex;
    gRt.texBase = s_origTex;   // display only - never written by this plugin
    if (lodOk)
        Log("VRAM: baseline render-distance LOD = %.4f, texture level = %d (textures are read-only)",
            s_origLod, s_origTex);
    else
        Log("VRAM: render-distance LOD not available yet (X-Plane registers it a bit "
            "later), baseline will be captured automatically");
}

void Ctrl_SetAuto(bool on) {

    gSet.autoManage = on ? 1 : 0;
    if (!on) {
        if (gRef[REF_LOD].ref) {
            double now = NowSec();
            gRt.lodNow = RefFloat(REF_LOD, gRt.lodNow);
            float before = gRt.lodNow > 0.001f ? gRt.lodNow : gRt.lodBase;
            WriteLod(gRt.lodBase, now, before);
        }
        gRt.factor = 1.0f;
        gRt.mode   = MODE_OFF;
        gRt.statusId = ST_OFF;
        s_ewma = -1.0f;
    } else {
        gRt.factor = 1.0f;
        s_lastDecision = 0.0;
    }
    Menu_Refresh();
}

void Ctrl_ToggleAuto() { Ctrl_SetAuto(!gSet.autoManage); }

void Ctrl_Reset() {
    double now = NowSec();
    if (gRef[REF_LOD].ref && s_origLod > 0.0f) {
        RefSetFloat(REF_LOD, s_origLod);
        s_written   = s_origLod;
        s_lastWrite = now;
        s_verifyPending = false;
    }
    // Texture level is never written by this plugin, so nothing to restore.
    gRt.lodBase = s_origLod > 0.0f ? s_origLod : gRt.lodBase;
    gRt.factor  = 1.0f;
    gSet.autoManage = 0;
    s_purgePhase = 0;
    gRt.purging  = false;
    gRt.mode     = MODE_OFF;
    gRt.statusId = ST_OFF;
    s_ewma = -1.0f;
    Log("VRAM: restored X-Plane render distance (lod=%.4f)", gRt.lodBase);
    Menu_Refresh();
}

// ---------------------------------------------------------------------------
// Texture paging distance
//
// sim/private/controls/tex/paging/max_distance: textures stay resident out to
// this distance (X-Plane ships 10000 m).  It is the second-biggest VRAM lever
// after the texture resolution itself, and lowering it only affects how much
// detail is kept for far-away scenery.
// ---------------------------------------------------------------------------
static XPLMDataRef s_pageDistRef   = nullptr;
static float       s_pageDistOrig  = 0.0f;
static bool        s_pageDistInit  = false;

static XPLMDataRef PageDistRef() {
    if (!s_pageDistInit) {
        s_pageDistInit = true;
        s_pageDistRef = XPLMFindDataRef("sim/private/controls/tex/paging/max_distance");
        if (s_pageDistRef) {
            XPLMDataRefInfo_t info = {};
            info.structSize = (int)sizeof(info);
            XPLMGetDataRefInfo(s_pageDistRef, &info);
            if (!(info.type & xplmType_Float) || !info.writable) s_pageDistRef = nullptr;
            else s_pageDistOrig = XPLMGetDataf(s_pageDistRef);
        }
        if (s_pageDistRef)
            Log("VRAM: texture paging distance = %.0f m (original)", (double)s_pageDistOrig);
        else
            Log("VRAM: texture paging distance dataref not available");
    }
    return s_pageDistRef;
}

void Ctrl_SetPagingDistance(float metres) {
    XPLMDataRef r = PageDistRef();
    if (!r) return;

    // Same rate limit as the texture budget - the pager re-evaluates on every
    // write as well.
    static double lastWrite = -1.0;
    static float  lastValue = -1.0f;
    double now = NowSec();
    if (fabsf(metres - lastValue) < 1.0f) return;
    if (lastWrite > 0.0 && (now - lastWrite) < 0.5) return;
    lastWrite = now;
    lastValue = metres;
    if (metres <= 0.0f) {
        XPLMSetDataf(r, s_pageDistOrig);
        gRt.pagingNow = s_pageDistOrig;
        return;
    }
    if (metres < 800.0f)   metres = 800.0f;
    if (metres > s_pageDistOrig) metres = s_pageDistOrig;
    XPLMSetDataf(r, metres);
    gRt.pagingNow = metres;
}
// ---------------------------------------------------------------------------
// Texture compression (opt-in)
//
// reno/comp_texes asks X-Plane to keep scenery textures compressed in VRAM
// instead of decompressing them on upload.  It is the single biggest VRAM
// lever left once the texture *resolution* is left alone, but it does change
// how textures are stored, so it is off unless the user asks for it.
// ---------------------------------------------------------------------------
static XPLMDataRef s_compTexRef = nullptr;
static float       s_compTexOrig = 0.0f;
static bool        s_compTexInit = false;

void Ctrl_SetTextureCompression(bool on) {
    if (!s_compTexInit) {
        s_compTexInit = true;
        s_compTexRef = XPLMFindDataRef("sim/private/controls/reno/comp_texes");
        if (s_compTexRef) {
            XPLMDataRefInfo_t info = {};
            info.structSize = (int)sizeof(info);
            XPLMGetDataRefInfo(s_compTexRef, &info);
            if (!info.writable) s_compTexRef = nullptr;
            else s_compTexOrig = XPLMGetDataf(s_compTexRef);
        }
        Log("VRAM: comp_texes %s (%.0f)", s_compTexRef ? "available" : "not available",
            (double)s_compTexOrig);
    }
    if (!s_compTexRef) return;
    XPLMSetDataf(s_compTexRef, on ? 1.0f : s_compTexOrig);
    Log("VRAM: texture compression %s (comp_texes = %.0f)",
        on ? "ON" : "off", (double)XPLMGetDataf(s_compTexRef));
}
// ---------------------------------------------------------------------------
// Texture budget
//
// X-Plane sizes its texture pool from its own conservative estimate of free
// VRAM (seen in the log: "1.63 gb available" on a 7.7 GB card) and starts
// halving the texture scale long before the card is actually full.  The two
// private multipliers below drive that estimate:
//
//   tex/paging/size_fudge_factor  (1.05) - multiplier on the pool size
//   tex/paging/max_overdrive      (16)   - how far it may exceed the budget
//
// Scaling both up lets the sim hold sharp textures with the VRAM that is
// actually there.  The values are relative to X-Plane's own, and restored when
// the multiplier goes back to 1.0 or the plugin is disabled.
// ---------------------------------------------------------------------------
static XPLMDataRef s_fudgeRef = nullptr;
static XPLMDataRef s_overRef  = nullptr;
static float       s_fudgeOrig = 1.05f;
static float       s_overOrig  = 16.0f;
static bool        s_budgetInit = false;

static void BudgetRefs() {
    if (s_budgetInit) return;
    s_budgetInit = true;
    s_fudgeRef = XPLMFindDataRef("sim/private/controls/tex/paging/size_fudge_factor");
    s_overRef  = XPLMFindDataRef("sim/private/controls/tex/paging/max_overdrive");
    if (s_fudgeRef) s_fudgeOrig = XPLMGetDataf(s_fudgeRef);
    if (s_overRef)  s_overOrig  = XPLMGetDataf(s_overRef);
    Log("VRAM: texture budget refs: fudge=%s (%.2f) overdrive=%s (%.1f)",
        s_fudgeRef ? "yes" : "no", (double)s_fudgeOrig,
        s_overRef ? "yes" : "no", (double)s_overOrig);
}

void Ctrl_SetTextureBudget(float mult) {
    BudgetRefs();
    if (mult < 1.0f) mult = 1.0f;
    if (mult > 4.0f) mult = 4.0f;

    // Hard rate limit inside the setter itself.
    //
    // Writing size_fudge_factor / max_overdrive makes X-Plane rebuild its
    // texture pools; doing that many times per second froze the sim.  Whatever
    // calls this - slider, auto controller, config - at most one write per
    // 500 ms gets through, and identical values are dropped entirely.
    static double lastWrite = -1.0;
    static float  lastValue = -1.0f;
    double now = NowSec();
    if (fabsf(mult - lastValue) < 0.001f) return;
    if (lastWrite > 0.0 && (now - lastWrite) < 0.5) return;
    lastWrite = now;
    lastValue = mult;
    if (s_fudgeRef) XPLMSetDataf(s_fudgeRef, s_fudgeOrig * mult);
    if (s_overRef)  XPLMSetDataf(s_overRef,  s_overOrig * mult);
    gRt.texBudget = mult;
    if (mult > 1.001f || lastValue >= 0.0f)
        Log("VRAM: texture budget x%.2f (fudge %.2f -> %.2f, overdrive %.1f -> %.1f)",
            (double)mult, (double)s_fudgeOrig, (double)(s_fudgeOrig * mult),
            (double)s_overOrig, (double)(s_overOrig * mult));
}
// ---------------------------------------------------------------------------
// Cloud / shadow trimming
//
// The knobs are private datarefs whose exact names differ between builds, so
// nothing is assumed: each candidate is looked up, type checked and read back
// after the write.  Originals are remembered so switching the trim off (or
// disabling the plugin) leaves X-Plane exactly as it was.
// ---------------------------------------------------------------------------
struct TrimKnob {
    const char* name;
    float       target;     // < 0 means "halve the current value"
    int         mode;       // 0 = absolute, 1 = halve, 2 = scale by target factor
    float       orig;
    bool        applied;
};

static TrimKnob s_trim[24];
static int      s_trimCount   = 0;
static int      s_trimApplied = 0;

static void BuildTrimList(int level) {
    s_trimCount = 0;
    auto add = [](const char* n, float target, int mode) {
        if (s_trimCount < 24) {
            s_trim[s_trimCount].name = n;
            s_trim[s_trimCount].target = target;
            s_trim[s_trimCount].mode = mode;
            s_trim[s_trimCount].orig = 0.0f;
            s_trim[s_trimCount].applied = false;
            ++s_trimCount;
        }
    };
    // Cloud *shape* is produced by the 3-D noise volume (noise_hi_resolution,
    // 128 voxels on this build).  Halving it makes the cloud base look shaved
    // flat, so it is never touched.  The savings below come from cloud shadows
    // and particulate detail, which do not change the silhouette.
    // Measured defaults: shadow_size 1024, shadow_map_width 512,
    // precipitation 512, shadow_cascades 2, shadow_steps 8.
    // SSAO / MSAA / texture quality are deliberately NOT touched - those are
    // X-Plane's own graphics settings, not something a plugin should override.
    add("sim/private/controls/clouds/precipitation_resolution", 0.0f, 1);  // halve
    add("sim/private/controls/cloud/shadow_steps",              6.0f, 0);
    if (level >= 2) {
        // Mild: keep the noise volume at full resolution (128) and only let
        // X-Plane compress it - precision drops a little, the cloud silhouette
        // does not change (halving the resolution is what flattened the base).
        add("sim/private/controls/clouds/noise_compression",        1.0f, 0);
        add("sim/private/controls/clouds/shadow_size",              0.0f, 1);  // halve
        add("sim/private/controls/new_clouds/shadow_map_width",     0.0f, 1);  // halve
        add("sim/private/controls/cloud/shadow_cascades",           1.0f, 0);
        // render targets and probes: half resolution is nearly invisible
        add("sim/private/controls/fbo/shadow_cam_size",             0.0f, 3);
        add("sim/private/controls/lighting/probe_resolution",       0.0f, 3);
        add("sim/private/controls/ibl/cloud_res",                   0.0f, 3);
        add("sim/private/controls/ibl/scattering_res",              0.0f, 3);
        add("sim/private/controls/scattering/cache_width",          0.0f, 3);
        add("sim/private/controls/scattering/cache_height",         0.0f, 3);
        add("sim/private/controls/cloud/resolution_max",            0.0f, 3);
        // volumetric fog is a second 3-D texture; it is low frequency, so half
        // resolution is hard to spot but returns ~7/8 of its memory
        add("sim/private/controls/volumetric/fog/resolution_x",     0.0f, 1);
        add("sim/private/controls/volumetric/fog/resolution_y",     0.0f, 1);
        add("sim/private/controls/volumetric/fog/resolution_z",     0.0f, 1);
    }
}
static bool ReadKnob(XPLMDataRef r, int type, float* out) {
    if (type & xplmType_Float) { *out = XPLMGetDataf(r); return true; }
    if (type & xplmType_Int)   { *out = (float)XPLMGetDatai(r); return true; }
    if (type & xplmType_Double){ *out = (float)XPLMGetDatad(r); return true; }
    return false;
}

static void WriteKnob(XPLMDataRef r, int type, float v) {
    if (type & xplmType_Float)  { XPLMSetDataf(r, v); return; }
    if (type & xplmType_Int)    { XPLMSetDatai(r, (int)lroundf(v)); return; }
    if (type & xplmType_Double) { XPLMSetDatad(r, (double)v); return; }
}

void Ctrl_RestoreCloudTrim() {
    for (int i = 0; i < s_trimCount; ++i) {
        if (!s_trim[i].applied) continue;
        XPLMDataRef r = XPLMFindDataRef(s_trim[i].name);
        if (r) {
            XPLMDataRefInfo_t info = {};
            info.structSize = (int)sizeof(info);
            XPLMGetDataRefInfo(r, &info);
            WriteKnob(r, info.type, s_trim[i].orig);
        }
        Log("VRAM: trim restored '%s' = %.3f", s_trim[i].name, (double)s_trim[i].orig);
        s_trim[i].applied = false;
    }
    s_trimApplied = 0;
    gRt.cloudTrimmed = 0;
}

static bool ApplyTrimLevel(int level) {
    Ctrl_RestoreCloudTrim();
    if (level <= 0) return true;
    BuildTrimList(level);

    for (int i = 0; i < s_trimCount; ++i) {
        XPLMDataRef r = XPLMFindDataRef(s_trim[i].name);
        if (!r) { Log("VRAM: trim: '%s' not present in this build", s_trim[i].name); continue; }
        XPLMDataRefInfo_t info = {};
        info.structSize = (int)sizeof(info);
        XPLMGetDataRefInfo(r, &info);
        if (!info.writable) {
            Log("VRAM: trim: '%s' is read-only", s_trim[i].name);
            continue;
        }
        float cur = 0.0f;
        if (!ReadKnob(r, info.type, &cur)) {
            Log("VRAM: trim: '%s' has unsupported type %d", s_trim[i].name, info.type);
            continue;
        }
        float target = s_trim[i].target;
        if (s_trim[i].mode == 1)      target = (cur > 256.0f) ? (cur * 0.5f) : cur;   // halve sizes
        else if (s_trim[i].mode == 3) target = (cur >= 32.0f) ? (cur * 0.5f) : cur;    // halve if pixel-like
        else if (s_trim[i].mode == 2) target = cur * target;                            // scale distances
        if (fabsf(target - cur) < 0.001f) {
            Log("VRAM: trim: '%s' already %.3f", s_trim[i].name, (double)cur);
            continue;
        }
        WriteKnob(r, info.type, target);
        float after = 0.0f;
        ReadKnob(r, info.type, &after);
        if (fabsf(after - target) > 0.01f) {
            Log("VRAM: trim: '%s' write ignored (%.3f -> %.3f, reads %.3f)",
                s_trim[i].name, (double)cur, (double)target, (double)after);
            continue;
        }
        s_trim[i].orig    = cur;
        s_trim[i].applied = true;
        ++s_trimApplied;
        Log("VRAM: trim '%s' %.3f -> %.3f", s_trim[i].name, (double)cur, (double)target);
    }
    gRt.cloudTrimmed = s_trimApplied;
    Log("VRAM: cloud trim level %d applied to %d knob(s)", level, s_trimApplied);
    return s_trimApplied > 0;
}
bool Ctrl_ApplyCloudTrim(int level) {
    gSet.cloudTrim = level < 0 ? 0 : (level > 3 ? 3 : level);
    s_autoCloud = 0;
    gRt.cloudAuto = 0;
    if (gSet.cloudTrim == 3) return ApplyTrimLevel(0);   // Auto starts clean
    return ApplyTrimLevel(gSet.cloudTrim);
}

// Cloud trim mode: 0 = Off, 1 = Light, 2 = Max, 3 = Auto.  Exactly one mode is
// active - Auto never layers on top of a manual level.
static void UpdateCloudPressure(double now, float pct) {
    s_ewma = (s_ewma < 0.0f) ? pct : (s_ewma * 0.65f + pct * 0.35f);
    if (gSet.cloudTrim != 3) return;

    int want = s_autoCloud;
    if (s_ewma > gSet.targetPct + 6.0f)        want = 2;
    else if (s_ewma > gSet.targetPct)          want = (want < 1) ? 1 : want;
    else if (s_ewma < gSet.targetPct - 10.0f)  want = 0;
    else if (s_ewma < gSet.targetPct - 4.0f && want > 1) want = 1;

    if (want != s_autoCloud && (now - s_lastCloudChg) > 10.0) {
        s_autoCloud = want;
        s_lastCloudChg = now;
        ApplyTrimLevel(s_autoCloud);
        gRt.cloudAuto = s_autoCloud;
        Log("VRAM: auto cloud trim -> level %d (vram %.1f%%)", s_autoCloud, s_ewma);
    }
}
// Private datarefs are NOT returned by XPLMCountDataRefs/GetDataRefsByIndex, so
// they have to be looked up by name.  Log what this build actually has.
void Ctrl_ProbeVramKnobs() {
    static const char* kNames[] = {
        "sim/private/controls/clouds/noise_hi_resolution",
        "sim/private/controls/clouds/noise_lo_resolution",
        "sim/private/controls/clouds/noise_compression",
        "sim/private/controls/clouds/full_msaa",
        "sim/private/controls/clouds/shadow_size",
        "sim/private/controls/clouds/shadow_extent_km",
        "sim/private/controls/clouds/categorize_max_distance_km",
        "sim/private/controls/clouds/composite_mode",
        "sim/private/controls/clouds/precipitation_resolution",
        "sim/private/controls/cloud/shadow_cascades",
        "sim/private/controls/cloud/shadow_steps",
        "sim/private/controls/new_clouds/shadow_map_width",
        "sim/private/controls/shadow/csm/far_limit_exterior",
        "sim/private/controls/shadow/csm/lambda",
        "sim/private/controls/reno/LOD_bias_rat",
        "sim/private/controls/reno/tex_res",
        "sim/private/controls/fbo/downsample",
        "sim/private/controls/ssao/enable",
        "sim/private/controls/ssao/ssao_enable",
        "sim/private/controls/hdr/aircraft_hdr",
        "sim/private/controls/hdr/glass_hdr",
        "sim/private/controls/hdr/bloom_taps",
        "sim/private/controls/fbo/shadow_cam_size",
        "sim/private/controls/msaa/early_resolve_mode",
        "sim/private/controls/volumetric/fog/resolution_x",
        "sim/private/controls/volumetric/fog/resolution_y",
        "sim/private/controls/volumetric/fog/resolution_z",
        "sim/private/controls/volumetric/fog/max_distance_km",
        "sim/private/controls/volumetric/fog/samples",
        "sim/private/controls/new_clouds/async_shadow",
        "sim/private/controls/clouds/categorize_max_distance_km",
        "sim/private/controls/cloud/shadow_extent_km",
        "sim/private/controls/terrain/num_cluster_draw",
        "sim/private/controls/water/enable_refraction",
        "sim/private/controls/ssao/downsample",
        "sim/private/controls/reno/comp_texes",
        "sim/private/controls/reno/aniso_filter",
        "sim/private/controls/tex/preload_dist",
        "sim/private/controls/tex/distance_pad",
        "sim/private/controls/fbo/shadow_cam_size",
        "sim/private/controls/lighting/probe_resolution",
        "sim/private/controls/lighting/probe_count",
        "sim/private/controls/ibl/cloud_res",
        "sim/private/controls/ibl/scattering_res",
        "sim/private/controls/scattering/cache_width",
        "sim/private/controls/scattering/cache_height",
        "sim/private/controls/cloud/resolution_max",
        "sim/private/controls/shadow/passes_per_frame",
        "sim/private/controls/tex/paging/max_distance",
        "sim/private/controls/tex/paging/size_fudge_factor",
        "sim/private/controls/tex/paging/max_overdrive",
        "sim/private/controls/tex/paging/downscale_cooldown",
        "sim/private/controls/gfx/managed_buffers/paging_arena_bytes",
        "sim/private/controls/gfx/managed_buffers/paging_enabled",
    };
    int found = 0, writable = 0;
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
        XPLMDataRef r = XPLMFindDataRef(kNames[i]);
        if (!r) { Log("VRAM: probe '%s' = absent", kNames[i]); continue; }
        XPLMDataRefInfo_t info = {};
        info.structSize = (int)sizeof(info);
        XPLMGetDataRefInfo(r, &info);
        ++found;
        if (info.writable) ++writable;
        float v = 0.0f;
        if (ReadKnob(r, info.type, &v))
            Log("VRAM: probe '%s' type=%d wr=%d = %.3f",
                kNames[i], info.type, (int)info.writable, (double)v);
        else
            Log("VRAM: probe '%s' type=%d wr=%d (unsupported read)",
                kNames[i], info.type, (int)info.writable);
    }
    Log("VRAM: probe done - %d of %d present, %d writable",
        found, (int)(sizeof(kNames) / sizeof(kNames[0])), writable);
}
// One-off discovery: list every cloud / shadow related dataref so the trim
// list can be tuned for this exact X-Plane build.
void Ctrl_DumpVramCandidates() {
#if defined(XPLM400)
    int n = XPLMCountDataRefs();
    if (n <= 0) return;
    std::vector<XPLMDataRef> refs((size_t)n);
    XPLMGetDataRefsByIndex(0, n, refs.data());
    int shown = 0;
    Log("VRAM: dataref discovery - %d datarefs total", n);
    for (int i = 0; i < n && shown < 60; ++i) {
        if (!refs[i]) continue;
        XPLMDataRefInfo_t info = {};
        info.structSize = (int)sizeof(info);
        XPLMGetDataRefInfo(refs[i], &info);
        if (!info.name) continue;
        std::string nm(info.name);
        if (nm.find("cloud") == std::string::npos &&
            nm.find("shadow") == std::string::npos &&
            nm.find("ssao") == std::string::npos &&
            nm.find("fbo") == std::string::npos) continue;
        int type = info.type;
        char val[64] = "";
        if (type & xplmType_Int)         snprintf(val, sizeof(val), "= %d", XPLMGetDatai(refs[i]));
        else if (type & xplmType_Float)  snprintf(val, sizeof(val), "= %.3f", (double)XPLMGetDataf(refs[i]));
        Log("VRAM:   %-60s type=%d wr=%d %s", info.name, type, (int)info.writable, val);
        ++shown;
    }
#endif
}
// X-Plane keeps textures resident out to tex/paging/max_distance metres (10000
// on this build).  Clamping it for a couple of seconds makes the pager drop
// everything far away - that is what actually frees VRAM; changing the render
// distance alone frees nothing (measured: 0.00 GB).
static bool StartPurge(float depthIn, float holdIn, bool verbose, float pagingM = 600.0f) {
    if (!AsyncIdle()) {
        if (verbose) {
            gRt.statusId = ST_LOADING;
            Log("VRAM: purge deferred, scenery is still loading");
        }
        return false;
    }
    double now = NowSec();

    if (!s_pagingRef) {
        s_pagingRef = XPLMFindDataRef("sim/private/controls/tex/paging/max_distance");
        if (s_pagingRef) {
            XPLMDataRefInfo_t info = {};
            info.structSize = (int)sizeof(info);
            XPLMGetDataRefInfo(s_pagingRef, &info);
            if (!info.writable) s_pagingRef = nullptr;
        }
    }
    if (s_pagingRef) {
        s_pagingOrig = XPLMGetDataf(s_pagingRef);
        s_pagingHold = holdIn > 0.5f ? holdIn : 2.0f;
        XPLMSetDataf(s_pagingRef, pagingM);     // keep textures within pagingM metres
        s_pagingActive  = true;
        s_purgePhase    = 1;
        s_purgeT0       = now;
        s_purgeHoldUsed = s_pagingHold;
        gRt.vramBeforeGB = (double)gRt.vram.processBytes / 1073741824.0;
        gRt.fpsBefore    = gRt.fps;
        gRt.purgeDoneSec = -1.0;
        s_measureAt      = now + (double)s_pagingHold + 1.5;
        gRt.purging      = true;
        gRt.lastPurgeSec = now;
        gRt.mode         = MODE_PURGE;
        gRt.statusId     = ST_PURGE_LOD;
        if (verbose)
            Log("VRAM: purge start (texture paging %.0f -> %.0f m for %.1fs, usage %.1f%%)",
                (double)s_pagingOrig, (double)pagingM, (double)s_pagingHold, gRt.vram.pct);
        return true;
    }

    if (!gRt.lodOk) return false;
    // Read the value that is really there right now: this is what the sim keeps
    // if our write is ignored, which is how a stuck write is detected.
    gRt.lodNow = RefFloat(REF_LOD, gRt.lodNow);
    float before = gRt.lodNow > 0.001f ? gRt.lodNow : gRt.lodBase * gRt.factor;
    // A shallow, short dip is enough to make X-Plane evict far scenery, and it
    // is far less noticeable than slamming the render distance to the minimum.
    float depth = depthIn > 0.02f ? depthIn : 0.18f;
    if (depth > 0.75f) depth = 0.75f;
    float dip = before * (1.0f - depth);
    float floorLod = gRt.lodBase * gSet.minFactor;
    if (dip < floorLod) dip = floorLod;
    WriteLod(dip, now, before);
    s_purgePhase    = 1;
    s_purgeT0       = now;
    s_purgeHoldUsed = holdIn > 0.5f ? holdIn : 1.5f;
    gRt.vramBeforeGB = (double)gRt.vram.processBytes / 1073741824.0;
    gRt.fpsBefore    = gRt.fps;
    gRt.purgeDoneSec = -1.0;
    s_measureAt      = now + (double)s_purgeHoldUsed + 1.5;  // just after the restore

    gRt.purging      = true;
    gRt.lastPurgeSec = now;
    gRt.mode         = MODE_PURGE;
    gRt.statusId     = ST_PURGE_LOD;
    if (verbose)
        Log("VRAM: purge start (usage %.1f%%, render distance %.3f -> %.3f for %.1fs, textures untouched)",
            gRt.vram.pct, (double)before, (double)dip, (double)s_purgeHoldUsed);
    return true;
}

void Ctrl_Purge() { StartPurge(gSet.purgeDepth, gSet.purgeHold, true, 600.0f); }

void Ctrl_ReloadScenery() {
    XPLMCommandRef cmd = XPLMFindCommand("sim/operation/reload_scenery");
    if (!cmd) {
        Log("VRAM: reload_scenery command not found");
        return;
    }
    Log("VRAM: requesting scenery reload");
    gRt.statusId = ST_LOADING;
    if (gRt.lodOk) {
        float before = gRt.lodNow > 0.001f ? gRt.lodNow : gRt.lodBase * gRt.factor;
        WriteLod(gRt.lodBase * gSet.minFactor * 0.95f, NowSec(), before);
    }
    XPLMCommandOnce(cmd);
    s_reapplyPending = true;
    s_reapplyAt      = NowSec() + 25.0;
}

void Ctrl_OnSceneryLoaded() {
    // After a scenery/texture reload, put our render distance back and let the
    // sampler settle before making decisions again.
    s_ewma         = -1.0f;
    s_lastDecision = NowSec() + 3.0;
    s_simLockCount = 0;
    if (gRt.lodOk && gSet.autoManage) {
        gRt.lodNow = RefFloat(REF_LOD, gRt.lodNow);
        float before = gRt.lodNow > 0.001f ? gRt.lodNow : gRt.lodBase * gRt.factor;
        WriteLod(gRt.lodBase * gRt.factor, NowSec(), before);
    }
    s_reapplyPending = false;
}

// ---------------------------------------------------------------------------
void Ctrl_Tick(double now, double dt) {
    if (!gRt.lodOk || !gRt.texOk) Refs_Retry(now);

    // Private datarefs are registered by X-Plane a few seconds after the plugin
    // is enabled, so the probe and the trim application must wait for them.
    if (gRt.lodOk && !s_probed) {
        s_probed = true;
        Ctrl_ProbeVramKnobs();
    }
    if (gSet.cloudTrim != 0 && s_probed && gRt.cloudTrimmed == 0 && s_trimTries < 8 &&
        (s_lastTrimTry < 0.0 || now - s_lastTrimTry > 2.0)) {
        s_lastTrimTry = now;
        ++s_trimTries;
        Ctrl_ApplyCloudTrim(gSet.cloudTrim);
    }

    // The cloud trim runs on its own mode (Auto) and must keep working even
    // when the render-distance auto management is off.
    {
        static XPLMDataRef aglRef = nullptr;
        static bool aglInit = false;
        if (!aglInit) {
            aglInit = true;
            aglRef = XPLMFindDataRef("sim/flightmodel/position/y_agl");
        }
        if (aglRef) s_aglM = XPLMGetDataf(aglRef);
    }
    // Spend idle VRAM on textures.
    //
    // X-Plane derives its texture pool from roughly a quarter of the DXGI
    // budget (measured: 1.6 GB of a 6.96 GB budget), so it starts halving the
    // texture scale while the card is only ~70% full and 2 GB sit unused.
    // While we are below the target and the user has not pinned a budget, scale
    // the pool up so that headroom is actually used.
    if (gSet.texBudgetAuto && gSet.texBudget <= 1.001f) {
        // X-Plane subtracts the memory the rest of the system reserves before
        // it sizes the texture pool, so in its own books it is always over
        // budget - there is never "free VRAM" from its point of view.  The only
        // lever is to enlarge what it believes its budget to be, so this is a
        // fixed boost that is only pulled back when the card is genuinely close
        // to full by our own measurement.
        float pct = gRt.vram.pct;
        float mult;
        // absolute thresholds - the card is what matters here, not our own
        // target, and X-Plane is always "over budget" by its own accounting
        if (pct >= 90.0f)      mult = 1.00f;   // genuinely full
        else if (pct >= 84.0f) mult = 1.60f;   // getting close
        else if (pct >= 78.0f) mult = 2.00f;   // comfortable
        else                   mult = 2.50f;   // lots of physical room
        if (fabsf(mult - gRt.texBudget) > 0.02f && (now - s_lastBudgetChg) > 8.0) {
            s_lastBudgetChg = now;
            Ctrl_SetTextureBudget(mult);
            Log("VRAM: auto texture budget x%.2f (usage %.1f%%, target %.0f%%)",
                (double)mult, pct, (double)gSet.targetPct);
        }
    }
    if (gRt.vram.valid) UpdateCloudPressure(now, gRt.vram.pct);
    if (!gRt.vram.valid) Vram_Sample(&gRt.vram);

    gRt.lodNow = gRef[REF_LOD].ref ? RefFloat(REF_LOD, 0.0f) : 0.0f;
    gRt.texNow = gRef[REF_TEXRES].ref ? RefInt(REF_TEXRES, -1) : -1;

    float ft = gRef[REF_FRAMETIME].ref ? RefFloat(REF_FRAMETIME, 0.0f) : 0.0f;
    if (ft > 1e-4f && ft < 1.0f) gRt.fps = 1.0f / ft;

    if (s_purgePhase) PumpPurge(now);

    // Take the post-purge numbers a little after the render distance is back,
    // so the freed VRAM has actually been returned.
    if (s_measureAt > 0.0 && now >= s_measureAt) {
        s_measureAt = -1.0;
        gRt.vramAfterGB = (double)gRt.vram.processBytes / 1073741824.0;
        gRt.fpsAfter    = gRt.fps;
        gRt.purgeDoneSec = now;
        Log("VRAM: purge result: VRAM %.2f -> %.2f GB (%+.2f GB), FPS %.0f -> %.0f (%+.1f%%)",
            gRt.vramBeforeGB, gRt.vramAfterGB, gRt.vramAfterGB - gRt.vramBeforeGB,
            (double)gRt.fpsBefore, (double)gRt.fpsAfter,
            gRt.fpsBefore > 1.0f ? ((double)gRt.fpsAfter / (double)gRt.fpsBefore - 1.0) * 100.0 : 0.0);
    }

    // Running FPS comparison: same machine, same scene, only the render distance
    // differs - so the difference between these two averages is the real gain.
    if (gRt.fps > 5.0f) {
        const float a = 0.02f;
        if (gRt.factor > 0.98f)
            gRt.fpsFull = (gRt.fpsFull > 1.0f) ? gRt.fpsFull * (1.0f - a) + gRt.fps * a : gRt.fps;
        else if (gRt.factor < 0.95f)
            gRt.fpsReduced = (gRt.fpsReduced > 1.0f) ? gRt.fpsReduced * (1.0f - a) + gRt.fps * a : gRt.fps;
    }

    if (s_reapplyPending && now > s_reapplyAt) {
        s_reapplyPending = false;
        if (gRt.lodOk) {
            float before = gRt.lodNow > 0.001f ? gRt.lodNow : gRt.lodBase * gRt.factor;
            WriteLod(gRt.lodBase * gRt.factor, now, before);
        }
    }

    if (!gRt.lodOk) {
        gRt.mode     = MODE_UNAVAIL;
        gRt.statusId = ST_WAITREF;
        return;
    }

    // --- did our last write stick? -----------------------------------------
    if (s_verifyPending && now >= s_verifyAt) {
        s_verifyPending = false;
        float delta = fabsf(gRt.lodNow - s_written);
        if (delta > 0.004f) {
            bool ignored = s_prevTarget >= 0.0f &&
                           fabsf(gRt.lodNow - s_prevTarget) <= 0.004f;
            // Somebody else (another plugin, or the sim itself) changed the value
            // between our write and the check.  We do NOT back off any more: the
            // next decision simply writes our value again.  Only a throttled note
            // goes to the log so it is still visible what happened.
            ++s_simLockCount;
            if (now - s_lastConflictLog > 20.0) {
                s_lastConflictLog = now;
                Log("VRAM: render distance changed under us (read %.4f, wrote %.4f) - re-asserting",
                    gRt.lodNow, s_written);
            }
        } else {
            s_simLockCount = 0;
        }
    }

    // ---- smoothed FPS for the governor ---------------------------------
    if (gRt.fps > 1.0f)
        gRt.fpsEwma = (gRt.fpsEwma < 1.0f) ? gRt.fps
                                           : gRt.fpsEwma * 0.88f + gRt.fps * 0.12f;

    // ---- texture blur guard (gentle by default) -------------------------
    // We never write the texture level.  If another plugin (LockYourTexture)
    // keeps it locked, this never triggers at all; if X-Plane itself lowers it
    // we only add a little headroom, and only after the drop has persisted for
    // a while (a transient dip during paging is not a real problem).
    bool texDropped = false;
    if (gRt.texOk && gRt.texBase > 0 && gRt.texNow > 0 && gSet.texGuard > 0) {
        if (gRt.texNow < gRt.texBase) {
            if (s_texDropSince <= 0.0) s_texDropSince = now;
            texDropped = (now - s_texDropSince) > 6.0;      // must persist
            if (!gRt.texDropped && texDropped)
                Log("VRAM: X-Plane kept textures at %d (below %d) for 6s - gentle headroom",
                    gRt.texNow, gRt.texBase);
        } else {
            if (gRt.texDropped) Log("VRAM: texture level back to %d", gRt.texNow);
            s_texDropSince = 0.0;
        }
    } else {
        s_texDropSince = 0.0;
    }
    gRt.texDropped = texDropped;

    bool autoOn = gSet.autoManage;
    gRt.autoOn = autoOn;


    if (s_purgePhase) {
        gRt.mode = MODE_PURGE;
        return;
    }
    if (!autoOn) {
        gRt.mode     = MODE_OFF;
        gRt.statusId = ST_OFF;
        return;
    }
    if (!AsyncIdle()) {
        gRt.mode     = MODE_YIELD;
        gRt.statusId = ST_LOADING;
        return;
    }

    if (!gRt.vram.valid) {
        gRt.mode     = MODE_IDLE;
        gRt.statusId = ST_UNAVAIL;
        return;
    }

    float pct = gRt.vram.pct;

    if (now - s_lastDecision < 0.9) return;
    s_lastDecision = now;

    float oldFactor = gRt.factor;
    float f = oldFactor;

    // Two objectives on one actuator (the render distance):
    //   VRAM  - hard cap, exceeding it makes X-Plane drop textures (visible)
    //   FPS   - soft target, optional
    // The deficit that is worse wins, and the step scales with it, so the
    // controller converges instead of oscillating.
    float guardDrop = (gSet.texGuard >= 2) ? 6.0f : 2.0f;
    float effTarget = gSet.targetPct - (texDropped ? guardDrop : 0.0f);
    float vramDef = s_ewma - effTarget;
    bool  fpsOn   = gSet.fpsTarget > 5.0f && gRt.fpsEwma > 1.0f;
    float fpsDef  = fpsOn ? (gSet.fpsTarget - gRt.fpsEwma) : 0.0f;

    gRt.limiter = texDropped ? 3 : (vramDef > 0.0f ? 1 : (fpsDef > 1.5f ? 2 : 0));

    bool pagingAtFloor = (gRt.pagingNow > 1.0f) &&
                         (gRt.pagingNow <= 3100.0f);
    if ((vramDef > 0.0f && pagingAtFloor) || fpsDef > 1.5f) {
        float need = std::max(vramDef / 8.0f, fpsDef / 12.0f);
        if (need < 0.35f) need = 0.35f;
        if (need > 2.0f)  need = 2.0f;
        f = std::max(gSet.minFactor, f - gSet.stepPct * need);
        gRt.mode     = MODE_REDUCE;
        gRt.statusId = ST_REDUCE;
    } else if (vramDef < -6.0f && (!fpsOn || fpsDef < -10.0f)) {
        f = std::min(gSet.maxFactor, f + gSet.stepPct * 0.5f);
        gRt.mode     = MODE_RECOVER;
        gRt.statusId = ST_RECOVER;
    } else {
        gRt.mode     = MODE_HOLD;
        gRt.statusId = ST_HOLD;
    }

    if (fabsf(f - oldFactor) > 1e-4f) {
        gRt.factor = f;
        WriteLod(gRt.lodBase * f, now, gRt.lodBase * oldFactor);
        if (gSet.debug)
            Log("VRAM: factor %.2f -> %.2f (lod %.3f, vram %.1f%%)",
                oldFactor, f, gRt.lodBase * f, pct);
    }
    gRt.statusArg = f * 100.0f;

    // Primary VRAM lever: the texture paging distance.
    //
    // Per metre of visual change this frees far more VRAM than the render
    // distance does - distant scenery stops keeping its full-resolution
    // textures resident.  It is pulled in first, and the render distance is
    // only used once paging is already at its floor.
    {
        float defPaging = PageDistRef() ? s_pageDistOrig : 10000.0f;
        float base = gSet.pagingM;                       // 0 = X-Plane default
        if (base <= 0.0f || base > defPaging) base = defPaging;
        float floorM = base * 0.30f;                     // never below 30% of it
        if (floorM < 2500.0f) floorM = 2500.0f;
        if (floorM > base) floorM = base;

        float want = gRt.pagingNow > 1.0f ? gRt.pagingNow : base;
        if (gRt.simOverrides) want = base;

        if (s_ewma > gSet.targetPct) {
            if (want > floorM) {
                // proportional: the further over target, the bigger the cut, so
                // a hard target (e.g. 4.8 GB) is reached in a few decisions
                float excess = s_ewma - gSet.targetPct;
                float step = 0.10f + excess / 100.0f;
                if (step > 0.35f) step = 0.35f;
                want = want * (1.0f - step);
                if (want < floorM) want = floorM;
            }
        } else if (s_ewma < gSet.targetPct - 6.0f) {
            if (want < base) {
                want = want * 1.15f;
                if (want > base) want = base;
            }
        }
        if (fabsf(want - gRt.pagingNow) > 1.0f) {
            Ctrl_SetPagingDistance(want);
            Log("VRAM: texture paging -> %.0f m (usage %.1f%%, render distance %.0f%%)",
                (double)want, s_ewma, (double)(gRt.factor * 100.0f));
        }
    }

    // Micro sweep: a very shallow, very short dip on a slow timer.  X-Plane
    // keeps far-away scenery resident in its paging cache; nudging the render
    // distance for ~1.5 s makes it drop what it no longer needs, which keeps
    // the VRAM floor low over a long flight without any visible change.
    // Only sweep when a 1.5 s texture reload cannot be noticed: healthy frame
    // rate and well above the ground.
    bool sweepOk = gRt.fps > 35.0f && s_aglM > 600.0f;
    if (gSet.microSweep && sweepOk && !s_purgePhase && s_ewma > gSet.targetPct - 2.0f &&
        (s_lastMicroSweep < 0.0 || now - s_lastMicroSweep > 55.0)) {
        if (StartPurge(0.10f, 1.5f, false, 2500.0f)) {
            s_lastMicroSweep = now;
            Log("VRAM: micro sweep at %.1f%% (paging -> 2500 m for 1.5s)", s_ewma);
        }
    }

    // Desperate: still over 92% while already at the minimum render distance.
    // The emergency purge only pulls the render distance down - textures stay
    // at X-Plane's own quality so nothing goes blurry.
    if (gSet.autoPurge && gRt.factor <= gSet.minFactor + 1e-4f && s_ewma > 92.0f &&
        (s_lastAutoPurge < 0.0 || now - s_lastAutoPurge > 120.0)) {
        s_lastAutoPurge = now;
        Log("VRAM: auto purge triggered (%.1f%% at minimum render distance)", s_ewma);
        Ctrl_Purge();
    }
}

// ---------------------------------------------------------------------------
// other-plug-in scan: warn instead of fighting
// ---------------------------------------------------------------------------
static bool ContainsNoCase(const char* hay, const char* needle) {
    if (!hay || !needle) return false;
    size_t n = strlen(needle);
    for (const char* p = hay; *p; ++p) {
        if (_strnicmp(p, needle, n) == 0) return true;
    }
    return false;
}

static bool LuaTouchesOurRefs(const std::filesystem::path& dir, std::string* hit) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return false;
    int scanned = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (++scanned > 400) break;   // keep start-up cost bounded
        if (!it->is_regular_file(ec)) continue;
        auto ext = it->path().extension().string();
        if (_stricmp(ext.c_str(), ".lua") != 0) continue;
        FILE* f = _wfopen(it->path().wstring().c_str(), L"rb");
        if (!f) continue;
        std::string text;
        char buf[8192];
        size_t got = 0;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
            text.append(buf, got);
            if (text.size() > 400000) break;
        }
        fclose(f);
        if (text.find("LOD_bias_rat") != std::string::npos ||
            text.find("reno/tex_res") != std::string::npos) {
            if (hit) *hit = it->path().filename().string();
            return true;
        }
    }
    return false;
}

void ScanOtherPlugins() {
    static const char* kWatch[] = { "LockYourTexture", "VisualXP", "AutoLOD", "Auto LOD",
                                    "RenderDistance", "3jFPS", "shade", "VRAM", "XPR",
                                    nullptr };
    char hits[3][96] = {};
    int  nHits = 0;

    int count = XPLMCountPlugins();
    XPLMPluginID me = XPLMGetMyID();
    for (int i = 0; i < count && nHits < 3; ++i) {
        XPLMPluginID id = XPLMGetNthPlugin(i);
        if (id == me) continue;
        char name[256] = {0}, path[512] = {0}, sig[256] = {0}, desc[512] = {0};
        XPLMGetPluginInfo(id, name, path, sig, desc);
        if (strstr(path, "VRAMManager") || strstr(sig, "vrammanager")) continue;
        for (int k = 0; kWatch[k] && nHits < 3; ++k) {
            if (ContainsNoCase(name, kWatch[k]) || ContainsNoCase(desc, kWatch[k])) {
                bool dup = false;
                for (int j = 0; j < nHits; ++j) if (strcmp(hits[j], name) == 0) dup = true;
                if (!dup) {
                    strncpy(hits[nHits], name, sizeof(hits[0]) - 1);
                    ++nHits;
                }
            }
        }
    }

    char root[512] = {0};
    XPLMGetSystemPath(root);
    std::string luaHit;
    if (nHits < 3) {
        std::filesystem::path scripts =
            std::filesystem::u8path(std::string(root) + "Resources/plugins/FlyWithLua/Scripts");
        if (LuaTouchesOurRefs(scripts, &luaHit)) {
            _snprintf(hits[nHits], sizeof(hits[0]) - 1, "FlyWithLua:%s", luaHit.c_str());
            ++nHits;
        }
    }

    gRt.conflict[0] = 0;
    if (nHits == 0) return;
    std::string s;
    for (int i = 0; i < nHits; ++i) {
        if (i) s += ", ";
        s += hits[i];
    }
    strncpy(gRt.conflict, s.c_str(), sizeof(gRt.conflict) - 1);
    Log("VRAM: other add-ons present: %s (informational only - this plugin keeps "
        "controlling the render distance)", gRt.conflict);
}



