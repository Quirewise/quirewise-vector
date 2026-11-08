#ifndef QUIREWISE_SVG_NATIVE_H
#define QUIREWISE_SVG_NATIVE_H

#include "quirewise/format.h"

#include <stddef.h>
#include <stdint.h>

typedef enum QwSvgShapeKind {
    QW_SVG_SHAPE_RECT = 1,
    QW_SVG_SHAPE_ELLIPSE,
    QW_SVG_SHAPE_LINE,
    QW_SVG_SHAPE_POLYLINE,
    QW_SVG_SHAPE_POLYGON,
    QW_SVG_SHAPE_PATH
} QwSvgShapeKind;

typedef enum QwSvgFillRule {
    QW_SVG_FILL_RULE_NONZERO = 0,
    QW_SVG_FILL_RULE_EVENODD = 1
} QwSvgFillRule;

typedef struct QwSvgColor {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t alpha;
} QwSvgColor;

typedef struct QwSvgContour {
    size_t first_point;
    size_t point_count;
    int is_closed;
} QwSvgContour;

typedef struct QwSvgShape {
    QwSvgShapeKind kind;
    double x;
    double y;
    double width;
    double height;
    double stroke_width;
    double *points;
    size_t point_count;
    QwSvgContour *contours;
    size_t contour_count;
    QwSvgColor fill;
    QwSvgColor stroke;
    QwSvgFillRule fill_rule;
    int has_fill;
    int has_stroke;
} QwSvgShape;

typedef struct QwSvgScene {
    double width;
    double height;
    QwSvgShape *shapes;
    size_t shape_count;
    size_t point_count;
} QwSvgScene;

/* Returns QW_STATUS_ERR_UNSUPPORTED when the document uses SVG features outside the
 * native scene subset. Other nonzero results describe invalid or oversized
 * input. */
int qw_svg_parse_native(const uint8_t *data, size_t size,
                        QwSvgScene **out_scene,
                        QwConvertReport *report);
void qw_svg_scene_drop(QwSvgScene *scene);

int qw_svg_render_native(const QwSvgScene *scene,
                         uint8_t **out_rgba,
                         uint32_t *out_width,
                         uint32_t *out_height,
                         QwConvertReport *report);

#endif
