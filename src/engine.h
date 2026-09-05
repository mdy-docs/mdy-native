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
 * OPEN a source as a document set, then render from it.
 *
 * Opening is where the set becomes queryable: every document's data — its
 * front matter merged with its ```data fences — is inserted into a nisaba
 * collection, and `$.find` and `$.findOne` run real queries against it. That
 * is mdy-docs' `openDocumentSet`, and it is why rendering takes an index into
 * an open set rather than a source and an offset.
 *
 * Results come back in DOCUMENT order, not the order the database returns
 * them: an `_id` to index map restores it, which is what makes a query's
 * answer the same on every build.
 */
int mdy_engine_open(mdy_engine *engine, const char *source, size_t len,
                    char *error, size_t error_len);

/*
 * Open a DIRECTORY as a document set — mdy-docs' `walkSources`, where every
 * file becomes a document and what its own file format means is applied:
 * `.mdy` is a template, `.md` is text that is never compiled, `.yaml` is a
 * data record, and anything else is its identity alone. `dist/`,
 * `node_modules/` and dotfiles are not sources.
 */
int mdy_engine_open_dir(mdy_engine *engine, const char *root,
                        char *error, size_t error_len);

/*
 * Every directory in the import graph, in post-order — a package comes after
 * everything it imports. `static/` is copied in this order so a site's own
 * files win over the ones its theme ships.
 */
size_t mdy_engine_root_count(mdy_engine *engine);
const char *mdy_engine_root_at(mdy_engine *engine, size_t index);

/* The document whose `path` is `entry`, or -1 — `main.mdy` is where a
 * directory starts unless a caller says otherwise. */
int mdy_engine_entry(mdy_engine *engine, const char *entry);

/*
 * Where a `$.publish` goes. `$.emit` writes an output; publish sends a
 * message to a page of this set BY NAME, and what happens to it is the
 * embedder's — exactly as an emitted output is. Core's whole job is deciding
 * that the name means a page, and refusing when it names none or several.
 *
 * `data_json` is the message's data as JSON, produced by the guest's own
 * JSON.stringify. `doc_index` is the document that published it.
 */
void mdy_engine_on_publish(mdy_engine *engine,
                           void (*fn)(void *ud, const char *name,
                                      const char *data_json, size_t doc_index),
                           void *ud);

/* How many documents the open set holds. */
size_t mdy_engine_count(mdy_engine *engine);

/*
 * Where a document's `$.emit(path, content)` goes.
 *
 * mdy has no opinion on what producing an output means — a build writes a
 * file, a server holds it in memory, a test collects it. Tokens in `content`
 * have already become the HTML they stood for, because a file is a string and
 * that is the shape it can hold.
 *
 * Without one, `$.emit` is a harmless no-op, which is also what mdy-docs does.
 */
void mdy_engine_on_emit(mdy_engine *engine,
                        void (*fn)(void *ud, const char *path, const char *content),
                        void *ud);

/*
 * Render the document at `index` to HTML. Caller frees.
 *
 * NULL on failure, with a message written into `error` — a compile error from
 * lamassu, a refused native, or anything the document threw.
 */
char *mdy_engine_render(mdy_engine *engine, size_t index,
                        char *error, size_t error_len);

#endif
