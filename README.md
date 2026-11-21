# here be snakes

![snakes](./logo.png)

snx is a Nix evaluator. You give it the same `.nix` files you already have and it gives you the same answers, only faster.

## What you get

**Same results as Nix.** We check snx against the official Nix conformance suite, which covers the language, parsing, derivations, flakes, NAR and the daemon wire protocol. At our last full run it passed 1405 of 1405 tests. Your expressions don't need changes and your flakes keep working.

**Less waiting.** On a full NixOS server configuration, with one worker, snx runs about 18% fewer instructions than the fastest evaluator we compared against. Wall time was about the same on a busy machine. On a quiet one, with huge memory pages switched on, it finished in 2.55 to 2.60 seconds against 2.76 to 2.82, so 7 to 9% faster. Small workloads (function calls, attribute sets, strings, updates) show the same pattern: fewer instructions on most of them, and we only keep a change if it doesn't make any benchmark slower.

**Honest limits.** snx doesn't evaluate in parallel yet, so with four workers the other evaluator still wins. It also uses more memory today: about 1.0 GB against 0.64 GB on the NixOS server case. Both are on our list.

## Where it's going

Errors you can read. Nix error traces are famously painful, so we want errors with a code, the exact line and column, a frame around the problem, a plain reason and a hint for the fix.

Parallel evaluation, so more workers means shorter waits.

Less memory, and a cache so files that never change don't get parsed and compiled again on every run.

## Numbers

These come from our own measurement notes (November 2026), taken on a shared machine, so treat wall times as rough. Instruction counts are the stable part.
