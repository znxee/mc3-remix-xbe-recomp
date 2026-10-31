# Car packs

An opt-in way to **add** cars to the game from folders of loose files, for example a PS2 mod pack, without editing the
XBE or the game's archives. It is off unless asked for. Task MC3-0022; it builds on the PS2 car loader of
`docs/PS2_CARS.md` (MC3-0021).

## Turning it on

```text
--mc3-car-packs=<folder>
```

| Flag | Default | What it does |
| --- | --- | --- |
| `--mc3-car-packs=<folder>` | off | Loads the car packs in `<folder>`: `<folder>` itself is one pack, or each of its subfolders is. |

A pack is laid out like the game's `ASSETS` tree; an `ASSETS` folder inside it is entered too, so a mod pack can be used
as it ships. Only what a pack holds matters, for example for one car:

```text
<pack>\tune\vehicle\vehicle.lst                      the game's list plus the new Vehicle blocks
<pack>\tune\progress\unlockingrules.csv              plus the new cars' rules
<pack>\tune\vehicle\customdata\<car>*.carcfg         its configurations
<pack>\tune\vehicle\customdata\<car>\...             default.mccarcustom, <car>.damage
<pack>\tune\garage\<car>.mcgarage                    garage cameras
<pack>\tune\camera\<car>_*.camtrackcs, _pov...       race cameras
<pack>\resources\vehicle\<car>\*.pck                 the PS2 car files (converted at load time, see PS2_CARS.md)
<pack>\fonts\*.strtbl                                names and texts, read as string sources
<pack>\strings.txt                                   names and texts, one "key = text" per line (UTF-8)
<pack>\garage.txt                                    the garage shops' menus for its cars (docs/GARAGE_SCRIPT.md)
```

`strings.txt` is the easy way to name a car: the menus ask for `<car>_short` (the label) among others.

```text
; strings.txt
vp_d_280z_75_short = Datsun 280Z
```

## Rules

Every file the game opens goes through `Stream::Open` (`0x1A2280`); the existing `Mc2LaPaths` hook there calls
`src/game/car_packs.cpp` first.

* **A file the game does not have** (a new car's own files) is served from the pack.
* **A list the game has** (`tune\vehicle\vehicle.lst`, `tune\progress\unlockingrules.csv`) is **merged**: the game's own
  copy, read through its asset manager, plus the lines each pack inserts into it. Lines a pack changes or drops stay as
  the game has them, so the retail entries cannot be broken by a pack made for another version, and several packs add
  up. The game reads `vehicle.lst` into lists that grow as needed (`AllocateCarData`, `0x26ABC0`), so a new `Vehicle`
  block is a new car in the menus.
* **Any other pack file the game has** replaces it.
* **Strings**: `txtStringTable::Get` (`0xEA750`) answers a key the game lacks with the pack's text (`strings.txt` first,
  then the first language of each `fonts\*.strtbl`; the first pack, in folder order, to name a key keeps it). Keys the
  game has are never changed.
* **Car files** in `resources\vehicle` go to the PS2 car loader.
* Served and merged files are written under the cache partition (`hdd\Z\carpacks`) and opened through the console's
  file system.

The log says what happened: `MC3PACK pack ...: N files`, `... N strings`, `MC3PACK tune\vehicle\vehicle.lst: N lines
added by packs`, and one line per file served.

## What a pack cannot add yet

* **Wheels and exhausts of its own.** They live in the shared rim and exhaust packs, which still load from the Xbox
  assets. A car configuration naming a wheel, tire, brake or exhaust past the game's own lists gets the stock one, and
  the log says so.
* **Front-end images** of a new car (`flash\ds_`, `ms_`, `ps_`, `vs_`): an Xbox image in the pack is used; otherwise the
  stock Jetta's is shown. The PS2 `.pck` images are not converted.
* A car that is locked by its unlocking rule is locked as for any car.

## How it was checked

A pack built from a community PS2 mod pack: its 28 new cars' files, `vehicle.lst`, `unlockingrules.csv` and
`mcstrings01.strtbl` (573 files, 3660 strings). Legacy profile:

* `vehicle.lst` gained the 28 new cars (589 lines); the cars unlocked from the start show in **Arcade > P1** by name
  (280Z, 240SX, 99 Eclipse, Solstice, Golf GTI, Scirocco, Focus SVT);
* the 280Z, a car the Xbox game never had, chosen there: driven 65 s in San Diego Cruise, pause, **Go to Menus**, no
  `exception` or `KeBugCheck`;
* a second pack with one `strings.txt` line renamed it to "Datsun 280Z" in the menu;
* without the flag nothing is logged and the game is unchanged.
