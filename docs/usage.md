# quirewise-vector usage guide

This guide covers the SVG/SVGZ and CAD workflows of `Quirewise::Vector`. Public
declarations live in `quirewise/svg.h`, `quirewise/svgz.h` and `quirewise/cad.h`.

## 1. Build and integrate

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/quirewise/install
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/quirewise/install
```

Consumer CMake:

```cmake
find_package(QuirewiseVector CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE Quirewise::Vector)
```

The package config resolves Runtime, Data, Compression and Charset. There is
nothing to initialize at runtime.

## 2. SVG

### 2.1 Render an SVG buffer

```c
#include <quirewise/svg.h>
#include <stdio.h>

int render_svg(const void *svg, size_t svg_size)
{
    QwSvgImage image = {0};
    QwSvgError error;
    QwSvgStatus rc = qw_svg_decode(svg, svg_size, &image, &error);

    if (rc != QW_SVG_OK) {
        fprintf(stderr, "SVG failed (%d): %s\n", (int)rc, error.message);
        return 0;
    }
    printf("%ux%u rgba, stride=%zu, bytes=%zu\n",
           image.width, image.height, image.stride, image.size);
    /* image.pixels is RGBA8; transparent areas retain zero alpha. */
    qw_svg_image_free(&image);
    return 1;
}
```

- The input bytes are borrowed; they may be freed after the call.
- The output is RGBA8 at 2x document units. The caller owns `image.pixels` and
  releases it with `qw_svg_image_free`; do not use `free`.
- `QwSvgError.message` is a fixed 192-byte buffer. It is useful for diagnostics;
  the status code is the stable contract.
- An SVG file whose features fall outside the native subset fails with
  `QW_SVG_ERR_UNSUPPORTED`; malformed XML fails with `QW_SVG_ERR_FORMAT`.

### 2.2 Render an SVGZ file

SVGZ is a gzip member containing an SVG document. Decompress first, then render:

```c
#include <quirewise/svg.h>
#include <quirewise/svgz.h>

int render_svgz(const void *gz, size_t gz_size)
{
    QwSvgzDocument document = {0};
    QwSvgImage image = {0};
    QwSvgError error;
    QwSvgzStatus zrc = qw_svgz_decode(gz, gz_size, &document);
    QwSvgStatus src;

    if (zrc != QW_SVGZ_OK) return 0;
    src = qw_svg_decode(document.svg_utf8, document.svg_size, &image, &error);
    qw_svgz_document_free(&document);
    if (src != QW_SVG_OK) return 0;
    qw_svg_image_free(&image);
    return 1;
}
```

`qw_svgz_decode` accepts exactly one gzip member and validates its CRC/length.
The decompressed bytes belong to `QwSvgzDocument` and are released with
`qw_svgz_document_free`.

## 3. CAD

### 3.1 Probe and parse

```c
QwCadDocument *document = NULL;
QwCadStatus rc;

if (!qw_cad_probe(data, size)) {
    /* not DXF/DWG */
    return 0;
}
rc = qw_cad_parse(data, size, &document);
if (rc != QW_CAD_OK) {
    fprintf(stderr, "CAD parse failed: %s\n", qw_cad_status_string(rc));
    return 0;
}
```

`qw_cad_probe` only says the bytes look like a supported container; parse can
still fail. `qw_cad_parse` borrows the input only for the call. The returned
document owns the model and must be released with `qw_cad_document_free`.

Supported containers:

| Format | What is parsed |
|---|---|
| DXF ASCII | header, layers, blocks, common 2D entities, text |
| DXF binary | same model as ASCII |
| DWG R13/R14/R2000/R2004/R2007/R2010/R2013/R2018 | common LINE/CIRCLE/ARC/LWPOLYLINE/TEXT geometry |

Other DWG entities and HATCH/ACIS/3D solids/proxy objects are not expanded.

### 3.2 Inspect the model

```c
QwCadBounds bounds;
if (qw_cad_bounds(document, &bounds) == QW_CAD_OK) {
    printf("bounds: (%g,%g)-(%g,%g)\n",
           bounds.min_x, bounds.min_y, bounds.max_x, bounds.max_y);
}
printf("%zu entities, %zu layers, %zu texts\n",
       qw_cad_entity_count(document), qw_cad_layer_count(document),
       qw_cad_text_count(document));
```

Entity/layer counts describe the parsed model. `qw_cad_bounds` returns
`QW_CAD_ERR_FORMAT` (or another status) when no finite bounds are available.

### 3.3 Read the text inventory

```c
size_t i;
for (i = 0u; i < qw_cad_text_count(document); ++i) {
    QwCadTextInfo text;
    if (qw_cad_text_at(document, i, &text) != QW_CAD_OK) continue;
    printf("%.*s  at (%g,%g) height=%g rotation=%g layer=%.*s\n",
           (int)text.text_size, text.text, text.x, text.y, text.height,
           text.rotation, (int)text.layer_size, text.layer);
}
```

`text` and `layer` are borrowed until `qw_cad_document_free`; they are not
NUL-terminated, so always use the size fields.

### 3.4 Render

Raster (fast preview; text is drawn as placeholder bars):

```c
QwCadImage image = {0};
if (qw_cad_render_rgba(document, 1600u, &image) == QW_CAD_OK) {
    /* image.pixels is RGBA8; height follows the model aspect ratio. */
    qw_cad_image_free(&image);
}
```

SVG (real `<text>` elements, one group per layer):

```c
char *svg = NULL;
size_t svg_size = 0u;
if (qw_cad_render_svg(document, &svg, &svg_size) == QW_CAD_OK) {
    fwrite(svg, 1u, svg_size, stdout);
    qw_cad_buffer_free(svg);
}
```

Use the SVG renderer when readable text matters. The raster renderer has no
glyph engine yet.

## 4. Error handling

### SVG

| Status | Meaning |
|---|---|
| `QW_SVG_OK` | rendered |
| `QW_SVG_ERR_INVALID_ARGUMENT` | missing buffer/output pointer |
| `QW_SVG_ERR_OUT_OF_MEMORY` | allocation failed |
| `QW_SVG_ERR_FORMAT` | malformed XML/SVG |
| `QW_SVG_ERR_UNSUPPORTED` | valid SVG outside the native subset |
| `QW_SVG_ERR_RESOURCE_LIMIT` | dimension/input/output cap exceeded |

### SVGZ

| Status | Meaning |
|---|---|
| `QW_SVGZ_OK` | decompressed |
| `QW_SVGZ_ERR_INVALID_ARGUMENT` | missing input/output |
| `QW_SVGZ_ERR_OUT_OF_MEMORY` | allocation failed |
| `QW_SVGZ_ERR_FORMAT` | not a gzip member |
| `QW_SVGZ_ERR_RESOURCE_LIMIT` | output cap exceeded |
| `QW_SVGZ_ERR_DECODE` | gzip CRC/length/stream error |

### CAD

| Status | Meaning |
|---|---|
| `QW_CAD_OK` | success |
| `QW_CAD_ERR_INVALID_ARGUMENT` | missing input/output |
| `QW_CAD_ERR_OUT_OF_MEMORY` | allocation failed |
| `QW_CAD_ERR_FORMAT` | malformed/truncated container |
| `QW_CAD_ERR_UNSUPPORTED` | recognized container outside the supported entity/version subset |
| `QW_CAD_ERR_RESOURCE_LIMIT` | entity/text/size cap exceeded |

`qw_svg_status_string`, `qw_svgz_status_string` and `qw_cad_status_string` turn
a code into a stable English message.

## 5. Memory and lifetime

- SVG: input borrowed for the call; output owned by `QwSvgImage`, released with
  `qw_svg_image_free`. Do not mix `free` and the library free function.
- SVGZ: output owned by `QwSvgzDocument`, released with
  `qw_svgz_document_free`.
- CAD: input borrowed for `qw_cad_parse`; document owned, released with
  `qw_cad_document_free`. Text/layer strings are borrowed from the document.
- CAD SVG output is released with `qw_cad_buffer_free`; raster output with
  `qw_cad_image_free`.
- No global mutable state: separate documents/images can be processed in
  parallel. A single document is read-only after parse and may be queried from
  multiple threads as long as no thread frees it.

## 6. Limits and format notes

- SVG support is a native subset: not every filter, mask, animation or external
  resource is implemented. Unsupported features fail rather than silently
  producing an empty image.
- SVGZ accepts one gzip member; concatenated members are rejected.
- DWG entity coverage is limited to the common entities listed above; other
  records are ignored. If a drawing depends on unsupported entities, the
  rendered result can be incomplete even though parsing succeeds.
- CAD raster text is a placeholder bar; use `qw_cad_render_svg` for text that
  must be readable.
- HATCH, ACIS/3D solids, proxy objects and exact NURBS splines are not
  expanded.
- DXF text is converted from the drawing code page to UTF-8 through
  `Quirewise::Charset`; R2007+ DXF strings are treated as UTF-8.

## 7. Related packages

- `Quirewise::Document` converts PDF/office inputs; the application's CAD route
  uses this package for DXF/DWG and renders the result into PDF.
- `Quirewise::Data` and `Quirewise::Charset` are used by the DXF XML/text
  paths.
- `Quirewise::Image` handles raster image inputs; this package handles vector
  SVG/CAD inputs.

## 8. License

MIT. See `LICENSE`.
