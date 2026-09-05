# mdy-native — the backend as a binary: mdy-docs' own JavaScript in QuickJS,
# with lamassu and nisaba linked as C rather than loaded as WebAssembly.
# See ../../docs/desktop-plan.md.
#
#   make native            build both halves and run the checks
#   make site SITE=<dir>   `mdy build`, natively
#   make bench             the same document set, native and over WASM in node
#
# PORTABILITY. Everything here is plain C with no dependency on a system
# package: QuickJS is a submodule built from source, and lamassu and nisaba are
# submodules of the parent. The only files that know which operating system
# this is are src/fsx.c and src/nis.c, because both engines are platform-clean
# — lamassu has no #ifdefs at all and nisaba's I/O is behind its bj_io
# callbacks. See docs/desktop-plan.md, Phase 4.

NISABA  ?= ../../third_party/nisaba-db
LAMASSU ?= ../../third_party/lamassu-js
QUICKJS ?= third_party/quickjs

# gnu11, not c11. glibc hides POSIX declarations under a strict standard —
# strdup came back as an implicit declaration on Linux and st_mtim as an
# unknown field, both of which compile fine on macOS, which exposes them
# regardless. lamassu's own Makefile carries the same note and reaches for
# -D_POSIX_C_SOURCE; gnu11 gets the same result and also works under mingw.
CFLAGS  += -std=gnu11 -Wall -Wextra -O2 -g -I$(LAMASSU)/include -I$(QUICKJS)
LDLIBS  += -lm

# Windows has no libpthread of its own and mingw's winpthreads is not needed:
# nothing here starts a thread. Elsewhere QuickJS wants it.
ifeq ($(OS),Windows_NT)
  EXE := .exe
else
  LDLIBS += -lpthread
  EXE :=
endif

# ---- QuickJS --------------------------------------------------------------
#
# Built from its own sources rather than linked from a system package, so the
# build is identical on every platform and pinned to one commit. quickjs-libc
# is deliberately NOT here: it is the `std`/`os` module layer, and this host
# supplies its own natives (see src/host.c). Leaving it out also leaves out its
# POSIX assumptions, which is most of what would need porting.
QJS_SRCS := $(QUICKJS)/quickjs.c $(QUICKJS)/dtoa.c $(QUICKJS)/libregexp.c \
            $(QUICKJS)/libunicode.c $(QUICKJS)/cutils.c
QJS_OBJS := $(patsubst $(QUICKJS)/%.c,build/qjs/%.o,$(QJS_SRCS))

# gnu11, not c11: quickjs.c uses `asm volatile` in its spin hint, which strict
# C hides behind __asm__. `-w` rather than a clang-specific -Wno-everything —
# this has to compile under gcc and mingw too, and QuickJS is third-party code
# that does not build clean under our warning set.
build/qjs/%.o: $(QUICKJS)/%.c
	@mkdir -p build/qjs
	$(CC) -std=gnu11 -O2 -DCONFIG_VERSION='"mdy-native"' -w -c $< -o $@

build/libquickjs.a: $(QJS_OBJS)
	$(AR) rcs $@ $(QJS_OBJS)

# ---- lamassu --------------------------------------------------------------
#
# Its two archives link directly now. They used to need a pre-link pass
# (`ld -r -all_load -unexported_symbol _js_dtoa`) because both engines defined
# js_dtoa — ld64-only, which made macOS the one platform this could be done on
# at all. lamassu's is `static` as of 52f0bfd, and a symbol-table comparison
# says that was the only name the two archives had in common: 181 exports
# against 273, one overlap.
LAM_LIBS := $(LAMASSU)/build/liblamassu_runtime.a $(LAMASSU)/build/liblamassu_frontend.a

$(LAM_LIBS):
	$(MAKE) -C $(LAMASSU) libs

# ---- nisaba ---------------------------------------------------------------
#
# All of nisaba's C compiles with cc and no changes — including the files named
# *_wasm.c, whose EMSCRIPTEN_KEEPALIVE is a no-op off-target and one of which
# holds the regex entry points rather than mere exports. db_wasm.c is the one
# genuine exception: it is the WASM export layer, and a native host is what
# replaces it.
#
# Its storage is not portable either, and that is the seam working:
# `bjio_host(fd)` reaches into Module.bjioHandles, a table of JS
# FileSystemSyncAccessHandle objects. bj_io is four callbacks, so a native host
# supplies its own — see src/nis.c.
NIS_SRCS := \
  $(NISABA)/wasm/src/db_keyenc.c $(NISABA)/wasm/src/regex.c \
  $(NISABA)/wasm/src/db_query.c $(NISABA)/wasm/src/db_update.c $(NISABA)/wasm/src/db.c \
  $(NISABA)/third_party/binjson/src/binjson.c \
  $(NISABA)/third_party/binjson-structures/src/bjfile.c \
  $(NISABA)/third_party/binjson-structures/src/hostio.c \
  $(NISABA)/third_party/binjson-structures/src/bplustree.c \
  $(NISABA)/third_party/binjson-structures/src/geo.c \
  $(NISABA)/third_party/binjson-structures/src/rtree.c \
  $(NISABA)/third_party/binjson-structures/src/diff.c \
  $(NISABA)/third_party/binjson-structures/src/textlog.c \
  $(NISABA)/third_party/binjson-structures/src/stemmer.c \
  $(NISABA)/third_party/binjson-structures/src/textindex.c \
  $(NISABA)/third_party/regex-engine/src/regexp.c \
  $(NISABA)/third_party/regex-engine/src/regex_wasm.c

NIS_INC := -I$(NISABA)/wasm/include -I$(NISABA)/third_party/binjson/include \
           -I$(NISABA)/third_party/binjson-structures/include \
           -I$(NISABA)/third_party/regex-engine/include

# TWO REGEX ENGINES IN ONE BINARY, and they are not the same code.
#
# nisaba vendors mdy-docs/regex-engine; lamassu has moved to mdy-docs/baru-re,
# which is its successor — same ancestry, different version (baru-re 0.5.0 lets
# the embedder supply the allocator, among other things). Neither prefixes its
# symbols, so four names are defined by both: an exact-duplicate link error.
#
# This is worth understanding rather than papering over, because the previous
# build DID paper over it. It pre-linked lamassu into one relocatable object
# with `ld -r -all_load`, which loads every symbol unconditionally, and then
# offered nisaba as an archive — so nisaba's regexp.o was simply never pulled,
# and any call it made to one of these four resolved to LAMASSU's differently
# versioned implementation. Silent, and the wrong kind of wrong.
#
# Renaming nisaba's four at compile time keeps each engine's calls inside its
# own engine, needs no change to either upstream, and works on every compiler.
# It also fails LOUDLY if the overlap ever grows: a new shared name is a new
# duplicate-symbol error, not a new silent binding.
#
# The real fix is for nisaba to use baru-re too, so there is one regex engine
# in the binary instead of two. That is an API migration and it is noted in
# docs/desktop-plan.md rather than done here.
NIS_RENAME := -Dcompile_into=nis_re_compile_into -Dparse_alt=nis_re_parse_alt \
              -Dvm_execute_internal=nis_re_vm_execute_internal \
              -Dvm_get_indices=nis_re_vm_get_indices

build/libnisaba.a: $(NIS_SRCS)
	@mkdir -p build/nis
	@for f in $(NIS_SRCS); do \
	  $(CC) -std=gnu11 -O2 $(NIS_INC) $(NIS_RENAME) -c $$f -o build/nis/`basename $$f .c`.o || exit 1; \
	done
	$(AR) rcs $@ build/nis/*.o

# ---- the MDY front end, in C ----------------------------------------------
#
# github.com/mdy-docs/parse — the same parser mdy-docs has in JavaScript,
# producing the same tree (87/87 documents of the reference corpus byte for
# byte). It is here because the front end is where a native build's time goes:
# a profile put every frame in the JavaScript layer, and this is the largest
# single thing in it.
PARSE     := third_party/parse
PARSE_LIB := $(PARSE)/build/libmdyast.a
PARSE_INC := -I$(PARSE)/include -I$(PARSE)/src

$(PARSE_LIB):
	$(MAKE) -C $(PARSE) build/libmdyast.a

# ---- the backend ----------------------------------------------------------

HOST_SRCS := src/host.c src/lam.c src/nis.c src/fsx.c src/oswin.c src/parse.c
HOST_HDRS := src/lam.h src/nis.h src/fsx.h src/oswin.h

build/mdy-native$(EXE): $(HOST_SRCS) $(HOST_HDRS) build/libquickjs.a build/libnisaba.a $(LAM_LIBS) $(PARSE_LIB)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc $(NIS_INC) $(PARSE_INC) $(HOST_SRCS) \
	  build/libnisaba.a $(LAM_LIBS) $(PARSE_LIB) build/libquickjs.a -o $@ $(LDLIBS)

# build/mdy.js is the bundle: mdy-docs through esbuild with the two engine
# imports aliased to shims/. See scripts-build.mjs.
# SHIMS is every shim, not the three that were listed: shims/parse.js was
# added later and left off, so a bundle built after editing it silently kept
# the old one — which then called the bridge with the old signature and handed
# hast the bridge's whole result as if it were a tree.
SHIMS := $(wildcard shims/*.js) $(wildcard shims/node/*.js)

build/mdy.js: entry.mjs scripts-build.mjs $(SHIMS)
	node scripts-build.mjs

build/site.js: site-entry.mjs scripts-build.mjs $(SHIMS)
	node scripts-build.mjs site

# mdy-docs' own suite, bundled to run against this backend. The test files are
# imported in place from ../../test — not copied — so they cannot drift.
build/tests.js: tests-entry.mjs scripts-build.mjs $(SHIMS) $(wildcard ../../test/*.js)
	node scripts-build.mjs tests

build/bench.js: bench-entry.mjs bench-body.mjs scripts-build.mjs $(SHIMS)
	node scripts-build.mjs bench

# `make site SITE=../../examples/docs-site OUT=/tmp/out` — the CLI's own build
# path, run natively.
SITE ?= fixture
OUT  ?= build/site-out

# ---- the golden outputs ---------------------------------------------------
#
# What the node CLI produces for three deterministic sites, committed so CI can
# check the native backend byte-for-byte on a platform where node cannot run
# the build at all (it needs the WASM engines, which are emscripten build
# products and are not in git). See golden/README.md.
# DETERMINISTIC SITES ONLY, and that is checked rather than assumed — build,
# touch every source, build again, diff. examples/docs-site is NOT here for
# exactly that reason: it renders a source file's mtime, and a git checkout
# sets mtimes to checkout time, so its output can never match a committed
# reference. `make check-determinism` is the test.
#
# fixture-pkg earns its place: it imports a PACKAGE, so its layouts and JS
# modules resolve against that package's directory rather than the site's.
# That is the case Windows is most likely to get wrong, because imports.js
# decides "inside the package" by string prefix on an absolute path.
GOLDEN_SITES := fixture fixture-pkg ../../examples/messaging

.PHONY: golden check-golden check-determinism
golden:
	@rm -rf golden/fixture golden/fixture-pkg golden/messaging
	@for d in $(GOLDEN_SITES); do \
	  node ../../bin/mdy.js build $$d --out golden/`basename $$d` > /dev/null || exit 1; \
	done
	@echo "regenerated golden/ — read the diff before committing it"

# A golden site whose output moves is worse than no golden site: it goes red
# for a reason that is not a regression. This proves each one is stable across
# the thing a git checkout actually changes — every file's mtime.
check-determinism: build/mdy-native$(EXE) build/site.js
	@bin=./build/mdy-native$(EXE); fail=0; \
	for d in $(GOLDEN_SITES); do \
	  n=`basename $$d`; \
	  rm -rf build/det-$$n-a build/det-$$n-b; \
	  $$bin build/site.js $$d build/det-$$n-a > /dev/null || exit 1; \
	  find $$d -type f -exec touch {} \; ; \
	  $$bin build/site.js $$d build/det-$$n-b > /dev/null || exit 1; \
	  if diff -r build/det-$$n-a build/det-$$n-b > /dev/null; then \
	    echo "  $$n: stable across an mtime change"; \
	  else \
	    echo "  $$n: OUTPUT MOVES — it cannot be a golden site"; fail=1; \
	  fi; \
	done; \
	exit $$fail

check-golden: build/mdy-native$(EXE) build/site.js
	@bin=./build/mdy-native$(EXE); fail=0; \
	for d in $(GOLDEN_SITES); do \
	  n=`basename $$d`; \
	  rm -rf build/check-$$n; \
	  $$bin build/site.js $$d build/check-$$n > /dev/null || exit 1; \
	  if diff -r golden/$$n build/check-$$n > /dev/null; then \
	    echo "  $$n: identical to golden"; \
	  else \
	    echo "  $$n: DIFFERS from golden"; diff -r golden/$$n build/check-$$n | head -20; fail=1; \
	  fi; \
	done; \
	exit $$fail

.PHONY: build native site bench test test-c-parser check-ingest check-engine clean

# A document from text into a nisaba collection, with no JavaScript in it:
# data fences, YAML, binjson, dc_insert_one, and a query back out. This is the
# ingest mdy.js does in JS — `collection.insertOne({ ...doc.data })` — done in
# C, and it is the seam the engine will be built on.
build/ingest-test$(EXE): test/ingest.c src/ingest.c src/nis.c $(NIS_SRCS) $(PARSE_LIB)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc $(NIS_INC) $(PARSE_INC) $(NIS_RENAME) \
	  test/ingest.c src/ingest.c src/nis.c $(NIS_SRCS) $(PARSE_LIB) -o $@ $(LDFLAGS)

check-ingest: build/ingest-test$(EXE)
	@./build/ingest-test$(EXE)

# One document, end to end, with no JavaScript engine but lamassu — the three
# passes of src/mdy.js done in C. QuickJS is not linked into this binary.
build/engine-test$(EXE): test/engine.c src/engine.c src/ingest.c src/nis.c src/fsx.c $(NIS_SRCS) $(LAM_LIBS) $(PARSE_LIB)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc $(NIS_INC) $(PARSE_INC) $(NIS_RENAME) \
	  test/engine.c src/engine.c src/ingest.c src/nis.c src/fsx.c $(NIS_SRCS) \
	  $(PARSE_LIB) $(LAM_LIBS) -o $@ $(LDLIBS)

# The same driver under AddressSanitizer. A use-after-free in the boundary
# between the tree, the document store and the VM is invisible without it:
# a freed key cell is silently reused and a property becomes a different one.
build/mdy-build-asan$(EXE): src/build_main.c src/engine.c src/ingest.c src/nis.c src/fsx.c $(NIS_SRCS) $(LAM_LIBS) $(PARSE_LIB)
	@mkdir -p build
	$(CC) -std=gnu11 -Wall -g -O1 -fsanitize=address -fno-omit-frame-pointer \
	  -Isrc $(NIS_INC) $(PARSE_INC) $(NIS_RENAME) -I$(LAMASSU)/include \
	  src/build_main.c src/engine.c src/ingest.c src/nis.c src/fsx.c $(NIS_SRCS) \
	  $(PARSE_LIB) $(LAM_LIBS) -o $@ $(LDLIBS)

build/mdy-build$(EXE): src/build_main.c src/engine.c src/ingest.c src/nis.c src/fsx.c $(NIS_SRCS) $(LAM_LIBS) $(PARSE_LIB)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc $(NIS_INC) $(PARSE_INC) $(NIS_RENAME) \
	  src/build_main.c src/engine.c src/ingest.c src/nis.c src/fsx.c $(NIS_SRCS) \
	  $(PARSE_LIB) $(LAM_LIBS) -o $@ $(LDLIBS)

# Twice: once normally, once collecting at EVERY safe point.
#
# The stress pass is not belt-and-braces. This engine hands the VM values it
# has just built, and one reachable only from the C stack is invisible to the
# collector; freeing it does not crash, it makes a property silently become a
# different property. Under a normal collector that needs a run long enough to
# collect at the wrong moment — it was found by diffing a 93-page site, where
# one link in nine hundred pointed at the wrong page. Under stress the same
# fault shows up in the fourth check.
check-engine: build/engine-test$(EXE)
	@./build/engine-test$(EXE)
	@echo "-- again, collecting at every safe point"
	@MDY_GC_STRESS=1 ./build/engine-test$(EXE)

# A whole site built both ways and diffed. SITE names the directory; there is
# no default, because the corpus that matters is whichever one you have.
#
#   make check-site SITE=../../../../site
check-site: build/mdy-build$(EXE)
	@node scripts-compare-site.mjs "$(SITE)"
# The 713 of mdy-docs' 776 tests that a runtime with no subprocesses, no HTTP
# server and no WebAssembly can run. See tests-entry.mjs for what is left out
# and why each one is a property of the runtime rather than a gap in the port.
test: build/mdy-native$(EXE) build/tests.js
	@./build/mdy-native$(EXE) build/tests.js
# `make build` rather than `make build/mdy-native`: the target's name carries
# .exe on Windows, and a caller should not have to know that.
# The same suite against the C front end, which is a SUBSET of what mdy-docs
# documents — it renders the reference corpus byte-for-byte and does not yet
# implement `#` comments, table captions or the `script` option. This prints
# what is missing as a number so it can be watched going down; it is not a
# gate, because the failures here are known and listed in README.md.
test-c-parser: build/mdy-native$(EXE)
	@MDY_PARSER=c node scripts-build.mjs tests
	@./build/mdy-native$(EXE) build/tests.js > build/c-parser.log 2>&1 || true
	@grep -c '^FAIL ' build/c-parser.log | sed 's/^/failing with the C front end: /'
	@grep -o 'cannot honour `[a-zA-Z]*`' build/c-parser.log | sort | uniq -c | sort -rn | sed 's/^/  /'
	@node scripts-build.mjs tests   # leave build/tests.js as `make test` expects it

build: build/mdy-native$(EXE)

native: build/mdy-native$(EXE) build/mdy.js
	@./build/mdy-native$(EXE) build/mdy.js

site: build/mdy-native$(EXE) build/site.js
	@./build/mdy-native$(EXE) build/site.js $(SITE) $(OUT)

bench: build/mdy-native$(EXE) build/bench.js
	@/usr/bin/time -l ./build/mdy-native$(EXE) build/bench.js 2>&1 | grep -E "native:|maximum resident"
	@/usr/bin/time -l node bench-node.mjs 2>&1 | grep -E "node:|maximum resident"

clean:
	rm -rf build
