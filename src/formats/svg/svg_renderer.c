#include "svg_native.h"

#include "../../core/byte_utils.h"
#include "quirewise/status.h"
#include "svg_limits.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { QW_SVG_NATIVE_MAX_RASTER_WORK = 120000000u };

static void svg_composite(uint8_t *pixel, const QwSvgColor *color)
{
    uint32_t alpha = color->alpha;
    uint32_t inverse = 255u - alpha;
    uint32_t destination_alpha = pixel[3];
    uint32_t output_alpha = alpha +
        (destination_alpha * inverse + 127u) / 255u;
    if (!output_alpha) return;
    pixel[0] = (uint8_t)(((uint32_t)color->red * alpha +
        ((uint32_t)pixel[0] * destination_alpha * inverse + 127u) / 255u +
        output_alpha / 2u) / output_alpha);
    pixel[1] = (uint8_t)(((uint32_t)color->green * alpha +
        ((uint32_t)pixel[1] * destination_alpha * inverse + 127u) / 255u +
        output_alpha / 2u) / output_alpha);
    pixel[2] = (uint8_t)(((uint32_t)color->blue * alpha +
        ((uint32_t)pixel[2] * destination_alpha * inverse + 127u) / 255u +
        output_alpha / 2u) / output_alpha);
    pixel[3] = (uint8_t)output_alpha;
}

static double svg_distance_to_segment_squared(double px, double py,
                                               double x1, double y1,
                                               double x2, double y2)
{
    double dx = x2 - x1;
    double dy = y2 - y1;
    double length_squared = dx * dx + dy * dy;
    double t = 0.0;
    if (length_squared > 0.0) {
        t = ((px - x1) * dx + (py - y1) * dy) / length_squared;
        if (t < 0.0) t = 0.0;
        else if (t > 1.0) t = 1.0;
    }
    dx = px - (x1 + t * (x2 - x1));
    dy = py - (y1 + t * (y2 - y1));
    return dx * dx + dy * dy;
}

static int svg_polygon_contains(const QwSvgShape *shape, double x, double y)
{
    int winding = 0;
    int crossing_parity = 0;
    size_t contour_index;
    for (contour_index = 0u; contour_index < shape->contour_count;
         ++contour_index) {
        const QwSvgContour *contour = &shape->contours[contour_index];
        size_t i;
        if (!contour->point_count) continue;
        for (i = 0u; i < contour->point_count; ++i) {
            size_t next = (i + 1u) % contour->point_count;
            size_t point = contour->first_point + i;
            size_t next_point = contour->first_point + next;
            double x1 = shape->points[point * 2u];
            double y1 = shape->points[point * 2u + 1u];
            double x2 = shape->points[next_point * 2u];
            double y2 = shape->points[next_point * 2u + 1u];
            double cross = (x2 - x1) * (y - y1) - (x - x1) * (y2 - y1);
            if (y1 <= y) {
                if (y2 > y && cross > 0.0) {
                    ++winding;
                    crossing_parity ^= 1;
                }
            } else if (y2 <= y && cross < 0.0) {
                --winding;
                crossing_parity ^= 1;
            }
        }
    }
    return shape->fill_rule == QW_SVG_FILL_RULE_EVENODD
        ? crossing_parity : winding != 0;
}

static size_t svg_shape_stroke_segment_count(const QwSvgShape *shape)
{
    size_t segment_count = 0u;
    size_t contour_index;
    for (contour_index = 0u; contour_index < shape->contour_count;
         ++contour_index) {
        const QwSvgContour *contour = &shape->contours[contour_index];
        if (contour->is_closed) segment_count += contour->point_count;
        else if (contour->point_count > 0u)
            segment_count += contour->point_count - 1u;
    }
    return segment_count;
}

static void svg_shape_bounds(const QwSvgShape *shape,
                             double *min_x, double *min_y,
                             double *max_x, double *max_y)
{
    size_t i;
    if (shape->kind == QW_SVG_SHAPE_RECT) {
        *min_x = shape->x;
        *min_y = shape->y;
        *max_x = shape->x + shape->width;
        *max_y = shape->y + shape->height;
    } else if (shape->kind == QW_SVG_SHAPE_ELLIPSE) {
        *min_x = shape->x - shape->width;
        *min_y = shape->y - shape->height;
        *max_x = shape->x + shape->width;
        *max_y = shape->y + shape->height;
    } else {
        *min_x = *max_x = shape->points[0];
        *min_y = *max_y = shape->points[1];
        for (i = 1u; i < shape->point_count; ++i) {
            double x = shape->points[i * 2u];
            double y = shape->points[i * 2u + 1u];
            if (x < *min_x) *min_x = x;
            if (x > *max_x) *max_x = x;
            if (y < *min_y) *min_y = y;
            if (y > *max_y) *max_y = y;
        }
    }
    if (shape->has_stroke) {
        double pad = shape->stroke_width * 0.5;
        *min_x -= pad;
        *min_y -= pad;
        *max_x += pad;
        *max_y += pad;
    }
}

static int svg_shape_fill_contains(const QwSvgShape *shape, double x, double y)
{
    if (shape->kind == QW_SVG_SHAPE_RECT) {
        return x >= shape->x && x <= shape->x + shape->width &&
               y >= shape->y && y <= shape->y + shape->height;
    }
    if (shape->kind == QW_SVG_SHAPE_ELLIPSE) {
        double dx = (x - shape->x) / shape->width;
        double dy = (y - shape->y) / shape->height;
        return dx * dx + dy * dy <= 1.0;
    }
    if (shape->kind == QW_SVG_SHAPE_POLYGON ||
        shape->kind == QW_SVG_SHAPE_POLYLINE ||
        shape->kind == QW_SVG_SHAPE_PATH)
        return svg_polygon_contains(shape, x, y);
    return 0;
}

static int svg_shape_stroke_contains(const QwSvgShape *shape,
                                     double x, double y)
{
    double half_width = shape->stroke_width * 0.5;
    double threshold = half_width * half_width;
    size_t contour_index;
    if (!shape->has_stroke || !(half_width > 0.0)) return 0;
    if (shape->kind == QW_SVG_SHAPE_RECT) {
        double x1 = shape->x;
        double y1 = shape->y;
        double x2 = shape->x + shape->width;
        double y2 = shape->y + shape->height;
        return svg_distance_to_segment_squared(x, y, x1, y1, x2, y1) <= threshold ||
               svg_distance_to_segment_squared(x, y, x2, y1, x2, y2) <= threshold ||
               svg_distance_to_segment_squared(x, y, x2, y2, x1, y2) <= threshold ||
               svg_distance_to_segment_squared(x, y, x1, y2, x1, y1) <= threshold;
    }
    for (contour_index = 0u; contour_index < shape->contour_count;
         ++contour_index) {
        const QwSvgContour *contour = &shape->contours[contour_index];
        size_t segments = contour->is_closed
            ? contour->point_count
            : (contour->point_count > 0u ? contour->point_count - 1u : 0u);
        size_t i;
        for (i = 0u; i < segments; ++i) {
            size_t next = (i + 1u) % contour->point_count;
            size_t point = contour->first_point + i;
            size_t next_point = contour->first_point + next;
            if (svg_distance_to_segment_squared(
                    x, y, shape->points[point * 2u],
                    shape->points[point * 2u + 1u],
                    shape->points[next_point * 2u],
                    shape->points[next_point * 2u + 1u]) <= threshold)
                return 1;
        }
    }
    return 0;
}

static int svg_render_shape(const QwSvgShape *shape, uint8_t *rgba,
                            uint32_t width, uint32_t height)
{
    double min_x;
    double min_y;
    double max_x;
    double max_y;
    int x0;
    int y0;
    int x1;
    int y1;
    svg_shape_bounds(shape, &min_x, &min_y, &max_x, &max_y);
    if (!isfinite(min_x) || !isfinite(min_y) || !isfinite(max_x) ||
        !isfinite(max_y)) return 0;
    if (max_x <= 0.0 || max_y <= 0.0 || min_x >= (double)width * 0.5 ||
        min_y >= (double)height * 0.5) return 1;
    if (min_x < 0.0) min_x = 0.0;
    if (min_y < 0.0) min_y = 0.0;
    if (max_x > (double)width * 0.5) max_x = (double)width * 0.5;
    if (max_y > (double)height * 0.5) max_y = (double)height * 0.5;
    x0 = (int)floor(min_x * 2.0);
    y0 = (int)floor(min_y * 2.0);
    x1 = (int)ceil(max_x * 2.0);
    y1 = (int)ceil(max_y * 2.0);
    if (x0 >= x1 || y0 >= y1) return 1;
    for (int py = y0; py < y1; ++py) {
        double sy = ((double)py + 0.5) * 0.5;
        for (int px = x0; px < x1; ++px) {
            double sx = ((double)px + 0.5) * 0.5;
            uint8_t *pixel = rgba + ((size_t)py * width + (size_t)px) * 4u;
            if (shape->has_fill && svg_shape_fill_contains(shape, sx, sy))
                svg_composite(pixel, &shape->fill);
            if (shape->has_stroke && svg_shape_stroke_contains(shape, sx, sy))
                svg_composite(pixel, &shape->stroke);
        }
    }
    return 1;
}

static int svg_shape_work(const QwSvgShape *shape, uint32_t width,
                          uint32_t height, uint64_t *out_work)
{
    double min_x;
    double min_y;
    double max_x;
    double max_y;
    double pixel_x0;
    double pixel_y0;
    double pixel_x1;
    double pixel_y1;
    uint64_t area;
    uint64_t factor = 1u;
    if (!out_work) return 0;
    svg_shape_bounds(shape, &min_x, &min_y, &max_x, &max_y);
    if (!isfinite(min_x) || !isfinite(min_y) || !isfinite(max_x) ||
        !isfinite(max_y)) return 0;
    if (max_x <= 0.0 || max_y <= 0.0 || min_x >= (double)width * 0.5 ||
        min_y >= (double)height * 0.5) {
        *out_work = 0u;
        return 1;
    }
    if (min_x < 0.0) min_x = 0.0;
    if (min_y < 0.0) min_y = 0.0;
    if (max_x > (double)width * 0.5) max_x = (double)width * 0.5;
    if (max_y > (double)height * 0.5) max_y = (double)height * 0.5;
    pixel_x0 = floor(min_x * 2.0);
    pixel_y0 = floor(min_y * 2.0);
    pixel_x1 = ceil(max_x * 2.0);
    pixel_y1 = ceil(max_y * 2.0);
    if (pixel_x1 <= pixel_x0 || pixel_y1 <= pixel_y0) {
        *out_work = 0u;
        return 1;
    }
    if ((shape->kind == QW_SVG_SHAPE_POLYGON ||
         shape->kind == QW_SVG_SHAPE_POLYLINE ||
         shape->kind == QW_SVG_SHAPE_PATH) && shape->has_fill)
        factor += (uint64_t)shape->point_count;
    if (shape->has_stroke) {
        if (shape->kind == QW_SVG_SHAPE_RECT) factor += 4u;
        else factor += (uint64_t)svg_shape_stroke_segment_count(shape);
    }
    area = (uint64_t)(pixel_x1 - pixel_x0) *
           (uint64_t)(pixel_y1 - pixel_y0);
    if (factor && area > UINT64_MAX / factor) return 0;
    *out_work = area * factor;
    return 1;
}

int qw_svg_render_native(const QwSvgScene *scene,
                         uint8_t **out_rgba,
                         uint32_t *out_width,
                         uint32_t *out_height,
                         QwConvertReport *report)
{
    double scaled_width;
    double scaled_height;
    uint32_t width;
    uint32_t height;
    size_t pixel_count;
    size_t rgba_size;
    uint8_t *rgba;
    size_t i;
    uint64_t raster_work = 0u;
    if (out_rgba) *out_rgba = NULL;
    if (out_width) *out_width = 0u;
    if (out_height) *out_height = 0u;
    if (!scene || !out_rgba || !out_width || !out_height) {
        if (report) (void)snprintf(report->note, sizeof(report->note),
                                   "SVG native renderer input is incomplete");
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    scaled_width = ceil(scene->width * 2.0);
    scaled_height = ceil(scene->height * 2.0);
    if (!(scaled_width > 0.0) || !(scaled_height > 0.0) ||
        scaled_width > 100000.0 || scaled_height > 100000.0 ||
        scaled_width * scaled_height > (double)QW_SVG_MAX_PIXELS) {
        if (report) (void)snprintf(report->note, sizeof(report->note),
                                   "SVG dimensions exceed the raster pixel limit");
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    width = (uint32_t)scaled_width;
    height = (uint32_t)scaled_height;
    if (!qw_checked_size_mul((size_t)width, (size_t)height, &pixel_count) ||
        !qw_checked_size_mul(pixel_count, 4u, &rgba_size)) {
        if (report) (void)snprintf(report->note, sizeof(report->note),
                                   "SVG raster size overflow");
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    for (i = 0u; i < scene->shape_count; ++i) {
        uint64_t shape_work;
        if (!svg_shape_work(&scene->shapes[i], width, height, &shape_work) ||
            shape_work > QW_SVG_NATIVE_MAX_RASTER_WORK - raster_work) {
            if (report) (void)snprintf(report->note, sizeof(report->note),
                                       "SVG native raster work limit exceeded");
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        }
        raster_work += shape_work;
    }
    rgba = (uint8_t *)malloc(rgba_size ? rgba_size : 1u);
    if (!rgba) {
        if (report) (void)snprintf(report->note, sizeof(report->note),
                                   "Could not allocate SVG raster buffer");
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    for (i = 0u; i < pixel_count; ++i) {
        rgba[i * 4u] = 0u;
        rgba[i * 4u + 1u] = 0u;
        rgba[i * 4u + 2u] = 0u;
        rgba[i * 4u + 3u] = 0u;
    }
    for (i = 0u; i < scene->shape_count; ++i) {
        if (!svg_render_shape(&scene->shapes[i], rgba, width, height)) {
            free(rgba);
            if (report) (void)snprintf(report->note, sizeof(report->note),
                                       "SVG shape exceeds raster coordinates");
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        }
    }
    *out_rgba = rgba;
    *out_width = width;
    *out_height = height;
    return QW_STATUS_OK;
}
