# mdy-native — the backend as a binary: QuickJS for mdy-docs' own JavaScript,
# lamassu (and later nisaba) linked as C rather than loaded as WebAssembly.
# See ../../docs/desktop-plan.md.
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
