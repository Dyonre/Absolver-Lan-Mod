# Forsaken "cursed parry" bug: root cause from the decompiles

Sources: `absolver_parrybug_decomp.c` (35 fns, names in `tools/ghidra/names_absolver_parrybug.txt`, run with `work/run_parrybug_decomp.ps1`),
`absolver_parryflow_decomp.c`, `absolver_defense_decomp.c`, struct layout from `work/structs_parry.txt` (PDB).

## Reported symptoms
1. First parry after respawn: no stamina return, and the parry anim is not released on success (feels self-stunned).
2. Same state after *you* get parried (Forsaken v Forsaken / Faejin).
3. A whiffed parry clears it.
4. Vanilla Forsaken gets no confirmed damage from a correct parry (separate, see bottom).

## Root cause (high confidence from code; not yet observed live)
`UDefenseComponent` keeps two parallel sets of parry-window state, one for the predicted timeline and one for the current timeline:

| state | current | prediction |
|---|---|---|
| open flag | `m_bParryWindowOpened` +0x1BA | `m_bParryWindowOpenedInPrediction` +0x1D0 |
| parry types | `m_ParryTypeArray` +0x1C0 | `m_ParryTypeArrayInPrediction` +0x1D8 |
| ParryPropertyDB weak ptr | `m_ParryProperty` +0x1F4 | `m_ParryPropertyInPrediction` +0x1FC |

`SetParryWindow` (RVA 0x34D6C0) pairs them wrongly. The prediction branch writes flag/array/**+0x1F4**, the current branch writes flag/array/**+0x1FC**. The flag and array are paired correctly, the DB pointer is crossed (`absolver_defense_decomp.c` ~1708-1720).

Hit resolution runs on the predicted timeline: `CheckAndHandleCapsuleCollision` calls `IsParryActive(true)` and `AreAttackAndParryMatching(..., prediction=true)`, which use the prediction flag and array, so the parry itself matches. On a match `HandleParried` -> `NotifyParrySuccessful` (RVA 0x33F3E0) reads the parrier's **+0x1FC** (`m_ParryPropertyInPrediction`) and passes it along. The only thing that writes +0x1FC is the *current-time* window pass, which lags the predicted hit.

`ClientNotifyParrySuccessful_Implementation` stores that pointer weakly in `m_ParrySuccessfulDelayed`. In `UpdateParrySuccessfulDelayed` (RVA 0x355420), if the weak pointer is invalid the entry is marked fully consumed and skipped silently. Everything the success is supposed to give is gated behind that pointer:
- `BPF_AddShards(db+0x34, Parry)` = the stamina return
- `PushSpecialAbilitySuccessfullLayer` (`ParryDB.m_ParryLayerDataOnSucceed`) = the availability layer that lets you act out of the parry anim
- `BPF_IncreaseGuardGauge(db+0x30)`
- the `OnParrySuccess` delegate / BP event still fires, so the parry "works" for the attacker (they get `Parried`/`SwitchIdle`), you just get none of the rewards.

That is exactly "parried the opponent, got no stamina and can't act".

## Why it matches every report
- **After respawn**: the component is fresh, +0x1FC is null until a current-time window has run once. The first predicted success reads null. By the time the current-time window opens, the entry is already consumed. The next parry reads a valid pointer.
- **After being parried**: `CheckAndHandleCapsuleCollision` calls `SetParryWindow(attackerComp, false, nullptr, Both, nullptr)` on the parried attacker. That writes null into *both* weak pointers, so the state is re-armed.
- **Whiff clears it**: a whiff lets the current-time window open and write a valid +0x1FC, with no predicted hit consuming it first.
- **Forsaken only**: Forsaken is the only style whose special ability is `Parry`. Kahlt uses `Absorb`, Windfall and Lost Prospect use `Avoid`, Stagger uses `Drunken`.
- **Forsaken v Forsaken / Faejin**: you only get cleared by other Parry users' hits.

## Fix options
- **One-byte exe patch (preferred)**: the instruction `add rcx, 0x1fc` starts at RVA `0x33F46F`; its `fc` byte is at RVA `0x33F472` (file offset `0x33EA72`); change it to `f4`, so `NotifyParrySuccessful` reads `m_ParryProperty` (+0x1F4), the field the prediction pass actually writes. Bytes there: `48 81 c1 fc 01 00 00` -> `48 81 c1 f4 01 00 00`.
- Alternative: swap the two displacements in `SetParryWindow` (RVA `0x34D79D` `f4`<->`0x34D7D4` `fc`), i.e. make the writes consistent with the names. That would also change what any other reader of those fields sees; none other found in the decompiles, but only a few functions were exported.
- Caveat for online play: the pointer is read by whichever machine runs `NotifyParrySuccessful` (the attacker's/host side) and sent to the parrier by RPC, so the patch has to be on that machine for the parrier to benefit.
- Not tried: do exe patches need the maintainer's go-ahead (per the 60 fps rule). Not applied.

## Verify in game
Respawn, parry the first hit: expect stamina back and release. Compare with and without the patched exe copy (`tools/disasm/Launch-Absolver-60fps.bat` pattern for a patched copy).

## Point 4 (no confirmed damage on parry)
Not a bug in code. After `HandleParried` the attacker plays `Parried` for `ParryDB.m_ParriedProfile[attackRow.m_iParriedDynamicProfile]` plus `m_ParriedAttackScaleCurve`, and the parrier's recovery is set by `ParryDB.m_iFrameRelease`. Whether it confirms is data: `m_ParriedProfile` durations, `m_iFrameRelease`, and each attack row's `m_iParriedDynamicProfile`. Tunable without code. The block-after-parry reaction works because `Parried` is interruptible by defence at those durations.

## Wallop early-avoid report (different style)
Not analysed. Avoid has the same predicted/current split (`m_AvoidWindowInfos` vs `m_AvoidWindowInfosNoPrediction`, `SetWantAvoided`, `FindActiveMatchingAvoid`); worth checking `OrderAvoided`/`DoesAvoidDismissesHit` for the same kind of crossed pairing.
