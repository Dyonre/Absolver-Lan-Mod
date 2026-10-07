// LanNative M3.5 parry observer: pure text helper (the read-only hook is in lannative.c).
//
// UDefenseComponent::NotifyParrySuccessful passes a ParryPropertyDB weak pointer along; if that pointer is invalid the stamina return, the
// availability layer and the guard-gauge gain are silently skipped (docs/disasm/forsaken-parry-bug-findings.md). Without the fix it reads
// +0x1FC (m_ParryPropertyInPrediction); with the one-byte fix it reads +0x1F4 (m_ParryProperty). This describes what the call is about to see.
#ifndef LANNATIVE_PARRYLOG_H
#define LANNATIVE_PARRYLOG_H

#include <stdio.h>

#define OFF_DEF_PARRYPROPERTY       0x1F4   // UDefenseComponent::m_ParryProperty (FWeakObjectPtr)
#define OFF_DEF_PARRYPROPERTY_PRED  0x1FC   // UDefenseComponent::m_ParryPropertyInPrediction (FWeakObjectPtr)

// fix_applied: LanNative changed the read from +0x1FC to +0x1F4. cur/pred_valid: whether each weak pointer resolves right now.
// Returns 1 when the pointer the function will read is valid (rewards given), 0 when it is not (rewards skipped: the bug).
static int parry_describe(int fix_applied, int cur_valid, int pred_valid, char *out, int cap) {
    int reads_valid = fix_applied ? cur_valid : pred_valid;
    snprintf(out, (size_t)cap, "reads +0x%X (%s) = %s; other field +0x%X = %s -> stamina/availability/guard rewards will be %s",
             fix_applied ? 0x1F4 : 0x1FC, fix_applied ? "m_ParryProperty, parry fix on" : "m_ParryPropertyInPrediction, parry fix off",
             reads_valid ? "valid" : "NULL",
             fix_applied ? 0x1FC : 0x1F4, (fix_applied ? pred_valid : cur_valid) ? "valid" : "null",
             reads_valid ? "given" : "SKIPPED (the Forsaken bug)");
    return reads_valid;
}

#endif
