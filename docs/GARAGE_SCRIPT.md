# Garage script

An opt-in text script for the garage shops' menus: which items a shop lists, in what order, their prices and whether
they are available. It is off unless asked for. Task MC3-0023; it works with or without the car packs of
`docs/CAR_PACKS.md` (MC3-0022).

## Turning it on

| Flag | Default | What it does |
| --- | --- | --- |
| `--mc3-garage-script=<file>` | off | Runs `<file>` on the shop menus. A car pack's own `garage.txt` (at its root) is run too. |
| `--mc3-garage-dump` | off | Writes every shop menu the game opens, as script, to `hdd\Z\carpacks\garage_dump\<shop>_<car>.txt`. |
| `--mc3-pack-shops` | off | Retail cars' shops take a car pack's menus (see "Pack shops" below). |

The dump is the starting point: turn it on, enter the shops of a car in the garage, and copy what you want to change
from the files it writes.

## Where the menus come from

Each car has four movies, `flash\<shop>_<car>.xbck`:

For a car pack, the requested `.xbck` may be supplied as a loose PS2 `.pck` with the same stem; the native Flash
constructors relocate the PS2 image. See `docs/CAR_PACKS.md` for the pack layout and the extraction rule for `ps.zip`
and `vs.zip`.

| Shop | What it sells |
| --- | --- |
| `ds` | detail shop: vinyls, badges, decals, license plate |
| `ms` | performance shop: engine, transmission, chassis, tires, brakes |
| `ps` | paint shop: paint, tint, rim and caliper colors, neon, nitrous color, HUD |
| `vs` | visual shop: body kits, rims and the other parts |

The first init action of each movie builds the shop's menus as global arrays: for every list `<List>Names` (the items),
`<List>Status` (one number per item) and `<List>Price` (`$0.00` text), and some `<List>Order`. For example the top of
the performance shop is `UpgradeTypesNames = [Power_Engine, Transmission, Chassis, Tires_Type, Brake, autoupgrade]`.
The garage draws its lists from these arrays, and the game reads the chosen item's name back from them (`0x139650`) to
know what was picked, so editing them edits the menus.

## The script

```text
; comments start with ; or #
[vp_eclipse_04 ms]                      ; a car and a shop; * for every car
UpgradeTypesNames  = [Chassis, Power_Engine, Brake]
UpgradeTypesStatus = [1, 1, 1]
UpgradeTypesPrice  = [$0.00, $0.00, $0.00]

[* ms]
TurboUpgradeNames = [Stock_Turbo, Turbo_Stage3, Level2_Turbo]
TurboUpgradeStatus = [4, 2, 2]
TurboUpgradePrice = [$0.00, $37.00, $2.00]
Names = "Default name"                  ; a plain value, not a list

[strings]
Power_Engine = Custom engine
Turbo_Stage3 = Special turbo
Turbo_Stage3_DESC = Turbo Level 1

alias Turbo_Stage3 = Level1_Turbo
```

* A shop section is `[<car> <shop>]`. `*` applies to every car; a car's own section is applied after the `*` ones, so it
  wins.
* `<Name> = [a, b, c]` sets a list; put an item in double quotes when it holds a comma. `<Name> = text` sets a plain
  value.
* A list the script does not name stays as the movie has it. A list it names is replaced whole, apart from the
  automatic icon `Order` described below. Keep parallel entries aligned when changing `Names`, `Price`, `Brand` and
  `Status`. The visual shop is a special case: the game
  behaves differently when `RimsStatus` is made very short. When `RimsStatus` is shortened, the script retains the
  movie's unused status tail at runtime; the first entries still come from the script.
* Items are names the game knows (a part, a level, a vinyl). The game matches the chosen name to its action, so
  removing, reordering, repricing or moving a known item can work, but the game also checks progression and resource
  availability. Test the particular item and car in game. A name the game has no action for does nothing when chosen
  unless it is an `alias`. The label shown is the string table's text for the name: set it with `[strings]` below or
  with `strings.txt` in a car pack.
* `[strings]` assigns labels globally by case-sensitive string key, including keys already present in the game. Text
  is UTF-8; quote it to preserve leading or trailing spaces. Invalid UTF-8 is logged and ignored. Later definitions
  replace earlier ones; the command-line script is read first, then each pack's `garage.txt`. These labels override
  both retail strings and pack `strings.txt`, and require `--mc3-garage-script`.
* `alias <new name> = <existing name>` defines a case-sensitive global alias, including when written inside a section.
  Choose a new name that the game does not already use: resolution also occurs outside the garage.
  Give the new name a label in `[strings]`, and put it in the appropriate `Names` list with corresponding `Status` and
  `Price` entries. Some menus also look up `<new name>_DESC`; define that key to supply a description. The target must
  be a retail item available in that category; alias chains are not supported.
  Later definitions win. Aliases require `--mc3-garage-script`.
* `Status` is not a universal unlock switch. In the tested menus `1` appeared on submenus, `2` on available items,
  `4` on the installed stock item. `5` occurs in part lists and `6` in paint dumps, but their individual effects have
  not been measured generally. In the tested visual rim list, changing an available rim's status from `2` to `5`
  did not lock it: the ring still showed its price. Setting a value does not establish that a part is unlocked or
  locked.

## Shop-specific observations

* Detail (`ds`): a shortened vinyl list showed its scripted prices. A vinyl from an Eclipse list could be bought and
  rendered on an SRT4; this is evidence for that pair, not every vinyl or car.
* Paint (`ps`): the game recalculates colour-menu status values after the init action. The script preserves explicitly
  supplied status entries during that recalculation, so a scripted available status and price can be used together.
  The hook leaves other game update calls unchanged. Plain values, installed-state interactions and other paint
  submenus need separate verification.
* Visual (`vs`): `RimsBrand` is parallel to `RimsNames` and `RimsPrice`; edit its visible entries in the same
  order. On a Monte Carlo a full-length edited list displayed and installed an Eclipse stock rim at the script price.
  Removing one entry still showed the tested rims and prices. A three-entry `RimsStatus` list instead showed even a
  normally available rim as locked; retaining the original status tail restored its price. The script now appends
  that original tail automatically. The exact point at which a shortened status list fails has not been measured.
  Changing a rim status value alone did not override the game's availability decision in this test.
* Performance (`ms`): the original checks below cover a reduced top-level list and a repriced upgrade. Unlocks remain
  subject to the game's progression rules.

These are observations from the specific tests, not a guarantee for all submenus, vehicles or items.

## Keeping icons with their items

When an edited `Names` array reorders or removes existing items, the script automatically derives its matching
`Order` array from the movie's original name-to-icon pairs. The example above therefore keeps Chassis, Engine and
Brakes with their own icons. No `.xbck` file is changed. Only lists with an original `Order` array participate;
`ChassisUpgradeNames` uses the movie's spelling `ChassisUpgradesOrder`.

You can also set `<List>Order = [...]` explicitly using the existing array syntax. Its entries are icon selectors,
not indexes into the edited `Names` array. Start with `--mc3-garage-dump`, pair each original name with its original
Order entry, then apply the same permutation/removals to both. Keep Order the same length as Names. Values are read
from your movie at runtime; they are not a universal icon numbering scheme across shops or cars.

An explicit Order in any matching section takes precedence over automatic inference, regardless of statement
position. Existing wildcard-first, car-specific-last assignment precedence still applies. When editing a full
dump, either remove its Order assignment to enable inference or update it along with Names. The last assignment
to a Names array is matched against the original movie, so layered wildcard and car-specific edits do not compound
the permutation. Status and Price remain explicit and must still be aligned by the author.

If a name is absent from the original list, an original name has conflicting icon entries, or the original arrays
have different lengths, inference leaves Order unchanged and logs `cannot derive ...; set ... explicitly`.
Provide a suitable explicit Order in those cases. This does not create artwork or add an action for a new name.

## How it works

Every file the game opens goes through `Stream::Open` (`0x1A2280`, the car packs' hook). When it opens a shop movie the
script has statements for, the movie's init action is kept. The statements are compiled to the same AVM1 the movies
use (push, `GetVariable("_global")`, `new Array`, `SetMember`), and when the movies' action interpreter (`0x68BC0`)
runs that init action, it runs the script's right after it, on the same movie. The movie file itself is not served
changed: its data does not come from the stream that open returns.

In the inspected Eclipse movie, the array initializer is a root DoAction; the movie also contains other DoActions
and PlaceObject2 clip events. The icon choice happens in those clip events, not in the garage movie's top-level menu.
The ring controller uses Names length and the selection to animate the numbered slots. Each slot's icon clip reads the corresponding
Order entry and selects its artwork frame. In the inspected performance movie these events run on their second
update and select the one-based frame corresponding to the zero-based selector. The garage movie selects the active
shop; the shop's other actions draw the labels, status and price. Native selection still reads the name with
`0x139650` and dispatches it through `0x146B40`, independently of the artwork selector.

RE used the shop's DoActions and clip events as well as the garage movie, with Thiekus' Ghidra symbols identifying
`ExecuteActionScript` (`0x68BC0`) and `swfINSTANCE_MaybeGotoFrame` (`0x64D00`). The existing interpreter override is
reused; no second override or per-frame icon hook is added. Order inference runs once when the shop movie is seen.

The log says what happened: `MC3GARAGE script <file>: N sections, N statements`, then for each movie `MC3GARAGE
flash\ms_vp_eclipse_04.xbck: N statements from the script` and, with the dump, `MC3GARAGE dump <file>`.

## What it cannot do

* New artwork or arbitrary movie layouts: existing Order-controlled icons follow reordered/filtered items, but
  lists without an Order array retain their movie-specific presentation. Inference cannot choose artwork for a name
  absent from the original list; use an explicit Order selecting an existing icon.
* An item name the game has no action for needs an alias to a known item; aliases do not create new part types.
* Adding a fourth entry to the Eclipse turbo list was not enough to make a fourth ring position appear; replacing an
  existing entry is the supported example above. Other lists may have their own length constraints.
* The game's installed-item check compares a retail name literally with `Names`; after reopening a list it may not
  mark the alias as installed. Other shops and special cases have not been verified.
* The garage's own top level (Manage Vehicles, Customize Vehicle, Buy Vehicles, Exit Garage) lives in the garage movie,
  not in the shop movies.

## Showroom: the packs' cars for sale

| Flag | Default | What it does |
| --- | --- | --- |
| `--mc3-showroom-packs` | off | The Showroom (Buy Vehicles) lists the car packs' cars, so they can be bought. |

The Showroom is the garage movie's, `flash\toplevel_x.xbck`: nine parallel arrays in its init action, `ShowRoomNames`,
`ShowRoomPrice`, `ShowRoomStatus`, `ShowroomClass`, `ShowroomPerf`, `AccelMin`, `TopSpeedMin`, `HandlingMin` and
`PrizeStatus`. The retail list has the 94 cars and 23 `vp_dlslot_NN` placeholders (Xbox Live slots, no status, not
shown). With the flag the same mechanism as the shops runs new arrays right after that init action:

* Every car of the vehicle list (the merged `vehicle.lst`) that the Showroom lacks is added, at its vehicle-list index:
  it takes the placeholder there, or is appended when it is the next entry. The game writes the Remix SpecialEdition
  cars' class, perf and price into these arrays by vehicle index (`0x152420`), so the two orders are kept the same; a car
  that cannot keep its index is logged.
* A car's entries come from its pack's own garage movie, `flash\toplevel(_x).pck` or `.xbck` (a PS2 VIP-style movie is
  read by running its Showroom blocks in file order, the last assignment winning), then from `[showroom]` rows. The
  pack's garage movie is only read: the game's own one runs.
* A car with no entry anywhere is left out and logged (`... has no Showroom entry in its pack`).
* The list stays under 128 entries: the game finds a name's index with a signed byte (`0x144BE0`).
* The movie's ring shows `ShowRoomNames.length - 23 + XBliveCars` entries (the 23 placeholders are never shown), and
  `0x152420` sets `XBliveCars` from the Xbox Live car count (`[0x557EAC]`, 0 here). The flag raises that value, where
  the game writes it (`0x139520` called from `0x152420`), by the number of placeholders pack cars took, so every
  added car is reachable.

```text
[showroom]
; <car> = price, status, class, perf, accel, top speed, handling, prize
vp_d_golf_83 = $16000.00, 2, 1, d, 12.358152, 33.451324, 41.176467, 0
vp_eclipse_04 = $1.00                   ; trailing or empty fields keep the movie's; a listed car changes in place
```

`[showroom]` rows go in a pack's `garage.txt` or the `--mc3-garage-script` file and are read only with
`--mc3-showroom-packs`. With `--mc3-garage-dump` the resulting list is written as such rows to
`garage_dump\showroom_toplevel_x.txt`. The log has `MC3SHOWROOM <movie>: N cars` for each pack movie read and
`MC3SHOWROOM flash\toplevel_x.xbck: N vehicles; N pack cars on N placeholders, N appended ...`.

The statuses are the movies' own (`2`, `5`, `7` in the retail list). In the checks below `2` showed the car with its
price and `5` showed `Locked` with no model; other values were not measured.

Limits seen in the checks:

* The ring icons and the manufacturer logo in the footer are chosen by list position, not by name: an added car
  shows the Xbox Live icon of the placeholder whose place it took (or the last one), and some retail car's logo.
  New artwork would need a changed movie.
* The bars' numbers are the game's own calculation, as for retail cars; they are close to, not equal to, the
  `AccelMin`/`TopSpeedMin`/`HandlingMin` entries.
* `0x152420` only re-selects a car in the ring by name when its index is under 99 (the `[0x4B22E0] < 100` check);
  cars further down the list are not re-selected that way (the effect was not observed in the checks).

### How the Showroom was checked

Integration build, legacy profile, Player 1 of the `existing` save on an isolated `hdd6`, the VIP pack v1.0.2
(28 cars, PS2 `flash\toplevel.pck` and `toplevel_x.pck`):

* The log read 122 cars from `toplevel.pck` and 147 from `toplevel_x.pck`, and put all 28 cars at their own vehicle
  index (23 on placeholders, 5 appended, 122 entries). The dump matched the VIP's rows.
* Going left from the first retail car the ring reached all 122 entries (index 122 Firebird down to 96 Miura), with the
  VIP names, prices and classes. Before the `XBliveCars` correction it wrapped at 99.
* Golf GTI and 280Z (status `2`) showed their model, name and `$16,000`; Maybach, R8, Cobra and others (status `5`)
  showed `Locked`.
* With `[showroom] vp_d_golf_83 = $1.00`: Purchase this vehicle? took $1 ($1,402 -> $1,401), the owned-car list went
  from `vp_srt4_04` to `vp_srt4_04, vp_d_golf_83` (`0x53F80C`/`0x53F810` records) and the garage showed the Golf as the
  current car. No `exception` or `KeBugCheck` in the log.
* Without the flag the same pack gave the retail list and no `MC3SHOWROOM` line.
* Not checked: saving and reloading a profile with a bought pack car, Manage Vehicles with it, and a career race with
  it (the VIP cars were driven in Arcade before, MC3-0028).

## Pack shops: a mod's menus for the retail cars

| Flag | Default | What it does |
| --- | --- | --- |
| `--mc3-pack-shops` | off | A retail car's shops take the menus of a car pack's own shop movies. |

A PS2 mod such as the VIP ships `flash\{ds,ms,ps,vs}_<car>.pck` for the retail cars too, with its own menus: more
rims and rim sizes, extra hoods and plates, a Trim item in the Paint Shop. The game runs its own Xbox movie for a car it
has, so a served pack movie never shows. With the flag the pack's movie is read instead of served, and the arrays of
the game's init action that the pack's movie ends up with different are run after it (the garage script's mechanism):

* The pack's movie is read like the Showroom's: every action block whose constant pool names `_global`, in file order,
  the last assignment winning. The VIP keeps each movie's original init action and rebuilds the lists element by
  element in blocks appended at the end of the file.
* Only arrays that the game's own init action sets are changed; plain values are left out (later blocks set runtime
  values such as `Description`). A list keeps the movie's integer type.
* The script's own statements (`--mc3-garage-script`, `garage.txt`) run after the pack's, so they win.
* A new car (one the game has no shop movies for) runs a stock car's movies instead, see below; with the flag they carry
  the menus of the new car's own pack movies.

The log says `MC3PACK flashs_vp_srt4_04.xbck: the game's own is used, the pack's is read for its menus`, then
`MC3GARAGE flashs_vp_srt4_04.xbck: 11 menu arrays from <pack movie>`.

What the Xbox game does with an added item is its own: the names have to be ones it knows. The VIP's checked ones are
(`rim_14`..`rim_28` are in a table at `0x471BC8`, `Trim` is handled by the paint menu at `0x14EF90`, rims come from the
merged `customwheels.txt`, parts from the pack's PS2 car files). Brand logos in a ring are positional like the shops'
icons, so an added rim shows the logo of the last retail entry.

### How the pack shops were checked

Legacy profile, Player 1 (SRT4) of the `existing` save on `hdd6`, the VIP pack v1.0.2, `--mc3-pack-shops`
(`garage_tools/MC3-0028/shops_probe.py`):

* Visual > Wheels > Rims: 91 entries instead of 69; going left from the stock rim reached OZ Opera, Iforged Daytona and
  Concept, Advan RGII, showing `Locked` as retail rims do at that progress.
* Rim Size: `rim_14` to `rim_28` instead of 15 to 20; 28" showed `Unavailable` for the SRT4.
* Paint: the root ring is Paint ... Nitrous Color, Trim, Hud; Trim opened its colour ring (`chrome`).
* Detail: the plate list took the VIP's 81 entries (the log; `japan_tokyo_1` is outside the walked ring).
* The Showroom (`--mc3-showroom-packs`) still listed all 122 entries.
* Without the flag the same shops kept the retail lists (rims from Grac and GFG, sizes 15-20, no Trim) and the log had
  no `menu arrays` line. No `exception` or `KeBugCheck` in any run.
* Not checked: buying or installing an added item.

### A new car's shops

The garage loads a shop movie by name, `<shop>_<car>.swf` (`0x14E8E0` -> `0x139500`, which copies the name into the
movie wrapper), and the Flash loader finds a movie only among the game's own: a new car of a car pack got a shop with
no menu at all, whatever was served for it (its PS2 movie, or the Jetta's under its name). With car packs, a shop movie
the game does not have (loose or in its archives) for a pack car is now loaded as the stock Jetta's
(`MC3GARAGE flashs_vp_d_golf_83.xbck: the game has no such movie; vp_jetta_03's runs for it`). The garage script and
`--mc3-pack-shops` still treat it as the new car's movie: script sections and the pack's menus are the new car's, and
the Jetta's own shops are left alone (a stand-in's statements run only while it is the movie the game opened last).

Checked with the VIP Golf GTI bought in the Showroom: before, all four shops opened empty; now Performance (Engine),
Detail (Vinyls), Paint and Visual (Body Upgrades) open, and with `--mc3-pack-shops` the rim ring starts with the
Golf's own stock rim and has the VIP rims; without it the Jetta's lists show. The footer logo is positional (it showed
Mitsubishi). Not checked: buying or installing a part on a new car; a new motorbike would get a car's shop movies.

## How it was checked

MC3-0024, legacy profile, Eclipse, with no explicit Order assignments:

* `[Chassis, Power_Engine, Brake]`: each label showed its own icon, each selection opened the matching Chassis,
  Engine or Brakes submenu, and moving past Brakes wrapped back to Chassis with the matching icon.
* `[Power_Engine, Transmission, Tires_Type, Brake]`: removed Chassis from the middle and Auto Upgrade from the end;
  all four remaining labels/icons matched, and all four opened the correct submenus.
* The log confirmed automatic Order derivation for each list. The original shop arrays were dumped successfully.
* Native synthetic-array tests covered non-identity original Order values, filtering, layered Names edits, explicit
  Order precedence, unknown and duplicate names, empty output, inconsistent original lengths and the chassis spelling.
* Both the research build and the isolated repository's `build.bat` succeeded. The repository executable passed
  legacy Arcade > San Diego Cruise with 60 seconds of driving input, pause and Go to Menus, without garage flags;
  the log contained no `exception` or `KeBugCheck`. Repository guard tests and the history scan passed.
* Tests used a separate copy of the save and made no purchases. Other shops and cars were not visually validated for
  this change; automatic inference only applies where the movie supplies a matching Order array.

Earlier MC3-0023 checks:

Legacy profile, the stock Eclipse (`vp_eclipse_04`), the script above:

* the performance shop's ring lists Chassis, Engine and Brakes and wraps after three; choosing Brakes opens the brakes'
  levels;
* Engine > Turbo > Level 1 shows `$1.00` (the `*` section); buying it took $1 from the player's money and installed it;
* `--mc3-garage-dump` wrote `ms_vp_eclipse_04.txt` matching the movie's arrays;
* Exit Garage returns to the main menu; Arcade > Cruise driven 60 s, pause, Go to Menus with both flags and with none:
  no `exception` or `KeBugCheck`.
