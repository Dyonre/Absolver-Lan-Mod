/* AbsolverSE.h - the plugin API of the Absolver mod loader / script extender (API version 2).
 *
 * A mod folder may contain plugin.dll (C or C++, 64-bit). The loader loads it after the game's paks are mounted and calls
 *     int AbsPlugin_Load(const AbsApi *api);          // return 0 = ok, anything else = the plugin is switched off (and logged)
 * Optionally the DLL exports  void AbsPlugin_Unload(void)  (called at game exit).
 *
 * EVERY function takes the AbsApi pointer you were given as its first argument ("self"). That is how the loader knows which plugin is
 * calling, also from your own hook detours and your own threads. Typical use:  static const AbsApi *A;  ...  A->log(A, "hello %d", 5);
 * New fields are only ever APPENDED at the end of the struct; check api->size before using a field newer than the version you built against.
 *
 * Rules
 *  - A plugin is arbitrary code, so it only loads if its folder name is listed (without #) in <game folder>\Mods\plugins.txt.
 *    The loader appends "#Name" to that file the first time it sees an unlisted plugin.dll.
 *  - Keep your own copy of `api` for the duration of the game; the struct stays valid. Check api->version and api->size first.
 *  - Callbacks run in the game's threads. on_tick runs on the game thread; on_object_loaded can run on ANY thread (the async loader):
 *    do almost nothing in it, queue the rest with run_on_game_thread. Never block.
 *  - Every callback runs inside a guard: after 3 crashes in your code the loader switches your plugin's callbacks off and logs it.
 *  - Game addresses are offsets from api->game_base for ONE game build (Absolver 1.31 b1.25_575). hook_rva refuses to patch when the
 *    live bytes differ from the ones you pass, so a plugin cannot corrupt a different build.
 *  - Several plugins (and the loader itself) may hook the same function: hooks STACK. The `original` pointer hook_rva returns is "the next
 *    function in the chain": the previously installed detour, or finally the game's own code. Always call it (unless you mean to replace).
 *  - Strings are UTF-8 on this interface; Unreal property text is the engine's own text format (what ExportText/ImportText use).
 *  - Offline / private play only. The loader does not run at all without -NoEAC.
 */
#ifndef ABSOLVER_SE_H
#define ABSOLVER_SE_H
#include <stdint.h>
#include <stddef.h>

#define ABS_API_VERSION 2

struct AbsApi;
typedef struct AbsApi AbsApi;
struct AbsHitPre;              /* v2.19, defined after AbsApi */
struct AbsDefenseInfo;         /* v2.20, defined after AbsApi */
struct AbsOverlayInfo;         /* v2.21, defined after AbsApi */

/* on_hit outcome */
enum { ABS_HIT = 0, ABS_HIT_GUARDED = 1, ABS_HIT_GUARD_BROKEN = 2, ABS_HIT_ABSORBED = 3, ABS_HIT_PARRIED = 4, ABS_HIT_DODGED = 5, ABS_HIT_NO_EFFECT = 6 };
typedef struct AbsHitInfo {
    void *victim;              /* AFightingCharacter that was hit (or parried / dodged) */
    void *attacker;            /* the attacking actor (AFightingCharacter for melee), may be NULL */
    int attack_id;             /* attack row id as a number ("135" -> 135), -1 if the row name is not numeric */
    char attack[32];           /* attack row name (attacks DataTable / item attack) */
    float damage;              /* final damage after all multipliers (0 for guarded / absorbed / parried / dodged / no effect) */
    int outcome;               /* ABS_HIT_* */
    int perfect_link;          /* 1 if the hit was a perfect (timed) link */
} AbsHitInfo;
typedef struct AbsCharInfo {
    uint32_t size;             /* set to sizeof(AbsCharInfo) before the call: fields are only appended */
    char name[64];             /* object name, e.g. BP_AICharacter_Training_C_0 */
    char path[256];            /* full object path */
    float health, max_health;  /* HealthComponent m_fHealth, m_fMaxHealth + m_fBonusMaxHealth */
    float guard, max_guard;    /* DefenseComponent m_fCurrentGuard, m_fMaxGuard */
    int stance;                /* PlayerAnim m_eCurrentQuadrant: 0 FrontLeft, 1 FrontRight, 2 BackRight, 3 BackLeft, -1 unknown */
    int style;                 /* StatsComponent::BPF_GetFightingStyleID(Current): 0 Forsaken, 1 Windfall, 2 Lost Prospect, 3 Kahlt, 4 Stagger, 5 Faejin; -1 unknown */
    int is_player;             /* controlled by a PlayerController */
    int is_dead;               /* health <= 0 */
    /* appended in API 2.6 (check out->size): */
    int invincible;            /* UHealthComponent::m_bIsInvincible (1 = cannot be damaged), -1 unknown */
    int is_down;               /* HealthComponent m_bIsDown */
    int guarding;              /* DefenseComponent m_bIsGuarding */
    int attacking;             /* UAttackComponent::BPF_IsPlayingAttack (game thread only, else -1) */
} AbsCharInfo;
typedef struct AbsMatchInfo {  /* API 2.12 match_info: the current world's game mode / game state  */
    uint32_t size;             /* set to sizeof(AbsMatchInfo) before the call */
    char state[32];            /* GameState MatchState text: "EnteringMap" "WaitingToStart" "Intro" "StartCountdown" "InProgress" "WaitingPostMatch" ... ("" = no game state) */
    char mode[64];             /* game mode class short name, e.g. 1V1_GameMode_C ("" = none) */
    int is_duel;               /* the game mode is an AdversarialGameMode (1v1, 3v3, ...) */
    int team_count;            /* GameState m_iNumTeams (0 if unknown) */
    int winning_rounds;        /* GameState m_iNbWinningRounds: rounds to win the match (0 if unknown) */
    int winner_team;           /* GameState BPF_GetWinnerTeamIndex, only meaningful once the match is decided; -1 unknown */
    int team_score[4];         /* BPF_GetTeamScore(0..3): rounds won per team; -1 if unknown */
    int round;                 /* current round number = rounds played so far + 1 (while InProgress), else rounds played */
    float server_time;         /* GameState server world time seconds (0 if unknown) */
    void *game_mode, *game_state;
} AbsMatchInfo;
typedef struct AbsRankInfo {   /* API 2.15 rank_info  */
    uint32_t size;             /* set to sizeof(AbsRankInfo) before the call */
    int level;                 /* character level 0..60 = m_iGlobalLevel + unspent stat points (BPF_GetCharacterLevel): the level the XP curve tracks */
    int prestige_grade, prestige_level;   /* m_iPrestigeGrade (the badge), m_iPrestigeLevel (the number shown once grade > 0) */
    int visible_level;         /* StatsComponent.BPF_GetVisibleCharacterLevel(Current) */
    int xp;                    /* m_iGlobalXP: a CUMULATIVE total (Req(60) = 672,000; in Prestige the running total of the current grade) */
    int next_xp;               /* XP total of the next level / Prestige level (ranks.h rk_next_xp; Req(61) = 692,000 = next grade) */
    int next_xp_game;          /* the same threshold from the game's BPF_GetRequiredXPForLevel, -1 if not callable (should equal next_xp) */
    int gm_xp, gm_level;       /* game-mode points and level (Rift Coins: one per level, 5 points per level) */
    int spare_prestige_points; /* m_iSparePrestigePoints (the "Crystals" readout) */
} AbsRankInfo;
typedef struct AbsTrialReward {/* API 2.15 trial_reward */
    uint32_t size;
    int xp, points;            /* what the table gives */
    int rift_coins;            /* Rift Coins added by paying the points (-1 not paid / failed) */
    int paid;                  /* 1 = xp_add went through */
} AbsTrialReward;
typedef struct AbsTrialStatus {/* API 2.16 bot_trial_status */
    uint32_t size;             /* set to sizeof(AbsTrialStatus) before the call */
    char id[64];               /* bot_trials entry id */
    char phase[24];            /* "travel" "wait_match" "spawn" "fight" "round_end" "end" "done" "aborted" */
    int stage, stages;         /* current bot (0-based) of the gauntlet, number of bots */
    int player_score, bot_score, rounds_to_win;   /* rounds won in this stage */
    int total_player, total_bot; /* rounds won over the whole trial */
    int finished, won;         /* 1 when the trial is over; won = the player beat every bot */
    int xp, points;            /* reward paid at the end (trial_reward), 0 before */
    int ai_level;              /* AI level of the current bot */
    void *bot;                 /* the current bot character (NULL between rounds) */
} AbsTrialStatus;

struct AbsApi {
    uint32_t size;                 /* sizeof(AbsApi) of the loader: fields past this are not there */
    uint32_t version;              /* ABS_API_VERSION of the loader */
    uint8_t *game_base;            /* base address of Absolver-Win64-Shipping.exe */
    const char *plugin_name;       /* your mod folder's name */
    const char *plugin_dir;        /* absolute path of your mod folder, UTF-8, no trailing slash */

    /* ---- logging and settings */
    void (*log)(const AbsApi *self, const char *fmt, ...);                       /* printf-style; line goes to LanNative\lannative.log tagged with your name */
    int  (*config_get)(const AbsApi *self, const char *key, char *out, int cap); /* value of "key = value" from <your mod folder>\plugin.cfg; 1 if found */

    /* ---- events (return 0 on success). Several per plugin are fine. */
    int  (*on_tick)(const AbsApi *self, void (*cb)(float dt, void *user), void *user);                  /* every frame, game thread */
    int  (*on_object_loaded)(const AbsApi *self, void (*cb)(void *obj, void *user), void *user);        /* after any UObject finished loading; ANY thread; hot: be quick */
    int  (*on_engine_ready)(const AbsApi *self, void (*cb)(void *user), void *user);                    /* once, on the first game tick (GEngine exists); runs at once if that already happened */
    int  (*run_on_game_thread)(const AbsApi *self, void (*fn)(void *user), void *user);                 /* queue fn for the start of the next game tick; safe from any thread */
    int  (*is_game_thread)(const AbsApi *self);

    /* ---- code hooks */
    /* Patches `rva` (offset from game_base): `expected` are the original bytes, `stolen` how many whole instructions' bytes (14..24) to move.
     * Returns the next function in the chain (call it from the detour) or NULL (bytes differ / failed; nothing was changed).
     * If the function is already hooked by the loader or another plugin the hook is stacked on top of it: `expected` must still be the game's ORIGINAL bytes.
     * Installation suspends the other threads for a moment, so it is safe at any time (still: prefer doing it in AbsPlugin_Load). */
    void *(*hook_rva)(const AbsApi *self, const char *name, uint32_t rva, const uint8_t *expected, uint32_t stolen, void *detour);

    /* ---- reflection: objects, classes, properties, functions */
    void *(*find_object)(const AbsApi *self, const char *path);                     /* StaticFindObject: "/Game/DB/X.X" (already loaded objects only); NULL if absent */
    void *(*load_object)(const AbsApi *self, const char *path);                     /* StaticLoadObject: loads the asset if needed (game thread!) */
    void *(*object_class)(const AbsApi *self, void *obj);                           /* UClass* of an object */
    int   (*object_name)(const AbsApi *self, void *obj, char *out, int cap);        /* short name; returns length */
    int   (*class_name)(const AbsApi *self, void *cls, char *out, int cap);
    int   (*is_a)(const AbsApi *self, void *obj, const char *class_name);           /* 1 if obj's class or a parent class has that short name (e.g. "UserWidget") */
    void *(*find_property)(const AbsApi *self, void *cls, const char *name);        /* UProperty* by name, searching parent classes; NULL if none */
    int   (*property_offset)(const AbsApi *self, void *prop);                       /* byte offset of the value inside the object */
    int   (*get_property_text)(const AbsApi *self, void *obj, const char *name, char *out, int cap);          /* engine text of one property ("name[N]" = element N of a static array); returns length or -1 */
    int   (*set_property_text)(const AbsApi *self, void *obj, const char *name, const char *text);            /* ImportText in place ("name[N]" = element N of a static array); 1 on success. Same caveats as rows.json: validate numbers yourself */
    void *(*find_function)(const AbsApi *self, void *obj, const char *name);        /* UFunction* (Blueprint events included) or NULL */
    int   (*call_function)(const AbsApi *self, void *obj, void *func, void *params);/* UObject::ProcessEvent: params = the function's parameter block (inputs, then outputs), game thread only */
    int   (*for_each_object)(const AbsApi *self, void (*cb)(void *obj, void *user), void *user);              /* every live UObject (slow: scan, game thread); returns the count */

    /* ---- v2.1: script extender additions. Class / function names are the engine's SHORT names
     *      without the U/A prefix, as listed in the SDK dump: "StatsComponent", "BP_CharacterMenu_C", "BPF_IsFightingStyleUnlocked". */
    void *(*hook_symbol)(const AbsApi *self, const char *name, void *detour);              /* like hook_rva but by PDB name from LanNative\symbols.bin ("UObject::ProcessEvent"); returns the next function in the chain */
    uint32_t (*find_symbol)(const AbsApi *self, const char *name);                         /* RVA of a hookable symbol, 0 if unknown */
    void *(*find_class)(const AbsApi *self, const char *short_name);                      /* UClass* by short name (first call scans all objects; cached) */
    void *(*find_function_in_class)(const AbsApi *self, void *cls, const char *name);     /* UFunction* declared in the class or a parent class */
    /* on_call: pre runs BEFORE the function, may change *params, and returns nonzero to SKIP the original; post runs after it. cls may be NULL (any class); matched by name. Hot path: be quick. */
    int  (*on_call)(const AbsApi *self, const char *cls, const char *func, int (*pre)(void *obj, void *func, void *params, void *user), void (*post)(void *obj, void *func, void *params, void *user), void *user);
    int  (*trace)(const AbsApi *self, const char *cmd, const char *names);                /* ProcessEvent tracer: "on" [comma list of function/class names; empty = everything], "off", "clear", "dump" (writes LanNative\trace\) */
    /* replace_native: swaps UFunction::Func of a native (C++) Blueprint-callable function by name: thunk(void *context, void *frame, void *result). orig receives the old pointer. No byte patching. */
    int  (*replace_native)(const AbsApi *self, const char *cls, const char *func, void *thunk, void **orig);
    void *(*prop_ptr)(const AbsApi *self, void *obj, const char *name);                    /* address of a property's value inside obj (you know the type), NULL if absent */
    int  (*get_bool)(const AbsApi *self, void *obj, const char *name, int *out);           /* bool / bitfield property; 1 if found */
    int  (*set_bool)(const AbsApi *self, void *obj, const char *name, int value);
    /* call_function_params: args[i] = pointer to the value of the i-th non-return parameter (declaration order); out parameters are copied back; ret receives the return value (may be NULL). Game thread only. */
    int  (*call_function_params)(const AbsApi *self, void *obj, void *func, void **args, void *ret);
    void *(*construct_object)(const AbsApi *self, void *cls, void *outer);                 /* StaticConstructObject_Internal; NULL on failure. Game thread. */
    void *(*spawn_actor)(const AbsApi *self, void *cls, const float loc[3], const float rot[3]);   /* UWorld::SpawnActor in the current world (AlwaysSpawn); rot = pitch, yaw, roll in degrees */
    void *(*get_world)(const AbsApi *self);
    int  (*dump_sdk)(const AbsApi *self, const char *name);                               /* writes LanNative\sdk\<name>.txt: every class / struct / enum / function; returns the count. Game thread. Game derived: never commit. */
    int  (*set_timer)(const AbsApi *self, float seconds, int repeat, void (*cb)(void *user), void *user);   /* game-thread timer */
    int  (*on_world_loaded)(const AbsApi *self, void (*cb)(const char *map_path, void *user), void *user);  /* a new world object appeared (map travel); map_path like "/Game/Maps/.../Name.Name" */
    int  (*frame_peek_int)(const AbsApi *self, void *frame, int *out);                          /* in a replace_native thunk: the FIRST argument of the call (int-like), read from the bytecode without consuming it; 0 = not readable */
    int  (*object_path)(const AbsApi *self, void *obj, char *out, int cap);                    /* full path like /Game/Pkg.Obj or /Engine/Transient.Outer:Sub.Name; returns the length */
    int  (*dump_object)(const AbsApi *self, void *obj);                                       /* write every property of a live object (engine text) to LanNative\objdump_<name>.txt */
    int  (*trace_function)(const AbsApi *self, const char *cls, const char *func);          /* log every call of a function (also Blueprint-to-Blueprint and BPF_ calls, which skip ProcessEvent) into the tracer ring; start with trace("on") */
    void *(*duplicate_object)(const AbsApi *self, void *src, void *outer);                   /* StaticDuplicateObject: a copy of any object (data assets, ...); outer may be the transient package ("/Engine/Transient" via find_object). Game thread. */
    void *(*find_cdo)(const AbsApi *self, const char *short_class_name);                    /* the class default object ("Default__Name") */
    void *(*create_widget)(const AbsApi *self, void *widget_class, void *owning_player);    /* UWidgetBlueprintLibrary::Create: a UUserWidget of that class (not yet on screen). Game thread. */
    int   (*add_to_viewport)(const AbsApi *self, void *widget, int zorder);                 /* UUserWidget::AddToViewport */
    int   (*remove_from_parent)(const AbsApi *self, void *widget);                          /* UWidget::RemoveFromParent */
    void *(*get_local_player_controller)(const AbsApi *self);                               /* the local FightingPlayerController, NULL before one exists */
    int   (*make_text)(const AbsApi *self, const char *utf8, void *out_ftext24);               /* build an FText (24 bytes) from a string, via KismetTextLibrary::Conv_StringToText; 0 = ok. Copy it into a Text property with memcpy(prop_ptr(...), out, 24). Game thread. */
    int   (*text_to_string)(const AbsApi *self, const void *ftext24, char *out, int cap);        /* an FText back to UTF-8; 0 = ok */
    int   (*on_key)(const AbsApi *self, int vk, void (*cb)(int vk, void *user), void *user);   /* game-thread callback on each key-down edge of virtual key vk, only while the game window has the focus */
    int   (*on_damage)(const AbsApi *self, void (*cb)(void *victim, float amount, int flag, void *user), void *user);   /* after AFightingCharacter::ApplyDamage(amount, flag) ran on victim (hook installed on first use); attacker not available here */
    int   (*kv_get)(const AbsApi *self, const char *key, char *out, int cap);                 /* per-plugin sidecar store LanNative\data\<plugin>.txt: 1 if found. Mod data belongs here, not in the game's save format */
    int   (*kv_set)(const AbsApi *self, const char *key, const char *value);                  /* value NULL removes the key; 0 = ok */

    /* ---- v2.2: items foundation. All of these need the game thread unless noted. */
    void *(*create_asset)(const AbsApi *self, void *src, const char *path);                   /* copy of src (a UObject, e.g. a stock DataAsset) as "/Game/Mods/<Mod>/<Name>.<Name>" in its own package, rooted (the GC keeps it); if src is a UClass a new instance is constructed instead. Existing path with the same class: that object. NULL on failure. Create item objects at on_engine_ready, BEFORE the save is loaded */
    int   (*add_to_root)(const AbsApi *self, void *obj, int root);                           /* RootSet flag on (1) / off (0): keeps a runtime-created object alive. 1 = ok */
    void *(*table_row)(const AbsApi *self, const char *table_path, const char *row);          /* UDataTable::FindRowUnchecked: pointer to the row struct (cast with the SDK dump's offsets) or NULL; table_path like "/Game/DB/Shop/EquipmentShop.EquipmentShop" (must be loaded) */
    int   (*table_add_row)(const AbsApi *self, const char *table_path, const char *row, const char *copy_of);   /* add row `row` as a clone of row `copy_of` (the rows.json "add" engine); 1 = ok or exists already */
    int   (*row_set_text)(const AbsApi *self, const char *table_path, const char *row, const char *prop, const char *text);   /* ImportText into one row property, validated like rows.json; an object reference whose object is neither in memory nor in a pak is REFUSED (loading it would hang). A runtime-created object (create_asset) is accepted. 1 = ok */
    int   (*inventory_add)(const AbsApi *self, void *item, int material, int equip);          /* UInventoryComponent::BPF_AddItemToInventory with m_eSchoolItemType = None (4; 0 would make the item vanish on the next load); material 0 = default; returns the inventory index or -1. equip=1 also calls inventory_equip. Needs a world (local player controller) */
    int   (*inventory_equip)(const AbsApi *self, int index);                                  /* equip inventory entry `index` through InventoryMenu::BPF_Equip(index, 0) when a menu object exists (equipped flags + preset), else UInventoryComponent::EquipFromInventory; 1 = ok */
    int   (*inventory_index)(const AbsApi *self, void *item);                                 /* index of `item` in the local player's inventory or -1 (use it to avoid granting twice) */
    int   (*set_text)(const AbsApi *self, void *obj, const char *prop, const char *utf8);    /* make_text + store into a Text property (old text is not released: small leak). 1 = ok. Culture-invariant text: one language */
    int   (*on_save_loading)(const AbsApi *self, void (*cb)(void *save, void *user), void *user);   /* before USaveThePlaines::InitializeInventory resolves the item paths of a loaded save: last moment to create_asset the items the save names */
    int   (*on_save_written)(const AbsApi *self, void (*cb)(void *save, void *user), void *user);   /* after the save object's item path list was written (InitInventoryDataToSerializeFromInventoryArray): read the paths to keep a sidecar */

    /* ---- v2.3: combat and menu events. All callbacks run on the game thread; hooks are installed on first registration. docs/PLUGIN-API.md "Events (API 2.3)" */
    int   (*on_hit)(const AbsApi *self, void (*cb)(const struct AbsHitInfo *hit, void *user), void *user);                       /* one call per resolved impact (hit, guard, guard break, absorb) and per parry / dodge */
    int   (*on_attack_start)(const AbsApi *self, void (*cb)(void *character, int attack_id, const char *attack, void *user), void *user);   /* once per real attack (the ~30 prediction re-runs of OrderAttack::OnStart are filtered out) */
    int   (*on_pawn_spawned)(const AbsApi *self, void (*cb)(void *character, void *user), void *user);                         /* after AFightingCharacter::BeginPlay (player, NPCs, training dummy) */
    int   (*on_pawn_destroyed)(const AbsApi *self, void (*cb)(void *character, int reason, void *user), void *user);           /* before AFightingCharacter::EndPlay; reason = EEndPlayReason (0 destroyed, 1 level transition, 2 PIE end, 3 removed from world, 4 quit) */
    int   (*on_death)(const AbsApi *self, void (*cb)(void *victim, void *killer, int attack_id, const char *attack, void *user), void *user);   /* AFightingCharacter::ServerKilledBy; killer may be NULL */
    int   (*on_menu)(const AbsApi *self, void (*cb)(void *menu, const char *menu_class, const char *state, void *user), void *user);   /* after a menu widget's "EnterState <Name>" / "EnterState_<Name>" event: state = "<Name>" (CharacterMenu, ShopMenu, InventoryMenu, ...) */
    int   (*char_info)(const AbsApi *self, void *character, struct AbsCharInfo *out);                                           /* fill *out (set out->size = sizeof *out first); 1 = ok. style needs the game thread (-1 elsewhere) */

    /* ---- v2.4: content registry. Mods ship JSON in Mods\<mod>\content\<kind>\*.json; the loader validates every entry, resolves duplicate ids by load order and calls the
     *      kind's handler. Built in: "maps", "items", "styles", "powers", "npcs", "bots", "ranks", "bot_trials", "poise", "clones", "anim_sets", "weapons". register_content_kind adds more (call it in AbsPlugin_Load; entries are processed once at engine ready, again before a save resolves its items).
     *      validate(json, err, cap, user): json = compact text of ONE entry object; return 1 if usable, else 0 and write the reason into err (the entry is skipped and logged).
     *      apply(mod_id, entry_id, json, user): return 0 when done (it is not called again), nonzero to be retried at the next pass. Both run on the game thread, inside the plugin's crash guard.
     *      Returns 0 = registered, 1 = bad arguments / table full, 2 = the kind already has a handler. docs\PLUGIN-API.md "Content registry". */
    int   (*register_content_kind)(const AbsApi *self, const char *kind, int (*validate)(const char *json, char *err, int cap, void *user), int (*apply)(const char *mod_id, const char *entry_id, const char *json, void *user), void *user);

    /* ---- v2.5: runtime textures (item icons without cooking). Game thread only; path = "/Game/Mods/<Mod>/<Name>.<Name>"; the texture is rooted and found by that path
     *      (soft object paths such as m_InventoryIcon resolve to it). Returns the UTexture2D or NULL (reason logged). An existing object at that path is returned as is. */
    void *(*create_texture)(const AbsApi *self, const void *bgra, int width, int height, const char *path);   /* 32-bit BGRA pixels (straight alpha), row-major, top row first, 1..4096 per side */
    void *(*texture_from_file)(const AbsApi *self, const char *file_utf8, const char *path);                 /* PNG / JPEG / BMP / GIF / TIFF decoded by Windows Imaging Component; file = absolute path */

    /* ---- v2.6: test functions - drive fights without a keyboard and keep the practice dummy usable (docs/PLUGIN-API.md "Functions for automated checks"). Game thread only.
     *      A "character" is an AFightingCharacter (the player, an NPC, the training dummy). */
    int   (*test_heal)(const AbsApi *self, void *character);                                  /* health and guard to full, ghost damage and down state cleared; 1 = ok */
    int   (*test_set_invincible)(const AbsApi *self, void *character, int on);                /* HealthComponent::ServerSetInvicible; returns the resulting state (0/1) or -1 */
    int   (*test_attack)(const AbsApi *self, void *attacker, const char *attack_row, void *target);   /* the character performs attack row `attack_row` ("135", "BT2_01"...) at `target` (may be NULL: faces forward) through the game's own attack order, no keyboard, no deck needed; 1 = launched (confirm with on_attack_start / on_hit) */
    int   (*test_guard)(const AbsApi *self, void *character, int on);                         /* raise (1) / drop (0) the guard; 1 = ok */
    int   (*test_teleport)(const AbsApi *self, void *actor, const float loc[3], const float *yaw_degrees);   /* AActor::K2_TeleportTo; yaw NULL keeps the rotation; 1 = ok */
    int   (*test_get_location)(const AbsApi *self, void *actor, float out[3]);                /* world location of an actor's root component; 1 = ok */
    void *(*test_spawn_npc)(const AbsApi *self, const char *archetype_path, const float loc[3]);   /* spawn an NPC from an archetype asset ("/Game/DB/AI/NPCs/Trickster/Trickster.Trickster") through the game's AAISpawner chain and move it to loc (NULL = where it appears); returns the character or NULL */

    /* ---- v2.7: mod UI and hot reload */
    int   (*add_menu_item)(const AbsApi *self, const char *label, void (*cb)(void *user), void *user);   /* an entry on the F1 menu's "Mods" page (labelled "<your mod>: <label>"); cb runs on the game thread when the player selects it; 0 = ok, 1 = table full / bad arguments */
    int   (*reload_content)(const AbsApi *self);                                              /* hot reload: re-read Mods/<mod>/content (items, styles, plugin kinds) and rows.json and apply what changed without a restart (what cannot be reloaded is logged); returns the number of changed or new content entries, or -1 if nothing could be reloaded. Game thread. Same as a LanNative/reload.txt file and the first line of the F2 Mods page */

    /* ---- v2.8: test functions: dodge, AI, hittable, facing */
    int   (*test_dodge)(const AbsApi *self, void *character, const float dir[3]);             /* the character starts a dodge towards the WORLD direction dir (x, y; z ignored; for example away from the opponent) through AFightingCharacter::PlayDodgeOrder; a dodge that is active when an attack connects ends in on_hit outcome ABS_HIT_DODGED. 1 = launched */
    int   (*test_ai)(const AbsApi *self, void *character, int enabled);                       /* stop (0) / restart (1) the character's behaviour tree (AIController::BrainComponent StopLogic / RestartLogic): a stopped NPC stands still and only does what test_attack / test_guard / test_dodge tell it; 1 = ok */
    int   (*test_hittable)(const AbsApi *self, void *character, int on);                      /* an AI character can only be hit while its AI is running (the CanBeHitted lambda that AAISpawner::SpawnAI installs reads UAIComponent+0x140): on = 1 sets that flag so a stopped NPC or the practice dummy takes driven attacks, 0 clears it; the flag is verified against the lambda's bytes first; returns the resulting value or -1 */
    int   (*test_get_forward)(const AbsApi *self, void *actor, float out[3]);                 /* unit vector the actor's root component faces (for the player: where the camera looks; teleporting does not turn the player, the controller does); 1 = ok */

    /* ---- v2.9: powers. Game thread only. dir/slot 0 = key 1 / D-pad left, 1 = key 3 / D-pad right. */
    int   (*inventory_equip_slot)(const AbsApi *self, int index, int slot);                    /* FightingPlayerController::EquipFromInventory(index, slot): put inventory entry `index` (inventory_add / inventory_index) into power slot 0 or 1; 1 = ok. inventory_equip cannot do this (it uses slot -1, which a power slot refuses) */
    int   (*on_power_used)(const AbsApi *self, void (*cb)(void *character, void *item_data, void *actor, int dir, void *user), void *user);   /* a power's effect actor was spawned (BPE_BeginItemUseAction) after AFightingCharacter::LaunchItemUseOrder: character = user (player or NPC), item_data = the UUsableItemData, actor = the spawned AUsableItem instance, dir 0/1. The place to patch the INSTANCE (m_ItemAttackName, hit shape radius): never write the stock class defaults. Hooks are installed on first registration; 0 = ok */
    int   (*power_use)(const AbsApi *self, void *character, int dir);                          /* use the power in slot dir without a key (keys 1/3 do not reach powers in practice mode): for the local player (character NULL = the player) UInventoryComponent::CanUseEquippedItem must say CanUse (else refused, reason logged), then AFightingCharacter::LaunchItemUseOrder; for an NPC just the launch. The HUD's launch event is not sent. 1 = launched */

    /* ---- v2.10: decks, fighting styles, weapon / style events (docs/PLUGIN-API.md "Decks, styles and the weapon slot"). Game thread only.
     *      family: 0 BareHands, 1 Sword, 2 WarGloves. Decks are the live save's m_SavedData.m_SavedCombos[family] (what Meditation > Combat Deck shows). */
    int   (*combo_slot_ensure)(const AbsApi *self, int family, const char *name, int copy_from_index);   /* index of the deck named `name`; if missing it is created as a deep copy of deck copy_from_index (appended: the 4th deck gets index 3). -1 on error */
    int   (*combo_load)(const AbsApi *self, int family, const char *name);                                 /* make that deck current: FightingPlayerController::BPF_LoadComboFromName(name, family, Current) (also loads it into the attack component); 1 = ok */
    int   (*combo_current)(const AbsApi *self, int family, char *name, int cap);                           /* current deck name of the family; returns its index or -1 */
    int   (*set_combat_style)(const AbsApi *self, void *character, int style);                             /* local player (character NULL or the local pawn): FightingPlayerController::BPF_SetPlayerCombatStyle(style, Current); 1 = the style is now `style` */
    int   (*on_weapon_changed)(const AbsApi *self, void (*cb)(void *character, void *old_item, void *new_item, int handled, void *user), void *user);   /* the local player's equipped weapon (InventoryComponent.m_EquippedWeapon) changed; polled every tick; fires once with old_item NULL when the character appears; handled is always 0 (picked-up weapons are not reported here; use on_handled_weapon) */
    int   (*on_style_changed)(const AbsApi *self, void (*cb)(void *character, int old_style, int new_style, void *user), void *user);                   /* the local player's fighting style changed (PlayerFightingComponent.m_uiCombatStyle); polled every tick; first call old_style -1 */
    void *(*prop_ptr_path)(const AbsApi *self, void *obj, const char *path);                                /* address of a nested property: "m_SavedData.m_CurrentComboSaveName[1]", "OwningGameInstance.m_CurrentSave" (through structs, object pointers, static arrays and TArray elements); NULL (logged) if a step is missing */
    int   (*fname_make)(const AbsApi *self, const char *utf8, uint64_t *out);                               /* FName from a string (added to the name table); 0 = ok */
    int   (*fname_text)(const AbsApi *self, const void *fname, char *out, int cap);                         /* an FName (8 bytes) as UTF-8; 0 = ok */

    /* ---- v2.11: timers with ids. 256 slots shared by all plugins, at most 128 per plugin; one-shot slots are freed when they fire; a full table is logged. set_timer (above) still works (0 = ok, 1 = failed) */
    int   (*set_timer_id)(const AbsApi *self, float seconds, int repeat, void (*cb)(void *user), void *user);   /* like set_timer but returns the timer id (> 0), 0 = not created (reason logged) */
    int   (*cancel_timer)(const AbsApi *self, int timer_id);                                   /* remove one of YOUR timers (also from inside its own callback); 1 = removed, 0 = unknown id / not yours / already fired */

    /* ---- v2.12: modes (modes.inc, docs/PLUGIN-API.md "Modes"). Game thread only. */
    int   (*travel)(const AbsApi *self, const char *map_or_url);                               /* console "open <map>?listen" ("/Game/Maps/.../Name[.Name]" gets ?listen unless it has options; "open ..." text is sent as is). The world changes a moment later (on_world_loaded). 1 = issued. For a lone player in a 1v1 arena call solo_duel(1) first */
    int   (*console_cmd)(const AbsApi *self, const char *cmd);                                 /* KismetSystemLibrary::ExecuteConsoleCommand with the local player controller; 1 = issued */
    int   (*solo_duel)(const AbsApi *self, int on);                                            /* the loader's solo-duel switch: 1 = a hosted 1v1 starts with only the local player (m_iNumTeams 2 -> 1), 0 = stock, -1 = just read; returns the state. Takes effect for the NEXT travel */
    int   (*match_info)(const AbsApi *self, struct AbsMatchInfo *out);                         /* fill *out (set out->size first); 1 = a game state exists, 0 = none (open world: state "") */
    int   (*on_match_state)(const AbsApi *self, void (*cb)(const char *old_state, const char *new_state, void *user), void *user);   /* GameState MatchState changed (polled 10x per second by the loader); old_state "" on the first sight of a world */
    int   (*on_round_end)(const AbsApi *self, void (*cb)(const struct AbsMatchInfo *info, int winner_team, void *user), void *user);   /* a team score went up (a round was decided), polled 10x per second; winner_team = the team whose score rose. Not fired when scores are reset */
    int   (*set_mode_prop)(const AbsApi *self, const char *name, const char *text);            /* engine-text property write on the live game mode AND game state (those that have it), e.g. ("m_iNbWinningRounds","5"); returns a bit mask 1 = game mode, 2 = game state, 0 = neither/refused. Do it right after on_world_loaded, before the match starts */
    int   (*show_banner)(const AbsApi *self, const char *utf8, float seconds);                 /* centred message on the HUD for `seconds` (queued, max 8); 1 = queued. Uses a stock text widget (see docs) */
    int   (*hud_score)(const AbsApi *self, const char *utf8);                                  /* persistent score line at the top of the screen; NULL or "" removes it; 1 = ok */

    /* ---- v2.13: map script (mapscript.inc): the content kind "maps" (Mods\<mod>\content\maps\*.json) spawns lights / movable meshes / triggers per map. Plugins can drive it: */
    int   (*mapscript_phase)(const AbsApi *self, const char *name, int on);                    /* set a named phase flag (lights/actions of the scene test it); 1 = ok */
    int   (*mapscript_mesh)(const AbsApi *self, const char *mesh_id, const char *state);       /* move a scene mesh to a named state ("open", "closed"); 1 = ok */

    /* ---- v2.14: NPCs (npcs.inc, docs/PLUGIN-API.md "NPCs"). Game thread only. The content kind "npcs" is built into the loader. */
    void *(*npc_spawn)(const AbsApi *self, const char *archetype, const float loc[3], const float *yaw_deg, const char *class_path);   /* spawn an NPC AT loc (NULL = 350 uu in front of the local character): `archetype` = an ArchetypeAsset path ("/Game/DB/AI/NPCs/Trickster/Trickster.Trickster", or a mod copy "/Game/Mods/<mod>/<id>.<id>") or the plain id of an npcs content entry (then its faction / aggro / reactions apply). The spawner actor is placed at loc BEFORE the spawn and the pawn is never teleported afterwards (a teleported AI NPC never moves again). class_path NULL / "" = the game's generic AI character. Returns the character or NULL */
    int   (*npc_remove)(const AbsApi *self, void *character);                                  /* remove an NPC made by npc_spawn (UnSpawnAi + destroy our spawner), or destroy any other character actor; 1 = ok */
    int   (*npc_set_target)(const AbsApi *self, void *character, void *target);                /* UAIFightingComponent::BPF_ForceEnemy(target): the NPC goes for that actor (the player: get_local_player_controller + K2_GetPawn). NULL clears (untested); 1 = ok */
    int   (*npc_set_faction)(const AbsApi *self, void *character, int faction);                /* AFightingCharacter::ServerSetFaction(0..5) (written directly if that does not take); returns the faction read back or -1 */
    int   (*faction_can_target)(const AbsApi *self, int attacker_faction, int target_faction, int set);   /* UFactionsManager target table (6 x 6): set -1 = read only, 0 / 1 = write; returns the value now in the table or -1 */
    void *(*npc_archetype)(const AbsApi *self, void *character);                               /* the ArchetypeAsset the character runs (m_ArchetypeDescription.m_Archetype) or NULL */
    int   (*on_area)(const AbsApi *self, const float center[3], void *center_actor, float radius, void *who, void (*cb)(void *character, int entered, void *user), void *user);   /* loader-side poll (one 0.1 s timer for all plugins, 2D distance, 5 % hysteresis): cb(who, 1/0, user) when `who` enters / leaves radius uu around the point `center` or around `center_actor` (a character: the area is dropped when it is destroyed). who NULL = the local player (not "any character"). Returns a handle (> 0) or 0 */
    int   (*area_remove)(const AbsApi *self, int handle);                                      /* stop watching; 1 = removed */
    int   (*show_message)(const AbsApi *self, const char *utf8, float seconds);                /* on-screen text (the loader's toast, up to 3 lines of 96 characters, 1..30 s); 1 = shown */

    /* ---- v2.15: bots and ranks (bots.inc, docs/PLUGIN-API.md "Bots and ranks"). Game thread only. character NULL = the local player.
     *      The content kinds "bots" (spawn one with npc_spawn("<bot id>")) and "ranks" (reward tables for trial_reward) are built into the loader. */
    int   (*xp_add)(const AbsApi *self, void *character, int amount);                          /* StatsComponent.BPF_AddGlobalXP(GAMEMODE, amount, Current, notify 0): levels / Prestige rise exactly like after a combat trial; 1..10000000; 1 = called */
    int   (*gm_points_add)(const AbsApi *self, void *character, int points);                   /* native UStatsComponent::AddGameModeXP(points, Current): game-mode points, one Rift Coin (loot box) per 5; returns the Rift Coins added or -1 */
    int   (*rank_info)(const AbsApi *self, void *character, struct AbsRankInfo *out);           /* level, Prestige grade / level, XP, next threshold (set out->size first); 1 = ok */
    int   (*trial_reward)(const AbsApi *self, void *character, const char *ranks_id, int won, int rounds_won, int mentoring, int pay, struct AbsTrialReward *out);   /* the stock 1v1 reward rules (500 finish, +4000 win, +1500 for 2+ rounds won, +500 mentoring; 1 point each) or a "ranks" entry's table (ranks_id); pay = 1 grants it with xp_add + gm_points_add. Returns the XP; out may be NULL */
    float (*npc_difficulty)(const AbsApi *self, void *npc, float set);                         /* the AI's live difficulty level 0..100 (UAIFightingComponent m_fCurrentDifficultyLevel, rubber-bands during a fight); set >= 0 writes it; -1 = not an AI character */

    /* ---- v2.16: bot trials (bottrial.inc, docs/PLUGIN-API.md "Bot trials"). Game thread only. The content kind "bot_trials" is built into the loader. */
    int   (*bot_trial_start)(const AbsApi *self, const char *trial_id);                        /* start the bot_trials entry `trial_id` (solo duel + travel to its map, rounds against its bot(s), reward, travel back); 1 = started, 0 = unknown id / a trial is already running */
    int   (*bot_trial_status)(const AbsApi *self, struct AbsTrialStatus *out);                 /* the running (or last finished) trial; set out->size first; 1 = there is one */

    /* ---- v2.17: weapon animation sets (animstyle.inc, docs/PLUGIN-API.md "Animation style"). A character can use another
     *      style INDEX of the style-indexed animation tables (guard, hit reactions, locomotion, dodge, stance change, cancel, fidgets) while its fighting style (stats, special
     *      ability, save, menus) stays what it is. character NULL = the local player (kept across travel); other characters' overrides are dropped on a world change. */
    int   (*anim_style_set)(const AbsApi *self, void *character, int style_index);             /* animation lookups of `character` use table index style_index (0..63) from now on; -1 removes the override. Game thread. The hooks are installed at the first call with a style. 1 = ok */
    int   (*anim_style)(const AbsApi *self, void *character);                                  /* the style index the character's animation lookups use now (override, else the real style); -1 = not a character */
    int   (*real_style)(const AbsApi *self, void *character);                                  /* the fighting style as the game sees it (AFightingCharacter::BPF_GetCombatStyle, never overridden); -1 = none / uninitialised */
    int   (*on_handled_weapon)(const AbsApi *self, void (*cb)(void *character, void *weapon_data, int family, void *user), void *user);   /* the weapon IN HAND of the local player changed (drawn, sheathed, disarmed, picked up): weapon_data = the VisibleWeaponData in hand or NULL, family = the active weapon family (0 when nothing is in hand); polled per tick, fires once when the character appears. 0 = ok */

    /* ---- v2.18: per-character sidecar (sidecar.inc). Metadata about a CHARACTER that must not go into the game's save: LanNative\state\<GUID>.txt, keyed by the
     *      save's character GUID (a rename keeps it), one document per character, written through on every change. Keys are per plugin ("p.<plugin>.<key>" in the file).
     *      kv_get / kv_set stay the per-PLUGIN store. character NULL = the local player (the only character with a save). Game thread only. */
    int   (*sidecar_get)(const AbsApi *self, void *character, const char *key, char *out, int cap);   /* 1 = found (out = value), 0 = absent / no character save loaded */
    int   (*sidecar_set)(const AbsApi *self, void *character, const char *key, const char *value);    /* value NULL removes; key without '=', CR, LF; 1 = stored on disk */

    /* ---- v2.19: pre-hit, damage multipliers, poise (hitpre.inc, docs/PLUGIN-API.md "Pre-hit, damage_mod and poise"). Game thread only. character NULL = the local player
     *      (kept across travel); other characters' entries are dropped when they are removed or the world changes. Hooks are installed at the first use. */
    int   (*on_hit_pre)(const AbsApi *self, void (*cb)(struct AbsHitPre *hit, void *user), void *user);   /* before the game resolves an impact on a character: change the scales in *hit (see AbsHitPre). 0 = ok, 1 = bad arguments / 16 callbacks, 2 = hooks failed */
    int   (*damage_mod)(const AbsApi *self, void *character, const char *key, float value);     /* per-character multiplier 0..100 (1 = off): damage_taken, damage_dealt, stun_taken, stun_dealt, knockback_taken, knockback_dealt, special_ratio (the attacker's cut share). on_hit_pre starts from these. 1 = ok */
    int   (*poise_set)(const AbsApi *self, void *character, float poise, int heavy_power);      /* poise gauge: while > 0 hits below heavy_power (EAttackPowers 0 Light, 1 Medium, 2 Strong; -1 = default 2) do not interrupt and drain it by their damage; an empty gauge or a heavy hit interrupts and refills it. poise <= 0 removes. 1 = ok */
    float (*poise_get)(const AbsApi *self, void *character);                                   /* current gauge, -1 = no poise */
    /* ---- v2.20: defence outcome events (defense_ev.inc, docs/PLUGIN-API.md "Defence events"). Game thread. The hooks are installed at the first registration. */
    int   (*on_defense)(const AbsApi *self, void (*cb)(const struct AbsDefenseInfo *d, void *user), void *user);   /* one call per successful defence (guard, guard break, absorb, parry, deflect, dodge, avoid; see AbsDefenseInfo). 0 = ok, 1 = bad arguments / 16 callbacks, 2 = hooks failed */
    /* ---- v2.21: Dear ImGui overlay (overlay.cpp, docs/PLUGIN-API.md "Overlay"). The overlay is installed at the first overlay_register / overlay_set_mode, not at start-up.
     *      Threads: overlay_register, overlay_unregister, overlay_set_mode and overlay_raw_mouse may be called from any thread (the game thread, AbsPlugin_Load, a draw callback);
     *      they take a lock that the render thread holds while it draws, so a slow draw callback delays them. Draw callbacks run on the RENDER thread. */
    int   (*overlay_info)(const AbsApi *self, struct AbsOverlayInfo *out);                 /* any thread; fills what fits in out->size and sets out->size to the bytes filled. 0 = ok, 1 = bad arguments or the overlay has not started yet, 2 = unavailable (dead; out->state = -1). Bind the context only inside a draw callback */
    int   (*overlay_register)(const AbsApi *self, void (*draw)(void *user), void *user);   /* draw(user) runs once per overlay frame on the render thread, after the loader's windows, in registration order; 16 callbacks in total across all plugins. 0 = ok, 1 = bad arguments / the same (draw, user) twice / table full, 2 = unavailable */
    int   (*overlay_unregister)(const AbsApi *self, void (*draw)(void *user), void *user); /* 0 = removed (may still run once in the frame in progress), 1 = not found */
    int   (*overlay_set_mode)(const AbsApi *self, unsigned flags);                         /* your input needs, ABS_OV_*; 0 = nothing. Modes are per plugin and ORed with the other plugins'; frames run while any mode is set or a loader menu is open. 0 = ok, 1 = bad arguments, 2 = unavailable */
    int   (*overlay_raw_mouse)(const AbsApi *self, int *dx, int *dy);                      /* any thread: raw mouse movement collected since the last call while a plugin has ABS_OV_RAWMOUSE (the accumulator is shared: use it from one plugin at a time). 0 = ok (zeros when nobody has the mode), 1 = bad arguments / unavailable */
};

/* on_hit_pre: one call per impact on a character, BEFORE the game resolves it (guard / absorb are not known yet). The scales start from damage_mod (victim *_taken x attacker *_dealt). */
typedef struct AbsHitPre {
    uint32_t size;             /* sizeof(AbsHitPre) of the loader: fields are only appended */
    void *victim;              /* AFightingCharacter about to be hit */
    void *attacker;            /* the attacking actor, may be NULL / not a character */
    int attack_id;             /* attack row as a number, -1 if not numeric */
    char attack[32];           /* attack row name */
    int perfect_link;          /* 1 = perfect (timed) link */
    float damage_scale;        /* x the final damage (after the game's own multipliers; what on_hit reports), 0..100 */
    float special_ratio_scale; /* x the attacker's special ("cut") share of the raw damage; the share is clamped to 0..1 */
    float special_ratio;       /* >= 0: force the special share (0..1) instead; -1 = scale only */
    float stun_scale;          /* x the stun frames of this hit (hit, on guard, perfect link); melee attack rows only */
    float knockback_scale;     /* x the knockback distance (hit and guard) */
} AbsHitPre;

/* overlay_set_mode flags (v2.21). ImGui never reads or navigates with the gamepad; only ABS_OV_BLOCK_GAME changes what the game sees from it. */
enum {
    ABS_OV_DRAW     = 1,       /* your callbacks run; no input is taken (HUDs, viewport overlays that do their own picking) */
    ABS_OV_MOUSE    = 2,       /* mouse to ImGui and kept from the game; a virtual cursor is drawn (the game pins the OS cursor) */
    ABS_OV_KEYS     = 4,       /* keys and characters to ImGui; kept from the game only while ImGui wants them (io.WantCaptureKeyboard / WantTextInput). F1-F12 always reach the game, and so does a key release whose press the game saw; a press kept from the game takes its release with it */
    ABS_OV_RAWMOUSE = 8,       /* raw mouse movement goes to overlay_raw_mouse instead of the game and the virtual cursor (fly cameras, drag gizmos) */
    ABS_OV_BLOCK_GAME = 16,    /* while any plugin has it: the game gets no keyboard or mouse input and a neutral gamepad (see below). Does not give ImGui any input by itself */
    ABS_OV_HIDECURSOR = 32     /* the virtual cursor is not drawn while any plugin has it (a loader menu that is open still shows it) */
};
/* ABS_OV_BLOCK_GAME, exactly: kept from the game are all key presses and characters (except F1-F12), all mouse buttons, wheel and movement messages, and raw mouse movement;
 * the game's XInputGetState returns the state with buttons, triggers and sticks at zero (the packet number and the return code are unchanged). Still reaching the game: F1-F12,
 * the release of any key or mouse button whose press the game saw (nothing stays held), window, focus and size messages, and everything the game reads outside the window procedure and
 * XInputGetState (GetAsyncKeyState polling, rumble output). Nothing is blocked while the game window is not the foreground window. ImGui never reads the gamepad. */
/* overlay_info (v2.21): the loader's Dear ImGui context. At the top of every draw callback check the layout and bind your own copy of ImGui:
 *   if (i.imgui_version_num != IMGUI_VERSION_NUM || i.sizeof_imgui_io != sizeof(ImGuiIO) || i.sizeof_imgui_style != sizeof(ImGuiStyle) || i.sizeof_imgui_drawvert != sizeof(ImDrawVert) ||
 *       i.sizeof_imgui_drawidx != sizeof(ImDrawIdx) || i.sizeof_imgui_wchar != sizeof(ImWchar)) return;      // different build: draw nothing
 *   ImGui::SetAllocatorFunctions(i.imgui_alloc, i.imgui_free, i.imgui_alloc_user); ImGui::SetCurrentContext((ImGuiContext *)i.imgui_context);
 * Your ImGui build must also neutralise IM_ASSERT (no-op or RaiseException): the default assert would run outside the loader's recovery. */
typedef struct AbsOverlayInfo {
    uint32_t size;             /* in: sizeof(AbsOverlayInfo) of your header; out: the bytes the loader filled. Fields are only appended */
    int state;                 /* 0 = starting (no frame yet), 1 = ready, -1 = dead (the overlay failed to start or was switched off; it does not come back in this run) */
    void *imgui_context;       /* ImGuiContext * */
    const char *imgui_version; /* e.g. "1.91.5" */
    int imgui_version_num;     /* IMGUI_VERSION_NUM of the loader */
    void *(*imgui_alloc)(size_t size, void *user);
    void (*imgui_free)(void *ptr, void *user);
    void *imgui_alloc_user;
    int width, height;         /* client size of the game window in pixels */
    float ui_scale;            /* io.FontGlobalScale the loader sets each frame (height / 1080, at least 1): the default font only, scale your sizes with it */
    void *d3d_device;          /* ID3D11Device * (NULL until the first frame) */
    void *d3d_context;         /* ID3D11DeviceContext * (the immediate context) */
    int sizeof_imgui_io, sizeof_imgui_style, sizeof_imgui_drawvert, sizeof_imgui_drawidx, sizeof_imgui_wchar;   /* layout of the loader's ImGui build */
    unsigned caller_mode;      /* the calling plugin's current ABS_OV_* flags: 0 once the loader released them (3 faults, unload) */
    int caller_callbacks;      /* the calling plugin's registered draw callbacks (0 after a release) */
} AbsOverlayInfo;

/* on_defense (v2.20). type: */
enum { ABS_DEF_GUARD = 0, ABS_DEF_GUARD_BREAK = 1, ABS_DEF_ABSORB = 2, ABS_DEF_PARRY = 3, ABS_DEF_DEFLECT = 4, ABS_DEF_DODGE = 5, ABS_DEF_AVOID = 6 };
typedef struct AbsDefenseInfo {
    uint32_t size;             /* sizeof(AbsDefenseInfo) of the loader: fields are only appended */
    int type;                  /* ABS_DEF_*: guard / guard_break / absorb (Kahlt) from the hit resolution; parry (special ability), deflect (parry while the guard is up);
                                  dodge (EAvoidType Dodge) / avoid (Duck, Jump, sides, AllHitAvoid, DrunkenSouth) */
    void *defender;            /* the character that defended (guard break: whose guard broke) */
    void *attacker;            /* the attacking actor (AFightingCharacter for melee), may be NULL */
    int attack_id;             /* attack row as a number, -1 if not numeric */
    char attack[32];           /* attack row name */
    int avoid_type;            /* dodge / avoid: EAvoidType (0 Duck 1 Jump 2 LeftSide 3 RightSide 4 Dodge 5 AllHitAvoid 6 DrunkenSouth); -1 otherwise */
    int perfect_link;          /* guard / guard break / absorb: the defended hit was a perfect link */
    float time_in_attack;      /* seconds from the attacker's attack start (on_attack_start) to this defence, real time; -1 unknown */
    float frame_in_attack;     /* the same in 1/30 s combat frames; compare with startup; -1 unknown */
    int startup;               /* the attack row's startup frames (m_iFrameBuildUp); -1 = no row (item attack) */
    float delay;               /* parry / dodge / avoid: the game's prediction delay argument (s); 0 otherwise */
} AbsDefenseInfo;

typedef int  (*AbsPluginLoad_t)(const AbsApi *api);
typedef void (*AbsPluginUnload_t)(void);

#endif
