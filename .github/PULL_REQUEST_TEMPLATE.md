<!--
KVMapper PR template.

The boring checklist below isn't ceremony — every box exists because we
shipped a real bug that the box would have caught. Take 30 seconds.
-->

## Summary

<!-- One paragraph: what changes, why. -->

## Type

- [ ] Bug fix
- [ ] Feature
- [ ] Refactor / cleanup
- [ ] Docs / tests only

## Verification

- [ ] `python3 -m ziglang cc` builds all four PEs without warnings
- [ ] `python3 tests/verify_dist.py` passes (PE smoke test)
- [ ] `tests/native/run.sh` passes (unit + 10k fuzz)
- [ ] Tested on a real Windows machine (if touching DLL hot path or LL hooks)

## Regression archaeology

The [`tests/regression/`](../tree/main/tests/regression) folder catalogues subtle bugs we've already hit. **Before merging:**

- [ ] I have NOT removed any code marked with a "STOP SIGN" comment without reading the corresponding regression note
- [ ] If this PR introduces a subtle bug pattern (concurrency, timing, IPC), I have added a new regression note

Specific gotchas — confirm if your PR touches these areas:

- [ ] **hook.c / inject.c**: I did NOT remove `KV_TLS g_dispatching` ([why](../tree/main/tests/regression/v0.10_autorepeat_collision.md))
- [ ] **mapping_defs.h / shared_mem.c**: I did NOT shrink `KV_VERDICT_RING_SIZE` below 256 or `KV_VERDICT_WINDOW_MS` below ~25 ([why](../tree/main/tests/regression/v0.10_tick_boundary.md))
- [ ] **build.bat / *.bat**: file is still CRLF + pure ASCII (`file build.bat` reports "DOS batch file ... with CRLF line terminators")

## Notes for reviewer

<!-- Anything unusual, follow-ups, things you intentionally didn't do. -->
