/* The contract, and what is not here yet, is in engine.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lamassu.h"
#include "lamassu_compile.h"

#include "engine.h"
#include "mdyast.h"
#include "mdydata.h"
#include "mdydoc.h"
#include "mdyhtml.h"
#include "binjson.h"
#include "bplustree.h"
#include "db.h"
#include "ingest.h"

#include "mdybuild.h"
#include "mdyscript.h"
#include "toolkit.h"

/* A tree a `$.render` parked, and the id of the token standing for it. */
typedef struct { char id[24]; mdy_doc *doc; mdy_node *tree; } Held;

/* One document of an open set. Its TEXT is here; its DATA is in nisaba. */
typedef struct {
    mdy_chunk chunk;      /* the document's own text */
    mdy_chunk matter;     /* its front matter, unparsed */
    mdy_data *fences;     /* its ```data fences, and the body without them */
    uint8_t oid[12];
} Document;

struct mdy_engine {
    JsVm *vm;
    JsContext *ctx;
    /*
     * The render in progress. `$.compose` is a host call made from the middle
     * of one, and it needs the document being rendered; with one render at a
     * time this is where it lives.
     */
    mdy_doc *tree_owner;

    /* The open set. */
    Document *docs;
    size_t count;
    int handle;                 /* nisaba's, from nis_open */
    mdy_documents *source_docs;

    /* Trees a `$.render` parked, and the tokens standing for them. */
    Held *held;
    size_t held_count, held_cap;
    size_t next_token;
    int depth;                  /* renders inside renders */
    void (*on_emit)(void *ud, const char *path, const char *content);
    void *on_emit_ud;
    /* `_id` to index, in insertion order, so a hit maps back to its document. */
    uint8_t (*ids)[12];
};

/* ---- strings across the boundary ------------------------------------------- */

static uint16_t *to_utf16(const char *in, size_t len, size_t *out_len) {
    uint16_t *out = malloc((len + 1) * sizeof *out);
    if (!out) { *out_len = 0; return NULL; }
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)in[i];
        unsigned cp;
        size_t w;
        if (c < 0x80) { cp = c; w = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; w = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; w = 3; }
        else { cp = c & 0x07; w = 4; }
        if (i + w > len) { cp = 0xFFFD; w = 1; }
        else for (size_t k = 1; k < w; k++) cp = (cp << 6) | ((unsigned char)in[i + k] & 0x3F);
        i += w;
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            out[o++] = (uint16_t)(0xD800 + (cp >> 10));
            out[o++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else out[o++] = (uint16_t)cp;
    }
    *out_len = o;
    return out;
}

static char *from_utf16(const uint16_t *u, size_t len) {
    char *out = malloc(len * 4 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned cp = u[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[++i] - 0xDC00);
        if (cp < 0x80) out[o++] = (char)cp;
        else if (cp < 0x800) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        else { out[o++] = (char)(0xF0 | (cp >> 18)); out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
    }
    out[o] = '\0';
    return out;
}

static JsValue str(JsVm *vm, const char *s, size_t len) {
    size_t n = 0;
    uint16_t *u = to_utf16(s, len, &n);
    JsValue v = u ? js_string_new(vm, u, n) : js_undefined();
    free(u);
    return v;
}

static JsValue key(JsVm *vm, const char *s) {
    size_t n = 0;
    uint16_t *u = to_utf16(s, strlen(s), &n);
    JsValue v = u ? js_atom(vm, u, n) : js_undefined();
    free(u);
    return v;
}

/* ---- a tree, across the boundary --------------------------------------------
 *
 * `transform((tree) => …)` is the one place a document's own code sees the
 * tree, so the tree has to reach the guest and come back. mdy-docs sends it as
 * JSON in both directions; here it is built as VALUES, which is what
 * js_array_new and js_object_new made possible.
 *
 * The shape is hast's, exactly as the JSON was: every node has a `type`, an
 * element has `tagName`, `properties` and `children`, and a text node has a
 * `value`. A transform written against one works against the other.
 */

static JsValue tree_to_js(mdy_engine *e, const mdy_node *n);

static JsValue children_to_js(mdy_engine *e, const mdy_node *n) {
    JsValue array = js_array_new(e->ctx, 0);
    js_gc_protect(e->vm, &array);
    for (const mdy_node *c = n->first; c; c = c->next) {
        JsValue child = tree_to_js(e, c);
        js_gc_protect(e->vm, &child);
        js_array_push(e->vm, array, child);
        js_gc_unprotect(e->vm, &child);
    }
    js_gc_unprotect(e->vm, &array);
    return array;
}

static JsValue tree_to_js(mdy_engine *e, const mdy_node *n) {
    JsValue o = js_object_new(e->ctx);
    js_gc_protect(e->vm, &o);

    switch (n->type) {
        case MDY_TEXT:
        case MDY_RAW:
        case MDY_COMMENT: {
            const char *type = n->type == MDY_TEXT ? "text"
                             : n->type == MDY_RAW ? "raw" : "comment";
            js_object_set(e->vm, o, key(e->vm, "type"), str(e->vm, type, strlen(type)));
            const char *v = n->text ? n->text : "";
            js_object_set(e->vm, o, key(e->vm, "value"), str(e->vm, v, strlen(v)));
            break;
        }
        case MDY_DOCTYPE:
            js_object_set(e->vm, o, key(e->vm, "type"), str(e->vm, "doctype", 7));
            break;
        case MDY_ROOT:
            js_object_set(e->vm, o, key(e->vm, "type"), str(e->vm, "root", 4));
            js_object_set(e->vm, o, key(e->vm, "children"), children_to_js(e, n));
            break;
        case MDY_ELEMENT: {
            js_object_set(e->vm, o, key(e->vm, "type"), str(e->vm, "element", 7));
            js_object_set(e->vm, o, key(e->vm, "tagName"), str(e->vm, n->tag, strlen(n->tag)));
            JsValue props = js_object_new(e->ctx);
            js_gc_protect(e->vm, &props);
            for (const mdy_prop *p = n->props; p; p = p->next) {
                JsValue v;
                switch (p->type) {
                    case MDY_PROP_STRING: v = str(e->vm, p->as.string, strlen(p->as.string)); break;
                    case MDY_PROP_NUMBER: v = js_number(p->as.number); break;
                    case MDY_PROP_BOOL:   v = js_bool(p->as.boolean != 0); break;
                    case MDY_PROP_LIST: {
                        v = js_array_new(e->ctx, (uint32_t)p->list_len);
                        js_gc_protect(e->vm, &v);
                        for (size_t i = 0; i < p->list_len; i++)
                            js_array_push(e->vm, v, str(e->vm, p->list[i], strlen(p->list[i])));
                        js_gc_unprotect(e->vm, &v);
                        break;
                    }
                    default: v = js_undefined();
                }
                js_object_set(e->vm, props, key(e->vm, p->name), v);
            }
            js_object_set(e->vm, o, key(e->vm, "properties"), props);
            js_gc_unprotect(e->vm, &props);
            js_object_set(e->vm, o, key(e->vm, "children"), children_to_js(e, n));
            break;
        }
    }

    js_gc_unprotect(e->vm, &o);
    return o;
}

/* The string a JS value holds, as UTF-8. Caller frees. NULL when it is not a
 * string — which for a `type` or a `tagName` means the guest handed back
 * something that is not a node. */
static char *js_string_utf8(JsValue v) {
    size_t ulen = 0;
    const uint16_t *u = js_string_units(v, &ulen);
    return u ? from_utf16(u, ulen) : NULL;
}

static mdy_node *js_to_tree(mdy_engine *e, mdy_doc *doc, JsValue v);

static void js_children_to_tree(mdy_engine *e, mdy_doc *doc, mdy_node *parent, JsValue kids) {
    if (!js_is_array(kids)) return;
    uint32_t n = js_array_length(kids);
    for (uint32_t i = 0; i < n; i++) {
        mdy_node *child = js_to_tree(e, doc, js_array_get(kids, i));
        if (child) mdy_append(parent, child);
    }
}

static mdy_node *js_to_tree(mdy_engine *e, mdy_doc *doc, JsValue v) {
    if (!js_is_object(v)) return NULL;

    char *type = js_string_utf8(js_object_get(e->vm, v, key(e->vm, "type")));
    if (!type) return NULL;

    mdy_node *out = NULL;
    if (strcmp(type, "text") == 0 || strcmp(type, "raw") == 0 || strcmp(type, "comment") == 0) {
        char *value = js_string_utf8(js_object_get(e->vm, v, key(e->vm, "value")));
        out = mdy_new_text(doc, value ? value : "", value ? strlen(value) : 0);
        if (out) out->type = strcmp(type, "raw") == 0 ? MDY_RAW
                           : strcmp(type, "comment") == 0 ? MDY_COMMENT : MDY_TEXT;
        free(value);
    } else if (strcmp(type, "doctype") == 0) {
        out = mdy_new_text(doc, "", 0);
        if (out) out->type = MDY_DOCTYPE;
    } else if (strcmp(type, "root") == 0) {
        out = mdy_new_text(doc, "", 0);
        if (out) {
            out->type = MDY_ROOT;
            out->text = NULL;
            js_children_to_tree(e, doc, out, js_object_get(e->vm, v, key(e->vm, "children")));
        }
    } else if (strcmp(type, "element") == 0) {
        char *tag = js_string_utf8(js_object_get(e->vm, v, key(e->vm, "tagName")));
        out = mdy_new_element(doc, tag ? tag : "div", tag ? strlen(tag) : 3);
        free(tag);
        if (out) {
            /*
             * Properties come back by NAME, and the names a guest may have
             * added are not known in advance — so this walks whatever is
             * there rather than a fixed list. `className` is the one that is
             * an array; everything else is a string, a number or a boolean.
             */
            JsValue props = js_object_get(e->vm, v, key(e->vm, "properties"));
            if (js_is_object(props)) {
                for (size_t i = 0; i < js_object_size(props); i++) {
                    JsValue name = js_object_key_at(props, i);
                    char *pname = js_string_utf8(name);
                    if (!pname) continue;
                    JsValue pv = js_object_get(e->vm, props, name);
                    if (js_is_array(pv)) {
                        uint32_t n = js_array_length(pv);
                        for (uint32_t k = 0; k < n; k++) {
                            char *item = js_string_utf8(js_array_get(pv, k));
                            if (item && strcmp(pname, "className") == 0) mdy_add_class(doc, out, item);
                            free(item);
                        }
                    } else if (js_is_number(pv)) {
                        mdy_set_number(doc, out, pname, js_get_number(pv));
                    } else if (js_is_bool(pv)) {
                        mdy_set_bool(doc, out, pname, js_get_bool(pv));
                    } else {
                        char *sv = js_string_utf8(pv);
                        if (sv) mdy_set_string(doc, out, pname, sv, strlen(sv));
                        free(sv);
                    }
                    free(pname);
                }
            }
            js_children_to_tree(e, doc, out, js_object_get(e->vm, v, key(e->vm, "children")));
        }
    }

    free(type);
    return out;
}


/* ---- the document set --------------------------------------------------------
 *
 * Opening a source is mdy-docs' `openDocumentSet`: every document's data — its
 * front matter merged with its ```data fences — goes into a nisaba collection,
 * and `$.find` runs real queries against it.
 *
 * The `_id` to index map is not bookkeeping. A query answers in whatever order
 * the database walks its keys, and a document set answers in DOCUMENT order —
 * so a hit is mapped back to the document it came from and the answer sorted
 * by that. Without it a page's list of siblings would reorder between builds.
 */

/* ---- a JS value as binjson, for a query filter ------------------------------ */

static int js_to_binjson(mdy_engine *e, bj_builder *b, JsValue v) {
    if (js_is_null(v) || js_is_undefined(v)) return bj_put_null(b);
    if (js_is_bool(v)) return bj_put_bool(b, js_get_bool(v));
    if (js_is_number(v)) {
        double d = js_get_number(v);
        if (d == (double)(int64_t)d && d >= -9.2e18 && d <= 9.2e18)
            return bj_put_int(b, (int64_t)d);
        return bj_put_float(b, d);
    }
    if (js_is_string(v)) {
        char *s = js_string_utf8(v);
        if (!s) return -1;
        int rc = bj_put_string(b, (const uint8_t *)s, (uint32_t)strlen(s));
        free(s);
        return rc;
    }
    if (js_is_array(v)) {
        if (bj_begin_array(b) != 0) return -1;
        uint32_t n = js_array_length(v);
        for (uint32_t i = 0; i < n; i++)
            if (js_to_binjson(e, b, js_array_get(v, i)) != 0) return -1;
        return bj_end_array(b);
    }
    if (js_is_object(v)) {
        if (bj_begin_object(b) != 0) return -1;
        size_t n = js_object_size(v);
        for (size_t i = 0; i < n; i++) {
            JsValue k = js_object_key_at(v, i);
            char *name = js_string_utf8(k);
            if (!name) continue;
            int rc = bj_put_key(b, (const uint8_t *)name, (uint32_t)strlen(name));
            free(name);
            if (rc != 0) return -1;
            if (js_to_binjson(e, b, js_object_get(e->vm, v, k)) != 0) return -1;
        }
        return bj_end_object(b);
    }
    return bj_put_null(b);
}

/* ---- binjson back as JS values ----------------------------------------------
 *
 * The decoder is a visitor, so this keeps a stack of the containers it is
 * inside and hangs each finished value on whichever is on top.
 */
enum { BJ_STACK_MAX = 64 };

typedef struct {
    mdy_engine *e;
    JsValue stack[BJ_STACK_MAX];
    char *keys[BJ_STACK_MAX];
    int depth;
    JsValue result;
    int have_result;
} Decode;

static void decode_put(Decode *d, JsValue v) {
    if (d->depth == 0) { d->result = v; d->have_result = 1; return; }
    JsValue parent = d->stack[d->depth - 1];
    if (js_is_array(parent)) {
        js_array_push(d->e->vm, parent, v);
    } else {
        char *k = d->keys[d->depth - 1];
        if (k) {
            js_object_set(d->e->vm, parent, key(d->e->vm, k), v);
            free(k);
            d->keys[d->depth - 1] = NULL;
        }
    }
}

static void d_null(void *ctx) { decode_put(ctx, js_null()); }
static void d_bool(void *ctx, int t) { decode_put(ctx, js_bool(t != 0)); }
static void d_int(void *ctx, double v) { decode_put(ctx, js_number(v)); }
static void d_float(void *ctx, double v) { decode_put(ctx, js_number(v)); }
static void d_date(void *ctx, double v) { decode_put(ctx, js_number(v)); }
static void d_pointer(void *ctx, double v) { decode_put(ctx, js_number(v)); }
static void d_string(void *ctx, const uint8_t *s, uint32_t n) {
    Decode *d = ctx;
    decode_put(d, str(d->e->vm, (const char *)s, n));
}
static void d_binary(void *ctx, const uint8_t *s, uint32_t n) {
    (void)s; (void)n;
    decode_put(ctx, js_null());
}
/* `_id` is an OID, and a document set's own key: guest code has no use for
 * the bytes, and a string of them is the shape mdy-docs hands over. */
static void d_oid(void *ctx, const uint8_t *b) {
    Decode *d = ctx;
    char hex[25];
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 12; i++) { hex[i * 2] = H[b[i] >> 4]; hex[i * 2 + 1] = H[b[i] & 15]; }
    hex[24] = '\0';
    decode_put(d, str(d->e->vm, hex, 24));
}
static void d_array_begin(void *ctx, uint32_t count) {
    Decode *d = ctx;
    (void)count;
    if (d->depth >= BJ_STACK_MAX) return;
    JsValue a = js_array_new(d->e->ctx, count);
    js_gc_protect(d->e->vm, &a);
    d->keys[d->depth] = NULL;
    d->stack[d->depth++] = a;
}
static void d_object_begin(void *ctx, uint32_t count) {
    Decode *d = ctx;
    (void)count;
    if (d->depth >= BJ_STACK_MAX) return;
    JsValue o = js_object_new(d->e->ctx);
    js_gc_protect(d->e->vm, &o);
    d->keys[d->depth] = NULL;
    d->stack[d->depth++] = o;
}
static void d_key(void *ctx, const uint8_t *s, uint32_t n) {
    Decode *d = ctx;
    if (d->depth == 0) return;
    free(d->keys[d->depth - 1]);
    d->keys[d->depth - 1] = malloc(n + 1);
    if (d->keys[d->depth - 1]) {
        memcpy(d->keys[d->depth - 1], s, n);
        d->keys[d->depth - 1][n] = '\0';
    }
}
static void d_end(void *ctx) {
    Decode *d = ctx;
    if (d->depth == 0) return;
    JsValue done = d->stack[--d->depth];
    free(d->keys[d->depth]);
    d->keys[d->depth] = NULL;
    js_gc_unprotect(d->e->vm, &done);
    decode_put(d, done);
}

static JsValue binjson_to_js(mdy_engine *e, const uint8_t *bytes, size_t len, size_t *consumed) {
    Decode d = {0};
    d.e = e;
    d.result = js_undefined();
    bj_visitor v = {
        .on_null = d_null, .on_bool = d_bool, .on_int = d_int, .on_float = d_float,
        .on_string = d_string, .on_binary = d_binary, .on_oid = d_oid, .on_date = d_date,
        .on_pointer = d_pointer, .on_array_begin = d_array_begin, .on_array_end = d_end,
        .on_object_begin = d_object_begin, .on_key = d_key, .on_object_end = d_end,
        .ctx = &d,
    };
    if (bj_decode(bytes, len, &v, consumed) != 0) return js_undefined();
    return d.result;
}



/* ---- composition -------------------------------------------------------------
 *
 * `$.render` does not return HTML, or a tree, or text. It returns a TOKEN — a
 * few private-use characters standing for a tree the host has parked — and the
 * token travels through the document's own code like any other string, into a
 * variable, a template literal, an attribute. The tree goes back in once the
 * text around it has been parsed.
 *
 * That is what makes `$.render` need no indentation argument: the parser
 * already knows which element is open where the token landed, so there is no
 * column for the caller to compute.
 *
 *   U+E000 <id> U+E001
 *
 * The id is base36 so a counter and a content-derived identity are both
 * covered by one pattern — three regexes in compose.js have to agree about
 * what an id looks like, and once they did not.
 */
#define TOKEN_OPEN  "\xee\x80\x80"      /* U+E000 as UTF-8 */
#define TOKEN_CLOSE "\xee\x80\x81"      /* U+E001 */

/* A token's id at `s`, or 0. Writes the id and how many bytes it spanned. */
static size_t token_at(const char *s, size_t len, char *id, size_t id_cap) {
    if (len < 5 || memcmp(s, TOKEN_OPEN, 3) != 0) return 0;
    size_t i = 3;
    size_t n = 0;
    while (i < len && n + 1 < id_cap) {
        char c = s[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')) { id[n++] = c; i++; continue; }
        break;
    }
    id[n] = '\0';
    if (n == 0) return 0;
    if (i + 3 > len || memcmp(s + i, TOKEN_CLOSE, 3) != 0) return 0;
    return i + 3;
}

static Held *held_find(mdy_engine *e, const char *id) {
    for (size_t i = 0; i < e->held_count; i++)
        if (strcmp(e->held[i].id, id) == 0) return &e->held[i];
    return NULL;
}

/*
 * Park a tree and hand back the token that stands for it.
 *
 * Nothing is reclaimed within a render: a token can be written into a string,
 * kept in a variable, dropped, or used twice, and nothing here gets to decide
 * when the last of those happened.
 */
static char *hold_tree(mdy_engine *e, mdy_doc *doc, mdy_node *tree) {
    if (e->held_count == e->held_cap) {
        size_t want = e->held_cap ? e->held_cap * 2 : 8;
        Held *grown = realloc(e->held, want * sizeof *grown);
        if (!grown) return NULL;
        e->held = grown;
        e->held_cap = want;
    }
    Held *h = &e->held[e->held_count++];
    snprintf(h->id, sizeof h->id, "%zu", e->next_token++);
    h->doc = doc;
    h->tree = tree;

    size_t n = strlen(TOKEN_OPEN) + strlen(h->id) + strlen(TOKEN_CLOSE) + 1;
    char *token = malloc(n);
    if (token) snprintf(token, n, "%s%s%s", TOKEN_OPEN, h->id, TOKEN_CLOSE);
    return token;
}

static void release_held(mdy_engine *e) {
    for (size_t i = 0; i < e->held_count; i++) mdy_free(e->held[i].doc);
    free(e->held);
    e->held = NULL;
    e->held_count = e->held_cap = 0;
}

/* Whitespace, and nothing else. */
static int only_space(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r') return 0;
    return 1;
}

/* `^(?:\s*TOKEN)+\s*$` — a run that is nothing but tokens and space. */
static int only_tokens(const char *s, size_t len) {
    size_t i = 0;
    int found = 0;
    for (;;) {
        while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
        if (i >= len) return found;
        char id[24];
        size_t used = token_at(s + i, len - i, id, sizeof id);
        if (!used) return 0;
        i += used;
        found = 1;
    }
}

/* Elements that hold a line of their own, and so cannot hold one of somebody
 * else's — the ones a nested render is likely to produce at its top level. */
static int is_block_tag(const char *tag) {
    static const char *const BLOCK[] = { "p", "div", "section", "article", "main", "header", "footer" };
    for (size_t i = 0; i < sizeof BLOCK / sizeof BLOCK[0]; i++)
        if (strcmp(BLOCK[i], tag) == 0) return 1;
    return 0;
}

/*
 * Block wrappers off, phrasing content out. A block cannot sit inside a
 * sentence, so it gives up its wrapper and lends its content instead — as far
 * down as the blocks go, since unwrapping a <div> only to find a <p> under it
 * has solved nothing.
 */
static void unwrap_into(mdy_node *dest, mdy_node *source) {
    for (mdy_node *c = source->first; c;) {
        mdy_node *next = c->next;
        c->next = NULL;
        if (c->type == MDY_ELEMENT && is_block_tag(c->tag)) unwrap_into(dest, c);
        else if (c->type == MDY_TEXT && only_space(c->text ? c->text : "", c->text ? strlen(c->text) : 0)) ;
        else mdy_append(dest, c);
        c = next;
    }
}

/* What a held tree contributes where a BLOCK was expected: a root lends its
 * children, anything else stands for itself. */
static void block_content(mdy_node *dest, mdy_node *tree) {
    if (tree->type == MDY_ROOT) {
        for (mdy_node *c = tree->first; c;) {
            mdy_node *next = c->next;
            c->next = NULL;
            mdy_append(dest, c);
            c = next;
        }
    } else {
        mdy_append(dest, tree);
    }
}

/*
 * Tokens in a run of TEXT, as the inline content they become. Anything that is
 * not a token stays the text it was.
 */
static void inline_content(mdy_engine *e, mdy_doc *doc, mdy_node *dest,
                           const char *s, size_t len) {
    size_t i = 0, last = 0;
    while (i < len) {
        char id[24];
        size_t used = token_at(s + i, len - i, id, sizeof id);
        if (!used) { i++; continue; }
        if (i > last) mdy_append(dest, mdy_new_text(doc, s + last, i - last));
        Held *h = held_find(e, id);
        if (h && h->tree) {
            mdy_node *holder = mdy_new_element(doc, "span", 4);
            block_content(holder, h->tree);
            unwrap_into(dest, holder);
        } else {
            mdy_append(dest, mdy_new_text(doc, s + i, used));
        }
        i += used;
        last = i;
    }
    if (last < len) mdy_append(dest, mdy_new_text(doc, s + last, len - last));
}

/* The text of a node that is a `<p>` holding one text child, or a text node —
 * which is what `onlyTokens` asks about. */
static const char *sole_text(const mdy_node *n, size_t *len) {
    if (n->type == MDY_TEXT) { *len = n->text ? strlen(n->text) : 0; return n->text; }
    if (n->type == MDY_ELEMENT && strcmp(n->tag, "p") == 0 && n->first &&
        n->first == n->last && n->first->type == MDY_TEXT) {
        *len = n->first->text ? strlen(n->first->text) : 0;
        return n->first->text;
    }
    return NULL;
}

/*
 * Put the held trees back where their tokens are.
 *
 * A paragraph holding nothing but tokens is REPLACED by what they hold — a
 * render on a line of its own is that document, not a paragraph wrapping it.
 * A token inside a sentence gives up its blocks instead.
 */
static void splice_tree(mdy_engine *e, mdy_doc *doc, mdy_node *parent) {
    mdy_node *child = parent->first;
    parent->first = parent->last = NULL;

    while (child) {
        mdy_node *next = child->next;
        child->next = NULL;

        size_t len = 0;
        const char *text = sole_text(child, &len);

        if (text && only_tokens(text, len)) {
            size_t i = 0;
            int filled = 0;
            while (i < len) {
                char id[24];
                size_t used = token_at(text + i, len - i, id, sizeof id);
                if (!used) { i++; continue; }
                Held *h = held_find(e, id);
                if (h && h->tree) { block_content(parent, h->tree); filled = 1; }
                i += used;
            }
            if (filled) { child = next; continue; }
        }

        if (child->type == MDY_TEXT && child->text && strstr(child->text, TOKEN_OPEN)) {
            inline_content(e, doc, parent, child->text, strlen(child->text));
            child = next;
            continue;
        }

        if (child->first) splice_tree(e, doc, child);
        mdy_append(parent, child);
        child = next;
    }
}

/*
 * A string holding tokens, as HTML — the string-shaped half of composition.
 * `$.emit(url, $.render(page))` writes a page because a file is a string and
 * that is the shape it can hold.
 */
static char *fill_tokens(mdy_engine *e, const char *s, size_t len) {
    size_t cap = len + 256, out = 0;
    char *result = malloc(cap);
    if (!result) return NULL;
    result[0] = '\0';

    size_t i = 0, last = 0;
    while (i < len) {
        char id[24];
        size_t used = token_at(s + i, len - i, id, sizeof id);
        if (!used) { i++; continue; }

        Held *h = held_find(e, id);
        char *html = h && h->tree ? mdy_to_html(h->tree, NULL) : NULL;
        size_t plain = i - last;
        size_t add = plain + (html ? strlen(html) : 0);
        if (out + add + 1 > cap) {
            while (out + add + 1 > cap) cap *= 2;
            char *grown = realloc(result, cap);
            if (!grown) { free(html); free(result); return NULL; }
            result = grown;
        }
        memcpy(result + out, s + last, plain);
        out += plain;
        if (html) { memcpy(result + out, html, strlen(html)); out += strlen(html); free(html); }
        result[out] = '\0';
        i += used;
        last = i;
    }

    size_t tail = len - last;
    if (out + tail + 1 > cap) {
        char *grown = realloc(result, out + tail + 1);
        if (!grown) { free(result); return NULL; }
        result = grown;
    }
    memcpy(result + out, s + last, tail);
    out += tail;
    result[out] = '\0';
    return result;
}


/* ---- rendering, and rendering from inside a render --------------------------- */

static int index_of_id(mdy_engine *e, const char *hex);
static JsValue run_query(mdy_engine *e, JsValue query, int one);

/* A render produces a TREE; HTML is what the outermost caller asks for at the
 * end. That is the whole reason `$.render` can return a token. */
/* `req` is what the caller is answering with — `$.render(target, data)`'s
 * second argument, and an empty object for a render nobody asked a question
 * of. MDY neither reads it nor cares what shape it is. */
static mdy_doc *render_tree(mdy_engine *e, size_t index, JsValue req,
                            char *error, size_t error_len);

/*
 * `$.render(target, data)` — the target resolved, rendered, parked, and a
 * token handed back.
 *
 * A target is an index, or a query, or a document a `$.find` already returned
 * (which carries its own `_id`). All three end at a document index, which is
 * what the `_id` to index map is for.
 */
static int resolve_target(mdy_engine *e, JsValue target) {
    if (js_is_number(target)) {
        double at = js_get_number(target);
        return (at >= 0 && at < (double)e->count) ? (int)at : -1;
    }
    if (!js_is_object(target)) return -1;

    /* A document from `$.find` carries the id it was inserted with. */
    char *id = js_string_utf8(js_object_get(e->vm, target, key(e->vm, "_id")));
    if (id) {
        int at = index_of_id(e, id);
        free(id);
        if (at >= 0) return at;
    }

    JsValue hit = run_query(e, target, 1);
    if (!js_is_object(hit)) return -1;
    char *hit_id = js_string_utf8(js_object_get(e->vm, hit, key(e->vm, "_id")));
    if (!hit_id) return -1;
    int at = index_of_id(e, hit_id);
    free(hit_id);
    return at;
}

static bool render_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                          int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    int at = argc > 0 ? resolve_target(e, args[0]) : -1;
    if (at < 0) {
        const char *msg = "mdy-engine: $.render found no such document";
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }

    char err[256];
    mdy_doc *doc = render_tree(e, (size_t)at,
                               argc > 1 ? args[1] : js_undefined(), err, sizeof err);
    if (!doc) {
        /*
         * A nested render's failure is passed through rather than wrapped
         * again. mdy-docs wraps at every level, so a cycle reports
         * "document 0 failed: document 0 failed: …" thirty times over and the
         * reason falls off the end. The first message is the one that says
         * something.
         */
        char msg[320];
        if (strncmp(err, "mdy-engine:", 11) == 0)
            snprintf(msg, sizeof msg, "%s", err);
        else
            snprintf(msg, sizeof msg, "mdy-engine: document %d failed: %s", at, err);
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }

    char *token = hold_tree(e, doc, mdy_root(doc));
    if (!token) { *result = js_undefined(); return false; }
    *result = str(e->vm, token, strlen(token));
    free(token);
    return true;
}

/* `$.text(target)` — the same render, as the text it holds rather than a
 * token. What a document's own code wrote, with no markup around it. */
static void collect_text_into(const mdy_node *n, char **out, size_t *len, size_t *cap) {
    if (n->type == MDY_TEXT && n->text) {
        size_t add = strlen(n->text);
        if (*len + add + 1 > *cap) {
            while (*len + add + 1 > *cap) *cap = *cap ? *cap * 2 : 256;
            char *grown = realloc(*out, *cap);
            if (!grown) return;
            *out = grown;
        }
        memcpy(*out + *len, n->text, add);
        *len += add;
        (*out)[*len] = '\0';
    }
    if (n->type == MDY_COMMENT || n->type == MDY_DOCTYPE) return;
    for (const mdy_node *c = n->first; c; c = c->next) collect_text_into(c, out, len, cap);
}

static bool text_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                        int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    int at = argc > 0 ? resolve_target(e, args[0]) : -1;
    if (at < 0) {
        const char *msg = "mdy-engine: $.text found no such document";
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }
    char err[256];
    mdy_doc *doc = render_tree(e, (size_t)at,
                               argc > 1 ? args[1] : js_undefined(), err, sizeof err);
    if (!doc) {
        char msg[320];
        if (strncmp(err, "mdy-engine:", 11) == 0) snprintf(msg, sizeof msg, "%s", err);
        else snprintf(msg, sizeof msg, "mdy-engine: document %d failed: %s", at, err);
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }
    char *text = NULL;
    size_t len = 0, cap = 0;
    collect_text_into(mdy_root(doc), &text, &len, &cap);
    *result = str(e->vm, text ? text : "", len);
    free(text);
    /* The tree is held so it outlives this call, like any other render's. */
    char *token = hold_tree(e, doc, mdy_root(doc));
    free(token);
    return true;
}

/*
 * `$.emit(path, content)` — a named output. Tokens in the content become the
 * HTML they hold, because a file is a string and that is the shape it can
 * hold.
 */
static bool emit_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                        int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    if (argc < 2) { *result = js_null(); return true; }
    char *path = js_string_utf8(args[0]);
    char *content = js_string_utf8(args[1]);
    if (path && content) {
        char *filled = fill_tokens(e, content, strlen(content));
        if (filled && e->on_emit) e->on_emit(e->on_emit_ud, path, filled);
        free(filled);
    }
    free(path);
    free(content);
    *result = js_null();
    return true;
}

/* ---- opening a set ---------------------------------------------------------- */

/* nisaba keys its primary tree on OID bytes and needs a file behind it; a set
 * built from a source is in memory, so this is a temporary the OS reclaims. */
extern int nis_open(void);
extern int nis_insert(int handle, const uint8_t *doc, uint32_t len);
extern int nis_find(int handle, const uint8_t *filter, uint32_t filter_len,
                    uint8_t **out, size_t *out_len);
extern int nis_create_index(int handle, const char *name, const uint8_t *fields,
                            uint32_t fields_len, int unique, int sparse);

static void close_set(mdy_engine *e) {
    for (size_t i = 0; i < e->count; i++) mdy_data_free(e->docs[i].fences);
    free(e->docs);
    free(e->ids);
    mdy_documents_free(e->source_docs);
    e->docs = NULL;
    e->ids = NULL;
    e->source_docs = NULL;
    e->count = 0;
}

int mdy_engine_open(mdy_engine *e, const char *source, size_t len,
                    char *error, size_t error_len) {
    if (error && error_len) error[0] = '\0';
    close_set(e);

    e->source_docs = mdy_split_documents(source, len);
    if (!e->source_docs) return -1;

    size_t n = mdy_documents_count(e->source_docs);
    e->docs = calloc(n ? n : 1, sizeof *e->docs);
    e->ids = calloc(n ? n : 1, sizeof *e->ids);
    if (!e->docs || !e->ids) { close_set(e); return -1; }
    e->count = n;

    if (e->handle < 0) {
        e->handle = nis_open();
        if (e->handle < 0) {
            if (error && error_len) snprintf(error, error_len, "could not open a collection");
            close_set(e);
            return -1;
        }
    }

    /*
     * `path` is the natural key of a set built from a directory, and every
     * `$.render({ path: … })` resolves through a query on it — so without the
     * index each one is a scan of the whole set. SPARSE because a document
     * need not have a path at all, and NOT unique because one file can hold
     * several documents and they all carry its path.
     *
     * It is also what makes the document-order sort do real work: a query
     * answered from an index comes back in INDEX order, not `_id` order.
     */
    {
        bj_builder *spec = bj_builder_new();
        if (spec) {
            bj_begin_object(spec);
            bj_put_key(spec, (const uint8_t *)"path", 4);
            bj_put_int(spec, 1);
            bj_end_object(spec);
            size_t slen = 0;
            const uint8_t *bytes = bj_builder_data(spec, &slen);
            nis_create_index(e->handle, "path", bytes, (uint32_t)slen, 0, 1);
            bj_builder_free(spec);
        }
    }

    for (size_t i = 0; i < n; i++) {
        Document *d = &e->docs[i];
        d->chunk = mdy_documents_at(e->source_docs, i);
        mdy_chunk body;
        mdy_split_frontmatter(d->chunk.text, d->chunk.len, &d->matter, &body);
        d->fences = mdy_data_extract(body.text, body.len);

        /*
         * The document's DATA: its front matter, with each ```data fence
         * merged over it — `Object.assign({}, frontMatter, ...blocks)`. Its
         * text never goes in, which is what a measured build of mdy-docs
         * shows it doing.
         */
        char err[256];
        mdy_yaml *matter = NULL;
        if (d->matter.len) matter = mdy_yaml_parse(d->matter.text, d->matter.len, err, sizeof err);

        size_t fence_count = d->fences ? mdy_data_count(d->fences) : 0;
        const mdy_yaml_node **maps = calloc(fence_count + 1, sizeof *maps);
        mdy_yaml **parsed = calloc(fence_count + 1, sizeof *parsed);
        if (!maps || !parsed) { free(maps); free(parsed); mdy_yaml_free(matter); close_set(e); return -1; }

        size_t used = 0;
        if (matter) maps[used++] = mdy_yaml_root(matter);
        for (size_t f = 0; f < fence_count; f++) {
            const mdy_data_fence *fence = mdy_data_at(d->fences, f);
            mdy_yaml *y = mdy_yaml_parse(fence->source, fence->source_len, err, sizeof err);
            if (!y) continue;
            parsed[f] = y;
            maps[used++] = mdy_yaml_root(y);
        }

        mdy_oid_next(d->oid);
        memcpy(e->ids[i], d->oid, 12);

        bj_builder *b = bj_builder_new();
        int ok = b && mdy_bj_document(b, d->oid, maps, used) == 0 && !bj_builder_error(b);
        if (ok) {
            size_t dlen = 0;
            const uint8_t *bytes = bj_builder_data(b, &dlen);
            ok = bytes && nis_insert(e->handle, bytes, (uint32_t)dlen) == 0;
        }
        bj_builder_free(b);
        mdy_yaml_free(matter);
        for (size_t f = 0; f < fence_count; f++) mdy_yaml_free(parsed[f]);
        free(maps);
        free(parsed);

        if (!ok) {
            if (error && error_len)
                snprintf(error, error_len, "document %zu could not be inserted", i);
            close_set(e);
            return -1;
        }
    }

    return 0;
}

size_t mdy_engine_count(mdy_engine *e) { return e ? e->count : 0; }

void mdy_engine_on_emit(mdy_engine *e,
                        void (*fn)(void *ud, const char *path, const char *content),
                        void *ud) {
    if (!e) return;
    e->on_emit = fn;
    e->on_emit_ud = ud;
}

/* ---- querying ---------------------------------------------------------------
 *
 * A hit carries the `_id` it was inserted with, so it maps back to the
 * document it came from — and the answer is sorted by that, not by whatever
 * order the database walked its keys in. That is what makes a query's answer
 * the same on every build.
 */
static int index_of_id(mdy_engine *e, const char *hex) {
    static const char *H = "0123456789abcdef";
    for (size_t i = 0; i < e->count; i++) {
        char have[25];
        for (int k = 0; k < 12; k++) {
            have[k * 2] = H[e->ids[i][k] >> 4];
            have[k * 2 + 1] = H[e->ids[i][k] & 15];
        }
        have[24] = '\0';
        if (strcmp(have, hex) == 0) return (int)i;
    }
    return -1;
}

static JsValue run_query(mdy_engine *e, JsValue query, int one) {
    bj_builder *b = bj_builder_new();
    if (!b) return js_undefined();
    if (js_is_object(query) && !js_is_array(query)) {
        if (js_to_binjson(e, b, query) != 0) { bj_builder_free(b); return js_undefined(); }
    } else {
        bj_begin_object(b);
        bj_end_object(b);
    }
    size_t flen = 0;
    const uint8_t *filter = bj_builder_data(b, &flen);

    uint8_t *out = NULL;
    size_t out_len = 0;
    int rc = nis_find(e->handle, filter, (uint32_t)flen, &out, &out_len);
    bj_builder_free(b);
    if (rc != 0 || !out) return one ? js_null() : js_array_new(e->ctx, 0);

    /* The result is a binjson ARRAY of documents. */
    JsValue hits = binjson_to_js(e, out, out_len, NULL);
    free(out);
    if (!js_is_array(hits)) return one ? js_null() : js_array_new(e->ctx, 0);
    js_gc_protect(e->vm, &hits);

    /* Back into document order. */
    uint32_t n = js_array_length(hits);
    JsValue ordered = js_array_new(e->ctx, n);
    js_gc_protect(e->vm, &ordered);
    for (size_t want = 0; want < e->count; want++) {
        for (uint32_t i = 0; i < n; i++) {
            JsValue hit = js_array_get(hits, i);
            char *id = js_string_utf8(js_object_get(e->vm, hit, key(e->vm, "_id")));
            if (!id) continue;
            int at = index_of_id(e, id);
            free(id);
            if (at == (int)want) js_array_push(e->vm, ordered, hit);
        }
    }
    js_gc_unprotect(e->vm, &ordered);
    js_gc_unprotect(e->vm, &hits);

    if (!one) return ordered;
    return js_array_length(ordered) > 0 ? js_array_get(ordered, 0) : js_null();
}

static bool find_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                        int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    *result = run_query(e, argc > 0 ? args[0] : js_undefined(), 0);
    return true;
}

static bool find_one_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                            int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    *result = run_query(e, argc > 0 ? args[0] : js_undefined(), 1);
    return true;
}

/* `$.data(i)` — a document's own data, by index, without a query. */
static bool data_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                        int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    if (argc < 1 || !js_is_number(args[0])) { *result = js_null(); return true; }
    double at = js_get_number(args[0]);
    if (at < 0 || at >= (double)e->count) { *result = js_null(); return true; }

    bj_builder *b = bj_builder_new();
    if (!b) { *result = js_null(); return true; }
    bj_begin_object(b);
    bj_put_key(b, (const uint8_t *)"_id", 3);
    bj_put_oid(b, e->ids[(size_t)at]);
    bj_end_object(b);
    size_t flen = 0;
    const uint8_t *filter = bj_builder_data(b, &flen);
    uint8_t *out = NULL;
    size_t out_len = 0;
    int rc = nis_find(e->handle, filter, (uint32_t)flen, &out, &out_len);
    bj_builder_free(b);
    if (rc != 0 || !out) { *result = js_null(); return true; }
    JsValue hits = binjson_to_js(e, out, out_len, NULL);
    free(out);
    *result = js_is_array(hits) && js_array_length(hits) > 0 ? js_array_get(hits, 0) : js_null();
    return true;
}

/* ---- `$`, with every native refusing ---------------------------------------
 *
 * A native that is not implemented THROWS, naming itself. The alternative — a
 * stub returning undefined — produces a page that is quietly missing whatever
 * the document asked for, which is the failure this whole project has been
 * most careful to avoid.
 */
static bool refuse(JsContext *ctx, JsValue this_val, const JsValue *args, int argc,
                   JsValue *result) {
    (void)this_val;
    JsVm *vm = js_context_vm(ctx);
    char msg[160];
    char name[64] = "(unnamed)";
    if (argc > 0 && js_is_string(args[0])) {
        size_t ulen = 0;
        const uint16_t *u = js_string_units(args[0], &ulen);
        size_t n = 0;
        for (size_t i = 0; i < ulen && n < sizeof name - 1; i++)
            if (u[i] < 0x80) name[n++] = (char)u[i];
        name[n] = '\0';
    }
    snprintf(msg, sizeof msg,
             "mdy-engine: $.%s is not implemented in the C engine yet", name);
    *result = str(vm, msg, strlen(msg));
    return false;
}

/*
 * `$.compose(__out)` — the one native step 2 implements, and the reason the
 * tree conversions exist.
 *
 * A document with a `transform` needs its own finished tree, so the host takes
 * the lines it produced, parses them, and hands the tree BACK to the guest as
 * values. The guest transforms it and returns it; the host converts it back
 * and writes the HTML. mdy-docs does the same thing with JSON in both
 * directions.
 */
static mdy_doc *parse_lines(JsValue out, mdy_engine *e);

static bool compose_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                           int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    if (argc < 1 || !js_is_array(args[0])) {
        const char *msg = "mdy-engine: $.compose wants the lines a document produced";
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }
    mdy_doc *tree = parse_lines(args[0], e);
    if (!tree) {
        const char *msg = "mdy-engine: the produced lines did not parse";
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }
    /* Composed before the transforms see it: a transform works on the
     * document's FINISHED tree, renders and all. */
    splice_tree(e, tree, mdy_root(tree));
    /* The document owns the tree until the render finishes with it. */
    mdy_free(e->tree_owner);
    e->tree_owner = tree;
    *result = tree_to_js(e, mdy_root(tree));
    return true;
}

static void register_one(mdy_engine *e, const char *name, JsNativeFn fn) {
    size_t n = 0;
    uint16_t *u = to_utf16(name, strlen(name), &n);
    if (!u) return;
    js_register_native(e->ctx, u, n, fn, NULL);
    free(u);
}

static void register_natives(mdy_engine *e) {
    register_one(e, "__native", refuse);
    register_one(e, "__compose", compose_native);
    register_one(e, "__find", find_native);
    register_one(e, "__findOne", find_one_native);
    register_one(e, "__data", data_native);
    register_one(e, "__render", render_native);
    register_one(e, "__text", text_native);
    register_one(e, "__emit", emit_native);
}

/*
 * `$` is built in the wrapper source rather than as an object of native
 * function VALUES, because lamassu's public API has no way to make one:
 * js_register_native defines a global, and there is no js_function_new. So
 * one global native stands behind every name, exactly as mdy-docs' own
 * `__call` does — minus the JSON, since arguments now cross as values.
 *
 * Closing that gap is worth doing when `$` starts doing real work; a native
 * per name is clearer than a name-dispatching one, and it is the same small
 * addition to lamassu that js_array_new was.
 */


/* ---- the pieces ------------------------------------------------------------- */

mdy_engine *mdy_engine_new(void) {
    mdy_engine *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    JsVmConfig cfg = {0};
    e->vm = js_vm_new(&cfg);
    if (!e->vm) { free(e); return NULL; }
    e->handle = -1;
    e->ctx = js_context_new(e->vm);
    if (!e->ctx) { js_vm_free(e->vm); free(e); return NULL; }
    /* So a native can find the engine it belongs to. */
    js_context_set_userdata(e->ctx, e);
    register_natives(e);
    return e;
}

void mdy_engine_free(mdy_engine *e) {
    if (!e) return;
    close_set(e);
    mdy_free(e->tree_owner);
    js_context_free(e->ctx);
    js_vm_free(e->vm);
    free(e);
}

/*
 * The statements, wrapped as a function of their data.
 *
 * `req`, `res` and `$` are ARGUMENTS, so the same compiled function serves
 * every render of the document — which is the whole reason the script layer
 * produces statements that never mention the request.
 */
static char *wrap(const char *statements) {
    /*
     * Every `$` native, wired to the one global that stands behind them. They
     * all refuse for now — see the note above `refuse` — and a document that
     * calls one gets an error naming it rather than a page quietly missing
     * whatever it asked for.
     */
    static const char OPEN[] =
        "(async (req, res, $$) => {\n"
        "const $ = {\n"
        "  find: (q) => __find(q === undefined ? {} : q),\n"
        "  findOne: (q) => __findOne(q === undefined ? {} : q),\n"
        "  withTag: (t) => __find({ tags: String(t).toLowerCase() }),\n"
        "  render: (t, d) => __render(t, d),\n"
        "  text: (t, d) => __text(t, d),\n"
        "  emit: (p, c) => __emit(p, c),\n"
        "  publish: (n, d) => __native('publish', n, d),\n"
        "  parse: (s) => __native('parse', s),\n"
        "  markdown: (s) => __native('markdown', s),\n"
        "  node: (t) => __native('node', t),\n"
        "  html: (v) => __native('html', v),\n"
        "  table: (r, a) => __native('table', r, a),\n"
        "  toc: (t) => __native('toc', t),\n"
        "  data: (i) => __data(i),\n"
        "  compose: (o) => __compose(o),\n"
        "};\n"
        "const __transforms = [];\n"
        "const transform = (fn) => { __transforms.push(fn); };\n";

    /*
     * The epilogue, which is mdy-docs' own: a document with no transform hands
     * back its LINES and the host composes them; one with a transform asks for
     * its tree, runs each transform over it, and hands the TREE back. The two
     * shapes are told apart by which key the result carries.
     */
    static const char CLOSE[] =
        "\nif (__transforms.length > 0) {\n"
        "  let __tree = $.compose(__out);\n"
        "  res.doc = __tree;\n"
        "  for (const fn of __transforms) {\n"
        "    const returned = fn(__tree);\n"
        "    if (returned !== undefined && returned !== null) {\n"
        "      if (typeof returned !== \"object\" || typeof returned.type !== \"string\") {\n"
        "        throw \"transform must return a hast node ({ type, ... }), or undefined after changing the tree in place\";\n"
        "      }\n"
        "      __tree = returned;\n"
        "    }\n"
        "    res.doc = __tree;\n"
        "  }\n"
        "  return { tree: __tree };\n"
        "}\n"
        "return { out: __out };\n})";
    size_t n = strlen(OPEN) + strlen(MDY_TOOLKIT) + strlen(statements) + strlen(CLOSE) + 1;
    char *out = malloc(n);
    if (out) snprintf(out, n, "%s%s%s%s", OPEN, MDY_TOOLKIT, statements, CLOSE);
    return out;
}

/*
 * `scriptOutput`: the `[line, text]` pairs the document produced, flattened
 * into lines. One push can still yield several, when what was interpolated had
 * newlines in it.
 */
static char *flatten(JsValue out, size_t *out_len) {
    size_t cap = 4096, len = 0;
    char *text = malloc(cap);
    if (!text) return NULL;
    text[0] = '\0';

    uint32_t n = js_array_length(out);
    for (uint32_t i = 0; i < n; i++) {
        JsValue pair = js_array_get(out, i);
        JsValue value = js_array_get(pair, 1);
        size_t ulen = 0;
        const uint16_t *u = js_string_units(value, &ulen);
        char *piece = u ? from_utf16(u, ulen) : NULL;
        if (!piece) continue;
        size_t plen = strlen(piece);
        if (len + plen + 2 > cap) {
            while (len + plen + 2 > cap) cap *= 2;
            char *grown = realloc(text, cap);
            if (!grown) { free(piece); free(text); return NULL; }
            text = grown;
        }
        if (len) text[len++] = '\n';
        memcpy(text + len, piece, plen);
        len += plen;
        text[len] = '\0';
        free(piece);
    }
    *out_len = len;
    return text;
}

/* The lines a document produced, parsed — what `$.compose` returns and what a
 * document with no transform gets at the end. */
static mdy_doc *parse_lines(JsValue out, mdy_engine *e) {
    (void)e;
    size_t text_len = 0;
    char *text = flatten(out, &text_len);
    if (!text) return NULL;

    mdy_options options;
    mdy_options_default(&options);
    /* What the document engine asks the parser for: it has already taken the
     * front matter off and split the documents, and the code has already run
     * — so all three are the parser's business no longer. */
    options.frontmatter = 0;
    options.documents = 0;
    options.sanitize = 0;

    mdy_doc *tree = mdy_parse(text, text_len, &options);
    free(text);
    return tree;
}

static mdy_doc *render_tree(mdy_engine *e, size_t index, JsValue request,
                            char *error, size_t error_len) {
    if (error && error_len) error[0] = '\0';
    mdy_doc *out = NULL;
    mdy_script *script = NULL;

    /* A render inside a render inside a render is a cycle somebody wrote. */
    if (e->depth > 32) {
        if (error && error_len)
            snprintf(error, error_len, "mdy-engine: render depth exceeded (cyclic $.render?)");
        return NULL;
    }
    e->depth++;

#define FAIL(...) do { if (error && error_len) snprintf(error, error_len, __VA_ARGS__); goto done; } while (0)

    if (index >= e->count) {
        if (error && error_len) snprintf(error, error_len, "no document at index %zu", index);
        return NULL;
    }
    Document *d = &e->docs[index];
    /* 1. the body, which `mdy_engine_open` already took the data out of */
    size_t template_len = 0;
    const char *template_text = mdy_data_body(d->fences, &template_len);

    /* 2. the script layer */
    script = mdy_script_compile(template_text, template_len);
    if (!script) FAIL("the script layer could not compile this document");

    size_t src_len = 0;
    char *wrapped = wrap(mdy_script_source(script, &src_len));
    if (!wrapped) FAIL("out of memory");

    size_t ulen = 0;
    uint16_t *u = to_utf16(wrapped, strlen(wrapped), &ulen);
    const char *err_msg = NULL;
    uint32_t err_pos = 0;
    JsValue fn = js_compile_module(e->ctx, u, ulen, &err_msg, &err_pos);
    free(u);
    free(wrapped);
    if (js_is_undefined(fn)) FAIL("the document's code did not compile: %s", err_msg ? err_msg : "?");
    js_gc_protect(e->vm, &fn);

    JsValue promise = js_run_module(e->ctx, fn);
    js_gc_protect(e->vm, &promise);
    js_run_jobs(e->ctx);
    JsValue callable = js_promise_result(promise);
    if (!js_is_function(callable)) FAIL("the document's code did not produce a function");
    js_gc_protect(e->vm, &callable);

    /* the request, the response, and `$` */
    JsValue req = js_is_object(request) ? request : js_object_new(e->ctx);
    js_gc_protect(e->vm, &req);
    JsValue res = js_object_new(e->ctx);
    js_gc_protect(e->vm, &res);
    js_object_set(e->vm, res, key(e->vm, "data"), js_object_new(e->ctx));
    JsValue dollar = js_object_new(e->ctx);
    js_gc_protect(e->vm, &dollar);
    JsValue args[3] = { req, res, dollar };
    JsValue result = js_undefined();
    if (!js_call(e->ctx, callable, js_undefined(), args, 3, &result)) {
        size_t mlen = 0;
        const uint16_t *mu = js_string_units(result, &mlen);
        char *msg = mu ? from_utf16(mu, mlen) : NULL;
        if (error && error_len) snprintf(error, error_len, "%s", msg ? msg : "the document threw");
        free(msg);
        goto done;
    }
    js_gc_protect(e->vm, &result);

    if (js_is_promise(result)) {
        js_run_jobs(e->ctx);
        if (js_promise_state(result) != 1) {
            JsValue reason = js_promise_result(result);
            size_t mlen = 0;
            const uint16_t *mu = js_string_units(reason, &mlen);
            char *msg = mu ? from_utf16(mu, mlen) : NULL;
            if (error && error_len)
                snprintf(error, error_len, "%s", msg ? msg : "the document did not settle");
            free(msg);
            goto done;
        }
        JsValue settled = js_promise_result(result);
        js_gc_unprotect(e->vm, &result);
        result = settled;
        js_gc_protect(e->vm, &result);
    }

    if (!js_is_object(result)) FAIL("the document did not produce a result");

    /*
     * 3. the tree. A document with a transform already has one — it asked for
     * it through `$.compose` and handed back what its transforms made of it —
     * and one without hands back its lines for the host to parse.
     */
    JsValue transformed = js_object_get(e->vm, result, key(e->vm, "tree"));
    if (js_is_object(transformed)) {
        /* Already composed: `$.compose` spliced it before the transforms saw
         * it, which is what let a transform work on the finished tree. */
        mdy_doc *doc = mdy_doc_new();
        if (!doc) FAIL("out of memory");
        mdy_node *root = js_to_tree(e, doc, transformed);
        /*
         * `mdy_doc` owns its root, so the tree that came back is hung under
         * it: a root lends its children, and a transform that returned a
         * single element becomes that document's one child. Which is what
         * `blockContent` does with a held tree, for the same reason.
         */
        mdy_node *into = (mdy_node *)mdy_root(doc);
        if (root && root->type == MDY_ROOT) {
            for (mdy_node *c = root->first; c;) {
                mdy_node *next = c->next;
                c->next = NULL;
                mdy_append(into, c);
                c = next;
            }
        } else if (root) {
            mdy_append(into, root);
        }
        out = doc;
    } else {
        JsValue lines = js_object_get(e->vm, result, key(e->vm, "out"));
        if (!js_is_array(lines)) FAIL("the document did not produce its lines");
        mdy_doc *tree = parse_lines(lines, e);
        if (!tree) FAIL("the produced lines did not parse");
        /* The held trees go back where their tokens are. */
        splice_tree(e, tree, mdy_root(tree));
        out = tree;
    }

done:
#undef FAIL
    e->depth--;
    mdy_script_free(script);
    return out;
}

char *mdy_engine_render(mdy_engine *e, size_t index, char *error, size_t error_len) {
    if (error && error_len) error[0] = '\0';
    mdy_doc *doc = render_tree(e, index, js_undefined(), error, error_len);
    if (!doc) { release_held(e); return NULL; }
    char *html = mdy_to_html(mdy_root(doc), NULL);
    mdy_free(doc);
    /* Nothing a render parked outlives the render that asked for it. */
    release_held(e);
    return html;
}
