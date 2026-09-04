# golden

What the node CLI produces for three sites, committed so CI can check the
native backend against it on a platform where node cannot run the build at all.

**Why not just run both in CI.** The node path needs `lamassu.wasm` and
`nisaba.wasm`, which are emscripten build products and are not in git — so on
a Windows runner there is nothing to compare against. Building them would mean
an emscripten toolchain on every platform to answer a question about C.

A committed reference is also the stronger check. It is fixed rather than
recomputed, so a change in output shows up as a diff in a pull request instead
of two sides moving together and agreeing.

These three are here because all of them are **deterministic** — no dates, no
ordering by anything but document order — which was verified before committing
them, not assumed. Do not add a site whose output moves.

Regenerate with `make golden` after a deliberate change to what mdy-docs
emits, and read the diff: that is the point of them.
