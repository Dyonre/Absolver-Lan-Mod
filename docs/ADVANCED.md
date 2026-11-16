# Advanced notes

## Reaching game code
| Tool | Reaches | Notes |
|---|---|---|
| `on_call(class, function, pre, post)` | events and Blueprint script functions, Blueprint-to-Blueprint calls included; not native `BPF_*` functions called from Blueprints (use `replace_native`, or `trace_function` to log them) | `pre` may change the parameters or skip the original |
| `replace_native(class, function, thunk, &orig)` | native functions called through the Blueprint VM | swaps the function pointer; no code is patched |
| `hook_symbol(name, detour)` / `hook_rva(...)` | any native function, by PDB name (needs `symbols.bin`) or by address | hooks from several plugins stack: call the returned "next" pointer unless you replace the function |
| `call_function_params`, `get_property_text` / `set_property_text`, `prop_ptr`, `prop_ptr_path` | any reflected function or property by name, private ones included | raw offsets from `prop_ptr` reach the rest of an object |
| `patches.txt` (`patches = on`) | code bytes | memory only; a row is refused when the original bytes differ |

Direct C++-to-C++ calls do not pass through `on_call` or `replace_native`; hook the C++ function instead.

## Finding names and addresses
* `dump_sdk("name")` writes every class, struct, enum and function with offsets, parameters and native addresses to `LanNative\sdk\`.
* `trace("on", "names")` then `trace("dump", NULL)` records what the game calls while you do something (`LanNative\trace\`).
* `dump_object(obj)` writes every property of one object as text.
* `symbols.bin`: made from your own copy of the game with `tools/symtool` and `tools/lannative/gen_symbols.py`. Never share it or the dumps:
  they are derived from the game.
* Developer commands: with `LanNative\devkeys.txt` present, F11 runs the commands in `LanNative\lmtest.txt` (`tbl`, `rowprops`, `rowget`,
  `rowset`, `rowprobe`, `rowfull`, `rowdump`, `rowapply`, `objdump`, `objrt`, `url`); results go to `lannative.log` and files next to it.
  Delete both files when done.
* `tools/objdiff.py` turns two `objdump` files into the `object` edits of a `rows.json`.

## Threads
* Game thread: `on_tick`, events, timers, content and menu callbacks. Almost every API call needs it.
* `on_object_loaded` runs on loader threads: queue work with `run_on_game_thread`.
* ImGui draw callbacks run on the render thread: no engine calls there; copy data in `on_tick`.

## Failure handling
* Callbacks run inside an exception guard; after 3 faults a plugin's callbacks are switched off and the log names it.
* Code inside a native hook detour is not guarded.
* The first 40 access violations are logged with module and offset.
* Safe mode: Shift held at start, `-SESafe`, `LanNative\safe.txt`, or automatically after two starts that never reached the first tick.

## Limits
* One game build (1.31, b1.25_575). Hooks and patches refuse to apply when the original bytes differ.
* Fixed-size arrays in compiled code (three weapon families, ten equipment slots, six factions) cannot be grown by data; new things are
  mapped onto existing entries.
* The game cannot compile shaders; new materials, meshes, animations and maps come cooked for UE 4.18.3 in a pak.
* A plugin runs only on the PC that has it; other players see its effects only through what the game already replicates.
* Mod data belongs in `kv_*` / `sidecar_*`, not in new fields of the game's save; the save must stay loadable without the mod.
* A plugin is native code with full access to the process: enable only plugins from people you trust.
