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

These three are here because all of them are **deterministic**, and
`make check-determinism` is what proves it: build, `touch` every source, build
again, diff. A git checkout sets every file's mtime to checkout time, so a site
that renders one can never match a committed reference.

`examples/docs-site` is not here for exactly that reason — it emits
`formatDate(p.raw.mtime)`. It was in this directory briefly, on the strength of
a grep that returned nothing because the directory it searched did not exist.
That is why the check is a make target now rather than a habit.

`fixture-pkg` earns its place: it imports a *package*, so its layouts and JS
modules resolve against that package's directory rather than the site's. That
is the case Windows is most likely to get wrong, because `src/imports.js`
decides "inside the package" by string prefix on an absolute path — and a
drive letter or a backslash anywhere in that chain surfaces here as a wrong
answer rather than an error.

Regenerate with `make golden` after a deliberate change to what mdy-docs
emits, and read the diff: that is the point of them.
