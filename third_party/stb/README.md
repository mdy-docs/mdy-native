# stb

Sean Barrett's single-file public-domain (or MIT) libraries, vendored rather
than depended on: three headers, no build system, no platform binaries to
prebuild — which is the same reason lamassu and nisaba are here as source.

    stb_image.h          decode PNG/JPEG/GIF/BMP/… and read dimensions
    stb_image_write.h    encode PNG
    stb_image_resize2.h  resample

Pinned at the commit in `COMMIT`. Dual-licensed MIT / public domain; the
licence text is at the foot of each header.

**These are not the codecs mdy-docs uses.** The JavaScript side resizes with
`@jsquash` — Squoosh's codecs, wasm — so a resized PNG's BYTES differ between
the two engines even when the image is visually the same. The pixels come from
different resamplers and the file from different encoders. Nothing else in this
port diverges like that, so it is stated wherever `$.resize` is documented.
