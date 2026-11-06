// Plugins menu, commands, localization and About/diagnostics.
//
// The UI itself is the modern self-drawn panel in panel.cpp - there is no
// classic widget panel any more.
#include "vram_manager.h"
#include "panel.h"

// ---------------------------------------------------------------------------
// localization
// ---------------------------------------------------------------------------
static const char* kEn[S_COUNT] = {
    /* S_TITLE      */ "VRAM Manager",
    /* S_AUTO       */ "Auto manage",
    /* S_AUTOPURGE  */ "Auto rescue purge",
    /* S_TARGET     */ "Target VRAM",
    /* S_MINLOD     */ "Min distance",
    /* S_PURGE      */ "Purge",
    /* S_RESET      */ "Reset",
    /* S_SAVE       */ "Save",
    /* S_HIDE       */ "Hide",
    /* S_LANG       */ "EN/zh",
    /* S_ABOUT      */ "About",
    /* S_RELOADSC   */ "Reload scenery",
    /* S_LIVE       */ "Live",
    /* S_GPU        */ "GPU",
    /* S_MENU_SHOW  */ "Show / hide panel",
    /* S_MENU_AUTO  */ "Auto manage render distance",
    /* S_MENU_PURGE */ "Purge VRAM now",
    /* S_MENU_RESET */ "Restore X-Plane settings",
    /* S_MENU_RELOAD*/ "Reload scenery (hard purge)",
    /* S_MENU_UI    */ "UI size",
    /* S_MENU_THEME */ "Theme",
    /* S_MENU_ABOUT */ "About / diagnostics",
};

static const char* kZh[S_COUNT] = {
    /* S_TITLE      */ "显存管理器",
    /* S_AUTO       */ "自动管理",
    /* S_AUTOPURGE  */ "自动应急清理",
    /* S_TARGET     */ "目标显存",
    /* S_MINLOD     */ "最低距离",
    /* S_PURGE      */ "清理",
    /* S_RESET      */ "恢复",
    /* S_SAVE       */ "保存",
    /* S_HIDE       */ "隐藏",
    /* S_LANG       */ "中/EN",
    /* S_ABOUT      */ "关于",
    /* S_RELOADSC   */ "重载地景",
    /* S_LIVE       */ "实时",
    /* S_GPU        */ "显卡",
    /* S_MENU_SHOW  */ "显示 / 隐藏 面板",
    /* S_MENU_AUTO  */ "自动管理渲染距离",
    /* S_MENU_PURGE */ "立即清理显存",
    /* S_MENU_RESET */ "恢复 X-Plane 原始设置",
    /* S_MENU_RELOAD*/ "重载地景（强力清理）",
    /* S_MENU_UI    */ "界面大小",
    /* S_MENU_THEME */ "主题",
    /* S_MENU_ABOUT */ "关于 / 诊断信息",
};

int GetLang() {
    if (gSet.lang >= 0) return gSet.lang;
    return 0;
}

void SetLang(int lang) {
    gSet.lang = (lang == 1) ? 1 : 0;
    Config_Save();
}

const char* T(int id) {
    if (id < 0 || id >= S_COUNT) return "";
    return GetLang() == 1 ? kZh[id] : kEn[id];
}

// ---------------------------------------------------------------------------
// menu (always English)
// ---------------------------------------------------------------------------
static XPLMMenuID sMenu = nullptr;
static int sItemShow = -1, sItemAuto = -1, sItemPurge = -1, sItemReset = -1,
           sItemReload = -1, sItemUi = -1, sItemAbout = -1;

static XPLMCommandRef sCmdPanel = nullptr, sCmdAuto = nullptr, sCmdPurge = nullptr;
static bool sCommandsCreated = false;

static void About() {
    Log("VRAM: about - GPU %s | VRAM source %d | LOD %s",
        gRt.vram.adapter[0] ? gRt.vram.adapter : "?", gRt.vram.source, gRef[REF_LOD].name);
    Log("VRAM: LOD=%s tex=%s async=%s fps=%s", gRef[REF_LOD].name, gRef[REF_TEXRES].name,
        gRef[REF_ASYNC].name, gRef[REF_FRAMETIME].name);
    Log("VRAM: vram=%.2f GB used=%.2f GB total=%.2f GB pct=%.0f src=%d tex=%d (read-only)",
        (double)gRt.vram.vramBytes / 1073741824.0,
        (double)gRt.vram.processBytes / 1073741824.0,
        (double)gRt.vram.totalBytes / 1073741824.0, gRt.vram.pct, gRt.vram.source, gRt.texNow);
}

static void MenuHandler(void* inMenuRef, void* inItemRef) {
    (void)inMenuRef;
    intptr_t id = (intptr_t)inItemRef;
    switch (id) {
        case 1: Panel_Toggle(); break;
        case 2: Ctrl_ToggleAuto(); break;
        case 3: Ctrl_Purge(); break;
        case 4: Ctrl_Reset(); break;
        case 5: Ctrl_ReloadScenery(); break;
        case 6: Panel_CycleScale(); break;
        case 7: About(); break;
        default: break;
    }
}

static int CmdHandler(XPLMCommandRef inCommand, XPLMCommandPhase inPhase, void* inRefcon) {
    (void)inRefcon;
    if (inPhase != xplm_CommandBegin) return 0;
    if (inCommand == sCmdPanel) Panel_Toggle();
    else if (inCommand == sCmdAuto) Ctrl_ToggleAuto();
    else if (inCommand == sCmdPurge) Ctrl_Purge();
    return 0;
}

bool Menu_Ready() { return sMenu != nullptr; }

void Menu_Init() {
    static int failLogs = 0;
    if (sMenu) return;
    XPLMMenuID parent = XPLMFindPluginsMenu();
    if (!parent) {
        if (failLogs++ < 3) Log("VRAM: Plugins menu not ready yet, will retry");
        return;
    }
    // Create the top-level entry explicitly: on X-Plane 12 the inParentItem
    // argument attaches the submenu to an *existing* item, so passing 0 nested
    // this menu under "Hide Plugin Admin" and it was invisible.
    int topItem = XPLMAppendMenuItem(parent, "VRAM Manager", (void*)100, 1);
    if (topItem >= 0)
        sMenu = XPLMCreateMenu("VRAM Manager", parent, topItem, MenuHandler, nullptr);
    if (!sMenu) {
        sMenu = XPLMCreateMenu("VRAM Manager", parent, 0, MenuHandler, nullptr);
        if (sMenu) Log("VRAM: Plugins menu created (fallback path)");
    }
    if (!sMenu) {
        if (failLogs++ < 3) Log("VRAM: could not create the Plugins menu, will retry");
        return;
    }

    sItemShow   = XPLMAppendMenuItem(sMenu, "Show / hide panel", (void*)1, 1);
    XPLMAppendMenuSeparator(sMenu);
    sItemAuto   = XPLMAppendMenuItem(sMenu, "Auto manage render distance", (void*)2, 1);
    sItemPurge  = XPLMAppendMenuItem(sMenu, "Purge VRAM now", (void*)3, 1);
    sItemReset  = XPLMAppendMenuItem(sMenu, "Restore X-Plane settings", (void*)4, 1);
    sItemReload = XPLMAppendMenuItem(sMenu, "Reload scenery (hard purge)", (void*)5, 1);
    XPLMAppendMenuSeparator(sMenu);
    sItemUi     = XPLMAppendMenuItem(sMenu, "UI size", (void*)6, 1);
    sItemAbout  = XPLMAppendMenuItem(sMenu, "About / diagnostics", (void*)7, 1);

    if (!sCommandsCreated) {
        sCmdPanel = XPLMCreateCommand("vrammanager/panel", "VRAM Manager: show/hide panel");
        sCmdAuto  = XPLMCreateCommand("vrammanager/auto", "VRAM Manager: toggle auto manage");
        sCmdPurge = XPLMCreateCommand("vrammanager/purge", "VRAM Manager: purge VRAM");
        if (sCmdPanel) XPLMRegisterCommandHandler(sCmdPanel, CmdHandler, 1, nullptr);
        if (sCmdAuto)  XPLMRegisterCommandHandler(sCmdAuto, CmdHandler, 1, nullptr);
        if (sCmdPurge) XPLMRegisterCommandHandler(sCmdPurge, CmdHandler, 1, nullptr);
        sCommandsCreated = true;
    }

    Menu_Refresh();
    Log("VRAM: Plugins menu created");
}

void Menu_Shutdown() {
    // X-Plane removes a plugin's menus by itself when the plugin is disabled;
    // destroying an already-freed menu here would be a use-after-free.
    sMenu = nullptr;
    sItemShow = sItemAuto = sItemPurge = sItemReset = sItemReload = sItemUi =
        sItemAbout = -1;
}

void Menu_Refresh() {
    if (!sMenu) return;
    if (sItemShow >= 0)
        XPLMCheckMenuItem(sMenu, sItemShow,
                          Panel_Visible() ? xplm_Menu_Checked : xplm_Menu_Unchecked);
    if (sItemAuto >= 0)
        XPLMCheckMenuItem(sMenu, sItemAuto, gSet.autoManage ? xplm_Menu_Checked : xplm_Menu_Unchecked);
}
