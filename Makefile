# mdy-native — the mdy engine as a binary. No JavaScript engine but lamassu,
# which runs the templates; the walk, the document store, composition and the
# output are C. See ../../docs/desktop-plan.md.
#
#   make build/mdy-build          the engine, as a command
#   make check-engine             its unit checks, twice (see the target)
#   make check-sites              every site here, built BOTH ways and diffed
#   make check-site SITE=<dir>    one site, the same way
#
# The suite is check-sites: mdy-docs' own JavaScript against this engine over
# real input. It is node that drives it, and node that says what the answer
# should be — there is no second implementation of the expectations to drift.
#
# PORTABILITY. Everything here is plain C with no dependency on a system
# package: lamassu, nisaba and the front end are submodules. The only files that know which operating system
# this is are src/fsx.c and src/nis.c, because both engines are platform-clean
# — lamassu has no #ifdefs at all and nisaba's I/O is behind its bj_io
# callbacks. See docs/desktop-plan.md, Phase 4.

# Submodules of this repository, so the engine and its checks build from a
# clean clone. Inside an mdy-docs checkout they are the same two checkouts one
# level up, and pointing these at those avoids a second copy:
#     make NISABA=../../third_party/nisaba-db LAMASSU=../../third_party/lamassu-js
NISABA  ?= third_party/nisaba-db
LAMASSU ?= third_party/lamassu-js

# gnu11, not c11. glibc hides POSIX declarations under a strict standard —
# strdup came back as an implicit declaration on Linux and st_mtim as an
# unknown field, both of which compile fine on macOS, which exposes them
# regardless. lamassu's own Makefile carries the same note and reaches for
# -D_POSIX_C_SOURCE; gnu11 gets the same result and also works under mingw.
CFLAGS  += -std=gnu11 -Wall -Wextra -O2 -g -I$(LAMASSU)/include
LDLIBS  += -lm

# Windows has no libpthread of its own and mingw's winpthreads is not needed:
# nothing here starts a thread.
ifeq ($(OS),Windows_NT)
  EXE := .exe
else
  LDLIBS += -lpthread
  EXE :=
endif

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

# stb: three single-file headers, vendored. See third_party/stb/README.md.
STB_INC   := -Ithird_party/stb

$(PARSE_LIB):
	$(MAKE) -C $(PARSE) build/libmdyast.a


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
check-determinism: build/mdy-build$(EXE)
	@bin=./build/mdy-build$(EXE); fail=0; \
	for d in $(GOLDEN_SITES); do \
	  n=`basename $$d`; \
	  rm -rf build/det-$$n-a build/det-$$n-b; \
	  $$bin $$d --out build/det-$$n-a --quiet || exit 1; \
	  find $$d -type f -exec touch {} \; ; \
	  $$bin $$d --out build/det-$$n-b --quiet || exit 1; \
	  if diff -r build/det-$$n-a build/det-$$n-b > /dev/null; then \
	    echo "  $$n: stable across an mtime change"; \
	  else \
	    echo "  $$n: OUTPUT MOVES — it cannot be a golden site"; fail=1; \
	  fi; \
	done; \
	exit $$fail

check-golden: build/mdy-build$(EXE)
	@bin=./build/mdy-build$(EXE); fail=0; \
	for d in $(GOLDEN_SITES); do \
	  n=`basename $$d`; \
	  rm -rf build/check-$$n; \
	  $$bin $$d --out build/check-$$n --quiet || exit 1; \
	  if diff -r golden/$$n build/check-$$n > /dev/null; then \
	    echo "  $$n: identical to golden"; \
	  else \
	    echo "  $$n: DIFFERS from golden"; diff -r golden/$$n build/check-$$n | head -20; fail=1; \
	  fi; \
	done; \
	exit $$fail

.PHONY: check-ingest check-engine check-site check-sites clean

# A document from text into a nisaba collection, with no JavaScript in it:
# data fences, YAML, binjson, dc_insert_one, and a query back out. This is the
# ingest mdy.js does in JS — `collection.insertOne({ ...doc.data })` — done in
# C, and it is the seam the engine will be built on.
build/ingest-test$(EXE): test/ingest.c src/ingest.c src/nis.c $(NIS_SRCS) $(PARSE_LIB)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc $(NIS_INC) $(PARSE_INC) $(STB_INC) $(NIS_RENAME) \
	  test/ingest.c src/ingest.c src/nis.c $(NIS_SRCS) $(PARSE_LIB) -o $@ $(LDFLAGS)

check-ingest: build/ingest-test$(EXE)
	@./build/ingest-test$(EXE)

# One document, end to end, with no JavaScript engine but lamassu — the three
# passes of src/mdy.js done in C. No JavaScript engine but lamassu is linked
# into this binary.
build/engine-test$(EXE): test/engine.c src/engine.c src/ingest.c src/nis.c src/fsx.c src/images.c $(NIS_SRCS) $(LAM_LIBS) $(PARSE_LIB)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc $(NIS_INC) $(PARSE_INC) $(STB_INC) $(NIS_RENAME) \
	  test/engine.c src/engine.c src/ingest.c src/nis.c src/fsx.c src/images.c $(NIS_SRCS) \
	  $(PARSE_LIB) $(LAM_LIBS) -o $@ $(LDLIBS)

# The same driver under AddressSanitizer. A use-after-free in the boundary
# between the tree, the document store and the VM is invisible without it:
# a freed key cell is silently reused and a property becomes a different one.
build/mdy-build-asan$(EXE): src/build_main.c src/engine.c src/ingest.c src/nis.c src/fsx.c src/images.c $(NIS_SRCS) $(LAM_LIBS) $(PARSE_LIB)
	@mkdir -p build
	$(CC) -std=gnu11 -Wall -g -O1 -fsanitize=address -fno-omit-frame-pointer \
	  -Isrc $(NIS_INC) $(PARSE_INC) $(STB_INC) $(NIS_RENAME) -I$(LAMASSU)/include \
	  src/build_main.c src/engine.c src/ingest.c src/nis.c src/fsx.c src/images.c $(NIS_SRCS) \
	  $(PARSE_LIB) $(LAM_LIBS) -o $@ $(LDLIBS)

build/mdy-build$(EXE): src/build_main.c src/engine.c src/ingest.c src/nis.c src/fsx.c src/images.c $(NIS_SRCS) $(LAM_LIBS) $(PARSE_LIB)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc $(NIS_INC) $(PARSE_INC) $(STB_INC) $(NIS_RENAME) \
	  src/build_main.c src/engine.c src/ingest.c src/nis.c src/fsx.c src/images.c $(NIS_SRCS) \
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

# THE SUITE. Every site in this repository, built both ways and diffed.
#
# It replaced running mdy-docs' JavaScript test files inside a second engine.
# That proved "mdy-docs runs unchanged on the native backend" — a claim about
# a binary that no longer exists. This proves the thing that matters now: the
# C engine and mdy-docs agree, on real input, byte for byte. Pointing it at
# these five found eight bugs in an engine that already built a 93-page site
# identically, and not one of them failed a unit test.
#
# Two sites differ on purpose, by an EXACT amount, and the check fails if
# either count moves in either direction:
#
#   blog       1: its search index tokenizes its own `$.text` output, which
#                 contains a composition TOKEN — and a token's id depends on
#                 how many renders came before it. mdy-docs memoises a render
#                 on (document, request); this engine does not, so it holds
#                 more trees and the ids run ahead.
#   docs-site  2: its pages emit raw HTML for the markdown front end to stitch
#                 back, which is rehype-raw's HTML5 round trip. md4c does not
#                 do it, and the difference is blank lines around the block.
CHECK_SITES := ../../examples/blog:1 ../../examples/docs-site:2 \
               ../../examples/messaging:0 fixture:0 fixture-pkg:0
check-sites: build/mdy-build$(EXE)
	@fail=0; for s in $(CHECK_SITES); do \
	  dir=$${s%:*}; want=$${s##*:}; \
	  printf '%-34s ' "$$dir"; \
	  node scripts-compare-site.mjs "$$dir" --expect "$$want" || fail=1; \
	done; exit $$fail





clean:
	rm -rf build
