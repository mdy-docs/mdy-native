/*
 * One document, rendered — with no JavaScript engine but lamassu.
 *
 * This is mdy-docs' three passes (src/mdy.js's file comment) done in C:
 *
 *   1. the source is split into documents on bare `---`, and each into the
 *      YAML at its top and the body below it, with ```data fences pulled out
 *      of that body                                     — mdydoc.h, mdydata.h
 *   2. the body's `%` and `{{ }}` lines compile to one run of JavaScript
 *      statements, which run inside lamassu and return the lines the document
 *      produced                                         — mdyscript.h, lamassu
 *   3. those lines are parsed to hast, and that is the last change of
 *      representation there is                          — mdyast.h
 *
 * and then, because a page has to become a file, the tree is written as HTML
 * (mdyhtml.h).
 *
 * QuickJS is not involved at any point. lamassu runs the DOCUMENT's code,
 * which is what it has always been for; everything around it is C.
 *
 * WHAT THIS DOES NOT DO YET, and each is a step of its own:
 *   - `$` is present but every native refuses, loudly. A document that calls
 *     `$.render` gets an error naming it rather than a wrong page.
 *   - `transform` needs the tree to reach the guest and come back.
 *   - Positions point at the GENERATED lines rather than the source ones:
 *     mdy-docs carries a line map from the script layer into the parser, and
 *     this does not yet. It does not change any HTML, only where a warning
 *     would say it came from.
 */
#ifndef MDY_ENGINE_H
#define MDY_ENGINE_H

#include <stddef.h>

typedef struct mdy_engine mdy_engine;

mdy_engine *mdy_engine_new(void);
void mdy_engine_free(mdy_engine *engine);

/*
 * Render the document at `index` of `source` to HTML. Caller frees.
 *
 * NULL on failure, with a message written into `error` — a compile error from
 * lamassu, a refused native, or anything the document threw.
 */
char *mdy_engine_render(mdy_engine *engine, const char *source, size_t len,
                        size_t index, char *error, size_t error_len);

/* How many documents the source holds. */
size_t mdy_engine_count(mdy_engine *engine, const char *source, size_t len);

#endif
