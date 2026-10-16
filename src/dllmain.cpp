// ArkSplitscreen — proxy xinput1_3.dll for ARK: Survival Evolved (preaquatica, build 358.26).
//
// Re-enables the console splitscreen code left in the PC build (press Menu/Start on controller 2 to join):
//
// 1. XInput proxy: forwards every export to the real System32\xinput1_3.dll.
// 2. Player id: ULocalPlayer::GetPreferredUniqueNetId gives splitscreen players (ControllerId 1-3), who have no
//    Steam account, a stable fake FUniqueNetIdUInt64 (player 1's Steam id + ControllerId). Without an id the game
//    crashes in AShooterGameMode::StartNewShooterPlayer when player 2 joins.
// 3. Player count: UShooterGameViewportClient::NotifyPlayerAdded/Removed track local players. Optionally keeps the
//    global GIsSplitscreen in sync (console builds set it; the PC build reads it but never sets it).
// 4. Save data: single-player saves "the" local player as LocalPlayer.arkprofile, chosen by asking the controller
//    IsLocalController()/IsLocalPlayerController() — true for every splitscreen player, so player 2 loaded player
//    1's data and took over their character. Five call sites in the player-data code now answer "not local" for
//    splitscreen players 2+, who get the server-style save file named after their (fake) id instead.
// 5. HUDs: the game only builds a HUD at map load, and ReAdjustUI only clears every HUD's "built" flag, so a
//    player joining mid-game never got a HUD and player 1's was never re-laid-out. AShooterHUD::DrawHUD (vtable
//    hook) builds any unbuilt HUD once splitscreen is used; the game's layout code then splits them top/bottom.
// 6. Inventory: AShooterCharacter::Tick asked "is an inventory open?" without saying whose, so every local
//    character raised its hand when anyone opened theirs. That call now passes the character's own controller.
// 8. Radial menu (hold E): URadialSelectorHUD_MultiUse::DrawHUD has console splitscreen sizes — circle 0.63 but
//    text 0.70 (both 0.85 single-player), so labels spill out of the ring. The text scale's load is pointed at the
//    mod's own value (default 0.63, same proportions as single-player; ini SplitscreenRadialTextScale).
// 7. Lighting: with two views the renderer clears several scene textures once per view with "everything except
//    this view's rectangle", so each view's clear wiped the other view's lighting (only the 1-pixel seam where
//    the rectangles overlap survived; every surface rendered black). While 2+ players are in, clears with an
//    exclude rectangle are skipped — the views cover the whole screen, so there's nothing outside them to clear.
//
// Every address is an RVA in the exact ShooterGame.exe this was built against (notes/symbol-findings.md). Each
// patch site is checked byte-for-byte first; on any mismatch that fix is skipped and logged.
//
// ArkSplitscreen.ini next to the DLL ([Settings]): SetSplitscreenFlag, RebuildHUDs, FixSplitscreenLighting (0/1),
// SplitscreenRadialTextScale (float), and [SplitscreenCVars] name=value render settings applied only while 2+
// players are in.

#define WIN32_LEAN_AND_MEAN
#include <unordered_map>
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <intrin.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {

// ---- Addresses (RVAs from ShooterGame.pdb) ------------------------------------------------------------

constexpr uintptr_t kRvaGetPreferredUniqueNetId = 0x02A62470;  // ULocalPlayer::GetPreferredUniqueNetId
constexpr uintptr_t kRvaNotifyPlayerAdded = 0x00FBD010;        // UShooterGameViewportClient::NotifyPlayerAdded
constexpr uintptr_t kRvaNotifyPlayerRemoved = 0x00FBD1B0;      // UShooterGameViewportClient::NotifyPlayerRemoved
constexpr uintptr_t kRvaClearCommandExecute = 0x026F1720;      // FRHICommand<FRHICommandClear>::ExecuteAndDestruct
constexpr uintptr_t kRvaGIsSplitscreen = 0x04982E0E;           // bool GIsSplitscreen
constexpr uintptr_t kRvaGDistanceFieldShadowing = 0x04938B6C;  // int, r.DistanceFieldShadowing
constexpr uintptr_t kRvaGStencilOptimization = 0x04938C6C;     // int, r.StencilOptimization
constexpr uintptr_t kRvaGEngine = 0x049A80B8;                  // UEngine* GEngine
constexpr uintptr_t kRvaGetGameUserSettings = 0x02C3CA40;      // UEngine::GetGameUserSettings
constexpr size_t kUserSettingsUIScaleOffset = 0x13C;           // UShooterGameUserSettings UI scale (options slider)
constexpr uintptr_t kRvaGSupportsDepthBoundsTest = 0x049A7262; // bool
constexpr uintptr_t kRvaShadowRenderProjection = 0x037C1EC0;   // FProjectedShadowInfo::RenderProjection
constexpr uintptr_t kRvaShadowRenderDepth = 0x037C1580;        // FProjectedShadowInfo::RenderDepth
constexpr uintptr_t kRvaUInt64IdVtable = 0x045719A8;           // FUniqueNetIdUInt64::`vftable'
constexpr uintptr_t kRvaUInt64IdCompare = 0x001642E0;          // its first slot, used as a sanity check
constexpr uintptr_t kRvaIsSplitscreenPlayer = 0x02AC4880;      // APlayerController::IsSplitscreenPlayer(int*)
constexpr uintptr_t kRvaInitUIScenes = 0x00FEE1D0;             // AShooterHUD::InitUIScenes(bool)
constexpr uintptr_t kRvaHudGetPlayerOwner = 0x01003140;        // AShooterHUD::GetPlayerOwner
constexpr uintptr_t kRvaShooterHudVtable = 0x045E9950;         // AShooterHUD::`vftable'
constexpr uintptr_t kRvaDrawHUD = 0x00FE5D90;                  // AShooterHUD::DrawHUD
constexpr uintptr_t kRvaGetUISceneFromClass = 0x00FC1BD0;      // UShooterGameViewportClient::GetUISceneFromClass
constexpr uintptr_t kRvaConsoleManagerSingleton = 0x049825C0;  // IConsoleManager::Singleton
constexpr uintptr_t kRvaFindConsoleObjectUnfiltered = 0x020B7630;

// Hook prologues: whole instructions, none RIP-relative, so they can be relocated into a trampoline.
// GetPreferredUniqueNetId: push rsi / push r14 / sub rsp,38h / mov rax,[rcx+58h] / mov [rsp+60h],rbp
constexpr uint8_t kPrologueGetPreferredUniqueNetId[] = {0x40, 0x56, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x38, 0x48,
                                                        0x8B, 0x41, 0x58, 0x48, 0x89, 0x6C, 0x24, 0x60};
// NotifyPlayerAdded: mov [rsp+18h],r8 / mov [rsp+10h],edx / mov [rsp+8],rcx / sub rsp,38h
constexpr uint8_t kPrologueNotifyPlayerAdded[] = {0x4C, 0x89, 0x44, 0x24, 0x18, 0x89, 0x54, 0x24, 0x10,
                                                  0x48, 0x89, 0x4C, 0x24, 0x08, 0x48, 0x83, 0xEC, 0x38};
// NotifyPlayerRemoved: same, but sub rsp,28h
constexpr uint8_t kPrologueNotifyPlayerRemoved[] = {0x4C, 0x89, 0x44, 0x24, 0x18, 0x89, 0x54, 0x24, 0x10,
                                                    0x48, 0x89, 0x4C, 0x24, 0x08, 0x48, 0x83, 0xEC, 0x28};
// Clear ExecuteAndDestruct: sub rsp,58h / movups xmm0,[rdx+20h] / mov rcx,[rcx+28h] / movzx r9d,byte [rdx+39h]
constexpr uint8_t kPrologueClearCommandExecute[] = {0x48, 0x83, 0xEC, 0x58, 0x0F, 0x10, 0x42, 0x20, 0x48,
                                                    0x8B, 0x49, 0x28, 0x44, 0x0F, 0xB6, 0x4A, 0x39};
constexpr size_t kAbsJmpSize = 14;  // FF 25 00000000 + 8-byte target

constexpr size_t kLocalPlayerControllerIdOffset = 0x50;  // ULocalPlayer::ControllerId
constexpr size_t kHudUIScenesBuiltOffset = 0xA7D;        // AShooterHUD: InitUIScenes skips itself if set
constexpr size_t kVtDrawHUD = 0xA90;                     // AShooterHUD vtable slot
constexpr size_t kVtIsLocalPlayerController = 0xAA0;     // APlayerController vtable slots
constexpr size_t kVtIsLocalController = 0xAA8;
constexpr size_t kClearExcludeRectOffset = 0x20;         // FRHICommandClear::ExcludeRect {minX,minY,maxX,maxY}
constexpr int kMaxSplitscreenPlayers = 4;

// `call qword ptr [rax + slot]` (FF 90 disp32) sites in the player-data code; rcx = the controller at each.
struct LocalCheckSite {
    const char* where;
    uintptr_t rva;
    size_t slot;
};
constexpr LocalCheckSite kLocalCheckSites[] = {
    {"LoadPlayerData (LocalPlayer file name)", 0x00EE6983, kVtIsLocalController},
    {"DeletePlayerData (LocalPlayer file name)", 0x00EE8DEA, kVtIsLocalController},
    {"GetPlayerDataFor (PlayerData local flag)", 0x00EF3134, kVtIsLocalPlayerController},
    {"GetPlayerDataFor (PlayerData local flag, new data)", 0x00EF3474, kVtIsLocalPlayerController},
    {"StartNewShooterPlayer (PlayerData local flag)", 0x00EDDEB7, kVtIsLocalPlayerController},
};
constexpr size_t kCallSiteSize = 6;

// AShooterCharacter::Tick inventory check: GetUISceneFromClass(UI_Hub, controller = nullptr).
constexpr uintptr_t kRvaTickInventoryNullController = 0x00CBEE82;  // xor r8d, r8d
constexpr uintptr_t kRvaTickInventoryCall = 0x00CBEE93;            // call GetUISceneFromClass
constexpr uintptr_t kRvaTickCharacterLoad = 0x00CBEE61;            // mov rax, [rsp+3910h]  (this character)
constexpr uint32_t kTickCharacterStackOffset = 0x3910;
constexpr uint32_t kPawnControllerOffset = 0x4A0;                  // APawn::Controller

// URadialSelectorHUD_MultiUse::DrawHUD: `movss xmm0, [rip+X]` loading the splitscreen text scale (0.7).
constexpr uintptr_t kRvaRadialSplitscreenTextScaleLoad = 0x00C55AF4;
constexpr uint32_t kRadialSplitscreenTextScaleBits = 0x3F333333;  // 0.7f

// Console variables (ARK blocks the console; these are set directly).
constexpr size_t kVtCvarAsVariable = 0x28;      // IConsoleObject::AsVariable
constexpr size_t kVtCvarSet = 0x58;             // IConsoleVariable::Set(const TCHAR*, EConsoleVariableFlags)
constexpr size_t kVtCvarGetFloat = 0x68;        // IConsoleVariable::GetFloat
constexpr uint32_t kSetByConsole = 0x09000000;  // highest "set by" priority, so the change isn't refused

// ---- Engine layouts (UE 4.5 as compiled into ARK) -----------------------------------------------------

// SharedPointerInternals reference controller: no vtable, the deleter is a plain function pointer.
struct RefController {
    int32_t sharedRefs;
    int32_t weakRefs;
    void* object;
    void (*destroyObject)(void*);
};

struct SharedPtrRaw {  // TSharedPtr<FUniqueNetId, NotThreadSafe>
    void* object;
    RefController* controller;
};

struct UInt64NetId {  // FUniqueNetIdUInt64
    const void* vtable;
    uint64_t id;
};

using GetBytesFn = const uint8_t* (*)(const void* self);  // FUniqueNetId vtable +0x10
using GetSizeFn = int32_t (*)(const void* self);          // FUniqueNetId vtable +0x18
using GetPreferredUniqueNetIdFn = SharedPtrRaw* (*)(void* localPlayer, SharedPtrRaw* out);
using NotifyPlayerFn = void (*)(void* viewportClient, int32_t playerIndex, void* localPlayer);
using ClearCommandExecuteFn = void (*)(void* cmdList, uint8_t* cmd);
using ControllerBoolFn =bool (*)(void* controller);
using IsSplitscreenPlayerFn = bool (*)(const void* controller, int32_t* outIndex);
using InitUIScenesFn = void (*)(void* hud, bool force);
using HudGetPlayerOwnerFn = void* (*)(const void* hud);
using DrawHUDFn = void (*)(void* hud);
using FindConsoleObjectFn = void* (*)(void* manager, const wchar_t* name);
using CvarAsVariableFn = void* (*)(void* object);
using CvarSetFn = void (*)(void* variable, const wchar_t* value, uint32_t setBy);
using CvarGetFloatFn = float (*)(void* variable);

// ---- State --------------------------------------------------------------------------------------------

uintptr_t g_base = 0;
wchar_t g_logPath[MAX_PATH] = {};
wchar_t g_iniPath[MAX_PATH] = {};

bool g_setSplitscreenFlag = false;   // SetSplitscreenFlag=1 keeps GIsSplitscreen in sync with the player count
bool g_rebuildHUDs = true;           // RebuildHUDs=0 leaves HUDs alone
bool g_fixLighting = true;           // FixSplitscreenLighting=0 keeps the per-view exclude-rect clears
float g_radialTextScale = 0.63f;     // SplitscreenRadialTextScale; 0 or less leaves the game's 0.7
bool g_fixShadows = true;            // FixSplitscreenShadows=0 keeps distance-field shadows in splitscreen
float g_splitscreenHUDScale = 1.0f;  // SplitscreenHUDScale: multiplies the UI scale setting while 2+ players are in
float g_toolbarScale = 1.0f;         // SplitscreenToolbarScale: multiplies the toolbar's splitscreen scale
float g_radialTextDistance = 1.0f;   // SplitscreenRadialTextDistance: multiplies the radial labels' distance
float g_tooltipScaleY = 1.0f;        // SplitscreenTooltipScaleY: multiplies tooltips' splitscreen height
void UpdateRadialTextRadius(int32_t localPlayers);

GetPreferredUniqueNetIdFn g_originalGetPreferredUniqueNetId = nullptr;
NotifyPlayerFn g_originalNotifyPlayerAdded = nullptr;
NotifyPlayerFn g_originalNotifyPlayerRemoved = nullptr;
ClearCommandExecuteFn g_originalClearCommandExecute = nullptr;
DrawHUDFn g_originalDrawHUD = nullptr;

volatile int32_t g_localPlayerCount = 0;  // read on the render/RHI threads
bool g_splitscreenUsed = false;           // a second local player has joined at least once this session

uint64_t g_player1Id = 0;
// Fake ids live for the whole process: the ref counts start huge so the game never tries to free them.
UInt64NetId g_fakeIds[kMaxSplitscreenPlayers] = {};
RefController g_fakeControllers[kMaxSplitscreenPlayers] = {};
bool g_loggedFake[kMaxSplitscreenPlayers] = {};

void Log(const char* fmt, ...) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_logPath, L"a") != 0 || !f) {
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

// ---- 2. Player id -------------------------------------------------------------------------------------

void NeverDestroy(void*) {}

uint64_t ReadIdValue(const void* netId) {
    auto vtable = *reinterpret_cast<void* const* const*>(netId);
    auto getSize = reinterpret_cast<GetSizeFn>(vtable[3]);
    auto getBytes = reinterpret_cast<GetBytesFn>(vtable[2]);
    if (getSize(netId) != sizeof(uint64_t)) {
        return 0;
    }
    uint64_t value = 0;
    memcpy(&value, getBytes(netId), sizeof(value));
    return value;
}

SharedPtrRaw* HookedGetPreferredUniqueNetId(void* localPlayer, SharedPtrRaw* out) {
    g_originalGetPreferredUniqueNetId(localPlayer, out);

    int32_t controllerId = 0;
    memcpy(&controllerId, static_cast<uint8_t*>(localPlayer) + kLocalPlayerControllerIdOffset, sizeof(controllerId));

    if (controllerId == 0) {
        if (out->object && g_player1Id == 0) {
            g_player1Id = ReadIdValue(out->object);
            Log("player 1 id: %llu", static_cast<unsigned long long>(g_player1Id));
        }
        return out;
    }
    if (out->object || controllerId < 1 || controllerId >= kMaxSplitscreenPlayers) {
        return out;
    }

    UInt64NetId& fake = g_fakeIds[controllerId];
    RefController& controller = g_fakeControllers[controllerId];
    if (!fake.vtable) {
        // Derived from player 1 so the same pair of people always get the same characters back.
        // Fallback (player 1 unknown): a fixed id in the Steam individual-account range.
        uint64_t base = g_player1Id ? g_player1Id : 76561197960265728ull + 0x7FFF0000ull;
        fake.vtable = reinterpret_cast<const void*>(g_base + kRvaUInt64IdVtable);
        fake.id = base + static_cast<uint64_t>(controllerId);
        controller.sharedRefs = 0x10000000;
        controller.weakRefs = 0x10000000;
        controller.object = &fake;
        controller.destroyObject = &NeverDestroy;
    }
    if (!g_loggedFake[controllerId]) {
        g_loggedFake[controllerId] = true;
        Log("controller %d has no id; giving it fake id %llu", controllerId, static_cast<unsigned long long>(fake.id));
    }

    // Same as the game's own copy: point at the object and take a shared reference.
    out->object = &fake;
    out->controller = &controller;
    ++controller.sharedRefs;
    return out;
}

// ---- Console variables ([SplitscreenCVars]) -----------------------------------------------------------

void* FindCvar(const wchar_t* name) {
    void* manager = *reinterpret_cast<void**>(g_base + kRvaConsoleManagerSingleton);
    if (!manager) {
        return nullptr;
    }
    void* object = reinterpret_cast<FindConsoleObjectFn>(g_base + kRvaFindConsoleObjectUnfiltered)(manager, name);
    if (!object) {
        return nullptr;
    }
    auto vtable = *reinterpret_cast<void* const* const*>(object);
    return reinterpret_cast<CvarAsVariableFn>(vtable[kVtCvarAsVariable / sizeof(void*)])(object);
}

bool GetCvar(const wchar_t* name, float* out) {
    void* variable = FindCvar(name);
    if (!variable) {
        return false;
    }
    auto vtable = *reinterpret_cast<void* const* const*>(variable);
    *out = reinterpret_cast<CvarGetFloatFn>(vtable[kVtCvarGetFloat / sizeof(void*)])(variable);
    return true;
}

void SetCvar(const wchar_t* name, const wchar_t* value) {
    void* variable = FindCvar(name);
    if (!variable) {
        Log("cvar %ls not found", name);
        return;
    }
    auto vtable = *reinterpret_cast<void* const* const*>(variable);
    reinterpret_cast<CvarSetFn>(vtable[kVtCvarSet / sizeof(void*)])(variable, value, kSetByConsole);
    Log("cvar %ls = %ls", name, value);
}

struct CvarOverride {
    wchar_t name[96];
    wchar_t value[32];
    wchar_t previous[32];
    bool applied;
};
CvarOverride g_splitscreenCvars[16] = {};
size_t g_splitscreenCvarCount = 0;
bool g_splitscreenCvarsApplied = false;

void LoadSplitscreenCvars() {
    wchar_t section[4096] = {};
    GetPrivateProfileSectionW(L"SplitscreenCVars", section, 4096, g_iniPath);
    for (const wchar_t* line = section; *line && g_splitscreenCvarCount < 16; line += wcslen(line) + 1) {
        const wchar_t* eq = wcschr(line, L'=');
        if (!eq || line[0] == L';') {
            continue;
        }
        CvarOverride& o = g_splitscreenCvars[g_splitscreenCvarCount++];
        wcsncpy_s(o.name, line, static_cast<size_t>(eq - line));
        wcscpy_s(o.value, eq + 1);
    }
    // Screen-space subsurface scattering (skin, foliage) runs ONE full-frame pass with a single view's parameters
    // (found with RenderDoc: PostProcessSubsurface, event 15894 of the capture) instead of one per view, which turned
    // the other player's shadowed skin pure black -- the "super dark shadows". Off while 2+ players are in, unless the
    // ini says otherwise ([Settings] SplitscreenSubsurfaceOff=0, or an r.SSS.Scale line in [SplitscreenCVars]).
    bool listed = false;
    for (size_t i = 0; i < g_splitscreenCvarCount; ++i) {
        listed = listed || _wcsicmp(g_splitscreenCvars[i].name, L"r.SSS.Scale") == 0;
    }
    if (!listed && g_splitscreenCvarCount < 16 &&
        GetPrivateProfileIntW(L"Settings", L"SplitscreenSubsurfaceOff", 1, g_iniPath) != 0) {
        CvarOverride& o = g_splitscreenCvars[g_splitscreenCvarCount++];
        wcscpy_s(o.name, L"r.SSS.Scale");
        wcscpy_s(o.value, L"0");
    }
}

void ApplySplitscreenCvars(int32_t localPlayers) {
    if (localPlayers > 1 && !g_splitscreenCvarsApplied) {
        for (size_t i = 0; i < g_splitscreenCvarCount; ++i) {
            CvarOverride& o = g_splitscreenCvars[i];
            float previous = 0;
            o.applied = GetCvar(o.name, &previous);
            if (o.applied) {
                swprintf_s(o.previous, L"%g", previous);
                SetCvar(o.name, o.value);
            } else {
                Log("cvar %ls not found", o.name);
            }
        }
        g_splitscreenCvarsApplied = true;
    } else if (localPlayers <= 1 && g_splitscreenCvarsApplied) {
        for (size_t i = 0; i < g_splitscreenCvarCount; ++i) {
            if (g_splitscreenCvars[i].applied) {
                SetCvar(g_splitscreenCvars[i].name, g_splitscreenCvars[i].previous);
            }
        }
        g_splitscreenCvarsApplied = false;
    }
}

// ---- 3. Player count ----------------------------------------------------------------------------------

// Distance-field (ray-traced) sun shadows don't work with two views: they share global culling buffers, so one
// player's soft shadows land in the other's half, and on top of the regular sun shadows they made spots very dark.
// Turning them off while 2+ players are in (the regular cascaded sun shadows stay) fixed both in testing; turning
// off only async compute (the console's splitscreen path) helped but left some cross-talk.
volatile bool g_dfShadowsOverridden = false;  // also read on the render thread
int32_t g_dfShadowsSaved = 0;
int32_t g_stencilOptimizationSaved = 0;

// Stencil optimization: shadow projections mark their area in the stencil buffer and rely on the pass leaving it
// zeroed instead of clearing it afterwards (RenderProjection 037C4565). TEST: off in splitscreen, so the stencil is
// cleared after every projection.
void UpdateDistanceFieldShadowMode(int32_t localPlayers) {
    int32_t* dfShadows = reinterpret_cast<int32_t*>(g_base + kRvaGDistanceFieldShadowing);
    int32_t* stencilOptimization = reinterpret_cast<int32_t*>(g_base + kRvaGStencilOptimization);
    if (g_fixShadows && localPlayers > 1 && !g_dfShadowsOverridden) {
        g_dfShadowsSaved = *dfShadows;
        g_stencilOptimizationSaved = *stencilOptimization;
        g_dfShadowsOverridden = true;
        *dfShadows = 0;
        *stencilOptimization = 0;
        Log("distance-field shadows off for splitscreen (was %d); stencil optimization off (was %d); depth bounds "
            "test supported: %d",
            g_dfShadowsSaved, g_stencilOptimizationSaved, *reinterpret_cast<bool*>(g_base + kRvaGSupportsDepthBoundsTest));
    } else if (localPlayers <= 1 && g_dfShadowsOverridden) {
        *dfShadows = g_dfShadowsSaved;
        *stencilOptimization = g_stencilOptimizationSaved;
        g_dfShadowsOverridden = false;
        Log("distance-field shadows restored (%d), stencil optimization restored (%d)", g_dfShadowsSaved,
            g_stencilOptimizationSaved);
    }
}

// HUD scale: UPrimalUI::ProcessSplitscreenScaling (and the HUD notification overlay) size each player's UI by
// DPI scale x the UI scale from the options menu (GameUserSettings+0x13C). While 2+ players are in, that value is
// multiplied by SplitscreenHUDScale; it's set before the HUDs are rebuilt on join and restored on leave.
bool g_hudScaleOverridden = false;
float g_hudScaleSaved = 1.0f;

float* UserSettingsUIScale() {
    void* engine = *reinterpret_cast<void**>(g_base + kRvaGEngine);
    if (!engine) {
        return nullptr;
    }
    using GetGameUserSettingsFn = void* (*)(void* engine);
    void* settings = reinterpret_cast<GetGameUserSettingsFn>(g_base + kRvaGetGameUserSettings)(engine);
    return settings ? reinterpret_cast<float*>(static_cast<uint8_t*>(settings) + kUserSettingsUIScaleOffset) : nullptr;
}

void UpdateSplitscreenHUDScale(int32_t localPlayers) {
    if (g_splitscreenHUDScale == 1.0f && !g_hudScaleOverridden) {
        return;
    }
    float* scale = UserSettingsUIScale();
    if (!scale) {
        return;
    }
    if (localPlayers > 1 && !g_hudScaleOverridden) {
        g_hudScaleSaved = *scale;
        *scale = g_hudScaleSaved * g_splitscreenHUDScale;
        g_hudScaleOverridden = true;
        Log("HUD scale %.2f -> %.2f for splitscreen", g_hudScaleSaved, *scale);
    } else if (localPlayers <= 1 && g_hudScaleOverridden) {
        *scale = g_hudScaleSaved;
        g_hudScaleOverridden = false;
        Log("HUD scale restored to %.2f", g_hudScaleSaved);
    }
}

// Called before the original NotifyPlayer*: the original rebuilds every HUD (ReAdjustUI), and
// UPrimalUI::AddToViewport reads GIsSplitscreen to pick its splitscreen scale.
void OnLocalPlayerCountChanged(int32_t localPlayers) {
    Log("%d local player(s)", localPlayers);
    if (localPlayers > 1) {
        g_splitscreenUsed = true;
    }
    ApplySplitscreenCvars(localPlayers);
    UpdateDistanceFieldShadowMode(localPlayers);
    UpdateSplitscreenHUDScale(localPlayers);
    UpdateRadialTextRadius(localPlayers);
    if (g_setSplitscreenFlag) {
        *reinterpret_cast<bool*>(g_base + kRvaGIsSplitscreen) = localPlayers > 1;
    }
}

void SwapSplitscreenOrigins(void* viewportClient);  // swap screens, below
extern int32_t g_screensSwapped;

// The PvP respawn timer (AShooterGameMode::bIncreasePvPRespawnInterval, +0xE48, from the session settings) is
// switched off while 2+ players are in and restored afterwards ([Settings] SplitscreenNoPvPRespawnTimer=0 keeps it).
constexpr uintptr_t kRvaWorldGetAuthGameMode = 0x0025E4F0;
constexpr size_t kGameModePvPRespawnIntervalOffset = 0xE48;
bool g_noPvPRespawnTimer = true;
void* g_pvpTimerGameMode = nullptr;
uint8_t g_pvpTimerSaved = 0;

void UpdatePvPRespawnTimer(void* viewportClient, int32_t localPlayers) {
    if (!g_noPvPRespawnTimer || !viewportClient) {
        return;
    }
    using ObjectFn = void* (*)(const void* object);
    auto vtable = *reinterpret_cast<void* const* const*>(viewportClient);
    void* world = reinterpret_cast<ObjectFn>(vtable[0x108 / sizeof(void*)])(viewportClient);  // GetWorld
    void* gameMode = world ? reinterpret_cast<ObjectFn>(g_base + kRvaWorldGetAuthGameMode)(world) : nullptr;
    if (!gameMode) {
        return;
    }
    uint8_t* flag = static_cast<uint8_t*>(gameMode) + kGameModePvPRespawnIntervalOffset;
    if (localPlayers > 1 && g_pvpTimerGameMode != gameMode) {
        g_pvpTimerGameMode = gameMode;
        g_pvpTimerSaved = *flag;
        *flag = 0;
        Log("PvP respawn timer off for splitscreen (was %d)", g_pvpTimerSaved);
    } else if (localPlayers <= 1 && g_pvpTimerGameMode == gameMode) {
        *flag = g_pvpTimerSaved;
        g_pvpTimerGameMode = nullptr;
        Log("PvP respawn timer restored (%d)", g_pvpTimerSaved);
    }
}

void HookedNotifyPlayerAdded(void* viewportClient, int32_t playerIndex, void* localPlayer) {
    OnLocalPlayerCountChanged(g_localPlayerCount = g_localPlayerCount + 1);
    UpdatePvPRespawnTimer(viewportClient, g_localPlayerCount);
    g_originalNotifyPlayerAdded(viewportClient, playerIndex, localPlayer);
}

void HookedNotifyPlayerRemoved(void* viewportClient, int32_t playerIndex, void* localPlayer) {
    if (g_localPlayerCount > 0) {
        g_localPlayerCount = g_localPlayerCount - 1;
    }
    OnLocalPlayerCountChanged(g_localPlayerCount);
    UpdatePvPRespawnTimer(viewportClient, g_localPlayerCount);
    if (g_screensSwapped) {
        SwapSplitscreenOrigins(viewportClient);  // back to the normal layout before the game lays it out again
    }
    g_originalNotifyPlayerRemoved(viewportClient, playerIndex, localPlayer);
}

// ---- 7. Lighting --------------------------------------------------------------------------------------

void HookedClearCommandExecute(void* cmdList, uint8_t* cmd) {  // RHI/render thread
    if (g_fixLighting && g_localPlayerCount > 1) {
        int32_t rect[4];
        memcpy(rect, cmd + kClearExcludeRectOffset, sizeof(rect));
        if (rect[2] > rect[0] && rect[3] > rect[1]) {
            return;  // FRHICommandClear has a trivial destructor, so skipping Execute is enough
        }
    }
    g_originalClearCommandExecute(cmdList, cmd);
}

// The distance-field shadow of each view (flag bit 11) has no shadow map of its own, but after its ray-traced pass
// RenderProjection can carry on into a normal shadow-map projection that reads whatever the shared shadow-depth
// texture last held -- in splitscreen that is often the other player's cascade, which put player 1's shadows on
// player 2's screen (and the other way round when an inventory preview changed the render order). With the
// distance-field pass switched off in splitscreen (see UpdateDistanceFieldShadowMode) that projection is all that
// is left, so it's skipped.
constexpr size_t kShadowFlagsOffset = 0x594;       // FProjectedShadowInfo bitfield
constexpr uint32_t kShadowRayTracedDistanceField = 1u << 11;
using ShadowRenderProjectionFn = void (*)(void* shadow, void* cmdList, int32_t viewIndex, const void* view);
ShadowRenderProjectionFn g_originalShadowRenderProjection = nullptr;

// The main cause of player 2's wrong sun shadows: FProjectedShadowInfo::RenderDepth draws a shadow map from the
// point of view of the FIRST view the shadow is relevant to (it loops SceneRenderer->Views and never checks
// DependentView), overriding that view's matrices/uniform buffer for the pass. Player 2's cascades are usually
// relevant to player 1 too, so they were drawn as if from player 1's camera (and an inventory preview render, which
// comes first, did the same to player 1). For the duration of the call, the renderer's view list is narrowed to
// the shadow's own view.
constexpr size_t kShadowDependentViewOffset = 0x68;  // FProjectedShadowInfo::DependentView (FViewInfo*)
constexpr size_t kSceneRendererViewsOffset = 0x90;   // FSceneRenderer::Views (TArray<FViewInfo>: data, num, max)
constexpr size_t kViewInfoSize = 0x1C20;
using ShadowRenderDepthFn = void (*)(void* shadow, void* cmdList, void* sceneRenderer);
ShadowRenderDepthFn g_originalShadowRenderDepth = nullptr;

void HookedShadowRenderDepth(void* shadow, void* cmdList, void* sceneRenderer) {  // render thread
    if (g_fixShadows && g_localPlayerCount > 1) {
        uint8_t* dependentView =
            *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(shadow) + kShadowDependentViewOffset);
        auto viewsData = reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(sceneRenderer) + kSceneRendererViewsOffset);
        auto viewsNum = reinterpret_cast<int32_t*>(static_cast<uint8_t*>(sceneRenderer) + kSceneRendererViewsOffset + 8);
        uint8_t* data = *viewsData;
        int32_t num = *viewsNum;
        if (dependentView && num > 1 && dependentView >= data && dependentView < data + num * kViewInfoSize &&
            (dependentView - data) % kViewInfoSize == 0) {
            *viewsData = dependentView;
            *viewsNum = 1;
            g_originalShadowRenderDepth(shadow, cmdList, sceneRenderer);
            *viewsData = data;
            *viewsNum = num;
            return;
        }
    }
    g_originalShadowRenderDepth(shadow, cmdList, sceneRenderer);
}

void HookedShadowRenderProjection(void* shadow, void* cmdList, int32_t viewIndex, const void* view) {  // render thread
    if (g_dfShadowsOverridden) {
        uint32_t flags = *reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(shadow) + kShadowFlagsOffset);
        if (flags & kShadowRayTracedDistanceField) {
            return;
        }
    }
    g_originalShadowRenderProjection(shadow, cmdList, viewIndex, view);
}

// ---- 4. Save data -------------------------------------------------------------------------------------

int32_t SplitscreenIndexOf(void* controller) {  // -1 = not a splitscreen session
    if (!controller) {
        return -1;
    }
    int32_t index = -1;
    auto isSplitscreenPlayer = reinterpret_cast<IsSplitscreenPlayerFn>(g_base + kRvaIsSplitscreenPlayer);
    return isSplitscreenPlayer(controller, &index) ? index : -1;
}

bool LocalForPlayerData(void* controller, size_t slot) {
    auto vtable = *reinterpret_cast<void* const* const*>(controller);
    if (!reinterpret_cast<ControllerBoolFn>(vtable[slot / sizeof(void*)])(controller)) {
        return false;
    }
    return SplitscreenIndexOf(controller) <= 0;  // splitscreen players 2+ aren't "the" local player
}

bool PlayerDataIsLocalController(void* controller) {
    return LocalForPlayerData(controller, kVtIsLocalController);
}

bool PlayerDataIsLocalPlayerController(void* controller) {
    return LocalForPlayerData(controller, kVtIsLocalPlayerController);
}

// ---- 5. HUDs ------------------------------------------------------------------------------------------

// One InitUIScenes attempt per HUD every couple of seconds, so a HUD that can't be built yet isn't retried
// every frame.
struct HudAttempt {
    void* hud;
    ULONGLONG tick;
};
HudAttempt g_hudAttempts[8] = {};

bool ShouldAttemptHudBuild(void* hud) {
    constexpr ULONGLONG kRetryMs = 2000;
    ULONGLONG now = GetTickCount64();
    HudAttempt* oldest = &g_hudAttempts[0];
    for (HudAttempt& attempt : g_hudAttempts) {
        if (attempt.hud == hud) {
            if (now - attempt.tick < kRetryMs) {
                return false;
            }
            attempt.tick = now;
            return true;
        }
        if (attempt.tick < oldest->tick) {
            oldest = &attempt;
        }
    }
    *oldest = {hud, now};
    return true;
}

// Testing aid: [ToggleKeys] F5=r.Name,value in the ini. First press sets value, second restores.
struct ToggleKey {
    int vk;
    wchar_t name[96];
    wchar_t value[32];
    wchar_t previous[32];
    bool on;
    bool wasDown;
};
ToggleKey g_toggles[8] = {};
size_t g_toggleCount = 0;

void LoadToggleKeys() {
    for (int f = 1; f <= 12 && g_toggleCount < 8; ++f) {
        wchar_t key[8], line[160] = {};
        swprintf_s(key, L"F%d", f);
        GetPrivateProfileStringW(L"ToggleKeys", key, L"", line, 160, g_iniPath);
        const wchar_t* comma = wcschr(line, L',');
        if (!comma) {
            continue;
        }
        ToggleKey& t = g_toggles[g_toggleCount++];
        t.vk = VK_F1 + f - 1;
        wcsncpy_s(t.name, line, static_cast<size_t>(comma - line));
        wcscpy_s(t.value, comma + 1);
        Log("toggle key F%d: %ls=%ls", f, t.name, t.value);
    }
}

void PollToggleKeys() {
    for (size_t i = 0; i < g_toggleCount; ++i) {
        ToggleKey& t = g_toggles[i];
        bool down = (GetAsyncKeyState(t.vk) & 0x8000) != 0;
        if (down && !t.wasDown) {
            if (!t.on) {
                float previous = 0;
                if (!GetCvar(t.name, &previous)) {
                    Log("toggle: cvar %ls not found", t.name);
                } else {
                    swprintf_s(t.previous, L"%g", previous);
                    SetCvar(t.name, t.value);
                    t.on = true;
                    Log("toggle F%d ON: %ls=%ls (was %ls)", t.vk - VK_F1 + 1, t.name, t.value, t.previous);
                }
            } else {
                SetCvar(t.name, t.previous);
                t.on = false;
                Log("toggle F%d OFF: %ls=%ls", t.vk - VK_F1 + 1, t.name, t.previous);
            }
        }
        t.wasDown = down;
    }
}

// Players couldn't target each other ("hold X to invite to tribe" never appeared): the use trace runs on collision
// channel 26 (APrimalCharacter::GetAimedActor from AShooterPlayerController::GetAimedUseActor), and a LOCALLY
// controlled character's mesh ignores that channel (so your own body never blocks your aim). In splitscreen both
// characters are local, so each was invisible to the other's use trace. While 2+ players are in, every player's mesh
// blocks channel 26 again; each trace still skips its own character (AddIgnoredActor(self)).
constexpr size_t kCharacterMeshOffset = 0x508;                      // ACharacter::Mesh
constexpr uintptr_t kRvaSetCollisionResponseToChannel = 0x02AE5060;  // UPrimitiveComponent::...(channel, response)
constexpr uintptr_t kRvaGetCollisionResponseToChannel = 0x02AE54A0;
constexpr int32_t kUseTraceChannel = 26;
constexpr uint8_t kResponseBlock = 2;

void LetOtherPlayersTarget(void* controller) {
    if (g_localPlayerCount < 2 || !controller) {
        return;
    }
    using GetPlayerCharacterFn = void* (*)(const void* controller);
    void* character = reinterpret_cast<GetPlayerCharacterFn>(g_base + 0x0109D860)(controller);
    void* mesh = character ? *reinterpret_cast<void**>(static_cast<uint8_t*>(character) + kCharacterMeshOffset) : nullptr;
    if (!mesh) {
        return;
    }
    using GetResponseFn = uint8_t (*)(const void* component, int32_t channel);
    using SetResponseFn = void (*)(void* component, int32_t channel, uint8_t response);
    if (reinterpret_cast<GetResponseFn>(g_base + kRvaGetCollisionResponseToChannel)(mesh, kUseTraceChannel) !=
        kResponseBlock) {
        reinterpret_cast<SetResponseFn>(g_base + kRvaSetCollisionResponseToChannel)(mesh, kUseTraceChannel,
                                                                                   kResponseBlock);
        Log("player %d's character can now be targeted by the other player", SplitscreenIndexOf(controller) + 1);
    }
}

void HookedDrawHUD(void* hud) {
    PollToggleKeys();
    LetOtherPlayersTarget(reinterpret_cast<HudGetPlayerOwnerFn>(g_base + kRvaHudGetPlayerOwner)(hud));
    bool built = *(static_cast<uint8_t*>(hud) + kHudUIScenesBuiltOffset) != 0;
    if (g_rebuildHUDs && g_splitscreenUsed && !built) {
        void* controller = reinterpret_cast<HudGetPlayerOwnerFn>(g_base + kRvaHudGetPlayerOwner)(hud);
        if (controller && ShouldAttemptHudBuild(hud)) {
            Log("building HUD for splitscreen player %d (%d local players)", SplitscreenIndexOf(controller) + 1,
                g_localPlayerCount);
            reinterpret_cast<InitUIScenesFn>(g_base + kRvaInitUIScenes)(hud, false);
        }
    }
    g_originalDrawHUD(hud);
}

// ---- Patching helpers ---------------------------------------------------------------------------------

void WriteAbsJmp(uint8_t* at, const void* target) {
    at[0] = 0xFF;
    at[1] = 0x25;
    memset(at + 2, 0, 4);
    memcpy(at + 6, &target, sizeof(target));
}

bool WriteCode(void* at, const void* bytes, size_t size) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    memcpy(at, bytes, size);
    VirtualProtect(at, size, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return true;
}

// Memory within ±2 GB of the exe, so a 5-byte `call rel32` from the game's code can reach it.
uint8_t* AllocNearImage(size_t size) {
    constexpr uintptr_t kGranularity = 0x10000;
    constexpr uintptr_t kRange = 0x7FF00000;
    uintptr_t start = (g_base > kRange ? g_base - kRange : kGranularity) & ~(kGranularity - 1);
    for (uintptr_t addr = g_base - kGranularity; addr >= start && addr < g_base; addr -= kGranularity) {
        void* p = VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p) {
            return static_cast<uint8_t*>(p);
        }
    }
    return nullptr;
}

struct HookSpec {
    const char* name;
    uintptr_t rva;
    const uint8_t* prologue;
    size_t prologueSize;
    const void* detour;
    void** original;
};

// Checks every site first, then patches all of them; nothing is patched unless every site matches.
bool InstallHooks(const HookSpec* hooks, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (memcmp(reinterpret_cast<const void*>(g_base + hooks[i].rva), hooks[i].prologue, hooks[i].prologueSize) != 0) {
            Log("%s: bytes don't match this build of ShooterGame.exe; skipped", hooks[i].name);
            return false;
        }
    }
    // Trampolines: the relocated prologue, then a jump back to the rest of the original function.
    constexpr size_t kTrampolineSize = 64;
    auto trampolines = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kTrampolineSize * count, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampolines) {
        Log("VirtualAlloc failed (%lu); not hooking", GetLastError());
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        const HookSpec& hook = hooks[i];
        auto site = reinterpret_cast<uint8_t*>(g_base + hook.rva);
        uint8_t* trampoline = trampolines + kTrampolineSize * i;
        memcpy(trampoline, hook.prologue, hook.prologueSize);
        WriteAbsJmp(trampoline + hook.prologueSize, site + hook.prologueSize);
        *hook.original = trampoline;

        uint8_t patch[32];
        WriteAbsJmp(patch, hook.detour);
        memset(patch + kAbsJmpSize, 0x90, hook.prologueSize - kAbsJmpSize);
        if (WriteCode(site, patch, hook.prologueSize)) {
            Log("hooked %s", hook.name);
        }
    }
    return true;
}

// 4. Replaces each `call [rax+slot]` with `call stub; nop`; the stub is `mov rax, detour; jmp rax`
// (rax only held the vtable, and is the return register anyway).
void PatchLocalCheckSites() {
    constexpr size_t count = sizeof(kLocalCheckSites) / sizeof(kLocalCheckSites[0]);
    for (const LocalCheckSite& site : kLocalCheckSites) {
        const uint8_t expected[kCallSiteSize] = {0xFF, 0x90, static_cast<uint8_t>(site.slot & 0xFF),
                                                 static_cast<uint8_t>(site.slot >> 8), 0x00, 0x00};
        if (memcmp(reinterpret_cast<const void*>(g_base + site.rva), expected, kCallSiteSize) != 0) {
            Log("%s: bytes don't match this build; save-data fix skipped", site.where);
            return;
        }
    }
    constexpr size_t kStubSize = 16;
    uint8_t* stubs = AllocNearImage(kStubSize * count);
    if (!stubs) {
        Log("couldn't allocate memory near the exe; save-data fix skipped");
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        const LocalCheckSite& site = kLocalCheckSites[i];
        const void* detour = site.slot == kVtIsLocalController
                                 ? reinterpret_cast<const void*>(&PlayerDataIsLocalController)
                                 : reinterpret_cast<const void*>(&PlayerDataIsLocalPlayerController);
        uint8_t* stub = stubs + kStubSize * i;
        stub[0] = 0x48;  // mov rax, imm64
        stub[1] = 0xB8;
        memcpy(stub + 2, &detour, sizeof(detour));
        stub[10] = 0xFF;  // jmp rax
        stub[11] = 0xE0;

        auto at = reinterpret_cast<uint8_t*>(g_base + site.rva);
        int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(stub) - reinterpret_cast<intptr_t>(at + 5));
        uint8_t patch[kCallSiteSize] = {0xE8, 0, 0, 0, 0, 0x90};  // call rel32; nop
        memcpy(patch + 1, &rel, sizeof(rel));
        if (WriteCode(at, patch, kCallSiteSize)) {
            Log("patched %s", site.where);
        }
    }
}

// 6. Points the inventory check's `call GetUISceneFromClass` at a stub that passes the character's controller.
void PatchInventoryCheck() {
    const uint8_t xorR8[] = {0x45, 0x33, 0xC0};
    const uint8_t loadCharacter[] = {0x48, 0x8B, 0x84, 0x24, 0x10, 0x39, 0x00, 0x00};
    auto call = reinterpret_cast<uint8_t*>(g_base + kRvaTickInventoryCall);
    int32_t rel = 0;
    memcpy(&rel, call + 1, sizeof(rel));
    if (memcmp(reinterpret_cast<const void*>(g_base + kRvaTickInventoryNullController), xorR8, sizeof(xorR8)) != 0 ||
        memcmp(reinterpret_cast<const void*>(g_base + kRvaTickCharacterLoad), loadCharacter, sizeof(loadCharacter)) != 0 ||
        call[0] != 0xE8 || reinterpret_cast<uintptr_t>(call + 5) + rel != g_base + kRvaGetUISceneFromClass) {
        Log("inventory check site doesn't match this build; inventory fix skipped");
        return;
    }
    uint8_t* stub = AllocNearImage(64);
    if (!stub) {
        Log("couldn't allocate memory near the exe; inventory fix skipped");
        return;
    }
    // On entry rsp = caller's rsp - 8 (return address), so the caller's [rsp+3910h] is [rsp+3918h].
    size_t n = 0;
    auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) {
            stub[n++] = b;
        }
    };
    const uint32_t characterSlot = kTickCharacterStackOffset + 8;
    emit({0x4C, 0x8B, 0x84, 0x24});  // mov r8, [rsp + characterSlot]
    emit({static_cast<uint8_t>(characterSlot), static_cast<uint8_t>(characterSlot >> 8),
          static_cast<uint8_t>(characterSlot >> 16), static_cast<uint8_t>(characterSlot >> 24)});
    emit({0x4D, 0x8B, 0x80});  // mov r8, [r8 + Controller]
    emit({static_cast<uint8_t>(kPawnControllerOffset), static_cast<uint8_t>(kPawnControllerOffset >> 8), 0x00, 0x00});
    emit({0x4D, 0x85, 0xC0});  // test r8, r8
    emit({0x75, 0x03});        // jnz +3
    emit({0x31, 0xC0});        // xor eax, eax   (no controller: no inventory open)
    emit({0xC3});              // ret
    emit({0x48, 0xB8});        // mov rax, GetUISceneFromClass
    uintptr_t target = g_base + kRvaGetUISceneFromClass;
    memcpy(stub + n, &target, sizeof(target));
    n += sizeof(target);
    emit({0xFF, 0xE0});  // jmp rax

    int32_t newRel = static_cast<int32_t>(reinterpret_cast<intptr_t>(stub) - reinterpret_cast<intptr_t>(call + 5));
    if (WriteCode(call + 1, &newRel, sizeof(newRel))) {
        Log("patched AShooterCharacter::Tick inventory check (per-player)");
    }
}

// 8. Re-points the radial menu's splitscreen text-scale load at our own float (the original constant is shared).
void PatchRadialTextScale() {
    if (g_radialTextScale <= 0.0f) {
        return;
    }
    auto site = reinterpret_cast<uint8_t*>(g_base + kRvaRadialSplitscreenTextScaleLoad);
    int32_t disp = 0;
    memcpy(&disp, site + 4, sizeof(disp));
    uint32_t constantBits = 0;
    if (site[0] != 0xF3 || site[1] != 0x0F || site[2] != 0x10 || site[3] != 0x05) {
        Log("radial menu site doesn't match this build; radial text fix skipped");
        return;
    }
    memcpy(&constantBits, site + 8 + disp, sizeof(constantBits));
    if (constantBits != kRadialSplitscreenTextScaleBits) {
        Log("radial menu text scale isn't 0.7 in this build; radial text fix skipped");
        return;
    }
    auto value = reinterpret_cast<float*>(AllocNearImage(sizeof(float)));
    if (!value) {
        Log("couldn't allocate memory near the exe; radial text fix skipped");
        return;
    }
    *value = g_radialTextScale;
    int32_t newDisp = static_cast<int32_t>(reinterpret_cast<intptr_t>(value) - reinterpret_cast<intptr_t>(site + 8));
    if (WriteCode(site + 4, &newDisp, sizeof(newDisp))) {
        Log("radial menu splitscreen text scale: %.2f (game default 0.70)", g_radialTextScale);
    }
}

bool RepointFloatLoad(uintptr_t rva, uint32_t expectedBits, float value, const char* what);

// The player-action radial menu (whistles, emotes) has its own splitscreen text scale load: 0.7 at 00C60BF5.
void PatchPlayerActionRadialTextScale() {
    if (g_radialTextScale > 0.0f &&
        RepointFloatLoad(0x00C60BF5, kRadialSplitscreenTextScaleBits, g_radialTextScale, "radial (player-action) text")) {
        Log("radial menu (player-action) splitscreen text scale: %.2f (game default 0.70)", g_radialTextScale);
    }
}

// Repoints a `movss xmm0, [rip+X]` constant load (F3 0F 10 05 disp32) at our own float. Checks the instruction
// and the original constant first.
bool RepointFloatLoad(uintptr_t rva, uint32_t expectedBits, float value, const char* what) {
    auto site = reinterpret_cast<uint8_t*>(g_base + rva);
    if (site[0] != 0xF3 || site[1] != 0x0F || site[2] != 0x10 || site[3] != 0x05) {
        Log("%s: instruction doesn't match this build; skipped", what);
        return false;
    }
    int32_t disp = 0;
    memcpy(&disp, site + 4, sizeof(disp));
    uint32_t constantBits = 0;
    memcpy(&constantBits, site + 8 + disp, sizeof(constantBits));
    if (constantBits != expectedBits) {
        Log("%s: constant doesn't match this build; skipped", what);
        return false;
    }
    auto slot = reinterpret_cast<float*>(AllocNearImage(sizeof(float)));
    if (!slot) {
        Log("%s: couldn't allocate memory near the exe; skipped", what);
        return false;
    }
    *slot = value;
    int32_t newDisp = static_cast<int32_t>(reinterpret_cast<intptr_t>(slot) - reinterpret_cast<intptr_t>(site + 8));
    return WriteCode(site + 4, &newDisp, sizeof(newDisp));
}

// Radial menu label distance. Two radial menus: URadialSelectorHUD_MultiUse (hold-E on objects) and
// URadialSelectorHUD_PlayerAction (whistles/emotes). Each offsets a label from the centre by (radius x direction x
// 0xB8 scale) x a local ring scale -- MultiUse [rsp+0F4h] = 0.85, or 0.63 in splitscreen; PlayerAction [rsp+0CCh]
// = 0.85, or 0.72 in splitscreen. In splitscreen the labels sit outside the ring, so the two
// `mulss xmm0, [rsp+local]` per menu that apply the ring scale to the label offset are rewritten to
// `mulss xmm0, [rip+ours]; nop`, where ours = 0.85 normally and the splitscreen value x SplitscreenRadialTextDistance
// while 2+ players are in.
struct RadialLabelSites {
    const char* menu;
    uint8_t local;          // rsp displacement of the ring-scale local
    uintptr_t sites[2];
    float splitscreenScale;
    float* value;
};
RadialLabelSites g_radialLabelSites[] = {
    {"multi-use", 0xF4, {0x00C561D4, 0x00C561EF}, 0.63f, nullptr},
    {"player-action", 0xCC, {0x00C63065, 0x00C63080}, 0.72f, nullptr},
};

void UpdateRadialTextRadius(int32_t localPlayers) {
    for (RadialLabelSites& menu : g_radialLabelSites) {
        if (menu.value) {
            *menu.value = localPlayers > 1 ? menu.splitscreenScale * g_radialTextDistance : 0.85f;
        }
    }
}

void PatchRadialTextRadius() {
    if (g_radialTextDistance == 1.0f) {
        return;
    }
    for (RadialLabelSites& menu : g_radialLabelSites) {
        const uint8_t expected[] = {0xF3, 0x0F, 0x59, 0x84, 0x24, menu.local, 0x00, 0x00, 0x00};  // mulss xmm0,[rsp+X]
        bool match = true;
        for (uintptr_t rva : menu.sites) {
            match = match && memcmp(reinterpret_cast<const void*>(g_base + rva), expected, sizeof(expected)) == 0;
        }
        if (!match) {
            Log("radial label distance (%s): sites don't match this build; skipped", menu.menu);
            continue;
        }
        menu.value = reinterpret_cast<float*>(AllocNearImage(sizeof(float)));
        if (!menu.value) {
            continue;
        }
        for (uintptr_t rva : menu.sites) {
            auto site = reinterpret_cast<uint8_t*>(g_base + rva);
            int32_t disp = static_cast<int32_t>(reinterpret_cast<intptr_t>(menu.value) - reinterpret_cast<intptr_t>(site + 8));
            uint8_t patch[9] = {0xF3, 0x0F, 0x59, 0x05, 0, 0, 0, 0, 0x90};  // mulss xmm0, [rip+disp]; nop
            memcpy(patch + 4, &disp, sizeof(disp));
            WriteCode(site, patch, sizeof(patch));
        }
        Log("radial menu (%s) splitscreen label distance: x%.2f", menu.menu, g_radialTextDistance);
    }
    UpdateRadialTextRadius(g_localPlayerCount);
}

// Toolbar (item slots, UPlayerHUDUI+0x588 -- the widget UpdateItemSlotVisibility shows/hides). The game's own
// splitscreen scaling (UPlayerHUDUI::ProcessSplitscreenScaling) doesn't touch it, so after it runs the item-slot
// widget's render scale is set to its original scale x SplitscreenToolbarScale, pivoting on its bottom centre so it
// grows upwards. Originals are remembered per widget so repeated calls don't compound.
constexpr uintptr_t kRvaPlayerHUDUpdateItemSlotVisibility = 0x003ADCC0;  // UPlayerHUDUI, called every HUD tick
constexpr uintptr_t kRvaWidgetSetRenderScale = 0x0273C410;       // UWidget::SetRenderScale(FVector2D)
constexpr uintptr_t kRvaWidgetSetRenderTransformPivot = 0x0273C7B0;
constexpr size_t kPlayerHUDItemSlotsOffset = 0x588;
constexpr size_t kPlayerHUDItemSlotsHiddenOffset = 0x640;     // bool: UpdateItemSlotVisibility hides the slots
constexpr size_t kPlayerHUDItemSlotPanelChildOffset = 0x5D8;  // "ItemSlotPanelChild" (game scales it 1x2 in splitscreen)
constexpr size_t kPlayerHUDGamepadButtonsOffset = 0x5F8;  // "GamepadQuickbarButtons" canvas panel (controller icons)
constexpr size_t kWidgetRenderScaleOffset = 0x100;               // UWidget::RenderTransform.Scale
using ProcessSplitscreenScalingFn = void (*)(void* ui, bool unused);  // UpdateItemSlotVisibility(this)
using WidgetSetVector2DFn = void (*)(void* widget, uint64_t packedVector2D);
ProcessSplitscreenScalingFn g_originalPlayerHUDProcessSplitscreenScaling = nullptr;

uint64_t PackVector2D(float x, float y) {
    uint64_t packed = 0;
    memcpy(&packed, &x, 4);
    memcpy(reinterpret_cast<uint8_t*>(&packed) + 4, &y, 4);
    return packed;
}

// The item slots and the controller-button icons both sit inside "ItemSlotPanelChild" (UPlayerHUDUI+0x5D8, the
// toolbar container), which the game sizes/positions itself (UpdateQuickBarScaling, every tick before
// UpdateItemSlotVisibility). Only that container is scaled, so everything in it stays lined up. The game's own
// value is remembered and re-read whenever the game changes it, so the multiplier never compounds.
struct ToolbarScaleState {
    void* widget;
    float gameScale[2];  // the scale the game set
    float ourScale[2];   // what we last set
};
ToolbarScaleState g_toolbarStates[4] = {};
size_t g_toolbarStateNext = 0;
bool g_toolbarHidden = false;  // UPlayerHUDUI+0x640: the HUD's item slots are hidden (inventory open)

void ScaleToolbarContainer(void* widget) {
    auto scaleField = reinterpret_cast<float*>(static_cast<uint8_t*>(widget) + kWidgetRenderScaleOffset);
    ToolbarScaleState* state = nullptr;
    for (ToolbarScaleState& s : g_toolbarStates) {
        if (s.widget == widget) {
            state = &s;
            break;
        }
    }
    if (!state) {
        state = &g_toolbarStates[g_toolbarStateNext++ % (sizeof(g_toolbarStates) / sizeof(g_toolbarStates[0]))];
        state->widget = widget;
        memcpy(state->gameScale, scaleField, sizeof(state->gameScale));
        memcpy(state->ourScale, scaleField, sizeof(state->ourScale));
        Log("toolbar: container %p, game scale (%.2f, %.2f)", widget, scaleField[0], scaleField[1]);
    } else if (scaleField[0] != state->ourScale[0] || scaleField[1] != state->ourScale[1]) {
        memcpy(state->gameScale, scaleField, sizeof(state->gameScale));  // the game set a new scale
    }
    // While the inventory is open the game shows its own normal-size slots, so the toolbar goes back to normal.
    float k = g_localPlayerCount > 1 && !g_toolbarHidden ? g_toolbarScale : 1.0f;
    float want[2] = {state->gameScale[0] * k, state->gameScale[1] * k};
    if (scaleField[0] == want[0] && scaleField[1] == want[1]) {
        return;
    }
    reinterpret_cast<WidgetSetVector2DFn>(g_base + kRvaWidgetSetRenderTransformPivot)(widget, PackVector2D(0.5f, 1.0f));
    reinterpret_cast<WidgetSetVector2DFn>(g_base + kRvaWidgetSetRenderScale)(widget, PackVector2D(want[0], want[1]));
    memcpy(state->ourScale, want, sizeof(state->ourScale));
}

void HookedPlayerHUDProcessSplitscreenScaling(void* ui) {  // UpdateItemSlotVisibility(this), every HUD tick
    g_originalPlayerHUDProcessSplitscreenScaling(ui, false);
    if (g_toolbarScale == 1.0f) {
        return;
    }
    g_toolbarHidden = *(static_cast<uint8_t*>(ui) + kPlayerHUDItemSlotsHiddenOffset) != 0;
    if (void* container = *reinterpret_cast<void**>(static_cast<uint8_t*>(ui) + kPlayerHUDItemSlotPanelChildOffset)) {
        ScaleToolbarContainer(container);
    }
}

// Controller hints for players 2+: the viewport client computes ONE "gamepad active" value from a single player
// and broadcasts it to every UI (CheckGamepadActiveChanged), so a keyboard player 1 turned off player 2's
// controller hints. Players 2+ can only join with a controller, so while splitscreen is active their controller
// always counts as active (FSlateApplication::GetControllerActive, read per ControllerId) and their UIs always get
// "true" from the broadcast.
constexpr uintptr_t kRvaGetControllerActive = 0x022E6570;          // FSlateApplication::GetControllerActive(int)
constexpr uintptr_t kRvaPrimalUICheckGamepadActive = 0x00C05A90;   // UPrimalUI::CheckGamepadActiveChanged(bool)
constexpr uintptr_t kRvaHubCheckGamepadActive = 0x0141D460;        // UUI_Hub::CheckGamepadActiveChanged(bool)
constexpr uintptr_t kRvaGetOwningLocalPlayer = 0x02716410;         // UUserWidget::GetOwningLocalPlayer
using GetControllerActiveFn = bool (*)(void* app, int32_t controllerId);
using CheckGamepadActiveFn = void (*)(void* ui, bool active);
GetControllerActiveFn g_originalGetControllerActive = nullptr;
CheckGamepadActiveFn g_originalPrimalUICheckGamepadActive = nullptr;
CheckGamepadActiveFn g_originalHubCheckGamepadActive = nullptr;

bool HookedGetControllerActive(void* app, int32_t controllerId) {
    if (controllerId > 0 && g_localPlayerCount > 1) {
        return true;
    }
    return g_originalGetControllerActive(app, controllerId);
}

bool UIOwnedBySecondaryPlayer(void* ui) {
    if (g_localPlayerCount < 2) {
        return false;
    }
    using GetOwningLocalPlayerFn = void* (*)(const void* widget);
    void* localPlayer = reinterpret_cast<GetOwningLocalPlayerFn>(g_base + kRvaGetOwningLocalPlayer)(ui);
    return localPlayer && *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(localPlayer) + kLocalPlayerControllerIdOffset) > 0;
}

void HookedPrimalUICheckGamepadActive(void* ui, bool active) {
    g_originalPrimalUICheckGamepadActive(ui, active || UIOwnedBySecondaryPlayer(ui));
}

void HookedHubCheckGamepadActive(void* ui, bool active) {
    g_originalHubCheckGamepadActive(ui, active || UIOwnedBySecondaryPlayer(ui));
}

// Camera mode: AShooterCharacter::SetCameraMode saves the choice into the shared GameUserSettings (+0x148 first
// person riding, +0x149 third person), and every local character's Tick applies +0x149 -- so player 2 toggling
// third person also switched player 1. For players 2+ the settings bytes are put back after the call, so the
// saved preference stays player 1's.
constexpr uintptr_t kRvaSetCameraMode = 0x00CD36E0;  // AShooterCharacter::SetCameraMode(bool,bool,bool)
constexpr size_t kUserSettingsCameraModeOffset = 0x148;  // 2 bytes: +0x148, +0x149
using SetCameraModeFn = void (*)(void* character, bool a, bool b, bool c);
SetCameraModeFn g_originalSetCameraMode = nullptr;

// The camera-mode bytes of the shared settings, or nullptr when the character belongs to player 1 / no splitscreen.
uint8_t* SecondaryPlayerCameraBytes(void* character) {
    void* controller = *reinterpret_cast<void**>(static_cast<uint8_t*>(character) + kPawnControllerOffset);
    if (g_localPlayerCount < 2 || SplitscreenIndexOf(controller) <= 0) {
        return nullptr;
    }
    float* uiScale = UserSettingsUIScale();  // settings+0x13C; the camera bytes are in the same object
    return uiScale ? reinterpret_cast<uint8_t*>(uiScale) - kUserSettingsUIScaleOffset + kUserSettingsCameraModeOffset
                   : nullptr;
}

// Players 2+ get their own copy of the camera-mode setting (+0x148 first-person riding, +0x149 third person):
// their toggles are saved here and the shared settings are put back to player 1's choice. Starts as a copy of
// player 1's on the first use.
uint8_t g_secondaryCameraSettings[0x200] = {};
bool g_secondaryCameraInitialized = false;

void InitSecondaryCameraSettings(const uint8_t* sharedCameraBytes) {
    if (!g_secondaryCameraInitialized) {
        g_secondaryCameraSettings[kUserSettingsCameraModeOffset] = sharedCameraBytes[0];
        g_secondaryCameraSettings[kUserSettingsCameraModeOffset + 1] = sharedCameraBytes[1];
        g_secondaryCameraInitialized = true;
    }
}

template <typename Call>
void KeepSharedCameraMode(void* character, Call call) {
    uint8_t* cameraBytes = SecondaryPlayerCameraBytes(character);
    if (!cameraBytes) {
        call();
        return;
    }
    InitSecondaryCameraSettings(cameraBytes);
    uint8_t saved[2] = {cameraBytes[0], cameraBytes[1]};
    cameraBytes[0] = g_secondaryCameraSettings[kUserSettingsCameraModeOffset];  // the call sees player 2's setting
    cameraBytes[1] = g_secondaryCameraSettings[kUserSettingsCameraModeOffset + 1];
    call();
    g_secondaryCameraSettings[kUserSettingsCameraModeOffset] = cameraBytes[0];  // and saves into it
    g_secondaryCameraSettings[kUserSettingsCameraModeOffset + 1] = cameraBytes[1];
    cameraBytes[0] = saved[0];
    cameraBytes[1] = saved[1];
}

void HookedSetCameraMode(void* character, bool a, bool b, bool c) {
    KeepSharedCameraMode(character, [&] { g_originalSetCameraMode(character, a, b, c); });
}
// AShooterCharacter::ZoomIn / ZoomOut are what the third-person toggle actually uses (+0x149 = 0 / 1, then save).
constexpr uintptr_t kRvaCharacterZoomIn = 0x00C9F700;
constexpr uintptr_t kRvaCharacterZoomOut = 0x00C9F990;
using CharacterVoidFn = void (*)(void* character);
CharacterVoidFn g_originalZoomIn = nullptr;
CharacterVoidFn g_originalZoomOut = nullptr;

// Slate turns controller trigger presses into mouse-wheel events (FSlateApplication::OnControllerAnalog ->
// OnMouseWheel), and when no widget takes the wheel, FSceneViewport::OnMouseWheel hands it to the game as controller
// 0's MouseScrollUp/Down -- player 1's camera zoom. So player 2 pressing LT in their inventory knocked player 1 out of
// third person. While OnControllerAnalog runs for a controller, a scroll reaching UShooterGameViewportClient::InputKey
// goes to that controller instead of controller 0 (as in single player, where it reaches the player whose inventory
// is open). A real mouse wheel doesn't come through OnControllerAnalog and is untouched.
constexpr uintptr_t kRvaViewportInputKey = 0x00FC6410;
constexpr uintptr_t kRvaSlateOnControllerAnalog = 0x022E5C20;  // FSlateApplication::OnControllerAnalog(FKey, int, float)
constexpr uintptr_t kRvaKeyMouseScrollUp = 0x049BFFE8;          // EKeys::MouseScrollUp (FKey; FName first)
constexpr uintptr_t kRvaKeyMouseScrollDown = 0x049C0000;        // EKeys::MouseScrollDown
using InputKeyFn = bool (*)(void* client, void* viewport, int32_t controllerId, const void* key, int32_t event,
                            float amount, bool gamepad);
using ControllerAnalogFn = bool (*)(void* app, const void* key, int32_t controllerId, float value);
InputKeyFn g_originalInputKey = nullptr;
ControllerAnalogFn g_originalControllerAnalog = nullptr;
int32_t g_analogControllerId = -1;  // controller whose analog input Slate is handling right now

bool HookedControllerAnalog(void* app, const void* key, int32_t controllerId, float value) {
    int32_t previous = g_analogControllerId;
    g_analogControllerId = controllerId;
    bool result = g_originalControllerAnalog(app, key, controllerId, value);
    g_analogControllerId = previous;
    return result;
}

bool HookedInputKey(void* client, void* viewport, int32_t controllerId, const void* key, int32_t event, float amount,
                    bool gamepad) {
    if (controllerId == 0 && g_analogControllerId > 0 && key &&
        (memcmp(key, reinterpret_cast<const void*>(g_base + kRvaKeyMouseScrollUp), 8) == 0 ||
         memcmp(key, reinterpret_cast<const void*>(g_base + kRvaKeyMouseScrollDown), 8) == 0)) {
        controllerId = g_analogControllerId;
    }
    return g_originalInputKey(client, viewport, controllerId, key, event, amount, gamepad);
}
void HookedZoomIn(void* character) {
    KeepSharedCameraMode(character, [&] { g_originalZoomIn(character); });
}

void HookedZoomOut(void* character) {
    KeepSharedCameraMode(character, [&] { g_originalZoomOut(character); });
}

// AShooterCharacter::Tick forces third person on the local character whenever the shared settings say so
// (`call GetGameUserSettings` at 00CBEC04, then reads +0x149) -- so player 1's toggle also switched player 2. For
// players 2+ the call returns their own copy (g_secondaryCameraSettings) instead.
constexpr uintptr_t kRvaTickCameraSettingsCall = 0x00CBEC04;

void* TickCameraSettings(void* engine, void* character) {
    if (uint8_t* shared = SecondaryPlayerCameraBytes(character)) {
        InitSecondaryCameraSettings(shared);
        return g_secondaryCameraSettings;
    }
    using GetGameUserSettingsFn = void* (*)(void* engine);
    return reinterpret_cast<GetGameUserSettingsFn>(g_base + kRvaGetGameUserSettings)(engine);
}

void PatchTickCameraSettings() {
    auto call = reinterpret_cast<uint8_t*>(g_base + kRvaTickCameraSettingsCall);
    int32_t rel = 0;
    memcpy(&rel, call + 1, sizeof(rel));
    const uint8_t readThirdPerson[] = {0x0F, 0xB6, 0x80, 0x49, 0x01, 0x00, 0x00};  // movzx eax, byte [rax+149h]
    if (call[0] != 0xE8 || reinterpret_cast<uintptr_t>(call + 5) + rel != g_base + kRvaGetGameUserSettings ||
        memcmp(call + 5, readThirdPerson, sizeof(readThirdPerson)) != 0) {
        Log("character camera tick site doesn't match this build; skipped");
        return;
    }
    uint8_t* stub = AllocNearImage(32);
    if (!stub) {
        return;
    }
    // On entry rsp = caller's rsp - 8, so the caller's [rsp+3910h] (this character) is [rsp+3918h].
    const uint8_t loadCharacter[] = {0x48, 0x8B, 0x94, 0x24, 0x18, 0x39, 0x00, 0x00};  // mov rdx, [rsp+3918h]
    memcpy(stub, loadCharacter, sizeof(loadCharacter));
    stub[8] = 0x48;  // mov rax, imm64
    stub[9] = 0xB8;
    const void* target = reinterpret_cast<const void*>(&TickCameraSettings);
    memcpy(stub + 10, &target, sizeof(target));
    stub[18] = 0xFF;  // jmp rax
    stub[19] = 0xE0;
    int32_t newRel = static_cast<int32_t>(reinterpret_cast<intptr_t>(stub) - reinterpret_cast<intptr_t>(call + 5));
    if (WriteCode(call + 1, &newRel, sizeof(newRel))) {
        Log("patched AShooterCharacter::Tick camera-mode sync (per-player)");
    }
}

// Swap screens (player 2's pause menu): players 1 and 2 trade halves. The halves come from
// UGameViewportClient::SplitscreenInfo (+0x70: per split type, an array of {SizeX, SizeY, OriginX, OriginY}). The HUD
// pieces placed by half don't follow that by themselves, so everything that picks a half is mirrored while swapped:
// - `half = (owner == first local player) ? 0 : 1` in the UMG HUD widgets (UPrimalUI::ProcessSplitscreenScaling),
//   the radial/whistle menus, the harvest list (ShowInfoItems) and DrawUIHUD: the 0 and 1 are exchanged.
// - UHUDOVerlayWidget::DrawWidget clips each player's canvas drawing to (height / players) * IsSplitscreenPlayer
//   index; a stub at 00280BBB xors that index. (Flipping only one of these two hides the menus.)
// Then UShooterGameViewportClient::ReAdjustUI (what a player joining runs) rebuilds and repositions the HUDs.
constexpr uintptr_t kRvaLayoutPlayers = 0x02995ED0;   // UGameViewportClient::LayoutPlayers
constexpr uintptr_t kRvaReAdjustUI = 0x00FC3F60;      // UShooterGameViewportClient::ReAdjustUI
constexpr uintptr_t kRvaWorldGetGameViewport = 0x02C62E50;
int32_t g_screensSwapped = 0;

struct HalfIndexSite {
    uintptr_t rva;  // the jne of: jne +N / mov dword [rsp+X], first / jmp +M / mov dword [rsp+X], other
    bool disp32;    // [rsp+disp32] (C7 84 24) rather than [rsp+disp8] (C7 44 24)
    bool mirror;    // swapped: exchange the values (false: both players get the "first player" value)
    int32_t first, other;  // original values, read at startup
};
HalfIndexSite g_halfIndexSites[] = {
    {0x00C4D5F4, true, true},   // URadialSelectorHUD::DrawHUD
    {0x00C5006C, true, true},   // URadialSelectorHUD_Custom::DrawHUD (whistle commands)
    {0x00C53D9F, true, true},   // URadialSelectorHUD_Inventory::DrawHUD
    {0x00C55A9C, true, true},   // URadialSelectorHUD_MultiUse::DrawHUD
    {0x00C60B9D, true, true},   // URadialSelectorHUD_PlayerAction::DrawHUD
    {0x00FD2802, true, true},   // AShooterHUD::DrawUIHUD
    {0x00FEF681, true, true},   // AShooterHUD::ShowInfoItems (harvest list)
    {0x00C03742, false, true},   // UPrimalUI::ProcessSplitscreenScaling (UMG HUD widgets)
    // `(owner == first local player) ? -5000 : 0` in the canvas HUD
    {0x00FE4A75, true, true}, {0x00FE4D8C, true, true}, {0x00FE50B8, true, true}, {0x00FE53CC, true, true},
    {0x00FE57D6, true, true}, {0x00FE85E1, true, true}, {0x00FE881B, true, true}, {0x00FE8AA0, true, true},
    {0x00FE8D2B, true, true}, {0x00FE8FBC, true, true}, {0x00FE9337, true, true}, {0x00FE9AB5, true, true},
};
bool g_halfIndexSitesOk = false;
constexpr uintptr_t kRvaOverlayPlayerIndex = 0x00280BBB;  // cvtsi2ss xmm0, [rsp+110h] (IsSplitscreenPlayer index)
int32_t* g_overlaySwapFlag = nullptr;                    // in the overlay stub, so it's rip-reachable

// Byte offsets of the two immediates, or false if the bytes aren't the expected shape.
bool HalfIndexImmediates(const HalfIndexSite& site, size_t* first, size_t* second) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(g_base + site.rva);
    size_t mov2 = site.disp32 ? 15 : 12;
    uint8_t modrm = site.disp32 ? 0x84 : 0x44;
    if (p[0] != 0x75 || p[2] != 0xC7 || p[3] != modrm || p[4] != 0x24 || p[mov2 - 2] != 0xEB || p[mov2] != 0xC7 ||
        p[mov2 + 1] != modrm || p[mov2 + 2] != 0x24) {
        return false;
    }
    *first = site.disp32 ? 9 : 6;
    *second = mov2 + (site.disp32 ? 7 : 4);
    return true;
}

void CheckHalfIndexSites() {
    size_t first = 0, second = 0;
    for (HalfIndexSite& site : g_halfIndexSites) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(g_base + site.rva);
        if (!HalfIndexImmediates(site, &first, &second)) {
            Log("swap screens: HUD half site %08llX doesn't match this build; HUD won't follow a swap",
                static_cast<unsigned long long>(site.rva));
            return;
        }
        memcpy(&site.first, p + first, 4);
        memcpy(&site.other, p + second, 4);
    }
    g_halfIndexSitesOk = true;
}

void ApplyHalfIndexSites(bool swapped) {
    if (!g_halfIndexSitesOk) {
        return;
    }
    size_t first = 0, second = 0;
    for (const HalfIndexSite& site : g_halfIndexSites) {
        if (HalfIndexImmediates(site, &first, &second)) {
            uint8_t* p = reinterpret_cast<uint8_t*>(g_base + site.rva);
            int32_t a = site.first, b = site.other;
            if (swapped) {
                if (site.mirror) {
                    a = site.other;
                    b = site.first;
                } else {
                    b = site.first;
                }
            }
            WriteCode(p + first, &a, 4);
            WriteCode(p + second, &b, 4);
        }
    }
}

// UUI_HudOverlay (holds the overlay) clips itself with UUserWidget::SetAdditionalScissorRect to the top half when its
// IsSplitscreenPlayer index is 0, else the bottom (AddToViewport, `cmp [rsp+34h],0` at 0141E48B); its Tick maps the
// cursor by the same test (`cmp [rsp+310h],0` at 01421076). Each compare becomes a call to a stub that compares
// (index ^ swapped) instead; the flags survive the ret.
struct IndexCompareSite {
    const char* name;
    uintptr_t rva;
    uint8_t length;       // 5 (disp8) or 8 (disp32) byte `cmp dword [rsp+disp], 0`
    uint32_t stackDisp;   // disp in the caller's frame
};
const IndexCompareSite kIndexCompareSites[] = {
    {"UUI_HudOverlay::AddToViewport scissor", 0x0141E48B, 5, 0x34},
    {"UUI_HudOverlay::Tick cursor", 0x01421076, 8, 0x310},
};
int32_t* g_indexCompareFlags[2] = {};

void PatchIndexCompareSites() {
    for (size_t n = 0; n < 2; ++n) {
        const IndexCompareSite& site = kIndexCompareSites[n];
        auto p = reinterpret_cast<uint8_t*>(g_base + site.rva);
        bool ok = site.length == 5 ? (p[0] == 0x83 && p[1] == 0x7C && p[2] == 0x24 && p[3] == site.stackDisp && p[4] == 0)
                                   : (p[0] == 0x83 && p[1] == 0xBC && p[2] == 0x24 &&
                                      memcmp(p + 3, &site.stackDisp, 4) == 0 && p[7] == 0);
        if (!ok) {
            Log("swap screens: %s doesn't match this build", site.name);
            continue;
        }
        uint8_t* stub = AllocNearImage(64);
        if (!stub) {
            continue;
        }
        uint32_t disp = site.stackDisp + 8;  // our return address is on the stack
        uint8_t code[24] = {0x8B, 0x84, 0x24, 0, 0, 0, 0,   // mov eax, [rsp+disp+8]
                            0x33, 0x05, 0x06, 0, 0, 0,       // xor eax, [rip+6] (flag at +19)
                            0x83, 0xF8, 0x00,                // cmp eax, 0
                            0xC3,                            // ret
                            0xCC, 0xCC};
        memcpy(code + 3, &disp, 4);
        memcpy(stub, code, sizeof(code));
        g_indexCompareFlags[n] = reinterpret_cast<int32_t*>(stub + 19);
        *g_indexCompareFlags[n] = g_screensSwapped;
        uint8_t patch[8] = {0xE8, 0, 0, 0, 0, 0x0F, 0x1F, 0x00};  // call stub; 3-byte nop if needed
        int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(stub) - reinterpret_cast<intptr_t>(p + 5));
        memcpy(patch + 1, &rel, sizeof(rel));
        if (WriteCode(p, patch, site.length)) {
            Log("patched %s (swap screens)", site.name);
        }
    }
}

void PatchOverlayPlayerIndex() {
    auto site = reinterpret_cast<uint8_t*>(g_base + kRvaOverlayPlayerIndex);
    const uint8_t expected[] = {0xF3, 0x0F, 0x2A, 0x84, 0x24, 0x10, 0x01, 0x00, 0x00};
    if (memcmp(site, expected, sizeof(expected)) != 0) {
        Log("HUD overlay player-index site doesn't match this build; overlay won't follow a swap");
        return;
    }
    uint8_t* stub = AllocNearImage(64);
    if (!stub) {
        return;
    }
    // On entry rsp = caller's rsp - 8: xmm0 = float([caller rsp+110h] ^ flag).
    const uint8_t code[] = {0x8B, 0x84, 0x24, 0x18, 0x01, 0x00, 0x00,  // mov eax, [rsp+118h]
                            0x33, 0x05, 0x07, 0x00, 0x00, 0x00,        // xor eax, [rip+7] (the flag at +20)
                            0xF3, 0x0F, 0x2A, 0xC0,                    // cvtsi2ss xmm0, eax
                            0xC3,                                      // ret
                            0xCC, 0xCC};                               // pad; flag at +20
    memcpy(stub, code, sizeof(code));
    g_overlaySwapFlag = reinterpret_cast<int32_t*>(stub + 20);
    *g_overlaySwapFlag = 0;
    uint8_t patch[9] = {0xE8, 0, 0, 0, 0, 0x0F, 0x1F, 0x40, 0x00};  // call stub; 4-byte nop
    int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(stub) - reinterpret_cast<intptr_t>(site + 5));
    memcpy(patch + 1, &rel, sizeof(rel));
    if (WriteCode(site, patch, sizeof(patch))) {
        Log("patched UHUDOVerlayWidget::DrawWidget player region (swap screens)");
    }
}

void SwapSplitscreenOrigins(void* viewportClient) {
    auto vc = static_cast<uint8_t*>(viewportClient);
    uint8_t* types = *reinterpret_cast<uint8_t**>(vc + 0x70);
    int32_t typeCount = *reinterpret_cast<int32_t*>(vc + 0x78);
    for (int32_t type = 1; type <= 2 && type < typeCount; ++type) {  // TwoPlayer_Horizontal, TwoPlayer_Vertical
        float* players = *reinterpret_cast<float**>(types + type * 16);
        if (*reinterpret_cast<int32_t*>(types + type * 16 + 8) >= 2) {
            for (int i = 2; i < 4; ++i) {  // OriginX, OriginY of players 1 and 2
                float origin = players[i];
                players[i] = players[i + 4];
                players[i + 4] = origin;
            }
        }
    }
    g_screensSwapped ^= 1;
    ApplyHalfIndexSites(g_screensSwapped != 0);
    if (g_overlaySwapFlag) {
        *g_overlaySwapFlag = g_screensSwapped;
    }
    for (int32_t* flag : g_indexCompareFlags) {
        if (flag) {
            *flag = g_screensSwapped;
        }
    }
}

bool SwapScreens(void* controller) {
    using ObjectFn = void* (*)(const void* object);
    using VoidFn = void (*)(void* object);
    auto vtable = *reinterpret_cast<void* const* const*>(controller);
    void* world = reinterpret_cast<ObjectFn>(vtable[0x108 / sizeof(void*)])(controller);  // UObject::GetWorld
    void* viewportClient = world ? reinterpret_cast<ObjectFn>(g_base + kRvaWorldGetGameViewport)(world) : nullptr;
    if (!viewportClient || g_localPlayerCount != 2) {
        Log("swap screens: no viewport or not 2 players");
        return false;
    }
    SwapSplitscreenOrigins(viewportClient);
    reinterpret_cast<VoidFn>(g_base + kRvaLayoutPlayers)(viewportClient);
    reinterpret_cast<VoidFn>(g_base + kRvaReAdjustUI)(viewportClient);
    Log("swap screens: player 1 is now on the %s", g_screensSwapped ? "bottom" : "top");
    return true;
}

// HUD notifications in splitscreen. UHUDOVerlayWidget draws every player's notifications (centre banners like
// "LEVEL-UP IS AVAILABLE" / "You feel cold" / "Equip a tool...", the left-side "ADDED 1x ..." item notices, ...)
// through AShooterHUD::DrawHUDNotifications on ONE full-screen canvas, only clipping each player to their half. The
// elements are placed relative to the full screen (banner at the top, item notices at the middle), so player 2's
// banners landed in player 1's half and player 1's item notices on the split line -- and got clipped away. For the
// duration of the call the canvas is narrowed to the player's own half (top/bottom split): origin moved down,
// clip/size set to the half's height. UCanvas text/tile drawing adds the origin itself.
constexpr uintptr_t kRvaDrawHUDNotifications = 0x00FEFFA0;  // float AShooterHUD::DrawHUDNotifications(UCanvas*, float)
using DrawHUDNotificationsFn = float (*)(void* hud, void* canvas, float y);
DrawHUDNotificationsFn g_originalDrawHUDNotifications = nullptr;

struct CanvasRegion {  // UCanvas fields
    float org[2];      // +0x28 OrgX, OrgY
    float clip[2];     // +0x30 ClipX, ClipY
    int32_t size[2];   // +0x40 SizeX, SizeY
};

float HookedDrawHUDNotifications(void* hud, void* canvas, float y) {
    void* controller = reinterpret_cast<HudGetPlayerOwnerFn>(g_base + kRvaHudGetPlayerOwner)(hud);
    int32_t index = g_localPlayerCount > 1 ? SplitscreenIndexOf(controller) : -1;
    if (!canvas || index < 0 || index >= g_localPlayerCount) {
        return g_originalDrawHUDNotifications(hud, canvas, y);
    }
    if (g_localPlayerCount == 2) {
        index ^= g_screensSwapped;  // screen half, not player number
    }
    auto c = static_cast<uint8_t*>(canvas);
    CanvasRegion saved;
    memcpy(saved.org, c + 0x28, sizeof(saved.org));
    memcpy(saved.clip, c + 0x30, sizeof(saved.clip));
    memcpy(saved.size, c + 0x40, sizeof(saved.size));
    float clipHeight = saved.clip[1] / g_localPlayerCount;
    float org[2] = {saved.org[0], saved.org[1] + clipHeight * index};
    float clip[2] = {saved.clip[0], clipHeight};
    int32_t size[2] = {saved.size[0], saved.size[1] / g_localPlayerCount};
    memcpy(c + 0x28, org, sizeof(org));
    memcpy(c + 0x30, clip, sizeof(clip));
    memcpy(c + 0x40, size, sizeof(size));
    float result = g_originalDrawHUDNotifications(hud, canvas, y);    memcpy(c + 0x28, saved.org, sizeof(saved.org));
    memcpy(c + 0x30, saved.clip, sizeof(saved.clip));
    memcpy(c + 0x40, saved.size, sizeof(saved.size));
    return result;
}

void HookItemNotifications() {
    // DrawHUDNotifications: movss [rsp+18h],xmm2 / mov [rsp+10h],rdx / mov [rsp+8],rcx / push rsi / push rdi
    static const uint8_t kPrologue[] = {0xF3, 0x0F, 0x11, 0x54, 0x24, 0x18, 0x48, 0x89, 0x54,
                                        0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x56, 0x57};
    const HookSpec hook[] = {
        {"AShooterHUD::DrawHUDNotifications (per-player region)", kRvaDrawHUDNotifications, kPrologue,
         sizeof(kPrologue), reinterpret_cast<const void*>(&HookedDrawHUDNotifications),
         reinterpret_cast<void**>(&g_originalDrawHUDNotifications)},
    };
    InstallHooks(hook, 1);
}

// Dino name labels ("Lvl 2 Wild Dodo") in splitscreen. APrimalDinoCharacter::DrawFloatingHUD only draws the label once
// a per-dino "hidden until" time (interface this + 0x9F0 = actor + 0x10A8) has passed, and DrawDinoFloatingHUD sets it
// to now + rand(0..1 s) whenever the dino is too far from the player whose HUD is drawing. Both players' HUDs share
// that one value, so a dino far from either player kept the label hidden for both (it only showed when both were
// near, and flickered at the edge). The value is kept per player: swapped in before the call and saved after.
constexpr uintptr_t kRvaDinoDrawFloatingHUD = 0x006F3060;  // APrimalDinoCharacter::DrawFloatingHUD (interface this)
constexpr size_t kDinoLabelHiddenUntilOffset = 0x9F0;       // from the interface this
using DinoFloatingHUDFn = void (*)(void* self, void* hud);
DinoFloatingHUDFn g_originalDinoFloatingHUD = nullptr;
std::unordered_map<void*, double> g_dinoLabelHiddenUntil[2];  // key: interface this; one map per player

void HookedDinoFloatingHUD(void* self, void* hud) {
    void* controller =
        g_localPlayerCount > 1 && hud ? reinterpret_cast<HudGetPlayerOwnerFn>(g_base + kRvaHudGetPlayerOwner)(hud) : nullptr;
    int32_t player = controller ? SplitscreenIndexOf(controller) : -1;
    if (player < 0 || player > 1) {
        g_originalDinoFloatingHUD(self, hud);
        return;
    }
    auto& mine = g_dinoLabelHiddenUntil[player];
    if (mine.size() > 4096) {  // dinos come and go; stale entries only delay a label by up to a second
        mine.clear();
    }
    double* hiddenUntil = reinterpret_cast<double*>(static_cast<uint8_t*>(self) + kDinoLabelHiddenUntilOffset);
    double shared = *hiddenUntil;
    auto it = mine.find(self);
    *hiddenUntil = it != mine.end() ? it->second : 0.0;
    g_originalDinoFloatingHUD(self, hud);
    mine[self] = *hiddenUntil;
    *hiddenUntil = shared;
}

void HookDinoLabels() {
    static const uint8_t kPrologue[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x48, 0x83, 0xEC, 0x38};
    const HookSpec hook[] = {{"APrimalDinoCharacter::DrawFloatingHUD (per-player label timer)", kRvaDinoDrawFloatingHUD,
                              kPrologue, sizeof(kPrologue), reinterpret_cast<const void*>(&HookedDinoFloatingHUD),
                              reinterpret_cast<void**>(&g_originalDinoFloatingHUD)}};
    InstallHooks(hook, 1);
}

// World streaming in splitscreen. UWorldComposition::UpdateStreamingState(FSceneViewFamily*) streams the map tiles
// (rocks, foliage, ...) around the AVERAGE of all views' camera positions, so with the players far apart the tiles
// around both of them were unloaded (the reason the consoles tether player 2). The views' positions are recorded, and
// UWorldComposition::GetDistanceVisibleLevels runs once per position: a tile is shown if either player needs it (the
// more detailed LOD wins) and hidden only if neither does.
constexpr uintptr_t kRvaWorldStreamingFromViews = 0x02C789F0;  // UpdateStreamingState(FSceneViewFamily*)
constexpr uintptr_t kRvaGetDistanceVisibleLevels = 0x02C76B00;
constexpr uintptr_t kRvaMemoryRealloc = 0x020C5FF0;            // FMemory::Realloc(ptr, size, alignment)
constexpr uintptr_t kRvaMemoryFree = 0x00194340;               // FMemory::Free
constexpr size_t kViewOriginOffset = 0x190;                    // FSceneView: view origin (what the averaging reads)

struct DistanceVisibleLevel {  // FDistanceVisibleLevel
    int32_t tileIndex;
    void* streamingLevel;
    int32_t lodIndex;
};
static_assert(sizeof(DistanceVisibleLevel) == 24, "FDistanceVisibleLevel is 24 bytes");

struct LevelArray {  // TArray<FDistanceVisibleLevel>
    DistanceVisibleLevel* data;
    int32_t num;
    int32_t max;
};

using StreamingFromViewsFn = void (*)(void* composition, void* viewFamily);
using GetDistanceVisibleLevelsFn = void (*)(void* composition, const float* location, LevelArray* visible,
                                            LevelArray* hidden, void* names, int32_t* a, int32_t* b, bool* c,
                                            bool* d);
StreamingFromViewsFn g_originalStreamingFromViews = nullptr;
GetDistanceVisibleLevelsFn g_originalGetDistanceVisibleLevels = nullptr;
float g_streamingLocations[4][3] = {};
int32_t g_streamingLocationCount = 0;  // > 1 only during an UpdateStreamingState(views) call with several views

void HookedStreamingFromViews(void* composition, void* viewFamily) {
    g_streamingLocationCount = 0;
    if (viewFamily && g_localPlayerCount > 1) {
        auto views = *reinterpret_cast<uint8_t***>(viewFamily);
        int32_t count = *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(viewFamily) + 8);
        for (int32_t i = 0; i < count && i < 4 && views; ++i) {
            if (views[i]) {
                memcpy(g_streamingLocations[g_streamingLocationCount++], views[i] + kViewOriginOffset, 12);
            }
        }
    }
    g_originalStreamingFromViews(composition, viewFamily);
    g_streamingLocationCount = 0;
}

void AppendLevel(LevelArray* array, const DistanceVisibleLevel& level) {
    if (array->num >= array->max) {
        int32_t newMax = array->max < 8 ? 16 : array->max * 2;
        using ReallocFn = void* (*)(void* ptr, size_t size, uint32_t alignment);
        array->data = static_cast<DistanceVisibleLevel*>(reinterpret_cast<ReallocFn>(g_base + kRvaMemoryRealloc)(
            array->data, sizeof(DistanceVisibleLevel) * newMax, 0));
        array->max = newMax;
    }
    array->data[array->num++] = level;
}

void HookedGetDistanceVisibleLevels(void* composition, const float* location, LevelArray* visible, LevelArray* hidden,
                                    void* names, int32_t* a, int32_t* b, bool* c, bool* d) {
    int32_t extra = g_streamingLocationCount;
    if (extra < 2) {
        g_originalGetDistanceVisibleLevels(composition, location, visible, hidden, names, a, b, c, d);
        return;
    }
    g_originalGetDistanceVisibleLevels(composition, g_streamingLocations[0], visible, hidden, names, a, b, c, d);
    using FreeFn = void (*)(void* ptr);
    for (int32_t v = 1; v < extra; ++v) {
        LevelArray moreVisible = {}, moreHidden = {};
        int32_t tempA = 0, tempB = 0;
        bool tempC = false, tempD = false;
        g_originalGetDistanceVisibleLevels(composition, g_streamingLocations[v], &moreVisible, &moreHidden, names,
                                           &tempA, &tempB, &tempC, &tempD);
        for (int32_t i = 0; i < moreVisible.num; ++i) {
            const DistanceVisibleLevel& level = moreVisible.data[i];
            bool merged = false;
            for (int32_t j = 0; j < visible->num; ++j) {
                if (visible->data[j].tileIndex == level.tileIndex) {
                    if (level.lodIndex < visible->data[j].lodIndex) {  // -1 = full detail
                        visible->data[j].lodIndex = level.lodIndex;
                        visible->data[j].streamingLevel = level.streamingLevel;
                    }
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                AppendLevel(visible, level);
            }
        }
        if (moreVisible.data) {
            reinterpret_cast<FreeFn>(g_base + kRvaMemoryFree)(moreVisible.data);
        }
        if (moreHidden.data) {
            reinterpret_cast<FreeFn>(g_base + kRvaMemoryFree)(moreHidden.data);
        }
    }
    int32_t kept = 0;  // hidden only if no player needs the tile
    for (int32_t i = 0; i < hidden->num; ++i) {
        bool needed = false;
        for (int32_t j = 0; j < visible->num && !needed; ++j) {
            needed = visible->data[j].tileIndex == hidden->data[i].tileIndex;
        }
        if (!needed) {
            hidden->data[kept++] = hidden->data[i];
        }
    }
    hidden->num = kept;
    static ULONGLONG lastLog = 0;
    ULONGLONG now = GetTickCount64();
    if (now - lastLog > 30000) {
        lastLog = now;
        Log("world streaming: %d views -> %d tiles shown, %d hidden", extra, visible->num, hidden->num);
    }
}

void HookWorldStreaming() {
    static const uint8_t kViewsPrologue[] = {0x48, 0x8B, 0xC4, 0x41, 0x55, 0x41, 0x57, 0x48, 0x81,
                                             0xEC, 0x98, 0x00, 0x00, 0x00, 0x0F, 0x29, 0x70, 0xC8};
    static const uint8_t kLevelsPrologue[] = {0x4C, 0x8B, 0xDC, 0x55, 0x53, 0x49, 0x8D, 0xAB, 0xF8, 0xFD,
                                              0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xF8, 0x02, 0x00, 0x00};
    const HookSpec hooks[] = {
        {"UWorldComposition::UpdateStreamingState (each player's position)", kRvaWorldStreamingFromViews,
         kViewsPrologue, sizeof(kViewsPrologue), reinterpret_cast<const void*>(&HookedStreamingFromViews),
         reinterpret_cast<void**>(&g_originalStreamingFromViews)},
        {"UWorldComposition::GetDistanceVisibleLevels (stream around every player)", kRvaGetDistanceVisibleLevels,
         kLevelsPrologue, sizeof(kLevelsPrologue), reinterpret_cast<const void*>(&HookedGetDistanceVisibleLevels),
         reinterpret_cast<void**>(&g_originalGetDistanceVisibleLevels)},
    };
    InstallHooks(hooks, 2);
}

// Player 2's own sounds (grunts, tool swings) were muffled. Each sound uses the closest listener (one per view) and
// FActiveSound::HandleInteriorVolumes multiplies its high-frequency gain by that listener's InteriorSettings.InteriorLPF.
// FAudioDevice::SetListener grows the listener array with zeroed entries, then FListener::ApplyInteriorSettings returns
// early when the reverb volume is unchanged -- outdoors that is null both times, so listener 2+ keeps all-zero interior
// settings (InteriorLPF 0 = all treble removed) forever. Listener 0 was built by its constructor with defaults.
// Fix: after SetListener, if a listener's interior settings are still all zero, apply the passed settings (forcing the
// volume comparison to differ), falling back to the engine defaults.
constexpr uintptr_t kRvaSetListener = 0x02889820;
constexpr uintptr_t kRvaApplyInteriorSettings = 0x02889760;
constexpr size_t kAudioListenersOffset = 0x20;  // FAudioDevice::Listeners (TArray<FListener>)
constexpr size_t kListenerSize = 0xA0;
constexpr size_t kListenerInteriorOffset = 0x3C;  // FInteriorSettings, 0x24 bytes
constexpr size_t kInteriorSettingsSize = 0x24;
constexpr size_t kListenerVolumeOffset = 0x60;  // AReverbVolume* of the last applied settings

using SetListenerFn = void (*)(void* device, int index, const void* transform, float deltaSeconds, void* volume,
                               const void* interiorSettings);
using ApplyInteriorSettingsFn = void (*)(void* listener, void* volume, const void* interiorSettings);
SetListenerFn g_originalSetListener = nullptr;

bool InteriorSettingsZero(const uint8_t* settings) {
    for (size_t i = 0; i < kInteriorSettingsSize; ++i) {
        if (settings[i] != 0) {
            return false;
        }
    }
    return true;
}

void HookedSetListener(void* device, int index, const void* transform, float deltaSeconds, void* volume,
                       const void* interiorSettings) {
    g_originalSetListener(device, index, transform, deltaSeconds, volume, interiorSettings);
    auto* listeners = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(device) + kAudioListenersOffset);
    int32_t count = *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(device) + kAudioListenersOffset + 8);
    if (listeners == nullptr || index < 0 || index >= count) {
        return;
    }
    uint8_t* listener = listeners + index * kListenerSize;
    if (!InteriorSettingsZero(listener + kListenerInteriorOffset)) {
        return;
    }
    if (interiorSettings != nullptr && !InteriorSettingsZero(static_cast<const uint8_t*>(interiorSettings))) {
        *reinterpret_cast<void**>(listener + kListenerVolumeOffset) = reinterpret_cast<void*>(~uintptr_t{0});
        reinterpret_cast<ApplyInteriorSettingsFn>(g_base + kRvaApplyInteriorSettings)(listener, volume,
                                                                                     interiorSettings);
    }
    if (InteriorSettingsZero(listener + kListenerInteriorOffset)) {
        // FInteriorSettings defaults: not world settings; volume 1, time 0.5, LPF 1, LPF time 0.5 (exterior, interior)
        const float defaults[8] = {1.0f, 0.5f, 1.0f, 0.5f, 1.0f, 0.5f, 1.0f, 0.5f};
        memcpy(listener + kListenerInteriorOffset + 4, defaults, sizeof(defaults));
    }
    Log("audio listener %d: interior settings were empty (muffled sounds), initialized", index + 1);
}

void HookAudioListeners() {
    static const uint8_t kPrologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
                                        0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18};
    const HookSpec hook[] = {{"FAudioDevice::SetListener (interior settings for listeners 2+)", kRvaSetListener,
                              kPrologue, sizeof(kPrologue), reinterpret_cast<const void*>(&HookedSetListener),
                              reinterpret_cast<void**>(&g_originalSetListener)}};
    InstallHooks(hook, 1);
}

// Pause menu "TELEPORT TO PLAYER 1" for players 2+: their menu shows Resume / Leave Game / Deactivate Gamepad, and
// "Deactivate Gamepad" (UUI_PauseMenu+0x5B0) is pointless for them (they can only play on a controller). Its label
// is changed when the menu opens (AddToViewport) and its click (ClickedButton) teleports the player next to player 1
// with AShooterCharacter::TeleportTo, which moves the spot out of blocking geometry and reports failure.
// "SWAP SCREENS": the button at +0x5E0 (OptionsButton), which the menu collapses for players 2+, is shown again,
// relabelled, and its click swaps the screen halves.
// Player 1 gets "TELEPORT TO PLAYER 2": InviteFriendsButton (+0x5F8), collapsed in this menu layout and meaningless in
// a local game, is shown, relabelled and teleports player 1 next to player 2.
constexpr uintptr_t kRvaPauseMenuAddToViewport = 0x014FB850;
constexpr uintptr_t kRvaPauseMenuClickedButton = 0x015009E0;
constexpr size_t kPauseMenuGamepadButtonOffset = 0x5B0;
constexpr size_t kPauseMenuSwapButtonOffset = 0x5E0;
constexpr size_t kPauseMenuInviteButtonOffset = 0x5F8;
constexpr size_t kVtWidgetGetOwningPlayer = 0x3D0;  // UPrimalUI vtable: owning APlayerController
using PauseMenuVoidFn = void (*)(void* menu);
using PauseMenuClickedFn = void (*)(void* menu, void* widget);
PauseMenuVoidFn g_originalPauseMenuAddToViewport = nullptr;
PauseMenuClickedFn g_originalPauseMenuClickedButton = nullptr;

void* PauseMenuOwner(void* menu) {
    using GetOwningPlayerFn = void* (*)(void* menu);
    auto vtable = *reinterpret_cast<void* const* const*>(menu);
    return reinterpret_cast<GetOwningPlayerFn>(vtable[kVtWidgetGetOwningPlayer / sizeof(void*)])(menu);
}

void* FindTextBlock(void* widget, int depth) {
    using StaticClassFn = void* (*)();
    using IsAFn = bool (*)(const void* object, const void* uclass);
    using GetChildrenCountFn = int32_t (*)(const void* panel);
    using GetChildAtFn = void* (*)(const void* panel, int32_t index);
    auto isA = reinterpret_cast<IsAFn>(g_base + 0x0221D9A0);
    if (!widget) {
        return nullptr;
    }
    if (isA(widget, reinterpret_cast<StaticClassFn>(g_base + 0x00180CB0)())) {  // UTextBlock
        return widget;
    }
    if (depth <= 0 || !isA(widget, reinterpret_cast<StaticClassFn>(g_base + 0x0017F040)())) {  // UPanelWidget
        return nullptr;
    }
    int32_t count = reinterpret_cast<GetChildrenCountFn>(g_base + 0x0272D550)(widget);
    for (int32_t i = 0; i < count && i < 16; ++i) {
        if (void* found = FindTextBlock(reinterpret_cast<GetChildAtFn>(g_base + 0x0272D560)(widget, i), depth - 1)) {
            return found;
        }
    }
    return nullptr;
}

void SetButtonLabel(void* button, wchar_t* label) {
    void* text = FindTextBlock(button, 4);
    if (!text) {
        Log("pause menu: no label found in button %p", button);
        return;
    }
    struct {
        wchar_t* data;
        int32_t num, max;
    } fstring = {label, static_cast<int32_t>(wcslen(label) + 1), static_cast<int32_t>(wcslen(label) + 1)};
    using SetStringFn = void (*)(void* textBlock, const void* fstring);
    reinterpret_cast<SetStringFn>(g_base + 0x02735560)(text, &fstring);  // UTextBlock::SetString
}

void ShowRelabelledButton(void* button, wchar_t* label) {
    if (!button) {
        return;
    }
    using SetVisibilityFn = void (*)(void* widget, const uint8_t* visibility);
    const uint8_t visible = 0;  // ESlateVisibility::Visible
    reinterpret_cast<SetVisibilityFn>(g_base + 0x0273D030)(button, &visible);  // UWidget::SetVisibility
    SetButtonLabel(button, label);
}

void HookedPauseMenuAddToViewport(void* menu) {
    g_originalPauseMenuAddToViewport(menu);
    if (g_localPlayerCount == 2 && SplitscreenIndexOf(PauseMenuOwner(menu)) == 0) {
        // The menu only looks InviteFriendsButton up, it never binds its click; bind it the way the other buttons
        // are bound (UPrimalUI::InitializeButtonNameWithClick -> ClickedButton), once per menu.
        static void* boundMenu = nullptr;
        if (boundMenu != menu) {
            boundMenu = menu;
            uint64_t name = 0;
            using FNameCtorFn = void* (*)(uint64_t* name, const wchar_t* text, int32_t findType, bool splitNumber);
            reinterpret_cast<FNameCtorFn>(g_base + 0x0215CDF0)(&name, L"InviteFriendsButton", 1, true);  // FNAME_Add
            using InitButtonFn = void* (*)(void* ui, uint64_t name);
            reinterpret_cast<InitButtonFn>(g_base + 0x00BCC7E0)(menu, name);
        }
        static wchar_t teleportLabel[] = L"TELEPORT TO PLAYER 2";
        ShowRelabelledButton(*reinterpret_cast<void**>(static_cast<uint8_t*>(menu) + kPauseMenuInviteButtonOffset),
                             teleportLabel);
        return;
    }
    if (g_localPlayerCount < 2 || SplitscreenIndexOf(PauseMenuOwner(menu)) <= 0) {
        return;
    }
    auto m = static_cast<uint8_t*>(menu);
    static wchar_t teleportLabel[] = L"TELEPORT TO PLAYER 1";
    SetButtonLabel(*reinterpret_cast<void**>(m + kPauseMenuGamepadButtonOffset), teleportLabel);
    if (g_localPlayerCount == 2) {
        static wchar_t swapLabel[] = L"SWAP SCREENS";
        ShowRelabelledButton(*reinterpret_cast<void**>(m + kPauseMenuSwapButtonOffset), swapLabel);
    }
}

void* SecondLocalPlayerController(void* controller) {
    using GetGameInstanceFn = void* (*)(const void* actor);
    void* gameInstance = reinterpret_cast<GetGameInstanceFn>(g_base + 0x02861330)(controller);
    if (!gameInstance) {
        return nullptr;
    }
    // UGameInstance::LocalPlayers (TArray at +0x38); ULocalPlayer::PlayerController at +0x30.
    auto players = *reinterpret_cast<uint8_t***>(static_cast<uint8_t*>(gameInstance) + 0x38);
    int32_t count = *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(gameInstance) + 0x40);
    return players && count > 1 && players[1] ? *reinterpret_cast<void**>(players[1] + 0x30) : nullptr;
}

bool TeleportNextTo(void* controller, void* targetController);

bool TeleportToFirstPlayer(void* controller) {
    using GetGameInstanceFn = void* (*)(const void* actor);
    using GetFirstLocalPlayerControllerFn = void* (*)(const void* gameInstance);
    void* gameInstance = reinterpret_cast<GetGameInstanceFn>(g_base + 0x02861330)(controller);
    void* firstController =
        gameInstance ? reinterpret_cast<GetFirstLocalPlayerControllerFn>(g_base + 0x0297A2C0)(gameInstance) : nullptr;
    return TeleportNextTo(controller, firstController);
}

bool TeleportNextTo(void* controller, void* targetController) {
    using GetPlayerCharacterFn = void* (*)(const void* controller);
    using GetGameInstanceFn = void* (*)(const void* actor);
    using GetFirstLocalPlayerControllerFn = void* (*)(const void* gameInstance);
    using IsRidingDinoFn = bool (*)(void* controller);
    using TeleportToFn = bool (*)(void* actor, const float* location, const float* rotation, bool isATest, bool noCheck);
    auto getCharacter = reinterpret_cast<GetPlayerCharacterFn>(g_base + 0x0109D860);
    void* character = getCharacter(controller);
    void* target = targetController ? getCharacter(targetController) : nullptr;
    if (!character || !target || character == target) {
        Log("teleport: no character to move or no target character");
        return false;
    }
    if (reinterpret_cast<IsRidingDinoFn>(g_base + 0x010BC7C0)(controller)) {
        Log("teleport: player is riding a dino; dismount first");
        return false;
    }
    // AActor::RootComponent (+0x250) -> ComponentToWorld translation (+0xF0).
    auto root = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(target) + 0x250);
    if (!root) {
        return false;
    }
    const float* p1 = reinterpret_cast<const float*>(root + 0xF0);
    const float offsets[][2] = {{150, 0}, {-150, 0}, {0, 150}, {0, -150}, {250, 250}, {-250, -250}, {0, 0}};
    const float rotation[3] = {0, 0, 0};
    auto teleport = reinterpret_cast<TeleportToFn>(g_base + 0x00D4B180);  // AShooterCharacter::TeleportTo
    for (const auto& o : offsets) {
        float destination[3] = {p1[0] + o[0], p1[1] + o[1], p1[2] + 50.0f};
        if (teleport(character, destination, rotation, false, false)) {
            Log("teleport: player moved next to the other player (%.0f, %.0f, %.0f)", destination[0], destination[1],
                destination[2]);
            return true;
        }
    }
    Log("teleport: no free spot found near the other player");
    return false;
}

void HookedPauseMenuClickedButton(void* menu, void* widget) {
    if (g_localPlayerCount == 2 && widget &&
        widget == *reinterpret_cast<void**>(static_cast<uint8_t*>(menu) + kPauseMenuInviteButtonOffset)) {
        void* controller = PauseMenuOwner(menu);
        if (SplitscreenIndexOf(controller) == 0) {
            if (TeleportNextTo(controller, SecondLocalPlayerController(controller))) {
                auto vtable = *reinterpret_cast<void* const* const*>(menu);
                reinterpret_cast<PauseMenuVoidFn>(vtable[0x3C8 / sizeof(void*)])(menu);  // close the menu
            }
            return;  // instead of the friend-invite overlay
        }
    }
    if (g_localPlayerCount > 1 && widget &&
        widget == *reinterpret_cast<void**>(static_cast<uint8_t*>(menu) + kPauseMenuGamepadButtonOffset)) {
        void* controller = PauseMenuOwner(menu);
        if (SplitscreenIndexOf(controller) > 0) {
            if (TeleportToFirstPlayer(controller)) {
                // Close the menu the way ClickedButton does after opening the admin screen (vtable +0x3C8).
                auto vtable = *reinterpret_cast<void* const* const*>(menu);
                reinterpret_cast<PauseMenuVoidFn>(vtable[0x3C8 / sizeof(void*)])(menu);
            }
            return;  // instead of deactivating the gamepad
        }
    }
    if (g_localPlayerCount == 2 && widget &&
        widget == *reinterpret_cast<void**>(static_cast<uint8_t*>(menu) + kPauseMenuSwapButtonOffset)) {
        void* controller = PauseMenuOwner(menu);
        if (SplitscreenIndexOf(controller) > 0) {
            auto vtable = *reinterpret_cast<void* const* const*>(menu);
            reinterpret_cast<PauseMenuVoidFn>(vtable[0x3C8 / sizeof(void*)])(menu);  // close the menu first
            SwapScreens(controller);
            return;
        }
    }
    g_originalPauseMenuClickedButton(menu, widget);
}

void HookPauseMenu() {
    static const uint8_t kPrologueAddToViewport[] = {0x48, 0x89, 0x4C, 0x24, 0x08, 0x56, 0x57,
                                                     0x48, 0x81, 0xEC, 0x88, 0x0D, 0x00, 0x00};
    static const uint8_t kPrologueClicked[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24,
                                               0x08, 0x48, 0x81, 0xEC, 0x18, 0x02, 0x00, 0x00};
    const HookSpec hooks[] = {
        {"UUI_PauseMenu::AddToViewport (teleport label)", kRvaPauseMenuAddToViewport, kPrologueAddToViewport,
         sizeof(kPrologueAddToViewport), reinterpret_cast<const void*>(&HookedPauseMenuAddToViewport),
         reinterpret_cast<void**>(&g_originalPauseMenuAddToViewport)},
        {"UUI_PauseMenu::ClickedButton (teleport to player 1)", kRvaPauseMenuClickedButton, kPrologueClicked,
         sizeof(kPrologueClicked), reinterpret_cast<const void*>(&HookedPauseMenuClickedButton),
         reinterpret_cast<void**>(&g_originalPauseMenuClickedButton)},
    };
    InstallHooks(hooks, 2);
}

// Tooltips in splitscreen: the player HUD (UPlayerHUDUI) is scaled (1, 0.5) for a top/bottom split and ARK
// un-squashes some of its parts, but not the item tooltip shown when holding a hotbar slot, so it looks squashed.
// After the item tooltips' InitToolTip the tooltip gets render scale (1, SplitscreenTooltipScaleY).
using ToolTipInitFn = void (*)(void* tooltip, void* controller, const void* text, void* entry);
struct ToolTipHook {
    const char* name;
    uintptr_t rva;
    ToolTipInitFn original;
};
ToolTipHook g_toolTipHooks[] = {
    {"UPrimalItemToolTipWidget::InitToolTip", 0x0095BCB0, nullptr},
    {"UPrimalItemInventoryToolTipWidget::InitToolTip", 0x0094D260, nullptr},
    {"UTooltipSmartLayoutWidget::InitToolTip", 0x012B0080, nullptr},
};

// Tooltips for things in the world (items on the ground, dinos, structures) come from
// UUI_HudOverlay::ShowOverlayTooltip and aren't inside the squashed HUD, so they keep their own height.
using ShowOverlayTooltipFn = void (*)(void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                      uint64_t, uint64_t, uint64_t, uint64_t);
ShowOverlayTooltipFn g_originalShowOverlayTooltip = nullptr;
int g_overlayTooltipDepth = 0;

void HookedShowOverlayTooltip(void* overlay, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                              uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11) {
    ++g_overlayTooltipDepth;
    g_originalShowOverlayTooltip(overlay, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
    --g_overlayTooltipDepth;
}

// Only the HUD is squashed to half height; the inventory and other menus aren't. The stretch is applied only to
// tooltips that the player HUD (UPlayerHUDUI) shows through UPrimalUI::ShowToolTip.
using ShowToolTipFn = void (*)(void* ui, void* widget);
ShowToolTipFn g_originalShowToolTip = nullptr;
int g_hudTooltipDepth = 0;

void HookedShowToolTip(void* ui, void* widget) {
    using StaticClassFn = void* (*)();
    using IsAFn = bool (*)(const void* object, const void* uclass);
    bool fromHud = ui && reinterpret_cast<IsAFn>(g_base + 0x0221D9A0)(
                             ui, reinterpret_cast<StaticClassFn>(g_base + 0x01014A60)());  // UPlayerHUDUI
    g_hudTooltipDepth += fromHud ? 1 : 0;
    g_originalShowToolTip(ui, widget);
    g_hudTooltipDepth -= fromHud ? 1 : 0;
}

void ApplyToolTipScale(void* tooltip) {
    if (g_overlayTooltipDepth > 0 || g_hudTooltipDepth == 0) {
        return;
    }
    if (g_localPlayerCount > 1 && tooltip) {
        using SetVector2DFn = void (*)(void* widget, uint64_t packed);
        float one = 1.0f;
        uint64_t packed = 0;
        memcpy(&packed, &one, 4);
        memcpy(reinterpret_cast<uint8_t*>(&packed) + 4, &g_tooltipScaleY, 4);
        reinterpret_cast<SetVector2DFn>(g_base + 0x0273C410)(tooltip, packed);  // UWidget::SetRenderScale
    }
}

template <int Which>
void HookedToolTipInit(void* tooltip, void* controller, const void* text, void* entry) {
    g_toolTipHooks[Which].original(tooltip, controller, text, entry);
    ApplyToolTipScale(tooltip);
}

void PatchTooltipScale() {
    if (g_tooltipScaleY == 1.0f) {
        return;
    }
    static const uint8_t kPrologue[] = {0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18,
                                        0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08};
    const void* detours[] = {reinterpret_cast<const void*>(&HookedToolTipInit<0>),
                             reinterpret_cast<const void*>(&HookedToolTipInit<1>),
                             reinterpret_cast<const void*>(&HookedToolTipInit<2>)};
    for (int i = 0; i < 3; ++i) {
        const HookSpec hook[] = {{g_toolTipHooks[i].name, g_toolTipHooks[i].rva, kPrologue, sizeof(kPrologue), detours[i],
                                  reinterpret_cast<void**>(&g_toolTipHooks[i].original)}};
        InstallHooks(hook, 1);
    }
    const HookSpec overlay[] = {{"UUI_HudOverlay::ShowOverlayTooltip (world tooltips keep their height)", 0x01422900,
                                 kPrologue, sizeof(kPrologue), reinterpret_cast<const void*>(&HookedShowOverlayTooltip),
                                 reinterpret_cast<void**>(&g_originalShowOverlayTooltip)}};
    InstallHooks(overlay, 1);
    static const uint8_t kShowToolTipPrologue[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08,
                                                   0x56, 0x57, 0x48, 0x81, 0xEC, 0xB8, 0x0D, 0x00, 0x00};
    const HookSpec show[] = {{"UPrimalUI::ShowToolTip (only HUD tooltips are stretched)", 0x00BC4DB0,
                              kShowToolTipPrologue, sizeof(kShowToolTipPrologue),
                              reinterpret_cast<const void*>(&HookedShowToolTip),
                              reinterpret_cast<void**>(&g_originalShowToolTip)}};
    InstallHooks(show, 1);
    Log("splitscreen tooltip height: x%.2f", g_tooltipScaleY);
}

void HookGamepadActive() {
    static const uint8_t kPrologueGetControllerActive[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20,
                                                           0x4C, 0x8B, 0xD9, 0x8B, 0x89, 0x28, 0x05, 0x00, 0x00};
    static const uint8_t kPrologueCheckPrimalUI[] = {0x88, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08,
                                                     0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x44, 0x24, 0x30};
    static const uint8_t kPrologueCheckHub[] = {0x88, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08,
                                                0x48, 0x83, 0xEC, 0x28, 0x0F, 0xB6, 0x54, 0x24, 0x38};
    const HookSpec hooks[] = {
        {"FSlateApplication::GetControllerActive (players 2+ use a controller)", kRvaGetControllerActive,
         kPrologueGetControllerActive, sizeof(kPrologueGetControllerActive),
         reinterpret_cast<const void*>(&HookedGetControllerActive), reinterpret_cast<void**>(&g_originalGetControllerActive)},
        {"UPrimalUI::CheckGamepadActiveChanged", kRvaPrimalUICheckGamepadActive, kPrologueCheckPrimalUI,
         sizeof(kPrologueCheckPrimalUI), reinterpret_cast<const void*>(&HookedPrimalUICheckGamepadActive),
         reinterpret_cast<void**>(&g_originalPrimalUICheckGamepadActive)},
        {"UUI_Hub::CheckGamepadActiveChanged", kRvaHubCheckGamepadActive, kPrologueCheckHub, sizeof(kPrologueCheckHub),
         reinterpret_cast<const void*>(&HookedHubCheckGamepadActive), reinterpret_cast<void**>(&g_originalHubCheckGamepadActive)},
    };
    InstallHooks(hooks, 3);
    // SetCameraMode: mov [rsp+20h],r9b / mov [rsp+18h],r8b / mov [rsp+10h],dl
    static const uint8_t kPrologueSetCameraMode[] = {0x44, 0x88, 0x4C, 0x24, 0x20, 0x44, 0x88,
                                                     0x44, 0x24, 0x18, 0x88, 0x54, 0x24, 0x10};
    const HookSpec camera[] = {
        {"AShooterCharacter::SetCameraMode (per-player camera mode)", kRvaSetCameraMode, kPrologueSetCameraMode,
         sizeof(kPrologueSetCameraMode), reinterpret_cast<const void*>(&HookedSetCameraMode),
         reinterpret_cast<void**>(&g_originalSetCameraMode)},
    };
    InstallHooks(camera, 1);
    // ZoomIn / ZoomOut: mov [rsp+8],rcx / sub rsp,48h / mov rax,[rsp+50h]
    static const uint8_t kPrologueZoom[] = {0x48, 0x89, 0x4C, 0x24, 0x08, 0x48, 0x83,
                                            0xEC, 0x48, 0x48, 0x8B, 0x44, 0x24, 0x50};
    const HookSpec zoom[] = {
        {"AShooterCharacter::ZoomIn (per-player camera mode)", kRvaCharacterZoomIn, kPrologueZoom, sizeof(kPrologueZoom),
         reinterpret_cast<const void*>(&HookedZoomIn), reinterpret_cast<void**>(&g_originalZoomIn)},
        {"AShooterCharacter::ZoomOut (per-player camera mode)", kRvaCharacterZoomOut, kPrologueZoom, sizeof(kPrologueZoom),
         reinterpret_cast<const void*>(&HookedZoomOut), reinterpret_cast<void**>(&g_originalZoomOut)},
    };
    InstallHooks(zoom, 2);
    PatchTickCameraSettings();
    {
        static const uint8_t kInputKeyPrologue[] = {0x4C, 0x89, 0x4C, 0x24, 0x20, 0x44, 0x89, 0x44, 0x24, 0x18,
                                                    0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08};
        const HookSpec input[] = {{"UShooterGameViewportClient::InputKey (trigger scroll goes to its own player)",
                                   kRvaViewportInputKey, kInputKeyPrologue, sizeof(kInputKeyPrologue),
                                   reinterpret_cast<const void*>(&HookedInputKey),
                                   reinterpret_cast<void**>(&g_originalInputKey)}};
        InstallHooks(input, 1);
        static const uint8_t kAnalogPrologue[] = {0xF3, 0x0F, 0x11, 0x5C, 0x24, 0x20, 0x44, 0x89,
                                                  0x44, 0x24, 0x18, 0x55, 0x53, 0x56, 0x57};
        const HookSpec analog[] = {{"FSlateApplication::OnControllerAnalog (trigger scroll goes to its own player)",
                                    kRvaSlateOnControllerAnalog, kAnalogPrologue, sizeof(kAnalogPrologue),
                                    reinterpret_cast<const void*>(&HookedControllerAnalog),
                                    reinterpret_cast<void**>(&g_originalControllerAnalog)}};
        InstallHooks(analog, 1);
    }

}

void PatchToolbarScale() {
    if (g_toolbarScale == 1.0f) {
        return;
    }
    // UpdateItemSlotVisibility: mov [rsp+8],rcx / sub rsp,48h / mov rax,[rsp+50h]
    static const uint8_t kPrologue[] = {0x48, 0x89, 0x4C, 0x24, 0x08, 0x48, 0x83,
                                        0xEC, 0x48, 0x48, 0x8B, 0x44, 0x24, 0x50};
    const HookSpec hook[] = {
        {"UPlayerHUDUI::UpdateItemSlotVisibility (toolbar scale)", kRvaPlayerHUDUpdateItemSlotVisibility, kPrologue,
         sizeof(kPrologue), reinterpret_cast<const void*>(&HookedPlayerHUDProcessSplitscreenScaling),
         reinterpret_cast<void**>(&g_originalPlayerHUDProcessSplitscreenScaling)},
    };
    if (InstallHooks(hook, 1)) {
        Log("splitscreen toolbar scale: %.2f", g_toolbarScale);
    }
}

// 5. DrawHUD is virtual with no direct callers, so its vtable slot is swapped.
void HookDrawHUD() {
    auto slot = reinterpret_cast<uintptr_t*>(g_base + kRvaShooterHudVtable + kVtDrawHUD);
    if (*slot != g_base + kRvaDrawHUD) {
        Log("AShooterHUD vtable doesn't match this build; HUD fix skipped");
        return;
    }
    g_originalDrawHUD = reinterpret_cast<DrawHUDFn>(*slot);
    uintptr_t detour = reinterpret_cast<uintptr_t>(&HookedDrawHUD);
    DWORD oldProtect = 0;
    if (VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtect)) {
        *slot = detour;
        VirtualProtect(slot, sizeof(*slot), oldProtect, &oldProtect);
        Log("hooked AShooterHUD::DrawHUD (vtable)");
    }
}

void InstallFixes() {
    // The player id and player count are what every other fix builds on.
    auto uint64Vtable = reinterpret_cast<const uintptr_t*>(g_base + kRvaUInt64IdVtable);
    if (uint64Vtable[0] != g_base + kRvaUInt64IdCompare) {
        Log("FUniqueNetIdUInt64 vtable doesn't match this build; not patching anything");
        return;
    }
    const HookSpec core[] = {
        {"ULocalPlayer::GetPreferredUniqueNetId", kRvaGetPreferredUniqueNetId, kPrologueGetPreferredUniqueNetId,
         sizeof(kPrologueGetPreferredUniqueNetId), reinterpret_cast<const void*>(&HookedGetPreferredUniqueNetId),
         reinterpret_cast<void**>(&g_originalGetPreferredUniqueNetId)},
        {"UShooterGameViewportClient::NotifyPlayerAdded", kRvaNotifyPlayerAdded, kPrologueNotifyPlayerAdded,
         sizeof(kPrologueNotifyPlayerAdded), reinterpret_cast<const void*>(&HookedNotifyPlayerAdded),
         reinterpret_cast<void**>(&g_originalNotifyPlayerAdded)},
        {"UShooterGameViewportClient::NotifyPlayerRemoved", kRvaNotifyPlayerRemoved, kPrologueNotifyPlayerRemoved,
         sizeof(kPrologueNotifyPlayerRemoved), reinterpret_cast<const void*>(&HookedNotifyPlayerRemoved),
         reinterpret_cast<void**>(&g_originalNotifyPlayerRemoved)},
    };
    if (!InstallHooks(core, sizeof(core) / sizeof(core[0]))) {
        return;
    }
    PatchLocalCheckSites();
    HookDrawHUD();
    PatchInventoryCheck();
    PatchRadialTextScale();
    PatchPlayerActionRadialTextScale();
    PatchToolbarScale();
    PatchRadialTextRadius();
    HookGamepadActive();
    HookItemNotifications();
    HookPauseMenu();
    HookDinoLabels();
    HookWorldStreaming();
    HookAudioListeners();
    CheckHalfIndexSites();
    PatchOverlayPlayerIndex();
    PatchIndexCompareSites();
    PatchTooltipScale();
    const HookSpec lighting[] = {
        {"FRHICommand<FRHICommandClear>::ExecuteAndDestruct (lighting fix)", kRvaClearCommandExecute,
         kPrologueClearCommandExecute, sizeof(kPrologueClearCommandExecute),
         reinterpret_cast<const void*>(&HookedClearCommandExecute),
         reinterpret_cast<void**>(&g_originalClearCommandExecute)},
    };
    InstallHooks(lighting, 1);
    static const uint8_t kPrologueRenderProjection[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41,
                                                        0x57, 0x48, 0x8D, 0xAC, 0x24, 0xB0, 0xFB, 0xFF, 0xFF};
    // RenderDepth: mov r11,rsp / push rbx,rbp,rsi,rdi,r14 / sub rsp,9F0h
    static const uint8_t kPrologueRenderDepth[] = {0x4C, 0x8B, 0xDC, 0x53, 0x55, 0x56, 0x57, 0x41,
                                                   0x56, 0x48, 0x81, 0xEC, 0xF0, 0x09, 0x00, 0x00};
    const HookSpec shadows[] = {
        {"FProjectedShadowInfo::RenderDepth (per-view shadow maps)", kRvaShadowRenderDepth, kPrologueRenderDepth,
         sizeof(kPrologueRenderDepth), reinterpret_cast<const void*>(&HookedShadowRenderDepth),
         reinterpret_cast<void**>(&g_originalShadowRenderDepth)},
        {"FProjectedShadowInfo::RenderProjection (stale distance-field shadow fix)", kRvaShadowRenderProjection,
         kPrologueRenderProjection, sizeof(kPrologueRenderProjection),
         reinterpret_cast<const void*>(&HookedShadowRenderProjection),
         reinterpret_cast<void**>(&g_originalShadowRenderProjection)},
    };
    InstallHooks(shadows, 2);

}

// ---- 1. XInput proxy ----------------------------------------------------------------------------------

HMODULE g_realXInput = nullptr;
INIT_ONCE g_realXInputOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK LoadRealXInput(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t path[MAX_PATH];
    UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (len == 0 || len + 16 >= MAX_PATH) {
        return TRUE;
    }
    wcscat_s(path, L"\\xinput1_3.dll");
    g_realXInput = LoadLibraryW(path);
    return TRUE;
}

FARPROC RealXInput(const char* name) {
    InitOnceExecuteOnce(&g_realXInputOnce, LoadRealXInput, nullptr, nullptr);
    return g_realXInput ? GetProcAddress(g_realXInput, name) : nullptr;
}

template <typename Fn>
Fn Real(const char* name) {
    static Fn fn = reinterpret_cast<Fn>(RealXInput(name));
    return fn;
}

}  // namespace

extern "C" {

DWORD WINAPI Proxy_XInputGetState(DWORD user, void* state) {
    auto fn = Real<DWORD(WINAPI*)(DWORD, void*)>("XInputGetState");
    return fn ? fn(user, state) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputSetState(DWORD user, void* vibration) {
    auto fn = Real<DWORD(WINAPI*)(DWORD, void*)>("XInputSetState");
    return fn ? fn(user, vibration) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetCapabilities(DWORD user, DWORD flags, void* caps) {
    auto fn = Real<DWORD(WINAPI*)(DWORD, DWORD, void*)>("XInputGetCapabilities");
    return fn ? fn(user, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
}

void WINAPI Proxy_XInputEnable(BOOL enable) {
    auto fn = Real<void(WINAPI*)(BOOL)>("XInputEnable");
    if (fn) {
        fn(enable);
    }
}

DWORD WINAPI Proxy_XInputGetDSoundAudioDeviceGuids(DWORD user, GUID* render, GUID* capture) {
    auto fn = Real<DWORD(WINAPI*)(DWORD, GUID*, GUID*)>("XInputGetDSoundAudioDeviceGuids");
    return fn ? fn(user, render, capture) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetBatteryInformation(DWORD user, BYTE type, void* info) {
    auto fn = Real<DWORD(WINAPI*)(DWORD, BYTE, void*)>("XInputGetBatteryInformation");
    return fn ? fn(user, type, info) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetKeystroke(DWORD user, DWORD reserved, void* keystroke) {
    auto fn = Real<DWORD(WINAPI*)(DWORD, DWORD, void*)>("XInputGetKeystroke");
    return fn ? fn(user, reserved, keystroke) : ERROR_DEVICE_NOT_CONNECTED;
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }
    DisableThreadLibraryCalls(instance);

    GetModuleFileNameW(instance, g_logPath, MAX_PATH);
    wcscpy_s(g_iniPath, g_logPath);
    if (wchar_t* slash = wcsrchr(g_logPath, L'\\')) {
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_logPath), L"ArkSplitscreen.log");
    }
    if (wchar_t* slash = wcsrchr(g_iniPath, L'\\')) {
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_iniPath), L"ArkSplitscreen.ini");
    }
    g_setSplitscreenFlag = GetPrivateProfileIntW(L"Settings", L"SetSplitscreenFlag", 0, g_iniPath) != 0;
    g_rebuildHUDs = GetPrivateProfileIntW(L"Settings", L"RebuildHUDs", 1, g_iniPath) != 0;
    g_fixLighting = GetPrivateProfileIntW(L"Settings", L"FixSplitscreenLighting", 1, g_iniPath) != 0;
    g_fixShadows = GetPrivateProfileIntW(L"Settings", L"FixSplitscreenShadows", 1, g_iniPath) != 0;
    wchar_t scale[32] = {};
    GetPrivateProfileStringW(L"Settings", L"SplitscreenRadialTextScale", L"0.63", scale, 32, g_iniPath);
    g_radialTextScale = static_cast<float>(_wtof(scale));
    GetPrivateProfileStringW(L"Settings", L"SplitscreenHUDScale", L"1.0", scale, 32, g_iniPath);
    g_splitscreenHUDScale = static_cast<float>(_wtof(scale));
    if (g_splitscreenHUDScale <= 0.0f) {
        g_splitscreenHUDScale = 1.0f;
    }
    GetPrivateProfileStringW(L"Settings", L"SplitscreenTooltipScaleY", L"1.0", scale, 32, g_iniPath);
    g_tooltipScaleY = static_cast<float>(_wtof(scale));
    if (g_tooltipScaleY <= 0.0f) {
        g_tooltipScaleY = 1.0f;
    }
    GetPrivateProfileStringW(L"Settings", L"SplitscreenRadialTextDistance", L"1.0", scale, 32, g_iniPath);
    g_radialTextDistance = static_cast<float>(_wtof(scale));
    if (g_radialTextDistance <= 0.0f) {
        g_radialTextDistance = 1.0f;
    }
    GetPrivateProfileStringW(L"Settings", L"SplitscreenToolbarScale", L"1.0", scale, 32, g_iniPath);
    g_toolbarScale = static_cast<float>(_wtof(scale));
    if (g_toolbarScale <= 0.0f) {
        g_toolbarScale = 1.0f;
    }
    g_noPvPRespawnTimer = GetPrivateProfileIntW(L"Settings", L"SplitscreenNoPvPRespawnTimer", 1, g_iniPath) != 0;
    LoadSplitscreenCvars();
    LoadToggleKeys();

    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* exeName = wcsrchr(exe, L'\\');
    exeName = exeName ? exeName + 1 : exe;
    Log("ArkSplitscreen loaded into %ls (base %p): SetSplitscreenFlag=%d RebuildHUDs=%d FixSplitscreenLighting=%d "
        "FixSplitscreenShadows=%d",
        exeName, reinterpret_cast<void*>(g_base), g_setSplitscreenFlag, g_rebuildHUDs, g_fixLighting, g_fixShadows);
    for (size_t i = 0; i < g_splitscreenCvarCount; ++i) {
        Log("splitscreen cvar: %ls=%ls", g_splitscreenCvars[i].name, g_splitscreenCvars[i].value);
    }

    if (_wcsicmp(exeName, L"ShooterGame.exe") == 0) {
        InstallFixes();
    } else {
        Log("not ShooterGame.exe; only proxying XInput");
    }
    return TRUE;
}
