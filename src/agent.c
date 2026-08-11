// agent.c
//
// See agent.h for the full design rationale. The context switch is a
// save/restore *swap* of the LogoApp fields find_var and every existing
// opcode still read/write directly (throw_requested/throw_tag/
// run_depth/current_turtle) -- not a rewrite of any of that code.
// Variable *scope* storage is a partial exception: since it now lives
// inside each agent's own `Vm` (see agent.h's own comment and
// MAX_VM_SCOPE_DEPTH in vm.h), it never needs copying at all -- an
// agent's own vm.scopes/vm.scope_depth are already private to it,
// simply by virtue of each Agent owning a separate Vm. Before an
// agent's own turn, its saved state is copied into app's shared
// fields; after, app's (possibly changed) values are copied back into
// the agent's own storage.

#include "agent.h"
#include <stdlib.h>
#include <string.h>

static void agent_restore_state(LogoApp *app, Agent *a) {
    app->throw_requested = a->throw_requested;
    snprintf(app->throw_tag, sizeof(app->throw_tag), "%s", a->throw_tag);
    app->run_depth = a->run_depth;
    app->current_turtle = a->turtle_index;
}

static void agent_save_state(LogoApp *app, Agent *a) {
    a->throw_requested = app->throw_requested;
    snprintf(a->throw_tag, sizeof(a->throw_tag), "%s", app->throw_tag);
    a->run_depth = app->run_depth;
    // current_turtle is deliberately NOT saved back -- turtle_index is
    // fixed at spawn (decision #2); app->current_turtle is scratch
    // state for whichever agent's turn it currently is, freely
    // overwritten by the next agent's own restore.
}

// Every OTHER currently-tracked agent (skipping `waiting` itself) must
// be AGENT_FINISHED for `waiting`'s own AWAIT to resolve -- mirrors
// interpreter.c's own do_pause loop condition in spirit (a simple,
// re-checked-every-pass predicate, not a counted/signalled wakeup).
static gboolean every_other_agent_finished(Agent **agents, int agent_count, Agent *waiting) {
    for (int j = 0; j < agent_count; j++) {
        if (agents[j] == waiting) continue;
        if (agents[j]->state != AGENT_FINISHED) return FALSE;
    }
    return TRUE;
}

// Creates a fresh Agent to run `target_pc` from, assigning it the next
// unused turtle (decision #2), and adds it to `agents`/`*agent_count`
// -- or, if every MAX_TURTLES turtle is already taken, reports a clear
// message and creates nothing. Shared by scheduler_run's own two
// spawn sites: the very first LAUNCH (the one that got the caller into
// multi-agent mode at all, handled once up front below) and every
// later one reached during the round-robin itself, both of which need
// the exact same "make a new agent for this resolved target" logic.
static void spawn_agent(LogoApp *app, Agent **agents, int *agent_count, int target_pc) {
    if (app->turtle_count >= MAX_TURTLES) {
        append_output(app, "LAUNCH: no more turtles available, agent not started\n");
        return;
    }
    Agent *new_agent = calloc(1, sizeof(Agent));
    new_agent->turtle_index = app->turtle_count;
    init_turtle(app, &app->turtles[app->turtle_count]);
    app->turtle_count++;
    new_agent->vm.launch_target_pc = target_pc;
    new_agent->state = AGENT_READY;
    agents[(*agent_count)++] = new_agent;
}

void scheduler_run(LogoApp *app, AstPool *pool, BytecodeChunk *chunk, Agent *initial_agent) {
    // Bounded by MAX_TURTLES, not a separate cap: every agent (the
    // initial one included) owns exactly one turtle (decision #2), so
    // spawn_agent's own turtle-exhaustion check already prevents this
    // array from ever needing more than MAX_TURTLES slots.
    Agent *agents[MAX_TURTLES];
    int agent_count = 0;
    agents[agent_count++] = initial_agent;

    // `initial_agent` arrives already suspended on its own first
    // LAUNCH (that's what got the caller into multi-agent mode at
    // all -- see ui.c's own run_logo_script), with
    // initial_agent->vm.launch_target_pc already resolved but its own
    // *target* never actually spawned yet -- every LATER launch gets
    // spawned inline below, as part of the round-robin itself; this
    // first one needs the identical treatment once, up front, before
    // the loop starts.
    spawn_agent(app, agents, &agent_count, initial_agent->vm.launch_target_pc);
    initial_agent->state = AGENT_READY;

    for (;;) {
        gboolean any_ready = FALSE;
        for (int i = 0; i < agent_count; i++) {
            Agent *a = agents[i];
            if (a->state != AGENT_READY) continue;
            any_ready = TRUE;

            agent_restore_state(app, a);
            VmRunResult result;
            if (!a->started) {
                a->started = TRUE;
                result = vm_run(&a->vm, app, pool, chunk, a->vm.launch_target_pc);
            } else {
                result = vm_resume(&a->vm, app, pool, chunk);
            }
            agent_save_state(app, a);

            switch (result) {
                case VM_RUN_HALTED:
                    a->state = AGENT_FINISHED;
                    break;
                case VM_RUN_SUSPENDED_YIELD:
                    // Stays READY -- picked up again on a LATER pass of
                    // this same outer loop (not immediately), so every
                    // other READY agent gets a fair turn first.
                    a->state = AGENT_READY;
                    break;
                case VM_RUN_SUSPENDED_LAUNCH:
                    // LAUNCH never blocks its own caller, success or
                    // failure -- `a` keeps going regardless.
                    spawn_agent(app, agents, &agent_count, a->vm.launch_target_pc);
                    a->state = AGENT_READY;
                    break;
                case VM_RUN_SUSPENDED_AWAIT:
                    a->state = AGENT_WAITING_JOIN;
                    break;
                case VM_RUN_SUSPENDED_MOTION_DELAY:
                    // SETSPEED's throttle, hit while running as an
                    // agent -- treated like YIELD (stays READY, no
                    // teardown), not like WAIT below: there's no real
                    // timer in this synchronous scheduler to honor the
                    // delay with, but an automatic per-step throttle the
                    // script never explicitly asked for at this call
                    // site doesn't deserve the same reported refusal an
                    // explicit WAIT-like call gets (see this opcode's
                    // own bytecode.h comment). A global SETSPEED just
                    // has no visible slow-motion effect on agents yet.
                    a->state = AGENT_READY;
                    break;
                default:
                    // WAIT/WAITKEY/INPUT/PAUSE/ANIMATESPRITE inside an
                    // agent -- deliberately deferred (see agent.h's own
                    // file comment): an explicit, reported error, not a
                    // silent hang or an attempt at a guessed-at
                    // multi-agent semantic nobody has agreed on yet.
                    append_output(app, "Agent stopped: WAIT/WAITKEY/INPUT/PAUSE/ANIMATESPRITE are not yet supported inside a concurrent agent\n");
                    a->state = AGENT_FINISHED;
                    break;
            }
        }

        for (int i = 0; i < agent_count; i++) {
            if (agents[i]->state == AGENT_WAITING_JOIN && every_other_agent_finished(agents, agent_count, agents[i])) {
                agents[i]->state = AGENT_READY;
            }
        }

        if (!any_ready) break; // every agent FINISHED, or the rest are mutually WAITING_JOIN (see agent.h's own comment)
    }

    for (int i = 0; i < agent_count; i++) free(agents[i]);
}
