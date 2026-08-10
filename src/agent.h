#ifndef LOGO_AGENT_H
#define LOGO_AGENT_H

// agent.h
//
// Phase 6's own first slice (see docs/CONCURRENT_AGENTS_DESIGN.md):
// MultiLogo-style concurrent turtle agents, built entirely on top of
// the bytecode VM's own explicit pc/frame-array design and its
// existing suspend/resume mechanism -- LAUNCH/AWAIT/YIELD are just
// three more VmRunResult suspend reasons, same shape as WAIT/PAUSE.
// This first slice deliberately never needs a real GTK timer or
// keypress: WAIT/WAITKEY/INPUT/PAUSE/ANIMATESPRITE used *inside* an
// agent are an explicit, reported error (their own multi-agent
// semantics are a real, separate follow-up), not attempted here -- so
// the whole scheduler below is a plain, synchronous, headless-testable
// C loop, the same style vm.c's own tests already use, not GTK-aware
// code living in ui.c.

#include "vm.h"

typedef enum {
    AGENT_READY,
    // Suspended via AWAIT -- eligible again once every OTHER
    // currently-tracked agent is AGENT_FINISHED (see scheduler_run's
    // own comment on why this is the whole join condition, and its
    // one known, narrow, undocumented-elsewhere failure mode: two or
    // more agents all mutually AWAITing each other, with nothing else
    // left running, never resolves and is simply abandoned in place
    // once nothing is AGENT_READY -- not a hang, just an accepted,
    // narrow gap, matching this project's own tolerance for
    // comparably rare edge cases).
    AGENT_WAITING_JOIN,
    AGENT_FINISHED,
} AgentState;

// One concurrently-scheduled Logo "process" -- see
// docs/CONCURRENT_AGENTS_DESIGN.md's own "Mechanism" section for why
// each of these fields needs to be *per-agent* instead of the single
// shared LogoApp field it shadows during this agent's own turn (see
// agent_save_state/agent_restore_state in agent.c). `vm` is already
// fully self-contained (its own stack/frames/pc/vm_run_depth) --
// nothing here duplicates that. `pool`/`chunk` are deliberately NOT
// here: every agent shares the one compiled program the top-level
// script itself compiled from. Heap-only, like every other multi-MB
// struct in this codebase (`vm` alone is already documented that way)
// -- always calloc'd, never a stack local.
typedef struct {
    Vm vm;
    int turtle_index;   // fixed at spawn (decision #2) -- app->current_turtle only ever borrows this value for the duration of this agent's own turn, never writes it back
    Scope scopes[MAX_SCOPE_DEPTH];
    int scope_depth;
    gboolean throw_requested;
    char throw_tag[64];
    int run_depth;
    AgentState state;
    gboolean started;   // FALSE until this agent's own very first turn -- distinguishes "call vm_run at vm.launch_target_pc" from every later turn's plain vm_resume
} Agent;

// Runs `initial_agent` (already suspended on its own first LAUNCH, with
// its own scopes/scope_depth/throw_requested/throw_tag/run_depth/
// turtle_index already captured by the caller -- see ui.c's own
// run_logo_script) to completion via a synchronous cooperative
// round-robin, together with every agent it (or its own descendants)
// LAUNCHes along the way, all sharing `pool`/`chunk`. Blocks until
// every tracked agent is AGENT_FINISHED (or abandoned per
// AGENT_WAITING_JOIN's own comment above). Frees every Agent it owns,
// including `initial_agent` itself, before returning -- the caller
// must not touch `initial_agent` again after this call.
void scheduler_run(LogoApp *app, AstPool *pool, BytecodeChunk *chunk, Agent *initial_agent);

#endif
