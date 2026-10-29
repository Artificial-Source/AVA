---
name: ava-commit-messages
description: Write AVA git commit messages in the style the maintainers require. Use before every `git commit` in the AVA repository, and when rewording or reviewing AVA commit messages.
---

# Writing AVA Commit Messages

A commit message must explain the change so well that a reviewer who reads it
no longer feels the need to open the diff. Write it for a maintainer who knows
the codebase but has not seen this work.

## Structure

1. **Subject line.** Imperative mood, at most about 72 characters, no trailing
   period, no `type(scope):` prefix. Say what the commit does, not which files
   it touches: `Take one Session projection guard at a time to remove
   lock-order cycles`, not `Fix session.cpp`.
2. **Blank line.**
3. **The problem.** What was wrong or missing, and how it was noticed: a
   failing test, a CI run ID, a sanitizer report, a user report. Explain the
   mechanism when it is not obvious (for example, "a read guard that is a
   temporary inside an aggregate initializer stays locked until the whole
   expression completes").
4. **The change.** What the commit does about it. When it touches several
   sites, use a `- ` bullet per site that names the function or type and says
   what it did before and what it does now.
5. **Verification.** What was run and the result: test names or CTest
   filters, sanitizer presets, "full suite passes".

## Writing rules

- Wrap the body at 72 columns. Never write one long line per paragraph.
- Use normal English sentences. Real code names
  (`Session::create_detached_state`, `options_session_id_copy()`) and exact
  references (CI run IDs, issue numbers) are welcome; they make the message
  precise.
- Do not compress meaning into chains of abstract nouns. "Bulk-transfer
  removal liable to erase protocol authority" is unreadable; say who did what
  to which data and what broke.
- Prefer concrete words over invented labels. If a term is not a name in the
  code or a well-known technical term, explain it or drop it.
- Do not pad. No marketing words, no restating the subject line, no lists of
  every file changed.
- One logical change per commit. If the message needs two unrelated "problem"
  paragraphs, split the commit.

Pass the message through a file or heredoc (`git commit -F -`) so the line
breaks survive; do not build it from several `-m` flags.

## Good example

```text
Take one Session projection guard at a time to remove lock-order cycles

ThreadSanitizer reported lock-order inversions in the ACP peer test (CI
run 39869507564). A read guard that is a temporary inside an aggregate
initializer or argument list stays locked until the whole expression
completes, so these expressions nested several independent projection
mutexes:

- Session::create_detached_state built the aggregate with guard
  temporaries for agent selection, directories, model selection, stats
  and git branch all alive while the SessionStore copy locked the options
  wrapper. Each value is now copied in its own statement and moved into
  the aggregate.
- run_admitted_prompt took the agent-selection guard before the model
  guard in one place and after it in another. Both sites now take the
  guards in sequence without overlap.

The tsan preset (session_run_controller, subagent_coordinator,
subagent_delivery_manager, acp) passes clean; the full suite passes.
```

## Bad example

Unwrapped lines and condensed jargon; the reader cannot tell what changed:

```text
Document the retained frontend interaction contracts

The ordinary-state migration did not have a current reference for the interactive, history and service interfaces that must remain. That made bulk-transfer removal liable to erase protocol authority, ordering or synchronization along with obsolete display state.
```
