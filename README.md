# quirewise-vector

Standalone Quirewise vector library: SVG/SVGZ and CAD (DXF) reading and
rasterisation.

CMake target: `Quirewise::Vector`. Public headers are under
`include/quirewise/`.

## Capabilities

- SVG/SVGZ: native SVG subset parsing; SVGZ gzip decompression; RGBA8
  rasterisation at 2x document units.
- CAD: `quirewise/cad.h` parses DXF ASCII and binary files with header, layer
  table, blocks and the common 2D entity families (LINE, LWPOLYLINE, POLYLINE,
  CIRCLE, ARC, ELLIPSE, SPLINE control points, TEXT/MTEXT, INSERT with nested
  transforms, POINT, SOLID/TRACE/3DFACE, LEADER and block-carried DIMENSION).
  It exposes bounds, a text inventory and two renderers: RGBA8 raster and SVG
  with real `<text>` elements.
- DWG: R13/R14/R2000 (AC1012/AC1014/AC1015) segment directory, object map,
  object records, common entity head and LINE/CIRCLE/ARC/LWPOLYLINE/TEXT
  geometry and text; R2004 (AC1018) adds the encrypted container header, page
  map, section map, LZ77 data pages and `AcDb:Handles` object index; R2007
  (AC1021) adds RS deinterleaving, R21 decompression and its section/data-page
  layout. R2010+ is version-detected only and returns `UNSUPPORTED`. The
  implementation is modelled on `unitedrhino/go-cad` (Apache-2.0) but contains
  no Go or third-party CAD code.

## Boundaries

- The raster CAD renderer has no glyph engine yet; TEXT/MTEXT rasterise as
  placeholder bars. Use the SVG renderer when readable text matters.
- HATCH, ACIS/3D solids, proxy objects and exact NURBS splines are not
  expanded.
- DXF text is converted from `$DWGCODEPAGE` to UTF-8 through
  `Quirewise::Charset`; R2007+ DXF strings are treated as UTF-8.

## Tests

`ctest` runs the SVG fuzz smoke and the CAD DXF test (ASCII, binary, block
insert expansion, bounds, text inventory, raster ink and SVG output).

Build with `cmake -S . -B build` and `cmake --build build`.
