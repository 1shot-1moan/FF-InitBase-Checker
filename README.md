# FF InitBase Checker

A console-based tool that validates the IL2CPP pointer chain for **Free Fire** and **Free Fire MAX** running on BlueStacks (HD-Player).

Inject it, enter your `InitBase` offset, and it tells you in real time whether the full chain resolves to a valid local player — without touching a single line of game code.

---

## What is InitBase

`InitBase` is a fixed offset inside `libil2cpp.so` (the Unity IL2CPP runtime loaded in the BlueStacks Android VM). It points to the **TypeInfo block** of `MatchGame` — the anchor class for the entire game object chain.

```
libil2cpp.so base  (found via ADB /proc/maps)
    + InitBase     →  Read uint32  →  base  (TypeInfo block)
                                         ↓
                               (FF MAX: base IS facade — DirectFacade)
                               (FF:     Read(base) → facade)
                                         ↓
                               Read(facade + 0x5C)  →  statics
                               Read(statics + 0x0)  →  game   (MatchGame instance)
                               Read(game    + 0x50) →  match  (CurrentMatch)
                               Read(match   + 0x94) →  player (LocalPlayer)
```

Every step is validated. Green = valid pointer. Red = null or bad address.

---

## Requirements

| Requirement | Details |
|---|---|
| BlueStacks 5 | HD-Player.exe must be running |
| ScriptKittens.dll | Must be injected first — provides `Load(pVM)` entry and `Mem::` VM memory access |
| Free Fire or FF MAX | Game must be running inside BlueStacks |
| Visual Studio 2022 | To build from source |

> **Memory reads go through BlueStacks' hypervisor API (`PGMPhysRead`), not `ReadProcessMemory`.** ScriptKittens.dll hooks `BstkVMM.dll` and exposes `CPU / InternalRead / Cast / InternalWrite` exports. This DLL reuses those exports — no additional hooking needed.

---

## How to Use

1. Start **BlueStacks** and launch **Free Fire** or **Free Fire MAX**
2. Inject `ScriptKittens.dll` into `HD-Player.exe`
3. Inject `InitBaseChecker.dll` into `HD-Player.exe`
4. A console window opens:

**Step 1 — Select game:**
```
  [1]  Free Fire
  [2]  Free Fire MAX
```

**Step 2 — Enter InitBase:**
```
  Enter InitBase offset (hex, e.g. 9EC1C48)
  Known default: 0xB156C90
  Press Enter alone to use the default.

  > 0x_
```
Type the offset from your dump (no `0x` prefix needed) and press Enter. Press Enter alone to use the built-in default.

**Step 3 — Live chain display:**
```
========================================================
   SCRIPT KITTENS  -  INIT BASE CHECKER
========================================================
  Game    : Free Fire MAX
  Package : com.dts.freefiremax
  InitBase: 0xB156C90  [default]  Facade: Direct
  Il2Cpp  : 0x7B660000  [ADB OK]

--------------------------------------------------------
  base     :   0xAC471BA0  [OK]
  facade   :   0xAC471BA0  [Direct]
  statics  :   0x0014D20   [OK]
  game     :   0x40D1990   [OK]
  match    :   0x2851AA0   [OK]
  player   :   0x4264000   [OK]
--------------------------------------------------------
  RESULT : Chain VALID  -  player loaded!

========================================================
  [I] InitBase   [R] Retry   [G] Game   [X] Exit
```

---

## Runtime Keys

| Key | Action |
|---|---|
| `I` | Enter a new InitBase — resets and re-probes immediately |
| `R` | Restart ADB flow (re-scan `libil2cpp.so`) |
| `G` | Back to game selector |
| `X` | Exit |

---

## Reading Results

| Result | Meaning |
|---|---|
| All steps `[OK]`, player `[OK]` | InitBase is correct — chain fully valid |
| `game` is `---` or fail | InitBase correct up to TypeInfo, but no match is active (in lobby/menu) |
| `base` `[FAIL]` at step 0 | Wrong InitBase — pointer resolves to null or garbage |
| `facade` `[FAIL]` at step 1 | InitBase may point to wrong TypeInfo block |
| Any `[FAIL]` past step 2 | InitBase is valid but a chain offset changed (`0x50` / `0x94`) — re-check dump |

---

## After a Game Update

Only **one value changes** each update — `InitBase`. The tool handles everything else automatically.

1. Run your IL2CPP dumper on the new `libil2cpp.so`
2. Find the new `MatchGame` / `NFJPHMKKEBF` TypeInfo offset
3. Inject the DLL → press `[I]` → enter the new offset
4. Full green = confirmed ✓

No recompile. No code changes.

---

## Building

```
Visual Studio 2022
Platform: x64
Configuration: Release
```

Open `InitBaseChecker.sln` and build, or use MSBuild:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  "InitBaseChecker.sln" /p:Configuration=Release /p:Platform=x64 /t:Rebuild
```

Output: `x64\Release\InitBaseChecker.dll`

---

## Project Structure

```
InitBaseChecker/
├── InitBaseChecker.sln
└── InitBaseChecker/
    ├── dllmain.cpp        — ADB worker, chain walker, console render, Load() export
    ├── Internalmemory.h   — Mem::Init / Mem::Read / Mem::Write via ScriptKittens.dll
    ├── PEBWalk.h          — PEB module/export resolution (no GetProcAddress)
    ├── Adb.h              — HD-Adb.exe wrapper, FindModule, /proc/maps parser
    └── XorStr.h           — Compile-time string XOR encryption
```

---

## Known Defaults

| Game | InitBase | DirectFacade | UseRwSegment |
|---|---|---|---|
| Free Fire (`com.dts.freefireth`) | `0x9EC1C48` | No | No |
| Free Fire MAX (`com.dts.freefiremax`) | `0xB156C90` | Yes | Yes |

> These will be outdated after a game update. Use `[I]` to enter the current value.

---

**Script Kittens** — internal tooling
