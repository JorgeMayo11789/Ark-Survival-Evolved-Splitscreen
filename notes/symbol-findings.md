# ARK (preaquatica, build 19239750) — splitscreen-related symbols

Source: `ShooterGame.exe` + `ShooterGame.pdb` (2025-10-17), dumped with `tools/pdb_dump.py`.
RVA = offset from the exe's load base.

## Console splitscreen join flow (still compiled into the PC exe)

| RVA | Function |
|---|---|
| 00FC5F20 | `UShooterGameViewportClient::AddLocalPlayer(int ControllerId)` → bool |
| 00FC5960 | `UShooterGameViewportClient::CanAddSplitscreenLocalPlayer(int)` → bool (protected) |
| 00FC4BA0 | `UShooterGameViewportClient::CreateLocalPlayer(int)` |
| 00FC3FE0 | `UShooterGameViewportClient::RemoveLocalPlayer(int)` → bool |
| 00FC4470 | `UShooterGameViewportClient::OnRegisterSplitscreenLocalPlayerComplete(...)` |
| 00FC4630 | `UShooterGameViewportClient::OnSplitscreenPlayerJoinFailure(...)` |
| 00FC3F00 | `UShooterGameViewportClient::OnLocalPlayerAddedOrRemoved()` |
| 00FC3CE0 | `UShooterGameViewportClient::RecreateHUDs()` |
| 00FC3F60 | `UShooterGameViewportClient::ReAdjustUI()` |
| 00FC6410 | `UShooterGameViewportClient::InputKey(...)` (likely where a "press Start to join" would live) |
| 00FBD010 | `UShooterGameViewportClient::NotifyPlayerAdded(int, ULocalPlayer*)` |
| 00E51720 | `UShooterGameInstance::RemoveSplitScreenPlayers()` |
| 00E51AE0 | `UShooterGameInstance::GetUniqueNetIdFromControllerId(int)` |

## Splitscreen switches / UI scaling

| RVA | Symbol |
|---|---|
| 04982E0E | `bool GIsSplitscreen` (global) |
| 015C82D0 | `UVictoryCore::IsSplitscreen()` (static) |
| 02996400 | `UGameViewportClient::SetDisableSplitscreenOverride(bool)` |
| 02995D70 | `UGameViewportClient::UpdateActiveSplitscreenType()` |
| 00C034E0 | `UPrimalUI::ProcessSplitscreen(bool)` |
| 00C03590 | `UPrimalUI::ProcessSplitscreenScaling(bool)` |
| 003A8630 | `UPlayerHUDUI::ProcessSplitscreenScaling(bool)` |
| 0011ED50 | `UUI_PauseMenu::ProcessSplitscreenScaling(bool)` |

## Local profile (where the editor test crashed)

| RVA | Function |
|---|---|
| 00987610 | `UPrimalLocalProfile::Load(bool)` |
| 00989160 | `UPrimalLocalProfile::OnProfileBytesReady(bool, const TArray<uint8>&, int)` |
| 00983480 | `UPrimalLocalProfile::LoadFromFile(FObjectReader&)` → bool |
| 002399D0 | `UFogOfWar::LoadFromStruct(const FFogOfWarInfoStruct&)` (last log line before crash) |
| 002397F0 | `UFogOfWar::LoadFromFile(FObjectReader&, int)` |
| 009826F0 | `UPrimalLocalProfile::BeginPlay()` |
| 00983ED0 | `UPrimalLocalProfile::Save(bool)` |

Also present: `UShooterLocalPlayer::GetPersistentUser()` (01019D20), `UShooterLocalPlayer::SetControllerId(int)` (0101A260).

## What the code does (from disassembly, `tools/disasm.py` + `tools/xrefs.py`)

**Join trigger — `UShooterGameViewportClient::InputKey` (00FC6410).** The very first check: if the key is
`EKeys::Gamepad_Special_Right` (Start / Menu button), call `AddLocalPlayer(ControllerId)`. No platform check.

**`AddLocalPlayer(ControllerId)` (00FC5F20).** If `CanAddSplitscreenLocalPlayer` passes and a "spawning" flag
(this+0x1B0) isn't set: set the flag, start a 5 s timer to clear it, then on PC go straight to
`CreateLocalPlayer(ControllerId)` (the console login-UI branch is skipped by a stub that returns true).

**`CanAddSplitscreenLocalPlayer(ControllerId)` (00FC5960)** returns false unless ALL of:
1. at least 1.0 s since a timestamp at this+0x158 (cooldown);
2. exactly **1** local player exists (max 2 players total);
3. no local player already uses that ControllerId;
4. the viewport is in playing state (`IsInPlayingState`) or this is PIE;
5. if NetMode == Client: GameState byte +0x59D must be set (server allows splitscreen);
6. if NetMode == ListenServer: a WorldSettings player array must have < 8 entries;
7. player 1 must already have a character — otherwise it shows a HUD notification and refuses;
8. not during a Genesis 2 cinematic.

**`CreateLocalPlayer(ControllerId)` (00FC4BA0)** calls `UGameInstance::CreateLocalPlayer(ControllerId, Err, bSpawnActor=true)`
— the same engine path as Blueprint `Create Player` with Spawn Pawn on — then, on listen server/client with
`IsSplitScreen`, registers the player with the online session.

**Profiles — `AShooterPlayerController::SetPlayer` (01071F90).** Each local player controller creates its own
`UPrimalLocalProfile` via `NewObject` (outer = the controller), stores it at controller+0xA38 and calls
`BeginPlay`. It also creates a per-controller `UFogOfWar` at +0xA30. `LoadProfile` → `UPrimalLocalProfile::Load`.
Both players read the same `PlayerLocalData.arkprofile`.

## Real-game test 1 (vanilla game, `bUseSplitscreen=True`, Menu pressed on pad 2)

Crash log `Saved\Logs\10.14-00.18.54.crashstack`:
`InputKey → AddLocalPlayer → CreateLocalPlayer → UGameInstance::CreateLocalPlayer → ULocalPlayer::SpawnPlayActor
→ UWorld::SpawnPlayActor → AShooterGameMode::PostLogin → StartNewPlayer → StartNewShooterPlayer` (shootergamemode.cpp:3720).

Exe base that run: `0x7FF6657F0000` (derived from the AddLocalPlayer/InputKey return addresses), so the crash is at
**RVA 00EDE14D**:

```
00EDE0DF  call  AShooterGameMode::GetPlayerDataFor          ; PlayerData
00EDE10A  mov   rax, [PC + 0x480]                           ; PC->PlayerState
00EDE12F  add   rax, 0x4C0                                  ; &PlayerState->UniqueId (shared ptr)
00EDE138  call  <get raw pointer>
00EDE14D  mov   rax, [rax]        <-- CRASH: UniqueId pointer is null
00EDE158  call  [rax + 0x70]                                ; UniqueId->IsValid()
```

The source is roughly `if (PlayerData && PC && PC->PlayerState && PC->PlayerState->UniqueId->IsValid() ...)`:
it checks that the shared pointer object exists but not what's inside it. **Player 2 has no UniqueNetId** — on
Steam only the signed-in user gets one; on consoles the second player had their own account.
