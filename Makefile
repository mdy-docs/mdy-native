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

CFLAGS  += -std=c11 -Wall -Wextra -O2 -g -I$(LAMASSU)/include -I$(QUICKJS)
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
	  $(CC) -std=c11 -O2 $(NIS_INC) $(NIS_RENAME) -c $$f -o build/nis/`basename $$f .c`.o || exit 1; \
	done
	$(AR) rcs $@ build/nis/*.o

# ---- the backend ----------------------------------------------------------

HOST_SRCS := src/host.c src/lam.c src/nis.c src/fsx.c src/oswin.c
HOST_HDRS := src/lam.h src/nis.h src/fsx.h src/oswin.h

build/mdy-native$(EXE): $(HOST_SRCS) $(HOST_HDRS) build/libquickjs.a build/libnisaba.a $(LAM_LIBS)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc $(NIS_INC) $(HOST_SRCS) \
	  build/libnisaba.a $(LAM_LIBS) build/libquickjs.a -o $@ $(LDLIBS)

# build/mdy.js is the bundle: mdy-docs through esbuild with the two engine
# imports aliased to shims/. See scripts-build.mjs.
build/mdy.js: entry.mjs scripts-build.mjs shims/lamassu.js shims/nisaba.js shims/fs.js
	node scripts-build.mjs

build/site.js: site-entry.mjs scripts-build.mjs shims/lamassu.js shims/nisaba.js shims/fs.js
	node scripts-build.mjs site

build/bench.js: bench-entry.mjs bench-body.mjs scripts-build.mjs shims/lamassu.js shims/nisaba.js
	node scripts-build.mjs bench

# `make site SITE=../../examples/docs-site OUT=/tmp/out` — the CLI's own build
# path, run natively.
SITE ?= fixture
OUT  ?= build/site-out

.PHONY: native site bench clean
native: build/mdy-native$(EXE) build/mdy.js
	@./build/mdy-native$(EXE) build/mdy.js

site: build/mdy-native$(EXE) build/site.js
	@./build/mdy-native$(EXE) build/site.js $(SITE) $(OUT)

bench: build/mdy-native$(EXE) build/bench.js
	@/usr/bin/time -l ./build/mdy-native$(EXE) build/bench.js 2>&1 | grep -E "native:|maximum resident"
	@/usr/bin/time -l node bench-node.mjs 2>&1 | grep -E "node:|maximum resident"

clean:
	rm -rf build
