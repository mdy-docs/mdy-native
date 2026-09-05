/*
 * A document, as the database holds it.
 *
 * mdy-docs inserts a document's DATA — never its text. That data is the file's
 * identity merged with the YAML it declares:
 *
 *     {...frontMatter, ...dataFence1, ...dataFence2, path, name, ext, size, mtime}
 *
 * and nothing else; a measured build inserts 192 documents and 4.8 MB of it,
 * with no body anywhere. `_id` is a fresh ObjectId because nisaba's primary
 * tree is keyed on fixed-width OID bytes and will refuse anything else.
 *
 * This is the bridge from what the C front end read — mdy_yaml_node trees —
 * to the binjson bytes dc_insert_one takes. It is the last thing between a
 * parsed document and a collection that can be queried.
 */
#ifndef MDY_INGEST_H
#define MDY_INGEST_H

#include <stddef.h>
#include <stdint.h>

#include "binjson.h"
#include "mdyyaml.h"

/* One YAML value, as binjson. Sequences and mappings recurse. */
int mdy_bj_put_yaml(bj_builder *b, const mdy_yaml_node *node);

/*
 * A document: `_id` first, then every mapping merged in order.
 *
 * `Object.assign({}, ...parts)`: a key keeps the position of its FIRST
 * appearance and the value of its LAST, which is how a data fence overrides
 * front matter without moving it. A NULL mapping in the list is skipped, so a
 * caller need not compact its own array.
 */
int mdy_bj_document(bj_builder *b, const uint8_t oid[12],
                    const mdy_yaml_node *const *mappings, size_t count);

/* Twelve bytes, unique within this process: 4 of time, 5 of a per-run random
 * value, 3 of a counter — ObjectId's own shape, and enough for a key. */
void mdy_oid_next(uint8_t out[12]);

#endif
