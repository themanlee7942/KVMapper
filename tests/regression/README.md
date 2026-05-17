# Regression archaeology

This folder is **not a test runner** — it's an index of subtle bugs we hit and the patterns that cause them. When you're tempted to "simplify" or "clean up" something in the hook DLL or shared-memory code, search this folder first.

Each note is structured the same way:

1. **One-line summary** — what breaks
2. **The broken pattern** — actual code (diff-style) that was wrong
3. **Failure scenario** — concrete user steps that reproduce
4. **The fix** — what was changed and why
5. **Stop sign** — "if you see this pattern, do NOT remove it without reading this note"

If you find a new subtle bug, add a note here before closing the PR. The whole point is that the next person who comes through can resurrect the context in under a minute.

## Current notes

| File | One-line | Severity |
|---|---|---|
| [v0.10_autorepeat_collision.md](v0.10_autorepeat_collision.md) | Removing `g_dispatching` thread-local breaks self-firing rules under user auto-repeat | 🔴 infinite loop possible |
| [v0.10_tick_boundary.md](v0.10_tick_boundary.md) | `(vk ^ ts)` slot indexing + tight time window misses verdicts across a GetTickCount tick | 🔴 sourceFilter silently no-ops |

---

> **About the README badge** — the "regression notes" counter on the main README shows the number of `.md` files in this folder (including this index). It grows by one each time we catalogue a new subtle bug. A non-zero number means "this project takes archaeology seriously"; the bigger it gets, the more thoroughly each prior pitfall is documented for the next person.
