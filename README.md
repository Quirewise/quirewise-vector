# quirewise-vector

SVG/SVGZ rasterisation and CAD (DXF/DWG) parsing, text extraction and rendering.

`Quirewise::Vector` is an independent, MIT-licensed C11 library, installed as
the `QuirewiseVector` CMake package (target `Quirewise::Vector`). It can be
consumed without the Quirewise application.

## What it does

- **SVG/SVGZ**: parses a native SVG subset and rasterises it to RGBA8 at 2x
  document units; decompresses a bounded gzip SVGZ member to SVG bytes.
- **DXF**: parses ASCII and binary DXF with header, layers, blocks and common
  2D entities (LINE, LWPOLYLINE, POLYLINE, CIRCLE, ARC, ELLIPSE, SPLINE control
  points, TEXT/MTEXT, INSERT with nested transforms, POINT, SOLID/TRACE/3DFACE,
  LEADER and block-carried DIMENSION).
- **DWG**: decodes R13/R14/R2000/R2004/R2007/R2010/R2013/R2018 containers and
  the common LINE/CIRCLE/ARC/LWPOLYLINE/TEXT geometry.
- Reports entity/layer counts, model bounds and a text inventory (string,
  position, height, rotation, layer).
- Renders the CAD model as RGBA8 raster or as SVG with real `<text>` elements.

Deliberate boundaries: the raster CAD renderer has no glyph engine, so
TEXT/MTEXT become placeholder bars; use the SVG renderer when readable text
matters. HATCH, ACIS/3D solids, proxy objects and exact NURBS splines are not
expanded. SVG support is a documented subset, not the full SVG specification.
There is no SVG/CAD writer beyond the CAD SVG export.

## Requirements

- C11 compiler and CMake 3.21 or newer.
- Installed Quirewise packages: `QuirewiseRuntime`, `QuirewiseData`,
  `QuirewiseCompression` and `QuirewiseCharset`; the exported config resolves
  them. No third-party libraries.
- CTest for the unit and fuzz tests.

## Build, test and install

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/quirewise/install
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/quirewise/install
```

Shared builds are available with `-DQUIREWISE_BUILD_SHARED=ON`.

## Use it from CMake

```cmake
find_package(QuirewiseVector CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE Quirewise::Vector)
```

## Quick start

Parse a DXF/DWG file, print its text inventory, and write SVG:

```c
#include <quirewise/cad.h>

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    QwCadDocument *document = NULL;
    QwCadTextInfo text;
    char *svg = NULL;
    size_t svg_size = 0u;
    size_t i;
    FILE *file;
    long length;
    unsigned char *data;

    if (argc != 2) return 1;
    file = fopen(argv[1], "rb");
    if (!file) return 1;
    fseek(file, 0L, SEEK_END);
    length = ftell(file);
    rewind(file);
    data = length > 0L ? malloc((size_t)length) : NULL;
    if (!data || fread(data, 1u, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return 1;
    }
    fclose(file);

    if (!qw_cad_probe(data, (size_t)length) ||
        qw_cad_parse(data, (size_t)length, &document) != QW_CAD_OK) {
        fprintf(stderr, "CAD parse failed\n");
        free(data);
        return 1;
    }
    printf("%zu entities, %zu layers, %zu text items\n",
           qw_cad_entity_count(document), qw_cad_layer_count(document),
           qw_cad_text_count(document));
    for (i = 0u; i < qw_cad_text_count(document); ++i) {
        if (qw_cad_text_at(document, i, &text) == QW_CAD_OK)
            printf("  %.*s\n", (int)text.text_size, text.text);
    }
    if (qw_cad_render_svg(document, &svg, &svg_size) == QW_CAD_OK) {
        fwrite(svg, 1u, svg_size, stdout);
        qw_cad_buffer_free(svg);
    }
    qw_cad_document_free(document);
    free(data);
    return 0;
}
```

For SVG input, use `qw_svg_decode` and release the image with
`qw_svg_image_free`. See [docs/usage.md](docs/usage.md) for the SVG/SVGZ
recipes, bounds, raster rendering and error tables.

## API overview

| Header | Purpose |
|---|---|
| `quirewise/svg.h` | SVG decode/render, `QwSvgImage`, `QwSvgError` |
| `quirewise/svgz.h` | SVGZ gzip decompression, `QwSvgzDocument` |
| `quirewise/cad.h` | DXF/DWG probe, parse, model queries, text, raster/SVG render |

| Task | Functions |
|---|---|
| SVG render | `qw_svg_decode`, `qw_svg_image_free`, `qw_svg_status_string` |
| SVGZ decompress | `qw_svgz_decode`, `qw_svgz_document_free`, `qw_svgz_status_string` |
| CAD probe/parse | `qw_cad_probe`, `qw_cad_parse`, `qw_cad_format` |
| CAD model | `qw_cad_entity_count`, `qw_cad_layer_count`, `qw_cad_bounds` |
| CAD text | `qw_cad_text_count`, `qw_cad_text_at` |
| CAD render | `qw_cad_render_rgba`, `qw_cad_render_svg` |
| Release | `qw_cad_document_free`, `qw_cad_image_free`, `qw_cad_buffer_free` |

## Behavior and limits

- SVG input bytes are borrowed for the call. `QwSvgImage.pixels` is owned by the
  caller and released with `qw_svg_image_free`; output is RGBA8 with stride.
- SVGZ output `svg_utf8` is owned by the `QwSvgzDocument`; release it with
  `qw_svgz_document_free`. One gzip member is accepted.
- CAD input bytes are borrowed only for `qw_cad_parse`. The returned document
  owns its model; text strings inside `QwCadTextInfo` are borrowed until
  `qw_cad_document_free`.
- `qw_cad_render_svg` allocates the SVG buffer; release it with
  `qw_cad_buffer_free`. `qw_cad_render_rgba` allocates pixels; release them with
  `qw_cad_image_free`.
- Status values are format-specific enums (`QwSvgStatus`, `QwSvgzStatus`,
  `QwCadStatus`), not the shared `QwStatus`. `QW_CAD_ERR_UNSUPPORTED` means the
  container was recognised but its entity/version subset is not renderable.
- The raster renderer draws text as bars by design; the SVG renderer emits real
  text elements. HATCH/ACIS/3D solids and exact splines are not expanded.

## Testing

`ctest` runs the SVG fuzz smoke and the CAD DXF test (ASCII, binary, block
insert expansion, bounds, text inventory, raster ink and SVG output). The DWG
paths are exercised by the application and acceptance fixtures.

## Documentation

See [docs/usage.md](docs/usage.md) for SVG/SVGZ and CAD recipes, ownership,
status handling and limits.

## License

MIT. See [LICENSE](LICENSE).
