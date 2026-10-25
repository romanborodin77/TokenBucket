# Ledger — docs/superpowers/plans/2026-10-25-generator-bulk-views.md
Spec fc70cbf approved by owner (AskUserQuestion). Plan 4a9e9ee: approval question unanswered → recommended option (native execution) per owner's standing mandate.
Task 1 ruling: editor persists generator options only to settings passed via setGeneratorSettings (MainWindow passes its preferences); default QSettings would touch the owner's real registry from tests.
Task 1 RED: test_ui passwordGeneratorOptionsApplyAndPersist failed on missing generatorLength (task-1-red.txt). Generator/preferences logic was written together with its tests and passed on first run — no separate RED recorded for them.
Task 1 GREEN: Release exit0; full CTest 23/23, 114.83s, 546 passed/0 failed/0 skipped. Existing generator test alphabet updated to the approved symbol set.
Task 2 RED: views not persisted (size 0), v4 root with views accepted, v4 file did not require consent (task-2-red.txt).
Task 2 ruling: out-of-range CBOR integers in views map to 100 and are rejected by viewError (avoids qBound overload ambiguity, no silent clamping). Old fixtures that used 5 as "future version" now use 6; textfile legacy fixture drops the v5-only views key.
Task 2 GREEN: Release exit0; full CTest 22/23 — test_e2e quickPasteFullRoute lost foreground to a foreign window (GetForegroundWindow != receiver); whole test_e2e 16/16 twice in isolation. Totals 548 passed.
Task 3 RED: saveCurrentView returned false; legacy save asked no consent (task-3-red.txt).
Task 3 ruling: first layout (label + list + button under library) collapsed the project tree to 24 px at 420x480 (screenshot). Views moved into the "Новая запись" header row as one combo + "⋯" action menu; tree back to 50 px narrow / 186 px wide, asserted by savedViewsFitNarrowWindow (light/dark, 420x480 and 900x600). Plan's "savedViews list + saveView button" became combo "savedViews" + tool button "viewActions".
Task 3 GREEN: Release exit0; full CTest 23/23, 120.64s, 556 passed/0 failed/0 skipped.
Task 4 RED: scaffold returned empty selection/false (task-4-red.txt).
Task 4 rulings: dialog wrappers capture UUIDs+generation before the dialog and recheck after; context menu keeps a multi-selection instead of resetting to the clicked row; Delete with several rows routes to bulk delete; F2 with several rows asks to pick one; env export disabled for multi-selection.
Task 4 GREEN: Release exit0; full CTest 23/23, 113.40s, 560 passed/0 failed/0 skipped.
Task 5: manual review (reviewer agent unavailable), wording fix for plural forms; final full CTest 22/23 (559 passed) — test_e2e lost foreground to the owner's active browser; package 69 files/ZIP hashes verified in canonical dist.
