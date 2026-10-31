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
TurboUpgradePrice = [$0.00, $1.00, $2.00]
Names = "Default name"                  ; a plain value, not a list
```

* A section is `[<car> <shop>]`. `*` applies to every car; a car's own section is applied after the `*` ones, so it
  wins.
* `<Name> = [a, b, c]` sets a list; put an item in double quotes when it holds a comma. `<Name> = text` sets a plain
  value.
* A list the script does not name stays as the movie has it. A list it names is replaced whole, so keep its `Names`,
  `Status` and `Price` the same length.
* Items are names the game knows (a part, a level, a vinyl). The game matches the chosen name to its action, so
  removing, reordering, repricing or moving a known item (a vinyl or rim from another car's list) works; a name the
  game has no action for does nothing when chosen. The label shown is the string table's text for the name, so a new
  label comes from `strings.txt` in a car pack.
* `Status` values seen in the game's own menus: `1` on submenus, `2` on parts for sale, `4` on the stock part, `6` on
  some paint options. What each changes beyond that was not measured; a level's "Locked" comes from the game's
  progress, not from `Status`.

## How it works

Every file the game opens goes through `Stream::Open` (`0x1A2280`, the car packs' hook). When it opens a shop movie the
script has statements for, the movie's init action is kept. The statements are compiled to the same AVM1 the movies
use (push, `GetVariable("_global")`, `new Array`, `SetMember`), and when the movies' action interpreter (`0x68BC0`)
runs that init action, it runs the script's right after it, on the same movie. The movie file itself is not served
changed: its data does not come from the stream that open returns.

The log says what happened: `MC3GARAGE script <file>: N sections, N statements`, then for each movie `MC3GARAGE
flash\ms_vp_eclipse_04.xbck: N statements from the script` and, with the dump, `MC3GARAGE dump <file>`.

## What it cannot do

* Icons: the shop's ring of icons is drawn by the movie by position, so reordering a list moves the labels and not the
  icons. Removing items from the end of a list keeps them in step.
* New actions: an item name the game has no code for does nothing when chosen.
* The garage's own top level (Manage Vehicles, Customize Vehicle, Buy Vehicles, Exit Garage) lives in the garage movie,
  not in the shop movies.

## How it was checked

Legacy profile, the stock Eclipse (`vp_eclipse_04`), the script above:

* the performance shop's ring lists Chassis, Engine and Brakes and wraps after three; choosing Brakes opens the brakes'
  levels;
* Engine > Turbo > Level 1 shows `$1.00` (the `*` section); buying it took $1 from the player's money and installed it;
* `--mc3-garage-dump` wrote `ms_vp_eclipse_04.txt` matching the movie's arrays;
* Exit Garage returns to the main menu; Arcade > Cruise driven 60 s, pause, Go to Menus with both flags and with none:
  no `exception` or `KeBugCheck`.
