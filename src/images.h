#ifndef MDY_IMAGES_H
#define MDY_IMAGES_H

/*
 * What the engine needs to know about a picture: how big it is, and how to
 * make a smaller one.
 *
 * Two separate jobs, deliberately. Every image file in a walked directory gets
 * its DIMENSIONS read, because a record carrying width and height is what lets
 * a template lay a page out — and reading a header is cheap. Decoding only
 * happens when a document actually asks for a resize.
 */

#include <stddef.h>
#include <stdint.h>

/*
 * An image's dimensions from its header alone, without decoding it. 0 on
 * success. Non-zero for a format this does not know or a file too damaged to
 * say — which is not an error to the caller: mdy-docs keeps the record either
 * way, just without width and height.
 */
int mdy_image_size(const uint8_t *bytes, size_t len, int *width, int *height);

/*
 * A PNG, resized, as a new PNG. Returns the bytes (caller frees) and writes
 * the length, or NULL.
 *
 * PNG only, matching mdy-docs: its own CODECS table holds one entry, because
 * @jsquash's JPEG codec has a different init shape and was never wired. So
 * this is parity, not a shortfall — and the check on the extension lives in
 * the caller so the error message can name what it got.
 */
uint8_t *mdy_image_resize_png(const uint8_t *bytes, size_t len,
                              int width, int height, size_t *out_len);

#endif /* MDY_IMAGES_H */
