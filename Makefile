# mdy-native — the backend as a binary: QuickJS for mdy-docs' own JavaScript,
# lamassu (and later nisaba) linked as C rather than loaded as WebAssembly.
# See ../../docs/desktop-plan.md.
NISABA  ?= ../../third_party/nisaba-db
QUICKJS ?= /usr/local/Cellar/quickjs/2026-06-04/
LAMASSU ?= ../../third_party/lamassu-js

CFLAGS  += -std=c11 -Wall -Wextra -O2 -g -I$(LAMASSU)/include -I$(QUICKJS)include/quickjs


# lamassu and QuickJS share the `js_` namespace, and not only in headers:
# both define js_dtoa. lamassu's is internal — it is not in lamassu.h — so the
# two archives are pre-linked into one object with that symbol made local.
# Each engine then resolves its own, which is what both expect.
build/lamassu.o: $(LAMASSU)/build/liblamassu_frontend.a $(LAMASSU)/build/liblamassu_runtime.a
	@mkdir -p build
	ld -r -arch x86_64 -o $@ -all_load $^ -unexported_symbol _js_dtoa

build/bridge: src/host.c src/lam.c src/lam.h build/lamassu.o
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc src/host.c src/lam.c build/lamassu.o -o $@ \
	  $(QUICKJS)lib/quickjs/libquickjs.a -lm -lpthread

.PHONY: run clean
run: build/bridge
	@./build/bridge
clean:
	rm -rf build

# ---- nisaba, built natively ----------------------------------------------
#
# All of nisaba's C compiles with cc and no changes — including the files named
# *_wasm.c, whose EMSCRIPTEN_KEEPALIVE is a no-op off-target. db_wasm.c is the
# one genuine exception: it is the WASM export layer, and a native host is what
# replaces it.
#
# Its storage is not: `bjio_host(fd)` reaches into Module.bjioHandles, a table
# of JS FileSystemSyncAccessHandle objects. bj_io is four callbacks, so a
# native host supplies its own (see src/nis_probe.c).
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

build/libnisaba.a: $(NIS_SRCS)
	@mkdir -p build/nis
	@for f in $(NIS_SRCS); do \
	  $(CC) -std=c11 -O2 $(NIS_INC) -c $$f -o build/nis/`basename $$f .c`.o || exit 1; \
	done
	ar rcs $@ build/nis/*.o

build/nis_probe: src/nis_probe.c build/libnisaba.a
	@mkdir -p build
	$(CC) -std=c11 -O2 -D_GNU_SOURCE $(NIS_INC) src/nis_probe.c build/libnisaba.a -o $@ -lm

.PHONY: nisaba
nisaba: build/nis_probe
	@./build/nis_probe

# ---- the backend ----------------------------------------------------------
#
# Everything at once: mdy-docs' own JavaScript in QuickJS, both engines linked
# as C beneath it. build/mdy.js is the bundle (`node scripts-build.mjs`), with
# the two engine packages aliased to shims/ that call the natives below.
build/mdy-native: src/host.c src/lam.c src/nis.c src/fsx.c src/lam.h src/nis.h src/fsx.h build/lamassu.o build/libnisaba.a
	@mkdir -p build
	$(CC) $(CFLAGS) -D_GNU_SOURCE -Isrc $(NIS_INC) \
	  src/host.c src/lam.c src/nis.c src/fsx.c build/lamassu.o build/libnisaba.a -o $@ \
	  $(QUICKJS)lib/quickjs/libquickjs.a -lm -lpthread

build/mdy.js: entry.mjs scripts-build.mjs shims/lamassu.js shims/nisaba.js shims/fs.js
	node scripts-build.mjs

build/site.js: site-entry.mjs scripts-build.mjs shims/lamassu.js shims/nisaba.js shims/fs.js
	node scripts-build.mjs site

build/bench.js: bench-entry.mjs bench-body.mjs scripts-build.mjs shims/lamassu.js shims/nisaba.js
	node scripts-build.mjs bench

# `make site SITE=../../examples/blog OUT=/tmp/blog` — the CLI's own build
# path, run natively.
SITE ?= fixture
OUT  ?= build/site-out

.PHONY: native bench site
native: build/mdy-native build/mdy.js
	@./build/mdy-native build/mdy.js

# The same 200-document set both ways. `node` is mdy-docs over the WASM
# engines; `native` is this. See README.md for what the numbers said.
site: build/mdy-native build/site.js
	@./build/mdy-native build/site.js $(SITE) $(OUT)

bench: build/mdy-native build/bench.js
	@/usr/bin/time -l ./build/mdy-native build/bench.js 2>&1 | grep -E "native:|maximum resident"
	@/usr/bin/time -l node bench-node.mjs 2>&1 | grep -E "node:|maximum resident"
