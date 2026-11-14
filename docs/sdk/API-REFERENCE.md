# AbsolverSE API reference

Generated from `tools/lannative/AbsolverSE.h` (`python tools/sdk/gen_api_reference.py`). This page may lag the header: the header is the source of truth.  
`ABS_API_VERSION` = 2; 140 functions. Every function takes `self` (the `const AbsApi *` you were given) as its first argument. Fields are only ever appended: check `api->size >= offsetof(AbsApi, <field>) + sizeof(void *)` before using a field newer than the version you built against.  
Task-oriented guide: `docs/sdk/README.md`. Behaviour details: `docs/PLUGIN-API.md`.

## Header fields

| Name | Type | Description |
|---|---|---|
| `size` | `uint32_t` | sizeof(AbsApi) of the loader: fields past this are not there |
| `version` | `uint32_t` | ABS_API_VERSION of the loader |
| `game_base` | `uint8_t *` | base address of Absolver-Win64-Shipping.exe |
| `plugin_name` | `const char *` | your mod folder's name |
| `plugin_dir` | `const char *` | absolute path of your mod folder, UTF-8, no trailing slash |

## Functions

Since = the header block the field was added in (2.0 = the original v2 set, 2.1 = script extender additions).

### Since 2.0

| Name | Signature | Description |
|---|---|---|
| `log` | `void log(const AbsApi *self, const char *fmt, ...)` | printf-style; line goes to LanNative\lannative.log tagged with your name |
| `config_get` | `int config_get(const AbsApi *self, const char *key, char *out, int cap)` | value of "key = value" from <your mod folder>\plugin.cfg; 1 if found |
| `on_tick` | `int on_tick(const AbsApi *self, void (*cb)(float dt, void *user), void *user)` | every frame, game thread |
| `on_object_loaded` | `int on_object_loaded(const AbsApi *self, void (*cb)(void *obj, void *user), void *user)` | after any UObject finished loading; ANY thread; hot: be quick |
| `on_engine_ready` | `int on_engine_ready(const AbsApi *self, void (*cb)(void *user), void *user)` | once, on the first game tick (GEngine exists); runs at once if that already happened |
| `run_on_game_thread` | `int run_on_game_thread(const AbsApi *self, void (*fn)(void *user), void *user)` | queue fn for the start of the next game tick; safe from any thread |
| `is_game_thread` | `int is_game_thread(const AbsApi *self)` |  |
| `hook_rva` | `void * hook_rva(const AbsApi *self, const char *name, uint32_t rva, const uint8_t *expected, uint32_t stolen, void *detour)` | Patches `rva` (offset from game_base): `expected` are the original bytes, `stolen` how many whole instructions' bytes (14..24) to move. * Returns the next function in the chain (call it from the detour) or NULL (bytes differ / failed; nothing was changed). * If the function is already hooked by the loader or another plugin the hook is stacked on top of it: `expected` must still be the game's ORIGINAL bytes. * Installation suspends the other threads for a moment, so it is safe at any time (still: prefer doing it in AbsPlugin_Load). |
| `find_object` | `void * find_object(const AbsApi *self, const char *path)` | StaticFindObject: "/Game/DB/X.X" (already loaded objects only); NULL if absent |
| `load_object` | `void * load_object(const AbsApi *self, const char *path)` | StaticLoadObject: loads the asset if needed (game thread!) |
| `object_class` | `void * object_class(const AbsApi *self, void *obj)` | UClass* of an object |
| `object_name` | `int object_name(const AbsApi *self, void *obj, char *out, int cap)` | short name; returns length |
| `class_name` | `int class_name(const AbsApi *self, void *cls, char *out, int cap)` |  |
| `is_a` | `int is_a(const AbsApi *self, void *obj, const char *class_name)` | 1 if obj's class or a parent class has that short name (e.g. "UserWidget") |
| `find_property` | `void * find_property(const AbsApi *self, void *cls, const char *name)` | UProperty* by name, searching parent classes; NULL if none |
| `property_offset` | `int property_offset(const AbsApi *self, void *prop)` | byte offset of the value inside the object |
| `get_property_text` | `int get_property_text(const AbsApi *self, void *obj, const char *name, char *out, int cap)` | engine text of one property ("name[N]" = element N of a static array); returns length or -1 |
| `set_property_text` | `int set_property_text(const AbsApi *self, void *obj, const char *name, const char *text)` | ImportText in place ("name[N]" = element N of a static array); 1 on success. Same caveats as rows.json: validate numbers yourself |
| `find_function` | `void * find_function(const AbsApi *self, void *obj, const char *name)` | UFunction* (Blueprint events included) or NULL |
| `call_function` | `int call_function(const AbsApi *self, void *obj, void *func, void *params)` | UObject::ProcessEvent: params = the function's parameter block (inputs, then outputs), game thread only |
| `for_each_object` | `int for_each_object(const AbsApi *self, void (*cb)(void *obj, void *user), void *user)` | every live UObject (slow: scan, game thread); returns the count |

### Since 2.1

| Name | Signature | Description |
|---|---|---|
| `hook_symbol` | `void * hook_symbol(const AbsApi *self, const char *name, void *detour)` | like hook_rva but by PDB name from LanNative\symbols.bin ("UObject::ProcessEvent"); returns the next function in the chain |
| `find_symbol` | `uint32_t find_symbol(const AbsApi *self, const char *name)` | RVA of a hookable symbol, 0 if unknown |
| `find_class` | `void * find_class(const AbsApi *self, const char *short_name)` | UClass* by short name (first call scans all objects; cached) |
| `find_function_in_class` | `void * find_function_in_class(const AbsApi *self, void *cls, const char *name)` | UFunction* declared in the class or a parent class |
| `on_call` | `int on_call(const AbsApi *self, const char *cls, const char *func, int (*pre)(void *obj, void *func, void *params, void *user), void (*post)(void *obj, void *func, void *params, void *user), void *user)` | on_call: pre runs BEFORE the function, may change *params, and returns nonzero to SKIP the original; post runs after it. cls may be NULL (any class); matched by name. Hot path: be quick. |
| `trace` | `int trace(const AbsApi *self, const char *cmd, const char *names)` | ProcessEvent tracer: "on" [comma list of function/class names; empty = everything], "off", "clear", "dump" (writes LanNative\trace\) |
| `replace_native` | `int replace_native(const AbsApi *self, const char *cls, const char *func, void *thunk, void **orig)` | replace_native: swaps UFunction::Func of a native (C++) Blueprint-callable function by name: thunk(void *context, void *frame, void *result). orig receives the old pointer. No byte patching. |
| `prop_ptr` | `void * prop_ptr(const AbsApi *self, void *obj, const char *name)` | address of a property's value inside obj (you know the type), NULL if absent |
| `get_bool` | `int get_bool(const AbsApi *self, void *obj, const char *name, int *out)` | bool / bitfield property; 1 if found |
| `set_bool` | `int set_bool(const AbsApi *self, void *obj, const char *name, int value)` |  |
| `call_function_params` | `int call_function_params(const AbsApi *self, void *obj, void *func, void **args, void *ret)` | call_function_params: args[i] = pointer to the value of the i-th non-return parameter (declaration order); out parameters are copied back; ret receives the return value (may be NULL). Game thread only. |
| `construct_object` | `void * construct_object(const AbsApi *self, void *cls, void *outer)` | StaticConstructObject_Internal; NULL on failure. Game thread. |
| `spawn_actor` | `void * spawn_actor(const AbsApi *self, void *cls, const float loc[3], const float rot[3])` | UWorld::SpawnActor in the current world (AlwaysSpawn); rot = pitch, yaw, roll in degrees |
| `get_world` | `void * get_world(const AbsApi *self)` |  |
| `dump_sdk` | `int dump_sdk(const AbsApi *self, const char *name)` | writes LanNative\sdk\<name>.txt: every class / struct / enum / function; returns the count. Game thread. Game derived: never commit. |
| `set_timer` | `int set_timer(const AbsApi *self, float seconds, int repeat, void (*cb)(void *user), void *user)` | game-thread timer |
| `on_world_loaded` | `int on_world_loaded(const AbsApi *self, void (*cb)(const char *map_path, void *user), void *user)` | a new world object appeared (map travel); map_path like "/Game/Maps/.../Name.Name" |
| `frame_peek_int` | `int frame_peek_int(const AbsApi *self, void *frame, int *out)` | in a replace_native thunk: the FIRST argument of the call (int-like), read from the bytecode without consuming it; 0 = not readable |
| `object_path` | `int object_path(const AbsApi *self, void *obj, char *out, int cap)` | full path like /Game/Pkg.Obj or /Engine/Transient.Outer:Sub.Name; returns the length |
| `dump_object` | `int dump_object(const AbsApi *self, void *obj)` | write every property of a live object (engine text) to LanNative\objdump_<name>.txt |
| `trace_function` | `int trace_function(const AbsApi *self, const char *cls, const char *func)` | log every call of a function (also Blueprint-to-Blueprint and BPF_ calls, which skip ProcessEvent) into the tracer ring; start with trace("on") |
| `duplicate_object` | `void * duplicate_object(const AbsApi *self, void *src, void *outer)` | StaticDuplicateObject: a copy of any object (data assets, ...); outer may be the transient package ("/Engine/Transient" via find_object). Game thread. |
| `find_cdo` | `void * find_cdo(const AbsApi *self, const char *short_class_name)` | the class default object ("Default__Name") |
| `create_widget` | `void * create_widget(const AbsApi *self, void *widget_class, void *owning_player)` | UWidgetBlueprintLibrary::Create: a UUserWidget of that class (not yet on screen). Game thread. |
| `add_to_viewport` | `int add_to_viewport(const AbsApi *self, void *widget, int zorder)` | UUserWidget::AddToViewport |
| `remove_from_parent` | `int remove_from_parent(const AbsApi *self, void *widget)` | UWidget::RemoveFromParent |
| `get_local_player_controller` | `void * get_local_player_controller(const AbsApi *self)` | the local FightingPlayerController, NULL before one exists |
| `make_text` | `int make_text(const AbsApi *self, const char *utf8, void *out_ftext24)` | build an FText (24 bytes) from a string, via KismetTextLibrary::Conv_StringToText; 0 = ok. Copy it into a Text property with memcpy(prop_ptr(...), out, 24). Game thread. |
| `text_to_string` | `int text_to_string(const AbsApi *self, const void *ftext24, char *out, int cap)` | an FText back to UTF-8; 0 = ok |
| `on_key` | `int on_key(const AbsApi *self, int vk, void (*cb)(int vk, void *user), void *user)` | game-thread callback on each key-down edge of virtual key vk, only while the game window has the focus |
| `on_damage` | `int on_damage(const AbsApi *self, void (*cb)(void *victim, float amount, int flag, void *user), void *user)` | after AFightingCharacter::ApplyDamage(amount, flag) ran on victim (hook installed on first use); attacker not available here |
| `kv_get` | `int kv_get(const AbsApi *self, const char *key, char *out, int cap)` | per-plugin sidecar store LanNative\data\<plugin>.txt: 1 if found. Mod data belongs here, not in the game's save format |
| `kv_set` | `int kv_set(const AbsApi *self, const char *key, const char *value)` | value NULL removes the key; 0 = ok |

### Since 2.2

| Name | Signature | Description |
|---|---|---|
| `create_asset` | `void * create_asset(const AbsApi *self, void *src, const char *path)` | copy of src (a UObject, e.g. a stock DataAsset) as "/Game/Mods/<Mod>/<Name>.<Name>" in its own package, rooted (the GC keeps it); if src is a UClass a new instance is constructed instead. Existing path with the same class: that object. NULL on failure. Create item objects at on_engine_ready, BEFORE the save is loaded |
| `add_to_root` | `int add_to_root(const AbsApi *self, void *obj, int root)` | RootSet flag on (1) / off (0): keeps a runtime-created object alive. 1 = ok |
| `table_row` | `void * table_row(const AbsApi *self, const char *table_path, const char *row)` | UDataTable::FindRowUnchecked: pointer to the row struct (cast with the SDK dump's offsets) or NULL; table_path like "/Game/DB/Shop/EquipmentShop.EquipmentShop" (must be loaded) |
| `table_add_row` | `int table_add_row(const AbsApi *self, const char *table_path, const char *row, const char *copy_of)` | add row `row` as a clone of row `copy_of` (the rows.json "add" engine); 1 = ok or exists already |
| `row_set_text` | `int row_set_text(const AbsApi *self, const char *table_path, const char *row, const char *prop, const char *text)` | ImportText into one row property, validated like rows.json; an object reference whose object is neither in memory nor in a pak is REFUSED (loading it would hang). A runtime-created object (create_asset) is accepted. 1 = ok |
| `inventory_add` | `int inventory_add(const AbsApi *self, void *item, int material, int equip)` | UInventoryComponent::BPF_AddItemToInventory with m_eSchoolItemType = None (4; 0 would make the item vanish on the next load); material 0 = default; returns the inventory index or -1. equip=1 also calls inventory_equip. Needs a world (local player controller) |
| `inventory_equip` | `int inventory_equip(const AbsApi *self, int index)` | equip inventory entry `index` through InventoryMenu::BPF_Equip(index, 0) when a menu object exists (equipped flags + preset), else UInventoryComponent::EquipFromInventory; 1 = ok |
| `inventory_index` | `int inventory_index(const AbsApi *self, void *item)` | index of `item` in the local player's inventory or -1 (use it to avoid granting twice) |
| `set_text` | `int set_text(const AbsApi *self, void *obj, const char *prop, const char *utf8)` | make_text + store into a Text property (old text is not released: small leak). 1 = ok. Culture-invariant text: one language |
| `on_save_loading` | `int on_save_loading(const AbsApi *self, void (*cb)(void *save, void *user), void *user)` | before USaveThePlaines::InitializeInventory resolves the item paths of a loaded save: last moment to create_asset the items the save names |
| `on_save_written` | `int on_save_written(const AbsApi *self, void (*cb)(void *save, void *user), void *user)` | after the save object's item path list was written (InitInventoryDataToSerializeFromInventoryArray): read the paths to keep a sidecar |

### Since 2.3

| Name | Signature | Description |
|---|---|---|
| `on_hit` | `int on_hit(const AbsApi *self, void (*cb)(const struct AbsHitInfo *hit, void *user), void *user)` | one call per resolved impact (hit, guard, guard break, absorb) and per parry / dodge |
| `on_attack_start` | `int on_attack_start(const AbsApi *self, void (*cb)(void *character, int attack_id, const char *attack, void *user), void *user)` | once per real attack (the ~30 prediction re-runs of OrderAttack::OnStart are filtered out) |
| `on_pawn_spawned` | `int on_pawn_spawned(const AbsApi *self, void (*cb)(void *character, void *user), void *user)` | after AFightingCharacter::BeginPlay (player, NPCs, training dummy) |
| `on_pawn_destroyed` | `int on_pawn_destroyed(const AbsApi *self, void (*cb)(void *character, int reason, void *user), void *user)` | before AFightingCharacter::EndPlay; reason = EEndPlayReason (0 destroyed, 1 level transition, 2 PIE end, 3 removed from world, 4 quit) |
| `on_death` | `int on_death(const AbsApi *self, void (*cb)(void *victim, void *killer, int attack_id, const char *attack, void *user), void *user)` | AFightingCharacter::ServerKilledBy; killer may be NULL |
| `on_menu` | `int on_menu(const AbsApi *self, void (*cb)(void *menu, const char *menu_class, const char *state, void *user), void *user)` | after a menu widget's "EnterState <Name>" / "EnterState_<Name>" event: state = "<Name>" (CharacterMenu, ShopMenu, InventoryMenu, ...) |
| `char_info` | `int char_info(const AbsApi *self, void *character, struct AbsCharInfo *out)` | fill *out (set out->size = sizeof *out first); 1 = ok. style needs the game thread (-1 elsewhere) |

### Since 2.4

| Name | Signature | Description |
|---|---|---|
| `register_content_kind` | `int register_content_kind(const AbsApi *self, const char *kind, int (*validate)(const char *json, char *err, int cap, void *user), int (*apply)(const char *mod_id, const char *entry_id, const char *json, void *user), void *user)` |  |

### Since 2.5

| Name | Signature | Description |
|---|---|---|
| `create_texture` | `void * create_texture(const AbsApi *self, const void *bgra, int width, int height, const char *path)` | 32-bit BGRA pixels (straight alpha), row-major, top row first, 1..4096 per side |
| `texture_from_file` | `void * texture_from_file(const AbsApi *self, const char *file_utf8, const char *path)` | PNG / JPEG / BMP / GIF / TIFF decoded by Windows Imaging Component; file = absolute path |

### Since 2.6

| Name | Signature | Description |
|---|---|---|
| `test_heal` | `int test_heal(const AbsApi *self, void *character)` | health and guard to full, ghost damage and down state cleared; 1 = ok |
| `test_set_invincible` | `int test_set_invincible(const AbsApi *self, void *character, int on)` | HealthComponent::ServerSetInvicible; returns the resulting state (0/1) or -1 |
| `test_attack` | `int test_attack(const AbsApi *self, void *attacker, const char *attack_row, void *target)` | the character performs attack row `attack_row` ("135", "BT2_01"...) at `target` (may be NULL: faces forward) through the game's own attack order, no keyboard, no deck needed; 1 = launched (confirm with on_attack_start / on_hit) |
| `test_guard` | `int test_guard(const AbsApi *self, void *character, int on)` | raise (1) / drop (0) the guard; 1 = ok |
| `test_teleport` | `int test_teleport(const AbsApi *self, void *actor, const float loc[3], const float *yaw_degrees)` | AActor::K2_TeleportTo; yaw NULL keeps the rotation; 1 = ok |
| `test_get_location` | `int test_get_location(const AbsApi *self, void *actor, float out[3])` | world location of an actor's root component; 1 = ok |
| `test_spawn_npc` | `void * test_spawn_npc(const AbsApi *self, const char *archetype_path, const float loc[3])` | spawn an NPC from an archetype asset ("/Game/DB/AI/NPCs/Trickster/Trickster.Trickster") through the game's AAISpawner chain and move it to loc (NULL = where it appears); returns the character or NULL |

### Since 2.7

| Name | Signature | Description |
|---|---|---|
| `add_menu_item` | `int add_menu_item(const AbsApi *self, const char *label, void (*cb)(void *user), void *user)` | an entry on the F1 menu's "Mods" page (labelled "<your mod>: <label>"); cb runs on the game thread when the player selects it; 0 = ok, 1 = table full / bad arguments |
| `reload_content` | `int reload_content(const AbsApi *self)` | hot reload: re-read Mods/<mod>/content (items, styles, plugin kinds) and rows.json and apply what changed without a restart (what cannot be reloaded is logged); returns the number of changed or new content entries, or -1 if nothing could be reloaded. Game thread. Same as creating LanNative/reload.txt or the first line of the F2 Mods page |

### Since 2.8

| Name | Signature | Description |
|---|---|---|
| `test_dodge` | `int test_dodge(const AbsApi *self, void *character, const float dir[3])` | the character starts a dodge towards the WORLD direction dir (x, y; z ignored; for example away from the opponent) through AFightingCharacter::PlayDodgeOrder; a dodge that is active when an attack connects ends in on_hit outcome ABS_HIT_DODGED. 1 = launched |
| `test_ai` | `int test_ai(const AbsApi *self, void *character, int enabled)` | stop (0) / restart (1) the character's behaviour tree (AIController::BrainComponent StopLogic / RestartLogic): a stopped NPC stands still and only does what test_attack / test_guard / test_dodge tell it; 1 = ok |
| `test_hittable` | `int test_hittable(const AbsApi *self, void *character, int on)` | an AI character can only be hit while its AI is running: on = 1 sets the character's hittable flag so a stopped NPC or the practice dummy takes driven attacks, 0 clears it; returns the resulting value or -1 |
| `test_get_forward` | `int test_get_forward(const AbsApi *self, void *actor, float out[3])` | unit vector the actor's root component faces (for the player: where the camera looks; teleporting does not turn the player, the controller does); 1 = ok |

### Since 2.9

| Name | Signature | Description |
|---|---|---|
| `inventory_equip_slot` | `int inventory_equip_slot(const AbsApi *self, int index, int slot)` | FightingPlayerController::EquipFromInventory(index, slot): put inventory entry `index` (inventory_add / inventory_index) into power slot 0 or 1; 1 = ok. inventory_equip cannot do this (it uses slot -1, which a power slot refuses) |
| `on_power_used` | `int on_power_used(const AbsApi *self, void (*cb)(void *character, void *item_data, void *actor, int dir, void *user), void *user)` | a power's effect actor was spawned (BPE_BeginItemUseAction) after AFightingCharacter::LaunchItemUseOrder: character = user (player or NPC), item_data = the UUsableItemData, actor = the spawned AUsableItem instance, dir 0/1. The place to patch the INSTANCE (m_ItemAttackName, hit shape radius): never write the stock class defaults. Hooks are installed on first registration; 0 = ok |
| `power_use` | `int power_use(const AbsApi *self, void *character, int dir)` | use the power in slot dir without a key (keys 1/3 do not reach powers in practice mode): for the local player (character NULL = the player) UInventoryComponent::CanUseEquippedItem must say CanUse (else refused, reason logged), then AFightingCharacter::LaunchItemUseOrder; for an NPC just the launch. The HUD's launch event is not sent. 1 = launched |

### Since 2.10

| Name | Signature | Description |
|---|---|---|
| `combo_slot_ensure` | `int combo_slot_ensure(const AbsApi *self, int family, const char *name, int copy_from_index)` | index of the deck named `name`; if missing it is created as a deep copy of deck copy_from_index (appended: the 4th deck gets index 3). -1 on error |
| `combo_load` | `int combo_load(const AbsApi *self, int family, const char *name)` | make that deck current: FightingPlayerController::BPF_LoadComboFromName(name, family, Current) (also loads it into the attack component); 1 = ok |
| `combo_current` | `int combo_current(const AbsApi *self, int family, char *name, int cap)` | current deck name of the family; returns its index or -1 |
| `set_combat_style` | `int set_combat_style(const AbsApi *self, void *character, int style)` | local player (character NULL or the local pawn): FightingPlayerController::BPF_SetPlayerCombatStyle(style, Current); 1 = the style is now `style` |
| `on_weapon_changed` | `int on_weapon_changed(const AbsApi *self, void (*cb)(void *character, void *old_item, void *new_item, int handled, void *user), void *user)` | the local player's equipped weapon (InventoryComponent.m_EquippedWeapon) changed; polled every tick; fires once with old_item NULL when the character appears; handled is always 0 (picked-up weapons are not reported here; use on_handled_weapon) |
| `on_style_changed` | `int on_style_changed(const AbsApi *self, void (*cb)(void *character, int old_style, int new_style, void *user), void *user)` | the local player's fighting style changed (PlayerFightingComponent.m_uiCombatStyle); polled every tick; first call old_style -1 |
| `prop_ptr_path` | `void * prop_ptr_path(const AbsApi *self, void *obj, const char *path)` | address of a nested property: "m_SavedData.m_CurrentComboSaveName[1]", "OwningGameInstance.m_CurrentSave" (through structs, object pointers, static arrays and TArray elements); NULL (logged) if a step is missing |
| `fname_make` | `int fname_make(const AbsApi *self, const char *utf8, uint64_t *out)` | FName from a string (added to the name table); 0 = ok |
| `fname_text` | `int fname_text(const AbsApi *self, const void *fname, char *out, int cap)` | an FName (8 bytes) as UTF-8; 0 = ok |

### Since 2.11

| Name | Signature | Description |
|---|---|---|
| `set_timer_id` | `int set_timer_id(const AbsApi *self, float seconds, int repeat, void (*cb)(void *user), void *user)` | like set_timer but returns the timer id (> 0), 0 = not created (reason logged) |
| `cancel_timer` | `int cancel_timer(const AbsApi *self, int timer_id)` | remove one of YOUR timers (also from inside its own callback); 1 = removed, 0 = unknown id / not yours / already fired |

### Since 2.12

| Name | Signature | Description |
|---|---|---|
| `travel` | `int travel(const AbsApi *self, const char *map_or_url)` | console "open <map>?listen" ("/Game/Maps/.../Name[.Name]" gets ?listen unless it has options; "open ..." text is sent as is). The world changes a moment later (on_world_loaded). 1 = issued. For a lone player in a 1v1 arena call solo_duel(1) first |
| `console_cmd` | `int console_cmd(const AbsApi *self, const char *cmd)` | KismetSystemLibrary::ExecuteConsoleCommand with the local player controller; 1 = issued |
| `solo_duel` | `int solo_duel(const AbsApi *self, int on)` | LanNative's soloduel switch: 1 = a hosted 1v1 starts with only the local player (m_iNumTeams 2 -> 1), 0 = stock, -1 = just read; returns the state. Takes effect for the NEXT travel |
| `match_info` | `int match_info(const AbsApi *self, struct AbsMatchInfo *out)` | fill *out (set out->size first); 1 = a game state exists, 0 = none (open world: state "") |
| `on_match_state` | `int on_match_state(const AbsApi *self, void (*cb)(const char *old_state, const char *new_state, void *user), void *user)` | GameState MatchState changed (polled 10x per second by the loader); old_state "" on the first sight of a world |
| `on_round_end` | `int on_round_end(const AbsApi *self, void (*cb)(const struct AbsMatchInfo *info, int winner_team, void *user), void *user)` | a team score went up (a round was decided), polled 10x per second; winner_team = the team whose score rose. Not fired when scores are reset |
| `set_mode_prop` | `int set_mode_prop(const AbsApi *self, const char *name, const char *text)` | engine-text property write on the live game mode AND game state (those that have it), e.g. ("m_iNbWinningRounds","5"); returns a bit mask 1 = game mode, 2 = game state, 0 = neither/refused. Do it right after on_world_loaded, before the match starts |
| `show_banner` | `int show_banner(const AbsApi *self, const char *utf8, float seconds)` | centred message on the HUD for `seconds` (queued, max 8); 1 = queued. Uses a stock text widget (see docs/PLUGIN-API.md) |
| `hud_score` | `int hud_score(const AbsApi *self, const char *utf8)` | persistent score line at the top of the screen; NULL or "" removes it; 1 = ok |

### Since 2.13

| Name | Signature | Description |
|---|---|---|
| `mapscript_phase` | `int mapscript_phase(const AbsApi *self, const char *name, int on)` | set a named phase flag (lights/actions of the scene test it); 1 = ok |
| `mapscript_mesh` | `int mapscript_mesh(const AbsApi *self, const char *mesh_id, const char *state)` | move a scene mesh to a named state ("open", "closed"); 1 = ok |

### Since 2.14

| Name | Signature | Description |
|---|---|---|
| `npc_spawn` | `void * npc_spawn(const AbsApi *self, const char *archetype, const float loc[3], const float *yaw_deg, const char *class_path)` | spawn an NPC AT loc (NULL = 350 uu in front of the local character): `archetype` = an ArchetypeAsset path ("/Game/DB/AI/NPCs/Trickster/Trickster.Trickster", or a mod copy "/Game/Mods/<mod>/<id>.<id>") or the plain id of an npcs content entry (then its faction / aggro / reactions apply). The spawner actor is placed at loc BEFORE the spawn and the pawn is never teleported afterwards (a teleported AI NPC never moves again). class_path NULL / "" = the game's generic AI character. Returns the character or NULL |
| `npc_remove` | `int npc_remove(const AbsApi *self, void *character)` | remove an NPC made by npc_spawn (UnSpawnAi + destroy our spawner), or destroy any other character actor; 1 = ok |
| `npc_set_target` | `int npc_set_target(const AbsApi *self, void *character, void *target)` | UAIFightingComponent::BPF_ForceEnemy(target): the NPC goes for that actor (the player: get_local_player_controller + K2_GetPawn). NULL clears; 1 = ok |
| `npc_set_faction` | `int npc_set_faction(const AbsApi *self, void *character, int faction)` | AFightingCharacter::ServerSetFaction(0..5) (written directly if that does not take); returns the faction read back or -1 |
| `faction_can_target` | `int faction_can_target(const AbsApi *self, int attacker_faction, int target_faction, int set)` | UFactionsManager target table (6 x 6): set -1 = read only, 0 / 1 = write; returns the value now in the table or -1 |
| `npc_archetype` | `void * npc_archetype(const AbsApi *self, void *character)` | the ArchetypeAsset the character runs (m_ArchetypeDescription.m_Archetype) or NULL |
| `on_area` | `int on_area(const AbsApi *self, const float center[3], void *center_actor, float radius, void *who, void (*cb)(void *character, int entered, void *user), void *user)` | loader-side poll (one 0.1 s timer for all plugins, 2D distance, 5 % hysteresis): cb(who, 1/0, user) when `who` enters / leaves radius uu around the point `center` or around `center_actor` (a character: the area is dropped when it is destroyed). who NULL = the local player (not "any character"). Returns a handle (> 0) or 0 |
| `area_remove` | `int area_remove(const AbsApi *self, int handle)` | stop watching; 1 = removed |
| `show_message` | `int show_message(const AbsApi *self, const char *utf8, float seconds)` | on-screen text (the loader's toast, up to 3 lines of 96 characters, 1..30 s); 1 = shown |

### Since 2.15

| Name | Signature | Description |
|---|---|---|
| `xp_add` | `int xp_add(const AbsApi *self, void *character, int amount)` | StatsComponent.BPF_AddGlobalXP(GAMEMODE, amount, Current, notify 0): levels / Prestige rise exactly like after a combat trial; 1..10000000; 1 = called |
| `gm_points_add` | `int gm_points_add(const AbsApi *self, void *character, int points)` | native UStatsComponent::AddGameModeXP(points, Current): game-mode points, one Rift Coin (loot box) per 5; returns the Rift Coins added or -1 |
| `rank_info` | `int rank_info(const AbsApi *self, void *character, struct AbsRankInfo *out)` | level, Prestige grade / level, XP, next threshold (set out->size first); 1 = ok |
| `trial_reward` | `int trial_reward(const AbsApi *self, void *character, const char *ranks_id, int won, int rounds_won, int mentoring, int pay, struct AbsTrialReward *out)` | the stock 1v1 reward rules (500 finish, +4000 win, +1500 for 2+ rounds won, +500 mentoring; 1 point each) or a "ranks" entry's table (ranks_id); pay = 1 grants it with xp_add + gm_points_add. Returns the XP; out may be NULL |
| `npc_difficulty` | `float npc_difficulty(const AbsApi *self, void *npc, float set)` | the AI's live difficulty level 0..100 (UAIFightingComponent m_fCurrentDifficultyLevel, rubber-bands during a fight); set >= 0 writes it; -1 = not an AI character |

### Since 2.16

| Name | Signature | Description |
|---|---|---|
| `bot_trial_start` | `int bot_trial_start(const AbsApi *self, const char *trial_id)` | start the bot_trials entry `trial_id` (solo duel + travel to its map, rounds against its bot(s), reward, travel back); 1 = started, 0 = unknown id / a trial is already running |
| `bot_trial_status` | `int bot_trial_status(const AbsApi *self, struct AbsTrialStatus *out)` | the running (or last finished) trial; set out->size first; 1 = there is one |

### Since 2.17

| Name | Signature | Description |
|---|---|---|
| `anim_style_set` | `int anim_style_set(const AbsApi *self, void *character, int style_index)` | animation lookups of `character` use table index style_index (0..63) from now on; -1 removes the override. Game thread. The hooks are installed at the first call with a style. 1 = ok |
| `anim_style` | `int anim_style(const AbsApi *self, void *character)` | the style index the character's animation lookups use now (override, else the real style); -1 = not a character |
| `real_style` | `int real_style(const AbsApi *self, void *character)` | the fighting style as the game sees it (AFightingCharacter::BPF_GetCombatStyle, never overridden); -1 = none / uninitialised |
| `on_handled_weapon` | `int on_handled_weapon(const AbsApi *self, void (*cb)(void *character, void *weapon_data, int family, void *user), void *user)` | the weapon IN HAND of the local player changed (drawn, sheathed, disarmed, picked up): weapon_data = the VisibleWeaponData in hand or NULL, family = the active weapon family (0 when nothing is in hand); polled per tick, fires once when the character appears. 0 = ok |

### Since 2.18

| Name | Signature | Description |
|---|---|---|
| `sidecar_get` | `int sidecar_get(const AbsApi *self, void *character, const char *key, char *out, int cap)` | 1 = found (out = value), 0 = absent / no character save loaded |
| `sidecar_set` | `int sidecar_set(const AbsApi *self, void *character, const char *key, const char *value)` | value NULL removes; key without '=', CR, LF; 1 = stored on disk |

### Since 2.19

| Name | Signature | Description |
|---|---|---|
| `on_hit_pre` | `int on_hit_pre(const AbsApi *self, void (*cb)(struct AbsHitPre *hit, void *user), void *user)` | before the game resolves an impact on a character: change the scales in *hit (see AbsHitPre). 0 = ok, 1 = bad arguments / 16 callbacks, 2 = hooks failed |
| `damage_mod` | `int damage_mod(const AbsApi *self, void *character, const char *key, float value)` | per-character multiplier 0..100 (1 = off): damage_taken, damage_dealt, stun_taken, stun_dealt, knockback_taken, knockback_dealt, special_ratio (the attacker's cut share). on_hit_pre starts from these. 1 = ok |
| `poise_set` | `int poise_set(const AbsApi *self, void *character, float poise, int heavy_power)` | poise gauge: while > 0 hits below heavy_power (EAttackPowers 0 Light, 1 Medium, 2 Strong; -1 = default 2) do not interrupt and drain it by their damage; an empty gauge or a heavy hit interrupts and refills it. poise <= 0 removes. 1 = ok |
| `poise_get` | `float poise_get(const AbsApi *self, void *character)` | current gauge, -1 = no poise |

### Since 2.20

| Name | Signature | Description |
|---|---|---|
| `on_defense` | `int on_defense(const AbsApi *self, void (*cb)(const struct AbsDefenseInfo *d, void *user), void *user)` | one call per successful defence (guard, guard break, absorb, parry, deflect, dodge, avoid; see AbsDefenseInfo). 0 = ok, 1 = bad arguments / 16 callbacks, 2 = hooks failed |

### Since 2.21

| Name | Signature | Description |
|---|---|---|
| `overlay_info` | `int overlay_info(const AbsApi *self, struct AbsOverlayInfo *out)` | any thread; fills what fits in out->size and sets out->size to the bytes filled. 0 = ok, 1 = bad arguments or the overlay has not started yet, 2 = unavailable (dead; out->state = -1). Bind the context only inside a draw callback |
| `overlay_register` | `int overlay_register(const AbsApi *self, void (*draw)(void *user), void *user)` | draw(user) runs once per overlay frame on the render thread, after the loader's windows, in registration order; 16 callbacks in total across all plugins. 0 = ok, 1 = bad arguments / the same (draw, user) twice / table full, 2 = unavailable |
| `overlay_unregister` | `int overlay_unregister(const AbsApi *self, void (*draw)(void *user), void *user)` | 0 = removed (may still run once in the frame in progress), 1 = not found |
| `overlay_set_mode` | `int overlay_set_mode(const AbsApi *self, unsigned flags)` | your input needs, ABS_OV_*; 0 = nothing. Modes are per plugin and ORed with the other plugins'; frames run while any mode is set or a loader menu is open. 0 = ok, 1 = bad arguments, 2 = unavailable |
| `overlay_raw_mouse` | `int overlay_raw_mouse(const AbsApi *self, int *dx, int *dy)` | any thread: raw mouse movement collected since the last call while a plugin has ABS_OV_RAWMOUSE (the accumulator is shared: use it from one plugin at a time). 0 = ok (zeros when nobody has the mode), 1 = bad arguments / unavailable |

## Structs

`AbsHitPre` (`on_hit_pre`, 2.19), `AbsDefenseInfo` with the `ABS_DEF_*` types (`on_defense`, 2.20) and `AbsOverlayInfo` with the `ABS_OV_*` modes (`overlay_*`, 2.21) are documented in the header.

### `AbsHitInfo`

| Member | Type | Description | Since |
|---|---|---|---|
| `victim` | `void *` | AFightingCharacter that was hit (or parried / dodged) | 2.0 |
| `attacker` | `void *` | the attacking actor (AFightingCharacter for melee), may be NULL | 2.0 |
| `attack_id` | `int` | attack row id as a number ("135" -> 135), -1 if the row name is not numeric | 2.0 |
| `attack[32]` | `char` | attack row name (attacks DataTable / item attack) | 2.0 |
| `damage` | `float` | final damage after all multipliers (0 for guarded / absorbed / parried / dodged / no effect) | 2.0 |
| `outcome` | `int` | ABS_HIT_* | 2.0 |
| `perfect_link` | `int` | 1 if the hit was a perfect (timed) link | 2.0 |

### `AbsCharInfo`

| Member | Type | Description | Since |
|---|---|---|---|
| `size` | `uint32_t` | set to sizeof(AbsCharInfo) before the call: fields are only appended | 2.0 |
| `name[64]` | `char` | object name, e.g. BP_AICharacter_Training_C_0 | 2.0 |
| `path[256]` | `char` | full object path | 2.0 |
| `health` | `float` | HealthComponent m_fHealth, m_fMaxHealth + m_fBonusMaxHealth | 2.0 |
| `max_health` | `float` | HealthComponent m_fHealth, m_fMaxHealth + m_fBonusMaxHealth | 2.0 |
| `guard` | `float` | DefenseComponent m_fCurrentGuard, m_fMaxGuard | 2.0 |
| `max_guard` | `float` | DefenseComponent m_fCurrentGuard, m_fMaxGuard | 2.0 |
| `stance` | `int` | PlayerAnim m_eCurrentQuadrant: 0 FrontLeft, 1 FrontRight, 2 BackRight, 3 BackLeft, -1 unknown | 2.0 |
| `style` | `int` | StatsComponent::BPF_GetFightingStyleID(Current): 0 Forsaken, 1 Windfall, 2 Lost Prospect, 3 Kahlt, 4 Stagger, 5 Faejin; -1 unknown | 2.0 |
| `is_player` | `int` | controlled by a PlayerController | 2.0 |
| `is_dead` | `int` | health <= 0 | 2.0 |
| `invincible` | `int` | UHealthComponent::m_bIsInvincible (1 = cannot be damaged), -1 unknown | 2.6 |
| `is_down` | `int` | HealthComponent m_bIsDown | 2.6 |
| `guarding` | `int` | DefenseComponent m_bIsGuarding | 2.6 |
| `attacking` | `int` | UAttackComponent::BPF_IsPlayingAttack (game thread only, else -1) | 2.6 |

### `ABS_HIT_*` (on_hit outcome)

| Name | Value |
|---|---|
| `ABS_HIT` | 0 |
| `ABS_HIT_GUARDED` | 1 |
| `ABS_HIT_GUARD_BROKEN` | 2 |
| `ABS_HIT_ABSORBED` | 3 |
| `ABS_HIT_PARRIED` | 4 |
| `ABS_HIT_DODGED` | 5 |
| `ABS_HIT_NO_EFFECT` | 6 |
