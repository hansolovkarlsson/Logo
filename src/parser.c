// parser.c
//
// See parser.h for scope/rationale. Structure deliberately mirrors
// interpreter.c's own recursive-descent precedence chain (parse_factor
// -> parse_term -> parse_expr -> parse_comparison -> parse_bool_not ->
// parse_bool_and -> parse_condition) almost exactly -- per
// docs/BYTECODE_VM_DESIGN.md, that grammar didn't need redesigning,
// only retargeting from "evaluate immediately" to "build a tree."

#include "parser.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// --- Builtin signatures -----------------------------------------
//
// What the generic call-parsing code (parse_call_like below) needs to
// know per built-in: how many arguments, and what *kind* each one is.
// ARG_EXPR/ARG_CONDITION are both "parse one subtree, using the
// matching precedence chain, and attach it as a child"; ARG_BLOCK
// means "expect a [ ... ] and parse its contents as nested
// statements" (see parse_block) -- structurally different from a
// value expression, not just a different precedence level. IF/IFELSE
// (optional second block, with or without an ELSE keyword) and TO/END
// (name, then a variable number of :params, then a body-until-END) are
// both irregular enough that they get their own dedicated parsing
// functions instead of fitting this table at all. ARG_QUOTED_WORD is
// its own kind, not just ARG_EXPR, because it isn't one: confirmed
// directly in interpreter.c, MAKE's own varname argument is read via
// a raw `sscanf(ptr, "%s", ...)` requiring a literal "word token, not
// parse_expr -- unlike SETPROP's name argument, which does accept any
// expression (e.g. SETPROP WORD "turtle :n ...). MAKE "x -5 only sets
// x to -5, rather than "x" - 5 swallowing the minus as if MAKE's name
// were a general expression, specifically because of this restriction.
typedef enum { ARG_EXPR, ARG_CONDITION, ARG_BLOCK, ARG_QUOTED_WORD } ArgKind;

typedef struct {
    const char *name;
    int arg_count;
    ArgKind arg_kinds[AST_MAX_PARAMS];
} BuiltinSignature;

static const BuiltinSignature BUILTIN_SIGNATURES[] = {
    // Turtle motion -- all plain expression arguments.
    { "FD", 1, { ARG_EXPR } },
    { "FORWARD", 1, { ARG_EXPR } },
    { "BK", 1, { ARG_EXPR } },
    { "BACK", 1, { ARG_EXPR } },
    { "RT", 1, { ARG_EXPR } },
    { "RIGHT", 1, { ARG_EXPR } },
    { "LT", 1, { ARG_EXPR } },
    { "LEFT", 1, { ARG_EXPR } },
    { "SETXY", 2, { ARG_EXPR, ARG_EXPR } },
    { "SETHEADING", 1, { ARG_EXPR } },
    { "SETH", 1, { ARG_EXPR } },
    { "SETX", 1, { ARG_EXPR } },
    { "SETY", 1, { ARG_EXPR } },
    { "GETX", 0, { 0 } },
    { "GETY", 0, { 0 } },
    { "HEADING", 0, { 0 } },
    { "POS", 0, { 0 } },
    { "CANVASSIZE", 0, { 0 } },
    { "DISTANCE", 2, { ARG_EXPR, ARG_EXPR } },
    { "TOWARDS", 1, { ARG_EXPR } },
    { "PENUP", 0, { 0 } },
    { "PU", 0, { 0 } },
    { "PENDOWN", 0, { 0 } },
    { "PD", 0, { 0 } },
    { "HOME", 0, { 0 } },
    { "SETSPEED", 1, { ARG_EXPR } },
    { "SPEED", 0, { 0 } },
    { "TELL", 1, { ARG_EXPR } },
    { "WHO", 0, { 0 } },
    { "CLEAR", 0, { 0 } }, // confirmed the real command is CLEAR/CS, not CLEARSCREEN
    { "CS", 0, { 0 } },

    // Output/variables.
    { "PRINT", 1, { ARG_EXPR } },
    { "MAKE", 2, { ARG_QUOTED_WORD, ARG_EXPR } },
    { "THING", 1, { ARG_EXPR } },
    { "LOCAL", 1, { ARG_QUOTED_WORD } },
    { "NAMES", 0, { 0 } },
    { "PROCEDURES", 0, { 0 } },
    // TEXT/SHOW "name -- ARG_QUOTED_WORD like ERASE/DELETEFILE/LOAD,
    // matching interpreter.c's own raw sscanf("%s") convention for this
    // argument.
    { "TEXT", 1, { ARG_QUOTED_WORD } },
    { "SHOW", 1, { ARG_QUOTED_WORD } },
    { "OUTPUT", 1, { ARG_EXPR } },
    { "STOP", 0, { 0 } },

    // Block-taking control structures. IF/IFELSE aren't here -- their
    // optional second block needs its own parsing function (see
    // parse_if).
    { "REPEAT", 2, { ARG_EXPR, ARG_BLOCK } },
    { "WHILE", 2, { ARG_CONDITION, ARG_BLOCK } },
    { "FOREVER", 1, { ARG_BLOCK } },
    // FOR isn't here -- its header ([var start limit step], a
    // variable-length shape with a bareword name followed by 2-3
    // expressions) doesn't fit any single ArgKind, so it gets its own
    // parse_for, the same way IF/IFELSE and TO/END do (see parse_if).

    // CATCH/THROW (eval.c: shares app->throw_requested/throw_tag with
    // interpreter.c's own THROW/CATCH). tag is a plain expression, not
    // ARG_QUOTED_WORD -- interpreter.c's own THROW/CATCH both read it
    // via parse_expr, not a raw sscanf %s, so e.g. THROW WORD "a "b
    // works exactly like it does today.
    { "CATCH", 2, { ARG_EXPR, ARG_BLOCK } },
    { "THROW", 1, { ARG_EXPR } },

    // RUN/APPLY (eval.c: deferred execution, reusing FOREACH's own
    // re-entrant lex/parse machinery for RUN). Neither ever hands back
    // a value -- confirmed directly in docs/LANGUAGE.md, matching
    // interpreter.c's own RUN/APPLY -- so both are statement-only,
    // same as REPEAT/WHILE/FOREVER above.
    { "RUN", 1, { ARG_EXPR } },
    { "APPLY", 2, { ARG_EXPR, ARG_EXPR } },

    // General file I/O (eval.c: shares app->file_channels[] directly
    // with interpreter.c's own OPENREAD/OPENWRITE/etc). OPENREAD/
    // OPENWRITE/OPENAPPEND/READLINE/CLOSE/FILEPRINT's own arguments are
    // ordinary expressions in interpreter.c (parse_factor/parse_expr,
    // not a raw sscanf %s) -- DELETEFILE/LOAD/SAVE are the exceptions,
    // ARG_QUOTED_WORD like MAKE's varname (see eval.c's own note on
    // do_deletefile/do_load/do_save).
    { "OPENREAD", 1, { ARG_EXPR } },
    { "OPENWRITE", 1, { ARG_EXPR } },
    { "OPENAPPEND", 1, { ARG_EXPR } },
    { "READLINE", 1, { ARG_EXPR } },
    { "EOF?", 1, { ARG_EXPR } },
    { "DIRECTORY", 0, { 0 } },
    { "CLOSE", 1, { ARG_EXPR } },
    { "FILEPRINT", 2, { ARG_EXPR, ARG_EXPR } },
    { "DELETEFILE", 1, { ARG_QUOTED_WORD } },
    { "LOAD", 1, { ARG_QUOTED_WORD } },
    { "SAVE", 1, { ARG_QUOTED_WORD } },

    // SAVEBYTECODE/LOADBYTECODE (docs/ROADMAP.md's "Bytecode save/load/
    // assembler" Stage D) -- same ARG_QUOTED_WORD shape as SAVE/LOAD
    // right above, and deliberately named/grouped to parallel them. VM-
    // only (see vm.c's own call_builtin): there's no BytecodeChunk at
    // all for ast_eval/eval_logo's own tree-walkers to save, so those
    // engines gracefully fall through to do_user_procedure_call's own
    // "I don't know how to SAVEBYTECODE" if either is ever run through
    // one -- no special-casing needed there.
    { "SAVEBYTECODE", 1, { ARG_QUOTED_WORD } },
    { "LOADBYTECODE", 1, { ARG_QUOTED_WORD } },

    // ONKEY/ONCLICK (docs/ROADMAP.md's "Mouse/keyboard event triggers")
    // -- same ARG_QUOTED_WORD shape as SAVEBYTECODE/LOADBYTECODE right
    // above: a compile-time-literal procedure name, resolved against
    // chunk->procs[] at runtime (see vm.c's own exec_onkey/exec_onclick).
    // OFFKEY/OFFCLICK take no arguments -- they just clear whatever's
    // currently registered.
    { "ONKEY", 1, { ARG_QUOTED_WORD } },
    { "OFFKEY", 0, { 0 } },
    { "ONCLICK", 1, { ARG_QUOTED_WORD } },
    { "OFFCLICK", 0, { 0 } },

    // ONMOUSEMOVE -- same shape again, this time for pointer motion
    // (see vm.c's own exec_onmousemove).
    { "ONMOUSEMOVE", 1, { ARG_QUOTED_WORD } },
    { "OFFMOUSEMOVE", 0, { 0 } },

    // ONKEYUP/ONRELEASE -- key/button *release* mirrors of ONKEY/
    // ONCLICK above (see vm.c's own exec_onkeyup/exec_onrelease).
    { "ONKEYUP", 1, { ARG_QUOTED_WORD } },
    { "OFFKEYUP", 0, { 0 } },
    { "ONRELEASE", 1, { ARG_QUOTED_WORD } },
    { "OFFRELEASE", 0, { 0 } },

    // ERASE "name -- procedure deletion, not a drawing primitive
    // despite being grouped with the rest of this batch in
    // docs/ROADMAP.md (a miscategorization worth fixing there, not
    // repeating here) -- ARG_QUOTED_WORD like MAKE/LOCAL/DELETEFILE/
    // LOAD, matching interpreter.c's own raw sscanf("%s") convention.
    { "ERASE", 1, { ARG_QUOTED_WORD } },

    // Drawing/canvas primitives (eval.c: ARC shares the now-exposed
    // record_line directly; LABEL/FILL/ERASERECT share
    // app->labels[]/label_count and app->raster_ops[]/raster_op_count,
    // both plain LogoApp fields already public via logo_types.h, same
    // as lines[]/line_count already were). WRAP/FENCE/WINDOW are
    // one-line app->edge_mode setters -- the actual edge behavior lives
    // in move_turtle_to, already shared since the turtle-motion batch.
    { "ARC", 2, { ARG_EXPR, ARG_EXPR } },
    { "LABEL", 1, { ARG_EXPR } },
    { "FILL", 0, { 0 } },
    { "ERASERECT", 2, { ARG_EXPR, ARG_EXPR } },
    { "WRAP", 0, { 0 } },
    { "FENCE", 0, { 0 } },
    { "WINDOW", 0, { 0 } },
    { "CLEAN", 0, { 0 } },
    { "HIDETURTLE", 0, { 0 } },
    { "HT", 0, { 0 } },
    { "SHOWTURTLE", 0, { 0 } },
    { "ST", 0, { 0 } },
    { "SETPENCOLOR", 3, { ARG_EXPR, ARG_EXPR, ARG_EXPR } },
    { "SETPC", 3, { ARG_EXPR, ARG_EXPR, ARG_EXPR } },
    { "SETPENWIDTH", 1, { ARG_EXPR } },
    { "SETPW", 1, { ARG_EXPR } },
    { "SETBACKGROUND", 3, { ARG_EXPR, ARG_EXPR, ARG_EXPR } },
    { "SETBG", 3, { ARG_EXPR, ARG_EXPR, ARG_EXPR } },
    { "SETCANVASSIZE", 2, { ARG_EXPR, ARG_EXPR } },

    // A representative set of prefix math operators (expression
    // position only -- SUM/DIFFERENCE-shaped, not the infix +/-
    // grammar) demonstrating that the exact same AST_CALL mechanism
    // and signature table serve both statement and operator position,
    // just like interpreter.c's own command-dispatch and
    // procedure-used-as-operator paths both ultimately call
    // call_procedure.
    { "ABS", 1, { ARG_EXPR } },
    { "SQRT", 1, { ARG_EXPR } },
    { "POWER", 2, { ARG_EXPR, ARG_EXPR } },
    { "RANDOM", 1, { ARG_EXPR } },
    { "ROUND", 1, { ARG_EXPR } },
    { "INT", 1, { ARG_EXPR } },
    { "MOD", 2, { ARG_EXPR, ARG_EXPR } },
    { "SIN", 1, { ARG_EXPR } },
    { "COS", 1, { ARG_EXPR } },
    { "TAN", 1, { ARG_EXPR } },
    { "ASIN", 1, { ARG_EXPR } },
    { "ACOS", 1, { ARG_EXPR } },
    { "ARCTAN", 1, { ARG_EXPR } },
    { "LN", 1, { ARG_EXPR } },
    { "LOG", 1, { ARG_EXPR } },
    { "EXP", 1, { ARG_EXPR } },

    // 2026-08-11 Terrapin Logo comparison (docs/ROADMAP.md's "Language
    // completeness") -- VM-only (see vm.c's own call_builtin), same
    // "no BytecodeChunk for ast_eval/eval_logo to run any of this
    // against" reasoning as SAVEBYTECODE/ONKEY above, not touching
    // eval.c's own separate dispatch chain at all.
    { "PI", 0, { 0 } },
    { "RERANDOM", 0, { 0 } },
    { "ASCII", 1, { ARG_EXPR } },
    { "CHAR", 1, { ARG_EXPR } },
    { "UPPERCASE", 1, { ARG_EXPR } },
    { "LOWERCASE", 1, { ARG_EXPR } },
    { "BITAND", 2, { ARG_EXPR, ARG_EXPR } },
    { "BITOR", 2, { ARG_EXPR, ARG_EXPR } },
    { "BITXOR", 2, { ARG_EXPR, ARG_EXPR } },
    { "BITNOT", 1, { ARG_EXPR } },
    { "LSHIFT", 2, { ARG_EXPR, ARG_EXPR } },
    { "RSHIFT", 2, { ARG_EXPR, ARG_EXPR } },
    { "ARCTAN2", 2, { ARG_EXPR, ARG_EXPR } },
    { "SEC", 1, { ARG_EXPR } },
    { "CSC", 1, { ARG_EXPR } },
    { "COT", 1, { ARG_EXPR } },
    { "ASEC", 1, { ARG_EXPR } },
    { "ACSC", 1, { ARG_EXPR } },
    { "ACOT", 1, { ARG_EXPR } },
    { "TIME", 0, { 0 } },
    { "DATE", 0, { 0 } },
    { "MILLISECONDS", 0, { 0 } },
    { "DEFINED?", 1, { ARG_EXPR } },
    { "RANGE", 2, { ARG_EXPR, ARG_EXPR } },
    { "SPACEDRANGE", 3, { ARG_EXPR, ARG_EXPR, ARG_EXPR } },
    { "TURTLES", 0, { 0 } },

    // List/word operators (eval.c: list values are now real, built in
    // app->list_pool, the same pool interpreter.c's own list operators
    // build into). Not the higher-order MAP/FILTER/REDUCE/FOREACH yet
    // -- those take a [template with ?] argument, a different kind of
    // thing again (deferred execution against a substituted string),
    // not just another ARG_EXPR.
    { "FIRST", 1, { ARG_EXPR } },
    { "BUTFIRST", 1, { ARG_EXPR } },
    { "LAST", 1, { ARG_EXPR } },
    { "BUTLAST", 1, { ARG_EXPR } },
    { "COUNT", 1, { ARG_EXPR } },
    { "EMPTY?", 1, { ARG_EXPR } },
    { "WORD?", 1, { ARG_EXPR } },
    { "LIST?", 1, { ARG_EXPR } },
    { "NUMBER?", 1, { ARG_EXPR } },
    { "ARRAY?", 1, { ARG_EXPR } },
    { "FPUT", 2, { ARG_EXPR, ARG_EXPR } },
    { "LPUT", 2, { ARG_EXPR, ARG_EXPR } },
    { "WORD", 2, { ARG_EXPR, ARG_EXPR } },
    { "SENTENCE", 2, { ARG_EXPR, ARG_EXPR } },
    { "SE", 2, { ARG_EXPR, ARG_EXPR } },
    { "LIST", 2, { ARG_EXPR, ARG_EXPR } },
    { "PICK", 1, { ARG_EXPR } },
    { "FLATTEN", 1, { ARG_EXPR } },
    { "PARSE", 1, { ARG_EXPR } },
    { "SUBST", 3, { ARG_EXPR, ARG_EXPR, ARG_EXPR } },
    { "DOT", 2, { ARG_EXPR, ARG_EXPR } },
    { "CROSS", 2, { ARG_EXPR, ARG_EXPR } },
    { "MEMBER?", 2, { ARG_EXPR, ARG_EXPR } },
    // MAP/FILTER/REDUCE's first argument is a template list literal
    // (e.g. [? * 2]), parsed as ordinary ARG_EXPR data like any other
    // list -- eval.c re-lexes/re-parses its rendered text at runtime
    // for each element (see eval.c's own file comment), same as
    // interpreter.c's own MAP/FILTER/REDUCE re-parse it via parse_expr/
    // parse_condition mid-execution.
    { "MAP", 2, { ARG_EXPR, ARG_EXPR } },
    { "FILTER", 2, { ARG_EXPR, ARG_EXPR } },
    { "REDUCE", 2, { ARG_EXPR, ARG_EXPR } },
    // FOREACH's template is a statement, not an expression/condition
    // (e.g. [PRINT ?]), but it's still just ARG_EXPR data here at parse
    // time -- eval.c parses it as a whole program via logo_parse, not
    // logo_parse_expr/logo_parse_condition, once it has an element's
    // text to substitute in.
    { "FOREACH", 2, { ARG_EXPR, ARG_EXPR } },

    // Property lists (eval.c: a separate namespace from ordinary
    // variables, sharing app->plist_entries directly with eval_logo's
    // own SETPROP/GETPROP/etc). plistname/propname are each any
    // expression (matching interpreter.c's own convention -- unlike
    // MAKE's varname, these aren't ARG_QUOTED_WORD).
    { "SETPROP", 3, { ARG_EXPR, ARG_EXPR, ARG_EXPR } },
    { "GETPROP", 2, { ARG_EXPR, ARG_EXPR } },
    { "REMOVEPROP", 2, { ARG_EXPR, ARG_EXPR } },
    { "PROPLIST", 1, { ARG_EXPR } },

    // Prototype-style objects (eval.c: NEW is sugar for SETPROP obj
    // "prototype protoname, reusing property lists directly -- see
    // interpreter.c's own NEW/SEND for the original design).
    { "NEW", 2, { ARG_EXPR, ARG_EXPR } },
    // SEND's arity here is fixed at 3, NOT the old engine's positional
    // "however many args the resolved method takes" -- and that's a
    // deliberate syntax difference, not a simplification. The old
    // engine can look up the method's arity mid-parse (it evaluates
    // obj/message first, since parsing and execution are the same
    // pass there) and then keep consuming exactly that many more
    // expression tokens; this parser builds the whole AST before any
    // value exists to look a property up with, so there is no way to
    // know at parse time how many trailing tokens a SEND call owns.
    // Real languages with equivalent runtime dispatch hit the same
    // wall (Smalltalk's perform:withArguments:, Python's .apply()-
    // style calls) and solve it the same way: an explicit argument
    // list. SEND obj "message arglist here, mirroring APPLY's own
    // existing convention -- arglist is always required, [] for a
    // zero-argument message.
    { "SEND", 3, { ARG_EXPR, ARG_EXPR, ARG_EXPR } },

    // Arrays (eval.c: the one mutable value type -- SETITEM changes a
    // cell in place, and MAKE "b :a aliases the same list_pool cells
    // rather than copying, see set_var_array). Every arg is a plain
    // expression, matching interpreter.c's own convention.
    { "ARRAY", 1, { ARG_EXPR } },
    { "ITEM", 2, { ARG_EXPR, ARG_EXPR } },
    { "SETITEM", 3, { ARG_EXPR, ARG_EXPR, ARG_EXPR } },
    { "FILLARRAY", 2, { ARG_EXPR, ARG_EXPR } },

    // WAIT/WAITKEY/INPUT/PAUSE -- interpreter.c's own suspend-shaped
    // commands ported into this pipeline (see
    // docs/BYTECODE_VM_DESIGN.md's suspend/resume design). WAIT/PAUSE
    // are plain statements, matching interpreter.c's own parse (its own
    // strcasecmp dispatch, not parse_factor's keyword chain); WAITKEY/
    // INPUT take no arguments but do produce a value (both are in
    // interpreter.c's own parse_factor keyword chain), same shape as
    // GETX/POS/HOME above.
    { "WAIT", 1, { ARG_EXPR } },
    { "WAITKEY", 0, { 0 } },
    { "INPUT", 0, { 0 } },
    { "PAUSE", 0, { 0 } },
    // CONTINUE/CO -- PAUSE's own resume command, an ordinary statement
    // (matching interpreter.c's own strcasecmp dispatch) needing no
    // special compiler.c branch at all, unlike PAUSE itself: it never
    // suspends, just decrements app->pause_depth via the generic
    // OP_CALL_BUILTIN path, same shape as an ordinary zero-arg command.
    { "CONTINUE", 0, { 0 } },
    { "CO", 0, { 0 } },

    // Concurrent agents, Phase 6's own first slice (see
    // docs/CONCURRENT_AGENTS_DESIGN.md) -- eval.c: none of these, same
    // vm.c-only scope decision as every suspend/resume-era batch above.
    // LAUNCH's own procedure-name argument is a plain ARG_EXPR, matching
    // APPLY's own name argument (a computed name is fine -- there's no
    // AST to mutate, unlike ERASE/LOAD's compile-time-literal
    // ARG_QUOTED_WORD shape); no argument list in this first slice, a
    // launched agent's own procedure must take no inputs.
    { "LAUNCH", 2, { ARG_EXPR, ARG_EXPR } },
    { "AWAIT", 0, { 0 } },
    { "YIELD", 0, { 0 } },

    // Sprites (eval.c: none of these -- vm.c-only, matching the same
    // scope decision as WAIT/WAITKEY/INPUT/PAUSE above; bin/logo no
    // longer runs on ast_eval, so this pipeline doesn't need parity
    // there). LOADSPRITE/LOADSPRITESHEET/SETSPRITE are ARG_QUOTED_WORD,
    // matching ERASE/DELETEFILE/LOAD's own raw-name convention;
    // SETSPRITEFRAME/ANIMATESPRITE's arguments are plain expressions,
    // matching interpreter.c's own parse_expr-based reads for those.
    // ANIMATESPRITE is the one suspend-shaped member of this group (see
    // OP_ANIMATESPRITE in bytecode.h) -- the other five are ordinary,
    // synchronous commands.
    { "LOADSPRITE", 2, { ARG_QUOTED_WORD, ARG_QUOTED_WORD } },
    { "LOADSPRITESHEET", 4, { ARG_QUOTED_WORD, ARG_QUOTED_WORD, ARG_EXPR, ARG_EXPR } },
    { "SETSPRITE", 1, { ARG_QUOTED_WORD } },
    { "SETSPRITEFRAME", 1, { ARG_EXPR } },
    { "STAMPSPRITE", 0, { 0 } },
    { "ANIMATESPRITE", 2, { ARG_EXPR, ARG_EXPR } },
};
#define BUILTIN_SIGNATURE_COUNT (int)(sizeof(BUILTIN_SIGNATURES) / sizeof(BUILTIN_SIGNATURES[0]))

// --- Parser state -------------------------------------------------

#define MAX_HOISTED_PROCS 50 // matches MAX_PROCEDURES in logo_types.h

typedef struct {
    char name[32];
    int param_count;
} HoistedProc;

typedef struct {
    const LogoToken *tokens;
    int token_count;
    int pos;

    AstPool *pool;
    ParseResult *result;

    HoistedProc hoisted[MAX_HOISTED_PROCS];
    int hoisted_count;
} Parser;

static const LogoToken *peek(Parser *p) {
    return &p->tokens[p->pos];
}

static const LogoToken *advance_token(Parser *p) {
    const LogoToken *tok = &p->tokens[p->pos];
    if (tok->type != LOGO_TOK_EOF && tok->type != LOGO_TOK_ERROR) p->pos++;
    return tok;
}

static int token_is_bareword_ci(const LogoToken *tok, const char *keyword) {
    if (tok->type != LOGO_TOK_BAREWORD) return 0;
    size_t klen = strlen(keyword);
    if ((size_t)tok->length != klen) return 0;
    for (size_t i = 0; i < klen; i++) {
        if (tolower((unsigned char)tok->text[i]) != tolower((unsigned char)keyword[i])) return 0;
    }
    return 1;
}

static void token_text_copy(const LogoToken *tok, char *out, size_t out_size) {
    size_t n = (size_t)tok->length;
    if (n >= out_size) n = out_size - 1;
    memcpy(out, tok->text, n);
    out[n] = '\0';
}

static void report_error(Parser *p, int line, int col, const char *fmt, const char *arg) {
    if (p->result->error_count >= MAX_PARSE_ERRORS) return;
    ParseError *e = &p->result->errors[p->result->error_count++];
    e->line = line;
    e->col = col;
    snprintf(e->message, sizeof(e->message), fmt, arg);
}

static const BuiltinSignature *find_builtin(const LogoToken *tok) {
    for (int i = 0; i < BUILTIN_SIGNATURE_COUNT; i++) {
        if (token_is_bareword_ci(tok, BUILTIN_SIGNATURES[i].name)) return &BUILTIN_SIGNATURES[i];
    }
    return NULL;
}

static const HoistedProc *find_hoisted(Parser *p, const LogoToken *tok) {
    if (tok->type != LOGO_TOK_BAREWORD) return NULL;
    char name[32];
    token_text_copy(tok, name, sizeof(name));
    for (int i = 0; i < p->hoisted_count; i++) {
        if (strcasecmp(p->hoisted[i].name, name) == 0) return &p->hoisted[i];
    }
    return NULL;
}

// Defined much later in this file (needs parse_statement/parse_block,
// which need the whole expression/condition grammar first) -- forward-
// declared here so hoist_from_tokens below (needed much earlier, since
// logo_parse's own hoisting pre-pass runs before any of that) can
// reuse it directly for a LOAD'd file's own TO...END spans, rather
// than duplicating its logic.
static int parse_proc_def(Parser *p);

// --- Hoisting pre-pass, and eager LOAD-following ---------------------
//
// Scans a token stream for top-level "TO name :p1 :p2 ... END" spans,
// recording each name's parameter count into p->hoisted[] (without
// parsing any body). Matches interpreter.c's own TO handling (body
// extends to the very next END token, no nesting logic at all --
// confirmed directly: TO...END isn't nesting-aware today, so this
// pre-pass doesn't need to be either) so that resolving a call site's
// arity later doesn't depend on whether that TO appears earlier or
// later in the file (docs/BYTECODE_VM_DESIGN.md's forward-references
// decision).
//
// Also follows a LOAD "literal-path call found along the way -- the
// fix for a real, documented architectural gap (see
// docs/BYTECODE_VM_DESIGN.md's own LOAD milestone): interpreter.c's
// eval_logo parses and executes one statement at a time, so a LOAD'd
// file's own TO is registered into app->procedures[] the moment
// LOAD's own nested eval_logo call reaches it, and any later top-level
// statement in the SAME calling script (parsed afterward, in sequence)
// can already find it. This engine parses its entire top-level script
// once, up front, before executing anything at all -- so without this,
// a call to a LOAD'd procedure could never resolve, no matter what
// LOAD does at runtime: the caller's own parse would already be
// finished (and would have already reported "unknown word") long
// before do_load ever ran. Only ever needs to handle a literal
// quoted-word path -- LOAD's own ARG_QUOTED_WORD grammar (matching
// interpreter.c's own raw sscanf("%s") restriction) means that's the
// only argument shape LOAD can *ever* syntactically take in valid
// Logo, in either engine; there's no "computed path" case to eagerly
// resolve or fall back on. This is parse-time procedure *visibility*
// only, not early execution: a LOAD'd file's own non-TO top-level
// statements are never run here, only for real at LOAD's own actual
// position in the script, via do_load's own independent runtime
// reparse (eval.c).
//
// Genuinely two passes, not one, and not simply for tidiness: a LOAD'd
// file's own procedure body can itself forward-reference another
// procedure -- one hoisted later in the SAME file, from a *different*
// loaded file, or from the outer script itself -- exactly the same
// forward-reference freedom an ordinary top-level TO already has. That
// only works if p->hoisted[] is COMPLETE before any AST_PROC_DEF node
// gets built from it (parse_proc_def's own body-statement parsing
// resolves calls via find_hoisted(p, ...) as it goes). So pass 1
// (hoist_from_tokens/eager_follow_load) only ever discovers names/
// arities and reads+stores file content, recursing through every
// reachable LOAD -- never building a real node. Pass 2
// (build_eager_procedures) runs only after pass 1 has finished
// completely, re-lexing each stored buffer and building its own real
// AST_PROC_DEF nodes then, with p->hoisted[] finally whole. This does
// mean each eagerly-loaded file's content gets lexed twice (once
// transiently in pass 1 just to walk it for hoisting/nested LOADs,
// again in pass 2 to actually build nodes) and its own TO...END spans
// get parsed a third time, redundantly, when do_load reparses the
// whole file again at its own runtime position -- a deliberate,
// accepted tradeoff for correctness over speed (LOAD isn't a hot
// path).
#define MAX_EAGER_LOAD_DEPTH 8 // recursion cap against a self- or mutually-referential LOAD chain, same reasoning as MAX_RUN_DEPTH -- a real limit, not a soft guess
#define MAX_EAGER_LOAD_TOKENS 8192 // matches eval.c's own MAX_LOAD_TOKENS

static void hoist_from_tokens(Parser *p, const LogoToken *tokens, int depth);

// Pass 1's other half: reads, lexes, and hoists one LOAD'd file's own
// top-level TO...END spans (name/arity only) and recurses into any
// LOAD found within it -- heap-allocates its own transient token
// buffer (never a stack local: this recurses, and
// MAX_EAGER_LOAD_TOKENS-many LogoTokens is large enough to be a real
// per-frame stack-overflow risk at any nontrivial depth, the same
// class of mistake this project has been bitten by before -- see the
// eval_logo_recursion_margin memory). Silently does nothing if the
// file can't be read/is too large to eagerly hoist, or the depth cap
// is hit -- do_load's own independent runtime attempt is what reports
// the real error in that case, exactly as it already did before this
// feature existed.
static void eager_follow_load(Parser *p, const char *path, int depth) {
    if (depth >= MAX_EAGER_LOAD_DEPTH) return;
    if (p->result->eager_loaded_count >= MAX_EAGER_LOADS) return;

    FILE *f = fopen(path, "rb");
    if (f == NULL) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return; }
    char *contents = malloc((size_t)size + 1);
    size_t read_bytes = fread(contents, 1, (size_t)size, f);
    fclose(f);
    contents[read_bytes] = '\0';

    // Owned for as long as `p->pool` is -- pass 2 (build_eager_procedures)
    // re-lexes this exact buffer to build real AST_PROC_DEF nodes whose
    // own .text/.param_names/body_text point directly into it (see
    // ParseResult's own comment on eager_loaded_sources).
    p->result->eager_loaded_sources[p->result->eager_loaded_count++] = contents;

    LogoToken *tokens = malloc(sizeof(LogoToken) * MAX_EAGER_LOAD_TOKENS);
    int n = logo_lex(contents, tokens, MAX_EAGER_LOAD_TOKENS);
    if (n >= 0) hoist_from_tokens(p, tokens, depth + 1);
    free(tokens);
}

// Pass 1: scans `tokens` for top-level TO...END spans, recording each
// one's name/arity into p->hoisted[] (see MAX_HOISTED_PROCS above --
// same table an ordinary top-level TO already hoists into), and
// recurses into any LOAD "literal-path found along the way via
// eager_follow_load. Never builds an AST_PROC_DEF node -- see this
// section's own file comment for why that has to wait for pass 2.
static void hoist_from_tokens(Parser *p, const LogoToken *tokens, int depth) {
    int i = 0;
    while (tokens[i].type != LOGO_TOK_EOF && tokens[i].type != LOGO_TOK_ERROR) {
        if (token_is_bareword_ci(&tokens[i], "TO") && tokens[i + 1].type == LOGO_TOK_BAREWORD) {
            char name[32];
            token_text_copy(&tokens[i + 1], name, sizeof(name));
            int param_count = 0;
            int j = i + 2;
            while (tokens[j].type == LOGO_TOK_VARREF) {
                param_count++;
                j++;
            }
            if (p->hoisted_count < MAX_HOISTED_PROCS) {
                HoistedProc *h = &p->hoisted[p->hoisted_count++];
                snprintf(h->name, sizeof(h->name), "%s", name);
                h->param_count = param_count;
            }
            // Skip ahead to (and past) the next END, same "no nesting"
            // assumption interpreter.c's own strcasestr(ptr, "END") makes.
            while (tokens[j].type != LOGO_TOK_EOF && tokens[j].type != LOGO_TOK_ERROR &&
                   !token_is_bareword_ci(&tokens[j], "END")) {
                j++;
            }
            i = (tokens[j].type == LOGO_TOK_EOF || tokens[j].type == LOGO_TOK_ERROR) ? j : j + 1;
        } else if (token_is_bareword_ci(&tokens[i], "LOAD") && tokens[i + 1].type == LOGO_TOK_QUOTED_WORD) {
            char path[512];
            token_text_copy(&tokens[i + 1], path, sizeof(path));
            eager_follow_load(p, path, depth);
            i += 2;
        } else {
            i++;
        }
    }
}

// Pass 2: for every file pass 1 eagerly loaded (a flat list by now,
// p->result->eager_loaded_sources -- regardless of how deeply nested
// the original LOAD chain that discovered each one was), re-lexes its
// stored content and builds a real AST_PROC_DEF node for each of its
// own top-level TO...END spans directly into p->pool, via a nested
// Parser sharing p->pool/p->result but pointed at this buffer's own
// fresh tokens (reusing parse_proc_def directly rather than
// duplicating its body-parsing logic). Only ever called after
// hoist_from_tokens (pass 1) has finished completely -- across every
// eagerly-loaded file, however nested -- so p->hoisted[] is whole by
// the time any of these bodies get parsed, and a forward reference
// inside one resolves correctly regardless of which file (or the
// outer script) actually hoisted the name it's calling.
static void build_eager_procedures(Parser *p) {
    for (int f = 0; f < p->result->eager_loaded_count; f++) {
        const char *contents = p->result->eager_loaded_sources[f];
        LogoToken *tokens = malloc(sizeof(LogoToken) * MAX_EAGER_LOAD_TOKENS);
        int n = logo_lex(contents, tokens, MAX_EAGER_LOAD_TOKENS);
        if (n >= 0) {
            int j = 0;
            while (tokens[j].type != LOGO_TOK_EOF && tokens[j].type != LOGO_TOK_ERROR) {
                if (token_is_bareword_ci(&tokens[j], "TO") && tokens[j + 1].type == LOGO_TOK_BAREWORD) {
                    Parser nested = *p;
                    nested.tokens = tokens;
                    nested.pos = j;
                    parse_proc_def(&nested); // discards the returned node index -- reachable via find_proc_def by name afterward, nothing else needs it here
                    j = nested.pos; // parse_proc_def already advanced past this span's own END
                } else {
                    j++;
                }
            }
        }
        free(tokens);
    }
}

static void hoist_procedures(Parser *p) {
    hoist_from_tokens(p, p->tokens, 0);
    build_eager_procedures(p);
}

// --- Expression grammar ---------------------------------------------

static int parse_expr_value(Parser *p);
static int parse_condition(Parser *p);
static int parse_block(Parser *p);
static int parse_statement(Parser *p);

// A generic call's arguments, per `sig` -- shared by expression-position
// operator calls and statement-position commands alike, since both are
// just an AST_CALL node with the same argument-parsing rules (matching
// how interpreter.c's own command-dispatch and procedure-as-operator
// fallback both ultimately call call_procedure the same way).
static int parse_call_args(Parser *p, int call_node, int arg_count, const ArgKind *kinds) {
    for (int i = 0; i < arg_count; i++) {
        int arg;
        switch (kinds != NULL ? kinds[i] : ARG_EXPR) {
            case ARG_CONDITION: arg = parse_condition(p); break;
            case ARG_BLOCK: arg = parse_block(p); break;
            case ARG_QUOTED_WORD: {
                const LogoToken *wt = peek(p);
                if (wt->type == LOGO_TOK_QUOTED_WORD) {
                    arg = ast_alloc(p->pool, AST_WORD, wt->line, wt->col);
                    token_text_copy(wt, p->pool->nodes[arg].text, AST_MAX_TEXT);
                    advance_token(p);
                } else {
                    report_error(p, wt->line, wt->col, "%s", "expected a \"word");
                    arg = ast_alloc(p->pool, AST_WORD, wt->line, wt->col);
                }
                break;
            }
            default: arg = parse_expr_value(p); break;
        }
        ast_append_child(p->pool, call_node, arg);
    }
    return call_node;
}

// Tries to resolve `tok` as a callable (a built-in signature or a
// hoisted user procedure) and, if found, builds its AST_CALL node and
// parses its arguments. `require_value_only` restricts to signatures
// with no ARG_BLOCK/ARG_CONDITION argument -- expression (value-
// producing) position needs this (a block/condition argument isn't a
// value; REPEAT/WHILE can't sensibly appear inside PRINT ...), while
// statement position doesn't care. Returns the new node's index, or -1
// if `tok` isn't a recognized call under that restriction, so the
// caller can fall back to an "unknown word" error.
static int try_parse_call(Parser *p, int require_value_only) {
    const LogoToken *tok = peek(p);
    int line = tok->line, col = tok->col;

    const BuiltinSignature *sig = find_builtin(tok);
    if (sig != NULL) {
        int usable = 1;
        if (require_value_only) {
            for (int i = 0; i < sig->arg_count; i++) {
                if (sig->arg_kinds[i] == ARG_BLOCK || sig->arg_kinds[i] == ARG_CONDITION) usable = 0;
            }
        }
        if (usable) {
            advance_token(p);
            int node = ast_alloc(p->pool, AST_CALL, line, col);
            snprintf(p->pool->nodes[node].text, AST_MAX_TEXT, "%s", sig->name);
            return parse_call_args(p, node, sig->arg_count, sig->arg_kinds);
        }
    }

    const HoistedProc *proc = find_hoisted(p, tok);
    if (proc != NULL) {
        char name[32];
        token_text_copy(tok, name, sizeof(name));
        advance_token(p);
        int node = ast_alloc(p->pool, AST_CALL, line, col);
        snprintf(p->pool->nodes[node].text, AST_MAX_TEXT, "%s", name);
        return parse_call_args(p, node, proc->param_count, NULL);
    }

    return -1;
}

// A bareword in expression (value-producing) position: TRUE/FALSE
// literals, or a call resolved via try_parse_call (restricted to
// value-only signatures). Anything else is an unresolved identifier --
// reported, not silently accepted.
static int parse_bareword_value(Parser *p) {
    const LogoToken *tok = peek(p);
    int line = tok->line, col = tok->col;

    if (token_is_bareword_ci(tok, "TRUE") || token_is_bareword_ci(tok, "FALSE")) {
        char text[8];
        token_text_copy(tok, text, sizeof(text));
        for (char *c = text; *c; c++) *c = (char)toupper((unsigned char)*c);
        advance_token(p);
        int node = ast_alloc(p->pool, AST_WORD, line, col);
        snprintf(p->pool->nodes[node].text, AST_MAX_TEXT, "%s", text);
        return node;
    }

    int call = try_parse_call(p, /*require_value_only=*/1);
    if (call >= 0) return call;

    char name[64];
    token_text_copy(tok, name, sizeof(name));
    report_error(p, line, col, "unknown word: %s", name);
    advance_token(p);
    return ast_alloc(p->pool, AST_NUMBER, line, col); // a well-formed placeholder so parsing can continue
}

// A bareword in statement position: any call resolved via
// try_parse_call, with no value-only restriction -- REPEAT/WHILE's own
// block/condition arguments are exactly what statement position (and
// only statement position) needs to accept.
static int parse_call_statement(Parser *p) {
    const LogoToken *tok = peek(p);
    int line = tok->line, col = tok->col;

    int call = try_parse_call(p, /*require_value_only=*/0);
    if (call >= 0) return call;

    char name[64];
    token_text_copy(tok, name, sizeof(name));
    report_error(p, line, col, "unknown word: %s", name);
    advance_token(p);
    return ast_alloc(p->pool, AST_NUMBER, line, col);
}

// A [ ... ] list literal used as a *value* -- its contents are plain
// untyped atoms (AST_WORD leaves or nested AST_LIST_LITERAL), never
// operator-precedence-parsed, exactly mirroring interpreter.c's own
// parse_list_literal/LIST_ELEM_WORD (a list's own elements were never
// arithmetic expressions to begin with -- only RUN/APPLY, later,
// re-parses one as a program).
static int parse_list_literal(Parser *p) {
    const LogoToken *open = peek(p);
    int node = ast_alloc(p->pool, AST_LIST_LITERAL, open->line, open->col);
    advance_token(p); // [

    while (peek(p)->type != LOGO_TOK_RBRACKET) {
        const LogoToken *tok = peek(p);
        if (tok->type == LOGO_TOK_EOF || tok->type == LOGO_TOK_ERROR) {
            report_error(p, open->line, open->col, "%s", "[ list ]: missing closing ]");
            return node;
        }
        if (tok->type == LOGO_TOK_LBRACKET) {
            ast_append_child(p->pool, node, parse_list_literal(p));
        } else {
            int leaf = ast_alloc(p->pool, AST_WORD, tok->line, tok->col);
            // A leading -/+ glued to the next token with no whitespace
            // gap (e.g. -100 in [0 -100]) is a third instance of the
            // same fidelity gap class as the " and : fixes above, just
            // shaped differently: interpreter.c's own list-literal scan
            // has no concept of tokens at all, just a run of
            // non-whitespace/non-bracket characters, so [0 -100] is
            // genuinely the 2-element list `0 -100`, not `0`, `-`,
            // `100` (confirmed directly, not assumed -- COUNT [0 -100]
            // is 2 in the real interpreter). This lexer's uniform
            // tokenizing (needed for the rest of the grammar, where - is
            // subtraction/negation) always splits a leading sign off as
            // its own token; merge it back onto the very next token here
            // to match -- checking adjacency via the token's own text
            // pointers (no lexer whitespace-skip landed between them),
            // not just token type, so `0 - 100` (real subtraction, a
            // space on both sides) is correctly left as three elements.
            const char *sign = "";
            if (tok->type == LOGO_TOK_MINUS || tok->type == LOGO_TOK_PLUS) {
                const LogoToken *next = &p->tokens[p->pos + 1];
                if (next->type != LOGO_TOK_EOF && next->type != LOGO_TOK_ERROR &&
                    next->type != LOGO_TOK_LBRACKET && next->type != LOGO_TOK_RBRACKET &&
                    next->text == tok->text + tok->length) {
                    sign = (tok->type == LOGO_TOK_MINUS) ? "-" : "+";
                    advance_token(p); // consume the sign; tok becomes the merged-onto token below
                    tok = next;
                }
            }
            if (tok->type == LOGO_TOK_QUOTED_WORD) {
                // interpreter.c's own list-literal scan never treats "
                // specially -- inside [...] it's just another
                // character, so PRINT [a "b c] really does print `a "b
                // c` with the quote mark included (confirmed directly
                // against the running interpreter, not assumed): the
                // stored element is the literal text `"b`, not `b`.
                // This lexer's own " handling strips the mark before
                // the token ever reaches here, so it's added back to
                // match -- otherwise a quoted-word list element could
                // never be told apart from a bareword one once it's
                // re-serialized to text, which is exactly what MAP/
                // FILTER/REDUCE's own template substitution needs to
                // do at runtime (see eval.c's eval_value_to_source_text)
                // to tell "b apart from a plain b when re-quoting it.
                snprintf(p->pool->nodes[leaf].text, AST_MAX_TEXT, "%s\"%.*s", sign, tok->length, tok->text);
            } else if (tok->type == LOGO_TOK_VARREF) {
                // Same class of bug, same fix, for :name -- confirmed
                // directly that PRINT [MAKE "sum :sum + ?] really does
                // print `:sum` with the colon intact, and this lexer's
                // own : handling strips it before the token reaches
                // here (see the LOGO_TOK_QUOTED_WORD case just above).
                // Surfaced by FOREACH: a template that reads a variable
                // (`[MAKE "sum :sum + ?]`) silently lost the colon on
                // every re-parse, so :sum read back as the unrelated
                // bareword `sum` -- always 0, not a real accumulation.
                snprintf(p->pool->nodes[leaf].text, AST_MAX_TEXT, "%s:%.*s", sign, tok->length, tok->text);
            } else if (sign[0] != '\0') {
                snprintf(p->pool->nodes[leaf].text, AST_MAX_TEXT, "%s%.*s", sign, tok->length, tok->text);
            } else {
                token_text_copy(tok, p->pool->nodes[leaf].text, AST_MAX_TEXT);
            }
            advance_token(p);
            ast_append_child(p->pool, node, leaf);
        }
    }
    advance_token(p); // ]
    return node;
}

// Unary/literal/grouping level -- interpreter.c's parse_factor.
static int parse_factor(Parser *p) {
    const LogoToken *tok = peek(p);

    if (tok->type == LOGO_TOK_MINUS) {
        advance_token(p);
        int node = ast_alloc(p->pool, AST_NEG, tok->line, tok->col);
        ast_append_child(p->pool, node, parse_factor(p));
        return node;
    }
    if (tok->type == LOGO_TOK_PLUS) {
        advance_token(p); // unary + is a no-op, same as interpreter.c's own parse_factor
        return parse_factor(p);
    }
    if (tok->type == LOGO_TOK_LPAREN) {
        advance_token(p);
        int node = parse_expr_value(p);
        if (peek(p)->type == LOGO_TOK_RPAREN) {
            advance_token(p);
        } else {
            report_error(p, tok->line, tok->col, "%s", "( ...: missing closing )");
        }
        return node;
    }
    if (tok->type == LOGO_TOK_NUMBER) {
        advance_token(p);
        int node = ast_alloc(p->pool, AST_NUMBER, tok->line, tok->col);
        char buf[64];
        token_text_copy(tok, buf, sizeof(buf));
        p->pool->nodes[node].number = strtod(buf, NULL);
        return node;
    }
    if (tok->type == LOGO_TOK_QUOTED_WORD || tok->type == LOGO_TOK_RAW_TEXT) {
        advance_token(p);
        int node = ast_alloc(p->pool, AST_WORD, tok->line, tok->col);
        token_text_copy(tok, p->pool->nodes[node].text, AST_MAX_TEXT);
        return node;
    }
    if (tok->type == LOGO_TOK_VARREF) {
        advance_token(p);
        int node = ast_alloc(p->pool, AST_VARREF, tok->line, tok->col);
        token_text_copy(tok, p->pool->nodes[node].text, AST_MAX_TEXT);
        return node;
    }
    if (tok->type == LOGO_TOK_LBRACKET) {
        return parse_list_literal(p);
    }
    if (tok->type == LOGO_TOK_BAREWORD) {
        return parse_bareword_value(p);
    }

    report_error(p, tok->line, tok->col, "%s", "expected a value");
    return ast_alloc(p->pool, AST_NUMBER, tok->line, tok->col);
}

static int parse_term(Parser *p) {
    int node = parse_factor(p);
    for (;;) {
        const LogoToken *tok = peek(p);
        if (tok->type != LOGO_TOK_STAR && tok->type != LOGO_TOK_SLASH) break;
        AstBinOp op = (tok->type == LOGO_TOK_STAR) ? AST_OP_MUL : AST_OP_DIV;
        advance_token(p);
        int bin = ast_alloc(p->pool, AST_BINOP, tok->line, tok->col);
        p->pool->nodes[bin].binop = op;
        ast_append_child(p->pool, bin, node);
        ast_append_child(p->pool, bin, parse_factor(p));
        node = bin;
    }
    return node;
}

static int parse_expr_value(Parser *p) {
    int node = parse_term(p);
    for (;;) {
        const LogoToken *tok = peek(p);
        if (tok->type != LOGO_TOK_PLUS && tok->type != LOGO_TOK_MINUS) break;
        AstBinOp op = (tok->type == LOGO_TOK_PLUS) ? AST_OP_ADD : AST_OP_SUB;
        advance_token(p);
        int bin = ast_alloc(p->pool, AST_BINOP, tok->line, tok->col);
        p->pool->nodes[bin].binop = op;
        ast_append_child(p->pool, bin, node);
        ast_append_child(p->pool, bin, parse_term(p));
        node = bin;
    }
    return node;
}

// --- Condition grammar (IF/WHILE's own tests) ------------------------
//
// A separate entry point from parse_expr_value, matching
// interpreter.c's own parse_comparison/parse_bool_not/parse_bool_and/
// parse_condition: relational operators and NOT/AND/OR only mean
// anything here, not in a general value expression (there's no
// `MAKE "x (1 = 2)` producing a first-class boolean in today's
// grammar either). Unlike today's interpreter, both this and
// parse_expr_value now accept ( ... ) grouping -- the resolved
// "boolean grouping" decision in docs/BYTECODE_VM_DESIGN.md: a small,
// strictly additive fix to the asymmetry where only arithmetic had it.

static int parse_comparison(Parser *p) {
    int left = parse_expr_value(p);
    const LogoToken *tok = peek(p);
    AstCompareOp op;
    switch (tok->type) {
        case LOGO_TOK_LT: op = AST_CMP_LT; break;
        case LOGO_TOK_GT: op = AST_CMP_GT; break;
        case LOGO_TOK_EQ: op = AST_CMP_EQ; break;
        case LOGO_TOK_LE: op = AST_CMP_LE; break;
        case LOGO_TOK_GE: op = AST_CMP_GE; break;
        case LOGO_TOK_NE: op = AST_CMP_NE; break;
        default: return left; // no relational operator -- a bare expr, evaluated for truthiness later
    }
    advance_token(p);
    int node = ast_alloc(p->pool, AST_COMPARE, tok->line, tok->col);
    p->pool->nodes[node].cmpop = op;
    ast_append_child(p->pool, node, left);
    ast_append_child(p->pool, node, parse_expr_value(p));
    return node;
}

// One condition "atom": either NOT <atom>, ( <condition> ), or a
// parse_comparison. Parenthesized grouping lives here (rather than
// only at parse_condition/parse_bool_and) so `NOT (:x = 1 OR :y = 2)`
// groups correctly -- the parenthesized form recurses into the full
// parse_condition, same as arithmetic's own ( ... ) in parse_factor
// recurses into the full parse_expr_value.
static int parse_bool_not(Parser *p) {
    const LogoToken *tok = peek(p);
    if (token_is_bareword_ci(tok, "NOT")) {
        advance_token(p);
        int node = ast_alloc(p->pool, AST_NOT, tok->line, tok->col);
        ast_append_child(p->pool, node, parse_bool_not(p));
        return node;
    }
    if (tok->type == LOGO_TOK_LPAREN) {
        advance_token(p);
        int node = parse_condition(p);
        if (peek(p)->type == LOGO_TOK_RPAREN) {
            advance_token(p);
        } else {
            report_error(p, tok->line, tok->col, "%s", "( ...: missing closing )");
        }
        return node;
    }
    return parse_comparison(p);
}

static int parse_bool_and(Parser *p) {
    int node = parse_bool_not(p);
    while (token_is_bareword_ci(peek(p), "AND")) {
        const LogoToken *tok = peek(p);
        advance_token(p);
        int and_node = ast_alloc(p->pool, AST_AND, tok->line, tok->col);
        ast_append_child(p->pool, and_node, node);
        ast_append_child(p->pool, and_node, parse_bool_not(p));
        node = and_node;
    }
    return node;
}

static int parse_condition(Parser *p) {
    int node = parse_bool_and(p);
    while (token_is_bareword_ci(peek(p), "OR")) {
        const LogoToken *tok = peek(p);
        advance_token(p);
        int or_node = ast_alloc(p->pool, AST_OR, tok->line, tok->col);
        ast_append_child(p->pool, or_node, node);
        ast_append_child(p->pool, or_node, parse_bool_and(p));
        node = or_node;
    }
    return node;
}

// --- Statements -------------------------------------------------------

// [ ... ] used as a *block* (REPEAT/WHILE/IF's own bodies, a TO's own
// body): its contents are parsed as nested statements, not as list-
// literal data atoms -- the same bracket syntax means two structurally
// different things depending on which argument position consumes it
// (see the file comment on ArgKind above), never something the
// brackets' own contents can decide by themselves.
static int parse_block(Parser *p) {
    const LogoToken *open = peek(p);
    if (open->type != LOGO_TOK_LBRACKET) {
        report_error(p, open->line, open->col, "%s", "expected [ block ]");
        return ast_alloc(p->pool, AST_BLOCK, open->line, open->col);
    }
    advance_token(p);
    int node = ast_alloc(p->pool, AST_BLOCK, open->line, open->col);
    while (peek(p)->type != LOGO_TOK_RBRACKET) {
        if (peek(p)->type == LOGO_TOK_EOF || peek(p)->type == LOGO_TOK_ERROR) {
            report_error(p, open->line, open->col, "%s", "[ block ]: missing closing ]");
            return node;
        }
        ast_append_child(p->pool, node, parse_statement(p));
    }
    advance_token(p); // ]
    return node;
}

// IF/IFELSE share one shape in today's interpreter: a condition, a
// first block, and an *optional* second block -- with or without an
// ELSE keyword in front of it (confirmed directly against
// interpreter.c: IF alone already accepts a trailing else-block).
// AST_IF's third child is simply absent (next_sibling stays -1) when
// there's no else-block, rather than IF and IFELSE being different
// node types.
static int parse_if(Parser *p) {
    const LogoToken *tok = peek(p);
    advance_token(p); // IF or IFELSE
    int is_ifelse = token_is_bareword_ci(tok, "IFELSE");

    int node = ast_alloc(p->pool, AST_IF, tok->line, tok->col);
    ast_append_child(p->pool, node, parse_condition(p));
    ast_append_child(p->pool, node, parse_block(p));

    if (token_is_bareword_ci(peek(p), "ELSE")) {
        advance_token(p);
        ast_append_child(p->pool, node, parse_block(p));
    } else if (peek(p)->type == LOGO_TOK_LBRACKET) {
        ast_append_child(p->pool, node, parse_block(p));
    } else if (is_ifelse) {
        report_error(p, tok->line, tok->col, "%s", "IFELSE: expected two [ block ]s");
    }
    return node;
}

// FOR [var start limit step] [block] -- irregular like IF/TO: the
// header is [ bareword-varname expr expr expr? ], not a plain list
// literal (AST_LIST_LITERAL's own elements are untyped data atoms, not
// expression subtrees) or a single block/condition, so it gets its own
// parse function rather than fitting BUILTIN_SIGNATURES. start/limit/
// step are ordinary expressions, not just literals -- confirmed
// directly: interpreter.c accepts `FOR [i 1 :n + 1] [...]` -- so each
// is parsed straight off the live token stream via parse_expr_value,
// the same way REPEAT's count argument is, rather than being collected
// as list-literal text and re-lexed later the way FOREACH's template
// is (there's no substitution step here, so no need to reconstruct
// text at all). step is optional; whether the third header element is
// step or the loop already ended at ] is exec_for's job to resolve at
// runtime (see eval.c), same as interpreter.c defaulting step to +1/-1
// there.
static int parse_for(Parser *p) {
    const LogoToken *for_tok = peek(p);
    advance_token(p); // FOR
    int node = ast_alloc(p->pool, AST_FOR, for_tok->line, for_tok->col);

    if (peek(p)->type != LOGO_TOK_LBRACKET) {
        report_error(p, for_tok->line, for_tok->col, "%s", "FOR: expected [ var start limit step ]");
        return node;
    }
    advance_token(p); // [

    const LogoToken *var_tok = peek(p);
    if (var_tok->type != LOGO_TOK_BAREWORD) {
        report_error(p, var_tok->line, var_tok->col, "%s", "FOR: expected a loop variable name");
        return node;
    }
    token_text_copy(var_tok, p->pool->nodes[node].text, AST_MAX_TEXT);
    advance_token(p);

    ast_append_child(p->pool, node, parse_expr_value(p)); // start
    ast_append_child(p->pool, node, parse_expr_value(p)); // limit
    if (peek(p)->type != LOGO_TOK_RBRACKET) {
        ast_append_child(p->pool, node, parse_expr_value(p)); // step, if present
    }

    if (peek(p)->type == LOGO_TOK_RBRACKET) {
        advance_token(p);
    } else {
        report_error(p, for_tok->line, for_tok->col, "%s", "FOR: expected [ var start limit step ]");
    }

    ast_append_child(p->pool, node, parse_block(p)); // loop body
    return node;
}

// TO name :p1 :p2 ... [ body statements ] END -- irregular enough
// (a variable-length parameter list, then a body that ends at a
// keyword rather than a bracket) that it gets its own function rather
// than fitting BUILTIN_SIGNATURES. Arity was already recorded by
// hoist_procedures; this just builds the real AST_PROC_DEF now that
// bodies can reference procedures defined anywhere else in the file.
static int parse_proc_def(Parser *p) {
    const LogoToken *to_tok = peek(p);
    advance_token(p); // TO

    int node = ast_alloc(p->pool, AST_PROC_DEF, to_tok->line, to_tok->col);
    const LogoToken *name_tok = peek(p);
    if (name_tok->type != LOGO_TOK_BAREWORD) {
        report_error(p, name_tok->line, name_tok->col, "%s", "TO: expected a procedure name");
        return node;
    }
    token_text_copy(name_tok, p->pool->nodes[node].text, AST_MAX_TEXT);
    advance_token(p);

    while (peek(p)->type == LOGO_TOK_VARREF) {
        const LogoToken *param_tok = peek(p);
        AstNode *n = &p->pool->nodes[node];
        if (n->param_count < AST_MAX_PARAMS) {
            token_text_copy(param_tok, n->param_names[n->param_count], sizeof(n->param_names[0]));
            n->param_count++;
        }
        advance_token(p);
    }

    int body = ast_alloc(p->pool, AST_BLOCK, peek(p)->line, peek(p)->col);
    // peek(p)->text right here (before the body's own statements are
    // parsed) is the body's own start -- already past every :param
    // (each advance_token above lands on the next token with no
    // whitespace-skipping step of its own to add, since the lexer
    // already skips whitespace between tokens), mirroring
    // interpreter.c's own ptr position after its params-parsing loop.
    const char *body_start = peek(p)->text;
    while (!token_is_bareword_ci(peek(p), "END")) {
        if (peek(p)->type == LOGO_TOK_EOF || peek(p)->type == LOGO_TOK_ERROR) {
            char name[32];
            token_text_copy(name_tok, name, sizeof(name));
            report_error(p, to_tok->line, to_tok->col, "TO %s: missing END", name);
            ast_append_child(p->pool, node, body);
            return node;
        }
        ast_append_child(p->pool, body, parse_statement(p));
    }
    // peek(p) is now the END token itself -- its own .text is exactly
    // where interpreter.c's own strcasestr(ptr, "END") would land, so
    // this span matches Procedure.body's own bounds exactly (see
    // ast.h's comment on body_text/body_len).
    p->pool->nodes[node].body_text = body_start;
    p->pool->nodes[node].body_len = (int)(peek(p)->text - body_start);
    advance_token(p); // END
    ast_append_child(p->pool, node, body);
    return node;
}

// One statement: TO's own definition, IF/IFELSE, or a generic call
// (a built-in with a REPEAT/WHILE-style block/condition argument, or a
// plain expression-only built-in/procedure used for its side effects
// -- same AST_CALL either way, its return value just going unused,
// matching how interpreter.c's own statement-level dispatch and
// expression-level "procedure as operator" fallback both call
// call_procedure and simply differ in whether they keep the result).
static int parse_statement(Parser *p) {
    const LogoToken *tok = peek(p);
    if (token_is_bareword_ci(tok, "TO")) return parse_proc_def(p);
    if (token_is_bareword_ci(tok, "IF") || token_is_bareword_ci(tok, "IFELSE")) return parse_if(p);
    if (token_is_bareword_ci(tok, "FOR")) return parse_for(p);
    return parse_call_statement(p);
}

// Frees any eagerly-loaded source buffers a PREVIOUS logo_parse call
// on this exact ParseResult left behind, before either reusing it (a
// caller like eval.c's do_foreach calls logo_parse repeatedly on one
// long-lived scratch ParseResult across a whole list iteration) or
// discarding it for good (parse_result_destroy, below) -- shared by
// both so there's exactly one place this bookkeeping lives.
static void reset_eager_loaded(ParseResult *result) {
    for (int i = 0; i < result->eager_loaded_count; i++) {
        free(result->eager_loaded_sources[i]);
        result->eager_loaded_sources[i] = NULL;
    }
    result->eager_loaded_count = 0;
}

void parse_result_destroy(ParseResult *result) {
    if (result == NULL) return;
    reset_eager_loaded(result);
    free(result);
}

void logo_parse(const LogoToken *tokens, int token_count, ParseResult *result) {
    reset_eager_loaded(result);
    result->pool.node_count = 0;
    result->error_count = 0;

    Parser p;
    p.tokens = tokens;
    p.token_count = token_count;
    p.pos = 0;
    p.pool = &result->pool;
    p.result = result;
    p.hoisted_count = 0;

    hoist_procedures(&p);

    const LogoToken *first = peek(&p);
    int program = ast_alloc(p.pool, AST_BLOCK, first->line, first->col);
    while (peek(&p)->type != LOGO_TOK_EOF) {
        if (peek(&p)->type == LOGO_TOK_ERROR) {
            report_error(&p, first->line, first->col, "%s", peek(&p)->text);
            break;
        }
        ast_append_child(p.pool, program, parse_statement(&p));
    }
    result->program = program;
}

// Shared setup for logo_parse_expr/logo_parse_condition: a fresh
// Parser over `tokens`, its own (reset) pool, no hoisted procedures
// (a template snippet never defines one) -- then hands off to
// whichever grammar entry point the caller wants.
static int parse_snippet(const LogoToken *tokens, int token_count, ParseResult *result, int (*entry)(Parser *)) {
    reset_eager_loaded(result); // a snippet never itself contains LOAD, but a reused ParseResult might carry this over from an earlier logo_parse call on the same object
    result->pool.node_count = 0;
    result->error_count = 0;

    Parser p;
    p.tokens = tokens;
    p.token_count = token_count;
    p.pos = 0;
    p.pool = &result->pool;
    p.result = result;
    p.hoisted_count = 0;

    int node = entry(&p);
    result->program = node;
    return result->error_count > 0 ? -1 : node;
}

int logo_parse_expr(const LogoToken *tokens, int token_count, ParseResult *result) {
    return parse_snippet(tokens, token_count, result, parse_expr_value);
}

int logo_parse_condition(const LogoToken *tokens, int token_count, ParseResult *result) {
    return parse_snippet(tokens, token_count, result, parse_condition);
}
