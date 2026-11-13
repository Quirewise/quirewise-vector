# DWG test fixtures

These DWG files are copied from the public `unitedrhino/go-cad` test corpus
(`testdata/line_2000.dwg`, `testdata/line_R14.dwg`, `testdata/circle_2004.dwg`)
under the Apache License 2.0. They are test inputs only; no Go or third-party
implementation code is linked into Quirewise.

- `line_2000.dwg` — AC1015, one LINE from (50,50) to (100,100).
- `line_R14.dwg` — AC1014, one LINE from (50,50) to (100,100).
- `circle_R14.dwg` — AC1014, one CIRCLE at (50,50) r=50.
- `arc_R14.dwg` — AC1014, one ARC at (75,50) r=25, 0°–180°.
- `lw_entities-2d.dwg` — AC1015, mixed common entities incl. LWPOLYLINE.
- `circle_2004.dwg` — AC1018, used to check the explicit unsupported-version
  boundary (R2004 paged containers are a later phase).
