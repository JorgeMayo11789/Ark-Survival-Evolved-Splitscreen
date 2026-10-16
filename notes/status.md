# Status (2026-10-14)

## Working (tested in-game, preaquatica 358.26)
- Player 2 joins with **Menu/Start on controller 2** (the game's own console join code).
- Player 2 gets a stable fake id (player 1's Steam id + 1) — no crash on join.
- Player 2 has their own save file (`SavedArksLocal\<id>.arkprofile`) — no character takeover, black screen or
  respawn loop.
- Both players get their own HUD + hotbar in their half of the screen.
- Opening one player's inventory no longer makes the other player raise their hand.
- **Lighting:** fixed. Cause: with two views the renderer issues clears "everything except this view's rect" once
  per view (e.g. exclude (0,0)-(1805,2031) and (1804,0)-(3610,2031) side by side), so each view's clear wiped the
  other view's scene/G-buffer textures; only the 1-pixel overlap kept its lighting. The DLL skips clears with an
  exclude rectangle while 2+ players are in (hook on FRHICommand<FRHICommandClear>::ExecuteAndDestruct, RVA
  026F1720; FRHICommandClear: +0x10 color, +0x20 ExcludeRect, +0x30 depth, +0x34 stencil, +0x38/39/3A flags).

## Ruled out along the way (for reference)
GIsSplitscreen, distance-field AO, every shadow setting, auto-exposure, SSAO, tonemapper, fog (height and
atmospheric), TrueSky (sky, clouds, loss/scatter textures), tiled lighting, reflections, lighting cache, HZB,
simple lighting, resolution scale. RenderDoc couldn't attach (device lost with Steam/NVIDIA overlays hooked).

## Shadows (2026-10-14)
Symptom: grey shadows from around P2 in P1's half, P2 shadows very dark in spots. Cause: distance-field
(ray-traced) sun shadows; F-key test with r.DistanceFieldShadowing=0 removed it (user prefers the look).
Fix: GDistanceFieldShadowing (RVA 04938B6C) = 0 while 2+ players, restored after (ini FixSplitscreenShadows).
Ruled out on the way: shadow cache (GEnableShadowCache already off), per-view CSM projection (DependentView always
matched; each view gets 3 cascades + 1 DF pass), the skipped exclude-rect clears (light-attenuation clear is a
normal full clear, kept), r.DFFullResolution. Turning off only r.DFShadowAsyncCompute (the console's GIsSplitscreen
path) reduced the cross-talk but didn't remove it.
Second part (P2 "super dark" hard shadows; P1 got them while an inventory preview was open): a shadow trace showed
each view gets 3 cascades (RenderDepth then RenderProjection, one per atlas pass, all at atlas 0,0) plus one
distance-field shadow (flags +0x594 bit 11) that is projected without any RenderDepth. After its ray-traced pass,
RenderProjection (037C1EC0) can continue into a normal shadow-map projection (bit 16 set, checks at 037C1FC5) that
samples whatever the atlas last held -- usually the other view's cascade. Fix: while the DLL has distance-field
shadows off, skip RenderProjection for bit-11 shadows (no visible change; kept since it's logically right).
STILL OPEN: P2's regular (cascaded) sun shadows are wrong -- characters' backs/rocks shadowed that are sunlit for
P1 at the same spot. Facts: follows the 2nd view rendered, not screen position (swapping the halves via
UGameViewportClient::SplitscreenInfo at +0x70 moved it with P2 to the top); an extra scene render before the main
one (inventory character preview) makes P1 bad too, so "only the first view rendered in a frame is right";
1 cascade (r.Shadow.CSM.MaxCascades=1) still bad; r.Shadow.DistanceScale=0 removes it. Ruled out: pass order and
DependentView (trace: D then P per cascade, each view its own), viewport/scissor order, GetScreenToShadowMatrix
(standard), UpdateWorldToLight per-view overrides (View+0x454/+0x49c never set on PC), CSM transition scale.
2026-10-14 observation: while P1 is dead (respawn menu), P2's dark shadows are GONE -- P1's 3D view presumably
isn't rendered then, so P2 is the only/first view. Confirms "only the first view per frame is right".
Next ideas: per-view state in FSceneViewState used by shadows (P2's ViewState is the 2nd), something in
InitDynamicShadows that runs only for Views[0], GetShadowSplitBounds' virtual calls (+0xE0/+0xA0).
Session 2 (2026-10-14 evening) findings:
- Screenshots: with sun shadows on, nearly ALL of P2's half is "in shadow" (sunlit tree tops, characters' backs);
  r.Shadow.DistanceScale=0 makes P2 look like P1. Per-character (per-object, flags 0x021, DependentView null,
  drawn once and shared) shadows are ALSO wrong in P2 -> the shadow MAPS are fine, P2's projection/receiver
  side is wrong. "Only the first view rendered in a frame is right", across scene renderers (inventory preview).
- Diagnostics: P2's cascades are centred on P2's camera; D/P order correct; RenderDepth restores the view's
  matrices/UB properly (037C1A19..037C1BCB).
- Fixed anyway (logically right, no visible change): RenderDepth picks the FIRST relevant view, not DependentView
  (loop 037C1868..037C1999) -> DLL narrows SceneRenderer->Views (+0x90/+0x98, stride 0x1C20) to the shadow's
  DependentView during the call. Skip of bit-11 (DF) projections kept.
- Ruled out by test: stencil optimization (GStencilOptimization RVA 04938C6C off), depth bounds (D3D11 RHI
  EnableDepthBoundsTest is a no-op; GSupportsDepthBoundsTest=0 -> stencil path), r.ParallelShadows,
  r.RHICmdStateCacheEnable, r.LightPropagationVolume, r.AllowCachedUniformExpressions (small change on both),
  r.LightFunctionQuality, CSM transition, swapping the screen halves (bug follows P2).
- Ruled out by reading: UpdateWorldToLight overrides (View+0x454/+0x49c never set on PC), GetScreenToShadowMatrix
  (standard), TShadowProjectionPS::SetParameters (037EB5B0) / FShadowProjectionShaderParameters::Set (037BD4C0)
  (shadow texture from GSceneRenderTargets globals 049F4428/049F4438 by flag bit 3 -- bit 3 not set),
  UpdateDownsampledDepthSurface (loops all views), RenderDynamicSkyLighting (loops views; movable skylight only),
  ARK "AmbientShadow" (baked reflection-capture data), GUpdateStencilInfo (gameplay outlines).
- GFrameNumberRenderThread users worth checking next: FHierarchicalStaticMeshSceneProxy::GetDynamicMeshElements /
  AcceptOcclusionResults (foliage), FSkeletalMeshSceneProxy::GetMeshElementsConditionallySelectable,
  FGPUSkinCache, FStaticMeshSceneProxy::WillRender, FTrueSkyPlugin::RenderFrame.
- Recommended next step: a GPU frame capture (NVIDIA Nsight Graphics; RenderDoc crashed ARK) to look at P2's
  shadow projection inputs directly.
2026-10-15: inventory open -> its character preview renders first -> P1 gets the dark shadows; P2 opening theirs
affects P1 too. Ruled out: a GFrameNumberRenderThread-keyed cache (bumping it before every RenderProjection /
RenderDepth changed nothing), scene-depth resolve (ResolveSceneDepthTexture/ToAuxiliary use full-texture params).
2026-10-15 RenderDoc capture (works with Ansel off: NvCameraEnable.exe off; captures\ark_frame6243.rdc; scripts in
renderdoc\scripts). Shadow maps, projection (light attenuation 71613) and the sun lighting pass are CORRECT and
identical for both views. Per-pixel trace (pick_stages.py): P2's shadowed skin goes to exactly 0 at event 15894 --
a single FULL-FRAME (1808x1016) draw, ps 71703 = screen-space subsurface scattering blur (samples the SSS
irradiance buffer 71696 with the 32x64 kernel texture 71693, scene color 6961, depth), using P2's View UB (6067),
UV range cb0[18] = (1, 1, 0.998, 0.501) = one view's rect. It should run per view. CONFIRMED: r.SSS.Scale=0 removes P2's dark shadows. FIX: r.SSS.Scale=0 added to the
splitscreen cvar overrides by default (ini [Settings] SplitscreenSubsurfaceOff=1), restored when P2 leaves.
Possible later improvement: make the SSS pass run per view instead of disabling it.
Black textures when the players are very far apart: GONE after the SSS fix (user, 2026-10-15) -- likely the same
full-frame SSS pass sampling with the other view's parameters.

## HUD notifications region -- FIXED 2026-10-14 (banners confirmed for both players)
The overlay draws each player's notifications via AShooterHUD::DrawHUDNotifications (00FEFFA0) on one full-screen
UCanvas (1599x899 virtual) and only clips per player. Hook narrows the canvas to the player's half for the call:
OrgY (+0x2C) += ClipY/n*i, ClipY (+0x34) = ClipY/n, SizeY (+0x44) = SizeY/n (UCanvas: +0x28 Org, +0x30 Clip,
+0x40 Size). Fixes P2's top banners (LEVEL-UP / You feel cold / Equip a tool -- ShowInfoItems 00FEF570 places them
from canvas size) and P1's item notices. Replaced the earlier DrawItemHUDNOtifications-only SizeY hack.
Checked first-player getters in HUD code: AShooterHUD::DrawHUD's GetFirstLocalPlayerController checks only offset
full-screen effect overlays (-5000 vs 0); DrawUIHUD's GetFirstGamePlayer derives the splitscreen slot -- both fine.

## Item notifications -- FIXED 2026-10-14 (confirmed)
AShooterHUD::DrawItemHUDNOtifications (00FF6610) starts at canvas SizeY(+0x44)/2 = middle of the whole screen
(the split line); P1's overlay is clipped to the top half so P1's notices were cut off, P2's showed at the top of
its half. Hook scales SizeY to SizeY*(2i+1)/n during the call -> middle of each player's own half.
Radial labels: two menus -- MultiUse (00C55940; text 0.7 @00C55AF4, ring local [rsp+F4h], label mults
00C561D4/00C561EF) and PlayerAction (whistles, 00C60460; text 0.7 @00C60BF5 -> [rsp+98h], ring 0.72 @00C60C06 ->
[rsp+CCh], label mults 00C63065/00C63080). ini SplitscreenRadialTextDistance (0.65) / TextScale (0.5). Confirmed.
Still open: P2's "LEVEL-UP IS AVAILABLE" banner doesn't show (P1's does).

## HUD notifications (investigation notes)
Symptom: P1 never shows the left-side "ADDED x1 ..." item notices; P2 never shows the "LEVEL-UP IS AVAILABLE"
banner. (P1's banner and P2's item notices are fine.)
Path: UPrimalInventoryComponent::ClientItemMessageNotification / UPrimalItem::AddToInventory / SetQuantity ->
GetLocalOwnerHUD (00841580: owner Controller(+0x4A0), IsLocalController -> PC+0x4E8 MyHUD; per player, correct)
-> AShooterHUD::AddHUDNotification (00FFA9E0) -> TArray<FHUDNotification> HUD+0xA30 (num +0xA38).
Drawn only from UHUDOVerlayWidget::DrawWidget (00280100 -> 0028118D) -> AShooterHUD::DrawHUDNotifications
(00FEFFA0) -> DrawItemHUDNOtifications (00FF6610). The overlay finds its player via UPrimalUI::GetParentPrimalUI ->
vtable+0x3D0 -> GetShooterHUD, and in splitscreen (IsSplitscreenPlayer) builds a per-player rect from canvas
size, GetSplitscreenPlayerCount, index and GameUserSettings+0x13C (scale), then FCanvas transform math.
Diagnostics (TEMP hook on DrawHUDNotifications): BOTH HUDs are drawn every frame, P1's queue holds its notices
and they are laid out (y 9 -> 48); canvas is 1599x899 for both. P2 reserves the banner space (y 9 -> 48 with 0
queued) but it's not visible. So each player has one element drawn but not visible -> next: the overlay's
per-player FCanvas transform/clip in DrawWidget (after 00280339), and which UI tree hosts each element.

## Controller hints for player 2 (fixed 2026-10-14)
UShooterGameViewportClient::Tick computes ONE "gamepad active" value (+0x1B1) and broadcasts it to every UI via
CheckGamepadActiveChanged, so a keyboard P1 hid P2's controller hints. Hooks: FSlateApplication::
GetControllerActive (022E6570) returns true for ControllerId > 0 in splitscreen; UPrimalUI (00C05A90) and UUI_Hub
(0141D460) ::CheckGamepadActiveChanged force true for UIs owned by ControllerId > 0. Confirmed.

## Camera mode per player (fixed 2026-10-14)
Third-person toggle = AShooterCharacter::ZoomOut/ZoomIn (00C9F990/00C9F700) which write shared
GameUserSettings+0x149 (=1/0) and save; SetCameraMode (00CD36E0) writes +0x148/+0x149 too; every local
character's Tick (call GetGameUserSettings at 00CBEC04, then reads +0x149) forces third person when set. DLL keeps
a separate copy for players 2+ (g_secondaryCameraSettings, in-memory, starts as P1's): their ZoomIn/ZoomOut/
SetCameraMode see and save into it and the shared bytes are restored; their Tick read gets the copy via a stub
(character at caller [rsp+3910h]). Confirmed. Still shared for P2: LocalPossessedBy (00C7D59D), PlayDying,
ClearRidingDinoAsPassenger read +0x149.

## HUD scale
SplitscreenHUDScale (ini, default 1.0) multiplies GameUserSettings+0x13C (the options UI Scale) while 2+ players
are in (set before the join HUD rebuild, restored on leave). Tested at 2.0: right-side icons, top banner and
inventory doubled, the TOOLBAR did not. Toolbar = UPlayerHUDUI::ProcessSplitscreenScaling (003A8630), which picks
fixed scales via `movss xmm0,[rip+X]`: 0.75 @003A8720, 1.5 @003A872E, 1.0 @003A8806, 2.0 @003A8814 (position offset
85 * UI scale @003A88BD). In splitscreen it multiplies widgets UPlayerHUDUI+0x650 and +0x658 (their +0x100
FVector2D) by (0.75, 1.5) and +0x5D8 by (1.0, 2.0); +0x5F0's slot position is moved by 85*UIScale.
Tried: SplitscreenToolbarScale repoints the 0.75/1.5 loads (RepointFloatLoad) -> patch applied, NO visible change
on the toolbar, so +0x650/+0x658 aren't the hotbar (or the scaling runs before/without effect). Left at 1.0.
FINAL (2026-10-14, confirmed): only the toolbar CONTAINER is scaled -- "ItemSlotPanelChild" (UPlayerHUDUI+0x5D8)
holds both the item slots (+0x588) and the controller icons (+0x5F8) (verified via UWidget::Slot +0x30 ->
UPanelSlot Parent +0x28 / Content +0x30). The game sets its scale/translation in UpdateQuickBarScaling (003ADD80,
every tick); the DLL multiplies the game's value (re-read whenever the game changes it, so no compounding), pivot
bottom-centre, and uses k=1 while +0x640 (slots hidden = inventory open) is set. Scaling the slots/icons
individually as well had double-scaled them (2.25x). The active-slots highlight only shows after the game switches
to the controller scheme (game behaviour).
Earlier attempt notes: hook UPlayerHUDUI::UpdateItemSlotVisibility (003ADCC0, every HUD tick)
-> item-slot widget UPlayerHUDUI+0x588 -> UWidget::SetRenderTransformPivot(0.5,1) + SetRenderScale(orig*k)
(0273C410; scale lives at widget+0x100). ini SplitscreenToolbarScale (1.5 now). (+0x588 is null during
ProcessSplitscreenScaling, so that hook didn't work.) Also scaled with the same pivot: "GamepadQuickbarButtons"
(+0x5F8, controller icons) and "ItemSlotPanelChild" (+0x5D8; its unscaled frame showed as a faint 4-slot box when
P2 joined). Widget names come from UPlayerHUDUI::InitializeWidgets (003A8960). All confirmed fixed in-game.
RepointFloatLoad helper is unused now.

## Teleport button -- BUILT 2026-10-14, UNTESTED (user was away)
P2's "Deactivate Gamepad" button (UUI_PauseMenu+0x5B0, pointless for a controller-only P2) is relabelled
"TELEPORT TO PLAYER 1" in a hook on UUI_PauseMenu::AddToViewport (014FB850): label found by walking the button's
children (UPanelWidget::GetChildrenCount 0272D550 / GetChildAt 0272D560, IsA 0221D9A0 with UTextBlock::StaticClass
00180CB0 / UPanelWidget::StaticClass 0017F040) and set with UTextBlock::SetString (02735560). Hook on
ClickedButton (015009E0) for that widget + owner index > 0: refuses while riding (IsRidingDino 010BC7C0), else
AShooterCharacter::TeleportTo (00D4B180) to P1's location (P1 char via GetGameInstance 02861330 ->
GetFirstLocalPlayerController 0297A2C0 -> GetPlayerCharacter 0109D860; location = Actor+0x250 RootComponent ->
+0xF0) + offsets {150,0},{-150,0},{0,150},{0,-150},{250,250},{-250,-250},{0,0}, z+50, first success wins.
Controller presses go through UUI_PauseMenu::ClickedButtonWithController (015006F0) -> UPrimalUI base -> should
reach ClickedButton. Log lines: "teleport: ...", "pause menu: no label found ...".

Teleport CONFIRMED working in-game. Follow-up built (not yet deployed -- ARK was running): after a successful
teleport the pause menu closes via its vtable +0x3C8 (what ClickedButton calls after opening the admin screen);
TeleportToFirstPlayer now returns bool.

## DONE (confirmed in-game): hotbar/item tooltip squashed in splitscreen
PatchTooltipScale hooks InitToolTip of UPrimalItemToolTipWidget (0095BCB0), UPrimalItemInventoryToolTipWidget
(0094D260) and UTooltipSmartLayoutWidget (012B0080) (same 20-byte prologue) and after the original calls
UWidget::SetRenderScale(tooltip, (1, SplitscreenTooltipScaleY)). Hold-hotbar tooltip = SmartLayout + Inventory
variants. User settled on SplitscreenTooltipScaleY=2.0 (full height). World tooltips (ground items, dinos, structures)
come from UUI_HudOverlay::ShowOverlayTooltip (01422900, not inside the squashed HUD) -> skipped via a depth flag
(confirmed 2026-10-15). Inventory item/button tooltips were stretched too -> stretch only while UPrimalUI::ShowToolTip
(00BC4DB0) runs on a UPlayerHUDUI (StaticClass 01014A60) (confirmed 2026-10-15). TEMP logs + old shadow diag code removed.

## DONE (confirmed in-game 2026-10-15): Swap screens (P2 pause menu)
Button +0x5E0 (InitializeButtonNameWithClick(FName at +0x568), collapsed for non-first players) made Visible and
relabelled "SWAP SCREENS"; click closes the menu, then SwapScreens(): swap OriginX/Y of players 1/2 in
SplitscreenInfo (+0x70) types 1-2, LayoutPlayers (02995ED0), UShooterGameViewportClient::ReAdjustUI (00FC3F60 =
RecreateHUDs + RescaleOpenScenes). Undone when P2 leaves. Everything that places HUD by player is MIRRORED while swapped
(partial flips make things vanish -- learned the hard way):
- `(owner == first player) ? a : b` immediates exchanged (g_halfIndexSites): radials C4D5F4/C5006C(whistle)/C53D9F/
  C55A9C/C60B9D, DrawUIHUD FD2802, ShowInfoItems FEF681 (harvest list), ProcessSplitscreenScaling C03742 (UMG HUD),
  and the -5000/0 sites in DrawUIHUD/DrawHUD (FE4A75..FE9AB5).
- IsSplitscreenPlayer index xor'd: UHUDOVerlayWidget::DrawWidget clip region (stub at 00280BBB), UUI_HudOverlay::
  AddToViewport SetAdditionalScissorRect top/bottom (cmp at 0141E48B) and Tick cursor mapping (01421076) -- the
  HudOverlay scissor was the last missing piece (P1's whistle/harvest vanished until it was flipped).
- DrawHUDNotifications canvas narrowing uses index ^ swapped.
Diagnostics found: overlay FCanvas RT rect is full screen (0,0)-(1920,1080) for both, canvas transforms identical.
## DONE (confirmed 2026-10-15): player 2's character sounds (grunts, pickaxe swing) muffled, even next to player 1
UI sounds fine. Ruled out by reading: USoundNodeLocalPlayer::ParseNodes (01267DB0) uses Pawn->IsLocallyControlled()
(true for P2 too); listeners ARE set per view (UGameViewportClient::Draw -> FAudioDevice::SetListener 02889820);
FActiveSound::UpdateWaveInstances (0285E630) uses FindClosestListener (0285E4B0), then CheckOcclusion (0285F4B0:
RaycastTest from sound to listener, ignores the sound owner -- for a pickaxe swing the owner is the weapon, so P2's
own body could occlude), HandleInteriorVolumes (listener interior settings), ApplyAttenuation (LPF).
F9 occlusion skip: no change (log confirmed toggling) -> occlusion ruled out, TEMP code removed.
CAUSE FOUND: FAudioDevice::SetListener (02889820) grows Listeners (stride 0xA0) with memset 0, then tail-jumps to
FListener::ApplyInteriorSettings (02889760), which returns early if Volume == Listener+0x60 -- outdoors both null, so
listener 2's FInteriorSettings (+0x3C, 0x24 bytes; +0x58 = InteriorLPF) stay all zero. HandleInteriorVolumes
(0285FA90) multiplies HighFrequencyGain by lerp(.., InteriorLPF) = 0 for spatialized sounds whose closest listener
is P2's -> P2's own sounds lose all treble. FIX (confirmed working): HookAudioListeners re-applies the passed
settings (forcing the volume compare) when a listener's interior settings are all zero, else engine defaults.
## DONE (confirmed 2026-10-15, invite + tribe join work): Players can't target each other / no "hold X to invite to tribe"
AShooterPlayerController::GetAimedUseActor (0105C080, vtable +0xE08) traces via APrimalCharacter::GetAimedActor
(004FD590) on channels 5 and 26 (use channel) with max distance ~200 + extra. Diagnostics: never returned the other
player's character, trace hit nothing even point-blank; both LOCAL characters' capsule AND mesh (Character+0x508)
respond Ignore (0) to channels 5/26 (local characters ignore the use channel so your own body doesn't block your aim;
online the other player is remote and blocks). Fix: while 2+ players, HookedDrawHUD sets each HUD owner's character
mesh SetCollisionResponseToChannel(26, Block) (02AE5060; getter 02AE54A0). Own trace ignores self (AddIgnoredActor).
## DONE (confirmed 2026-10-15): World streaming around both players
Rocks/foliage vanished for BOTH players when far apart: UWorldComposition::UpdateStreamingState(FSceneViewFamily*)
(02C789F0) streams around the AVERAGE of all views' positions (sum of View+0x190 or view actor's vtable+0x1190,
divided by count). Fix: that hook records each view's origin (View+0x190); GetDistanceVisibleLevels (02C76B00;
args: this, location, visible TArray, hidden TArray, TSet<FName>&, int&, int&, bool&, bool*) runs once per position.
Visible lists merged by TileIndex (no duplicates; lower LOD index = more detail wins), hidden = only tiles nobody
needs. FDistanceVisibleLevel = {int TileIdx; ULevelStreaming*; int LOD} (24 bytes). Temp arrays freed with
FMemory::Free (00194340); appends via FMemory::Realloc (020C5FF0). Name set shared (union).
## Watch: crash on the loading screen entering the world (2026-10-15 14:18, once)
SWidget::SlatePrepass in the Slate loading thread (FSlateLoadingThreadTask / SlateThreadRunMainLoop), no mod frames on
the stack, single player at the time. Likely game-side race (game thread changing widgets while the loading thread
draws). If it repeats on world entry: check whether a mod hook touches widgets during the load.
## DONE (confirmed 2026-10-15): P1 dropping out of third person when P2 uses LT in the inventory
Input log: P2's LT press in the inventory -> Slate synthesizes a mouse-wheel event -> FSceneViewport::OnMouseWheel ->
UShooterGameViewportClient::InputKey(controller 0, MouseScrollUp) -> P1's input stack -> ZoomIn (first person).
Call chain: XInputInterface::SendControllerEvents -> FSlateApplication::OnControllerAnalog (022E5C20) -> OnMouseWheel ->
SViewport/FSceneViewport::OnMouseWheel (always controller 0). Fix: OnControllerAnalog hook records the controller id;
InputKey (00FC6410) re-targets MouseScrollUp/Down (EKeys 049BFFE8 / 049C0000) from controller 0 to it. First fix
(swallow while any pad trigger held) worked; replaced by the re-route per user request. Diagnostics removed.
## DONE (confirmed 2026-10-15): Dino name labels
Labels only showed when BOTH players were near the dino / flickered. APrimalDinoCharacter::DrawFloatingHUD (006F3060,
interface this = actor+0x6B8) draws only if TimeSince(this+0x9F0 = actor+0x10A8) > 1 s; DrawDinoFloatingHUD sets that
to now + rand()/RAND_MAX when the dino is too far from the drawing player (006F3CCD..006F3D21). One value shared by
both HUDs -> per-player copy swapped in/out around the call (HookDinoLabels). Diagnostic showed "asked" equal for both
players, "drawn" collapsing for both together.
## DONE (confirmed 2026-10-15): P1 pause menu "TELEPORT TO PLAYER 2"
InviteFriendsButton (+0x5F8; collapsed in this layout, only looked up, never click-bound) shown + relabelled for P1 in
splitscreen, click bound via UPrimalUI::InitializeButtonNameWithClick(FName) (00BCC7E0, FName ctor 0215CDF0),
ClickedButton -> TeleportNextTo(P1, LocalPlayers[1]->PlayerController). PvP respawn timer
(AShooterGameMode+0xE48 bIncreasePvPRespawnInterval) off in splitscreen (confirmed in log). World streaming fix
confirmed by user.

## (done) P2 pause-menu buttons (teleport to P1, swap screens)
P2's pause menu shows Resume / Leave Game / Deactivate Gamepad. UUI_PauseMenu::AddToViewport (014FB850) hides
buttons for non-first players with UWidget::SetVisibility (0273D030) on members +0x628 and +0x5E0 (after
GetFirstGamePlayer at 014FC8B0) and relabels the exit text (+0x5C8) via UTextBlock::SetString (02735560) to
"LEAVE GAME". Widget names used: InviteFriendsButton, HelpButton, SurvivorProfileButton, AdminCommandButton,
AdminMenuButton, EnableCreativeModeButton, DisableGamepadButton, PlayerListLabel, PressStartLabel,
GameOptionsTextBlock. Clicks: UUI_PauseMenu::ClickedButton (015009E0, compares the UWidget* to members).
Plan: map member offsets -> names (log once), show two hidden buttons for P2, relabel, intercept ClickedButton:
teleport = UCharacterMovementComponent::FindGoodSpot + AActor::SetActorLocation near P1 (as the listen-server
tether does); swap = swap SplitscreenInfo origins (+0x70 table; code from the shadow experiment) + HUD rebuild.

## Tether -- NOT NEEDED (2026-10-16)
Consoles tether the players because the world only streamed in around one point (the average of the views), so
rocks/foliage vanished when the players split up. Streaming now loads the world around every player (see "World
streaming around both players"), so the players can be far apart without a tether. Kept for reference only:
The game's listen-server tether still exists: AShooterGameMode::ListenServerClampPlayerLocations (RVA 00F24770),
called from AShooterGameMode::Tick at 00F00288 only when GetNetMode()==2 (jne at 00F0027E). Anchor = first local
player's pawn; range 23000 (__real@46b3b000) x GListenServerTetherDistanceMultiplier (written by GameState::Tick
from the server setting); warns via ClientNotifyListenServerOutOfRange, teleports with FindGoodSpot +
SetActorLocation, handles dinos/rafts/seats. It skips controllers where IsLocalController (vtable +0xAA8, call at
00F252B8) is true, so P2 would need the same "index>0 = not local" stub as the save-data sites.

## OPEN: dark shadows come back after a death/respawn + far apart (reported 2026-10-16, while recording)
Repro: P1 killed P2; P2 respawned, then P1 respawned far away -> P2's half had the old very-dark-shadow look.
Teleporting P2 to P1 moved the dark look over to P1's half (follows the location/view, not the player?).
The 2 -> 1 -> 2 player-count flips in the log (~19:23) were the user leaving and rejoining as P2 to try to clear
it -- that did NOT help, even though each rejoin re-applied r.SSS.Scale=0 and the shadow/stencil settings. So it
isn't our cvar state; the darkness seems tied to the place (it moved to P1 when P2 was teleported to P1's spot).
Ideas: per-location lighting/shadow data (e.g. distance-field / cached shadow maps, light-attenuation or a streaming
level's lighting that only one view loaded), or the streaming fix interacting with the respawn. Next time: note the
map spot, compare RenderDoc captures of the dark vs. normal half. Not fixed yet.

## OPEN (low priority): "name your dino" prompt after player 2's tame (reported 2026-10-16)
Opens over player 1's half and takes player 1's input. Possibly the console behaviour too; text entry needs the
keyboard (player 1) anyway, so maybe leave it. If fixed: find where the tame-naming popup is created and which
player controller / viewport half it is added to. Taming itself works for player 2.

## Not done / ideas
- Side-by-side layout: renders correctly, but ARK's HUD layout code only supports top/bottom.
- 3-4 players: the console code caps at 2 (`CanAddSplitscreenLocalPlayer` requires exactly 1 existing player).
- Online/listen-server play: untested; single-player only.

## Test setup
- `build\xinput1_3.dll` + `build\ArkSplitscreen.ini` in `ARK\ShooterGame\Binaries\Win64\`.
- `bUseSplitscreen=True` under `[/Script/EngineSettings.GameMapsSettings]` in
  `ARK\ShooterGame\Saved\Config\WindowsNoEditor\Engine.ini`.
- Single-player only, without BattlEye. Keep `SavedArksLocal` backed up.
- Log: `ARK\ShooterGame\Binaries\Win64\ArkSplitscreen.log`.
