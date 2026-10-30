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
<pack>\resources\vehicle\rim.ppf, exhaust.ppf          optional PS2 shared wheel/exhaust page files
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
* **A list the game has** (`tune\vehicle\vehicle.lst`, `tune\progress\unlockingrules.csv`, `customwheels.txt` and
  `customexhausts.txt`) is **merged**: the game's own
  copy, read through its asset manager, plus the lines each pack inserts into it. Lines a pack changes or drops stay as
  the game has them, so the retail entries cannot be broken by a pack made for another version, and several packs add
  up. The game reads `vehicle.lst` into lists that grow as needed (`AllocateCarData`, `0x26ABC0`), so a new `Vehicle`
  block is a new car in the menus.
* **Any other pack file the game has** replaces it.
* **Strings**: `txtStringTable::Get` (`0xEA750`) answers a key the game lacks with the pack's text (`strings.txt` first,
  then the first language of each `fonts\*.strtbl`; the first pack, in folder order, to name a key keeps it). Keys the
  game has are never changed.
* **Car files** in `resources\vehicle` go to the PS2 car loader. A pack may also provide `rim.ppf` and `exhaust.ppf`.
  The loader retains every retail Xbox page and converts the PS2-only tail into an Xbox PPF under
  `hdd\Z\carpacks`. It also extends the retail `decal.xbck`/`decal_g.xbck` rim and exhaust handle tables. Appended
  catalog indices refer to model names; they are not page numbers.
* Served and merged files are written under the cache partition (`hdd\Z\carpacks`) and opened through the console's
  file system.

The log says what happened: `MC3PACK pack ...: N files`, `... N strings`, `MC3PACK tune\vehicle\vehicle.lst: N lines
added by packs`, and one line per file served.

## Limits

* A pack with no `rim.ppf` or `exhaust.ppf` still uses the retail shared pages. A config that names an index outside
  the merged list is replaced with stock index 0 and the log says so. Tires and brakes currently remain retail-only.
* PS2 texture chunks have no direct D3D equivalent. A byte-identical texture already present in the retail PS2 prefix
  uses its paired Xbox chunk. A new texture uses a valid retail Xbox texture chunk with the same PS2 extent, preserving
  page offsets and lifecycle but not the mod texture's original pixels. The log reports exact and fallback counts.
* The garage creates two fixed 3 MiB vehicle heaps. With car packs enabled, the loader raises the foreground heap to
  5 MiB while retaining the 3 MiB background heap on a 64 MiB guest; a 128 MiB guest raises both to 8 MiB. The change
  is limited to the garage constructor's two allocation sites. This avoids the `VehHeap` overrun seen while loading
  the VIP SRT4 `_g.pck` without making the 64 MiB allocation fail.
* The compact PPF packet decoder is used only for shared rim and exhaust page models. Regular vehicle VIF streams are
  always decoded by the VIF path; treating arbitrary VIF bytes as PPF headers skips VU batches and produces missing
  vehicle panels.
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

MC3-0027 was checked with the isolated VIP Focus SVT pack using its actual custom indices (`RimMdlIdx 205`,
`ExhaustRear 92`), not clamped stock values. The generated files contained 185 retail + 105 converted PS2 rim pages
and 64 + 34 exhaust pages; the rim conversion used 100 exact and 246 fallback Xbox texture chunks. The combined car
entered San Diego Cruise, rendered for 60 s, paused and returned to the frontend with no guest exception or bugcheck.
Separate rim-only and exhaust-only runs also completed the page-in/page-out cycle. A no-car-pack Jetta baseline ran
the same 60 s cycle successfully.

The shared-only pack opened Customize Vehicle > Visual Shop on the retail SRT4. The full VIP pack initially trapped
at guest `0x325AE5` while loading `vp_srt4_04_g.pck`: the garage's `VehHeap` had used 3,128,016 of its 3,145,728
bytes. The corrected VIF path now emits 1,763 VU batches for that car (rather than treating its stream as a compact
PPF packet), yielding the complete SRT4 body. With the garage heap rule above, the full VIP pack passed on both the
default 64 MiB guest and 128 MiB: Player 1's SRT4 entered Performance, Detail, Visual and Paint; the PS2
`ms_`/`ds_`/`vs_`/`ps_` movies loaded; each shop produced a complete dump with no `not dumped`; and Turbo, Vinyl
Style, Front Bumper, Gloss, Visual > Exhaust Tips and Visual > Rims opened and returned. There was no guest exception,
bugcheck or purchase action. Captures show the intact car, an installed stock rim and installed exhaust tips. Repros:
`research/xbox_recomp/garage_tools/MC3-0027/vip_full_garage_vif_heap5plus3_64_20261029` and
`research/xbox_recomp/garage_tools/MC3-0027/vip_full_garage_vif_heap8_128_20261029`.

The selected Player 1 SRT4's Visual Shop rim list contains its own stock and retail options; it does not expose the
VIP Focus SVT's custom rim 205. The SRT4 run therefore verifies the full pack's garage load and shop navigation, while
the Focus Cruise runs above verify its custom rim and exhaust conversion. Novel PPF texture pixels still use the
fallback rule above.
