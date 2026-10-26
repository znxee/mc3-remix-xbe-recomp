# Garage script

An opt-in text script for the garage shops' menus: which items a shop lists, in what order, their prices and whether
they are available. It is off unless asked for. Task MC3-0023; it works with or without the car packs of
`docs/CAR_PACKS.md` (MC3-0022).

## Turning it on

| Flag | Default | What it does |
| --- | --- | --- |
| `--mc3-garage-script=<file>` | off | Runs `<file>` on the shop menus. A car pack's own `garage.txt` (at its root) is run too. |
| `--mc3-garage-dump` | off | Writes every shop menu the game opens, as script, to `hdd\Z\carpacks\garage_dump\<shop>_<car>.txt`. |

The dump is the starting point: turn it on, enter the shops of a car in the garage, and copy what you want to change
from the files it writes.

## Where the menus come from

Each car has four movies, `flash\<shop>_<car>.xbck`:

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
TurboUpgradePrice = [$0.00, $1.00, $2.00]
Names = "Default name"                  ; a plain value, not a list
```

* A section is `[<car> <shop>]`. `*` applies to every car; a car's own section is applied after the `*` ones, so it
  wins.
* `<Name> = [a, b, c]` sets a list; put an item in double quotes when it holds a comma. `<Name> = text` sets a plain
  value.
* A list the script names is replaced whole, so keep its `Names`, `Status` and `Price` the same length. Unmentioned
  lists stay as the movie has them, except for the automatic icon `Order` described below.
* Items are names the game knows (a part, a level, a vinyl). The game matches the chosen name to its action, so
  removing, reordering, repricing or moving a known item (a vinyl or rim from another car's list) works; a name the
  game has no action for does nothing when chosen. The label shown is the string table's text for the name, so a new
  label comes from `strings.txt` in a car pack.
* `Status` values seen in the game's own menus: `1` on submenus, `2` on parts for sale, `4` on the stock part, `6` on
  some paint options. What each changes beyond that was not measured; a level's "Locked" comes from the game's
  progress, not from `Status`.

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
* New actions: an item name the game has no code for does nothing when chosen.
* The garage's own top level (Manage Vehicles, Customize Vehicle, Buy Vehicles, Exit Garage) lives in the garage movie,
  not in the shop movies.

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
