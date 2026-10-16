# ArkSplitscreen

Brings back 2-player splitscreen (top/bottom) in **ARK: Survival Evolved** on Steam PC. The console versions have
splitscreen; the PC game still contains that code, but it is switched off and broken. This mod is a small DLL that
switches it back on and fixes what breaks.

**Single-player only. Never use it on official or BattlEye-protected servers.**

> **How this was made:** all of the code, the reverse engineering and the analysis tools were written by
> **Claude** (Anthropic's AI model), working in Claude Code. I tested every change in-game and found what needed
> fixing by playtesting, giving Claude notes on each bug (what happened, when, and screenshots) until it worked.

## Requirements

- ARK: Survival Evolved on Steam, **`preaquatica` beta branch, version 358.26** (build 19239750).
  The DLL checks every spot it patches first; on any other game version it skips its fixes (and says so in its log)
  and only passes controller input through.
- A controller for player 2 (XInput, e.g. an Xbox controller). Player 1 can use mouse and keyboard or controller 1.
- Launch the game **without BattlEye**.

## Install

1. Back up `ARK\ShooterGame\Saved\LocalProfiles` and `ARK\ShooterGame\Saved\SavedArksLocal`.
2. Download the latest release and copy `xinput1_3.dll` and `ArkSplitscreen.ini` into
   `ARK\ShooterGame\Binaries\Win64\`.
3. Open `ARK\ShooterGame\Saved\Config\WindowsNoEditor\Engine.ini` and add:
   ```
   [/Script/EngineSettings.GameMapsSettings]
   bUseSplitscreen=True
   ```
4. Start the game without BattlEye and load a single-player world.
5. Once player 1 has spawned, press **Menu/Start on controller 2** to join. Player 2 gets their own character and
   save.

## Uninstall

Delete `xinput1_3.dll`, `ArkSplitscreen.ini` and `ArkSplitscreen.log` from `Binaries\Win64`, and remove the two
lines from `Engine.ini`.

## Features

**Joining and saves**
- Player 2 can join without crashing (they get a stable id derived from player 1's).
- Player 2 has their own save file, so no character takeover, black screen or respawn loop. The same two players
  always get the same characters back.

**Per-player HUD**
- Each player gets their own HUD, hotbar, notifications and banners in their half of the screen; HUDs are rebuilt
  when someone joins or leaves.
- Controller button hints for player 2, readable radial (hold-E / whistle) menus, correctly sized tooltips, item
  thumbnails and hotbar.
- Dino name labels show for each player separately (before, both players had to be near the dino).

**Pause menu**
- Player 1: **Teleport to Player 2**.
- Player 2: **Teleport to Player 1** and **Swap Screens** (swaps which half each player is in, HUD included).

**Graphics**
- Fixes the black lighting and the very dark shadows on player 2's half.
- Fixes black textures when the players are far apart.

**World**
- The world (rocks, foliage, terrain) loads around **both** players, so you can play far apart; no tether needed.
- The PvP respawn timer is turned off while two players are in.
- Players can target each other, so "hold to invite to tribe" works.

**Controls and sound**
- Each player keeps their own first/third-person camera; player 2's triggers in the inventory no longer switch
  player 1's camera.
- Player 2's character sounds (grunts, tool swings) are no longer muffled.

## Settings

`ArkSplitscreen.ini` (next to the DLL) has comments for every option: HUD and hotbar size, tooltip and radial-menu
text size, the graphics fixes, and optional render settings applied only during splitscreen (e.g. a lower
`r.ScreenPercentage` for frame rate). The DLL writes `ArkSplitscreen.log` next to itself; include it when reporting
a problem.

## Known limits

- 2 players only; the game's console code caps splitscreen at two.
- Top/bottom only; ARK's HUD layout code doesn't support side-by-side.
- Single-player only; online and listen-server play are untested.
- Only the version listed above. A game update will need an updated DLL.

## What has and hasn't been tested

**Tested:**
- Playing on **The Island**, with both players together and with one player spawned far away from the other. That
  test found the vanishing rocks and foliage and led to the world streaming fix.
- Beginner engrams such as the harvesting tools and the spear, plus a thatch floor, a campfire and tribe invites.
- Taming a dino with player 2 works, with one known quirk (below).

**Known issue:** when player 2 finishes a tame, the "name your dino" box opens on player 1's half and only player
1 can type in it (name it with player 1's keyboard). The console version reportedly behaves the same way.

**Not tested yet (bug reports welcome):**
- Maps other than The Island.
- One player going into a cave while the other stays outside.
- Transferring characters to other maps.
- Riding dinos.
- Later engrams and items. Only beginner ones have been tried, so some later ones may still have splitscreen bugs.
- Unofficial servers. Not tested and not planned; the mod is meant for single-player.

## How it works

`xinput1_3.dll` is a proxy for the controller DLL the game loads: it forwards all controller calls to Windows' own
`xinput1_3.dll` and patches the game in memory (inline hooks and small code patches). How each problem was found,
with addresses: [notes/status.md](notes/status.md) and [notes/symbol-findings.md](notes/symbol-findings.md).

## Build

Run `build.bat` (needs Visual Studio 2019 with the C++ desktop workload). Output: `build\xinput1_3.dll`.

## Tools

`tools\` has the read-only analysis scripts used to find all this: `pdb_dump.py` (symbols to a TSV), `disasm.py`,
`xrefs.py`, `refs.py`, `writes.py`, `vtable.py`, `prologues.py`, `imports.py`, `dump_modules.py`. They need Python 3
and `capstone`. `renderdoc\scripts\` has the RenderDoc analysis scripts used for the graphics fixes (run with
`qrenderdoc.exe --python <script>` started from the repo folder; they read and write `captures\`, or the folder in
the `ARK_CAPTURES` environment variable).

## License

MIT, see [LICENSE](LICENSE). This covers the mod's own code and notes only, not ARK or any of its files.

## Disclaimer

Unofficial fan project, not affiliated with or endorsed by Studio Wildcard or Snail Games. Use at your own risk and
keep backups of your saves.
