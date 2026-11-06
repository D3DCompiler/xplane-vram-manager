// VRAM sampling with a fallback chain:
//   1. PDH  \GPU Process Memory(pid_...)\Dedicated Usage  (exact, includes Vulkan)
//   2. DXGI IDXGIAdapter3::QueryVideoMemoryInfo(0)        (per-process, driver view)
//   3. PDH  \GPU Adapter Memory(*)\Dedicated Usage        (whole GPU, last resort)
// Plus the DXGI budget / total VRAM of the strongest adapter.
#include "vram_manager.h"

#include <initguid.h>
#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>

static PDH_HQUERY   g_query     = nullptr;
static PDH_HCOUNTER g_cProc     = nullptr;   // per-process dedicated usage
static PDH_HCOUNTER g_cAdapter  = nullptr;   // whole-adapter dedicated usage

static IDXGIFactory1* g_factory = nullptr;
static IDXGIAdapter3* g_adapter = nullptr;
static uint64_t       g_vram    = 0;
static char           g_gpuName[128] = {0};
static bool           g_loggedSample = false;

static void CopyGpuName(const WCHAR* w, char* out, size_t outCount) {
    if (!w || !out || outCount == 0) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outCount - 1, nullptr, nullptr);
    if (n <= 0) { out[0] = 0; return; }
    out[outCount - 1] = 0;
}

bool Vram_Init() {
    // DXGI needs COM on this thread; X-Plane usually has it, but be explicit and
    // never fail hard if the apartment is already taken.
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Log("VRAM: CoInitializeEx = 0x%08lX", (unsigned long)hrCo);

    // --- performance counters ----------------------------------------------
    if (!g_query) {
        PDH_STATUS st = PdhOpenQueryW(nullptr, 0, &g_query);
        if (st == ERROR_SUCCESS) {
            st = PdhAddEnglishCounterW(g_query,
                    L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &g_cProc);
            if (st != ERROR_SUCCESS) {
                g_cProc = nullptr;
                Log("VRAM: per-process GPU counter unavailable (0x%08lX)", (unsigned long)st);
            }
            st = PdhAddEnglishCounterW(g_query,
                    L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &g_cAdapter);
            if (st != ERROR_SUCCESS) {
                g_cAdapter = nullptr;
                Log("VRAM: adapter GPU counter unavailable (0x%08lX)", (unsigned long)st);
            }
            PdhCollectQueryData(g_query);
        } else {
            g_query = nullptr;
            Log("VRAM: PdhOpenQueryW failed (0x%08lX)", (unsigned long)st);
        }
    }

    // --- DXGI: adapter + budget ---------------------------------------------
    if (!g_factory) {
        IDXGIFactory1* f = nullptr;
        HRESULT hr = CreateDXGIFactory1(IID_IDXGIFactory1, (void**)&f);
        Log("VRAM: CreateDXGIFactory1 = 0x%08lX", (unsigned long)hr);
        if (SUCCEEDED(hr) && f) {
            g_factory = f;
            IDXGIAdapter1* best    = nullptr;
            SIZE_T         bestMem = 0;
            for (UINT i = 0;; ++i) {
                IDXGIAdapter1* ad = nullptr;
                if (f->EnumAdapters1(i, &ad) != S_OK || !ad) break;
                DXGI_ADAPTER_DESC1 d = {};
                if (SUCCEEDED(ad->GetDesc1(&d)) &&
                    !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                    d.DedicatedVideoMemory > bestMem) {
                    bestMem = d.DedicatedVideoMemory;
                    if (best) best->Release();
                    best = ad;
                    continue;
                }
                ad->Release();
            }
            if (best) {
                DXGI_ADAPTER_DESC d = {};
                if (SUCCEEDED(best->GetDesc(&d))) {
                    g_vram = d.DedicatedVideoMemory;
                    CopyGpuName(d.Description, g_gpuName, sizeof(g_gpuName));
                }
                best->QueryInterface(IID_IDXGIAdapter3, (void**)&g_adapter);
                best->Release();
            }
        }
    }

    Log("VRAM: adapter=%s vram=%.2f GB pdh_process=%s pdh_adapter=%s dxgi=%s",
        g_gpuName[0] ? g_gpuName : "unknown",
        (double)g_vram / (1024.0 * 1024.0 * 1024.0),
        g_cProc ? "yes" : "no", g_cAdapter ? "yes" : "no", g_adapter ? "yes" : "no");
    return g_vram > 0;
}

void Vram_Shutdown() {
    if (g_query) {
        PdhCloseQuery(g_query);
        g_query = nullptr;
        g_cProc = nullptr;
        g_cAdapter = nullptr;
    }
    if (g_adapter) { g_adapter->Release(); g_adapter = nullptr; }
    if (g_factory) { g_factory->Release(); g_factory = nullptr; }
}

// Sum a wildcard counter array.  When pidTag is non-null only instances whose
// name starts with that tag are counted (returns their sum in *mine).
static bool ReadCounterArray(PDH_HCOUNTER counter, const char* pidTag,
                             uint64_t* mine, uint64_t* total) {
    *mine = 0;
    *total = 0;
    if (!counter) return false;

    DWORD      size = 0, count = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &size, &count, nullptr);
    if (st != PDH_MORE_DATA || size == 0) {
        if (st != ERROR_SUCCESS)
            Log("VRAM: PdhGetFormattedCounterArrayW(size) = 0x%08lX", (unsigned long)st);
        return false;
    }

    std::vector<BYTE> buf(size + 256, 0);
    st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &size, &count,
                                      (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data());
    if (st != ERROR_SUCCESS) {
        Log("VRAM: PdhGetFormattedCounterArrayW(data) = 0x%08lX", (unsigned long)st);
        return false;
    }

    PDH_FMT_COUNTERVALUE_ITEM_W* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
    const size_t tagLen = pidTag ? strlen(pidTag) : 0;
    uint64_t sum = 0, own = 0;
    for (DWORD i = 0; i < count; ++i) {
        LONGLONG v = items[i].FmtValue.largeValue;
        if (v < 0) v = 0;
        sum += (uint64_t)v;
        if (tagLen && items[i].szName) {
            char name[512] = {0};
            if (WideCharToMultiByte(CP_ACP, 0, items[i].szName, -1, name, sizeof(name) - 1,
                                    nullptr, nullptr) > 0 &&
                strncmp(name, pidTag, tagLen) == 0) {
                own += (uint64_t)v;
            }
        }
    }
    *mine  = (tagLen == 0) ? sum : own;   // no pid tag -> the caller wants the total
    *total = sum;
    return true;
}

bool Vram_Sample(VramInfo* out) {
    if (!out) return false;
    *out = VramInfo();

    if (g_query && (g_cProc || g_cAdapter)) PdhCollectQueryData(g_query);

    char pidTag[32];
    _snprintf(pidTag, sizeof(pidTag), "pid_%lu_", (unsigned long)GetCurrentProcessId());
    pidTag[sizeof(pidTag) - 1] = 0;

    uint64_t mine = 0, total = 0;
    bool haveProc = ReadCounterArray(g_cProc, pidTag, &mine, &total);
    out->totalBytes = total;
    out->fromPdh    = haveProc;

    uint64_t adapterUsage = 0, adapterTotal = 0;
    bool haveAdapterPdh = ReadCounterArray(g_cAdapter, nullptr, &adapterUsage, &adapterTotal);

    uint64_t dxgiUsage = 0;
    if (g_adapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO q = {};
        if (SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &q))) {
            out->budgetBytes = q.Budget;
            dxgiUsage        = q.CurrentUsage;
        }
    }

    // --- pick the best available source ------------------------------------
    if (haveProc && mine > 0) {
        out->processBytes = mine;
        out->source = 1;
    } else if (dxgiUsage > 0) {
        out->processBytes = dxgiUsage;
        out->source = 2;
    } else if (haveAdapterPdh && adapterUsage > 0) {
        out->processBytes = adapterUsage;   // whole GPU (other apps included)
        out->source = 3;
    } else {
        out->processBytes = 0;
        out->source = 0;
    }

    out->vramBytes = g_vram;
    strncpy(out->adapter, g_gpuName, sizeof(out->adapter) - 1);

    if (out->vramBytes > 0 && out->processBytes > 0) {
        double pct = 100.0 * (double)out->processBytes / (double)out->vramBytes;
        if (pct < 0.0) pct = 0.0;
        if (pct > 100.0) pct = 100.0;
        out->pct = (float)pct;
    }
    out->valid = out->vramBytes > 0 && out->processBytes > 0;

    if (!g_loggedSample) {
        g_loggedSample = true;
        Log("VRAM: first sample src=%d used=%.2f GB total=%.2f GB vram=%.2f GB budget=%.2f GB "
            "procPdh=%d adapterPdh=%d",
            out->source,
            (double)out->processBytes / 1073741824.0,
            (double)out->totalBytes / 1073741824.0,
            (double)out->vramBytes / 1073741824.0,
            (double)out->budgetBytes / 1073741824.0,
            haveProc ? 1 : 0, haveAdapterPdh ? 1 : 0);
    }
    return out->valid;
}
