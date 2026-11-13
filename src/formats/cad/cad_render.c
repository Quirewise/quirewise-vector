#include "formats/cad/cad_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct SvgBuffer {
    char *data;
    size_t size;
    size_t capacity;
    int failed;
} SvgBuffer;

static int svg_reserve(SvgBuffer *buffer, size_t extra)
{
    size_t need;
    if (buffer->failed) return 0;
    need = buffer->size + extra + 1u;
    if (need > buffer->capacity) {
        size_t next = buffer->capacity ? buffer->capacity : 1024u;
        char *grown;
        while (next < need) next *= 2u;
        grown = (char *)realloc(buffer->data, next);
        if (!grown) {
            buffer->failed = 1;
            return 0;
        }
        buffer->data = grown;
        buffer->capacity = next;
    }
    return 1;
}

static void svg_append(SvgBuffer *buffer, const char *text)
{
    size_t length = strlen(text);
    if (!svg_reserve(buffer, length)) return;
    memcpy(buffer->data + buffer->size, text, length);
    buffer->size += length;
    buffer->data[buffer->size] = '\0';
}

static void svg_printf(SvgBuffer *buffer, const char *format, ...)
{
    va_list args;
    va_list copy;
    int needed;
    va_start(args, format);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0 || !svg_reserve(buffer, (size_t)needed)) {
        va_end(args);
        return;
    }
    vsnprintf(buffer->data + buffer->size, (size_t)needed + 1u, format, args);
    buffer->size += (size_t)needed;
    va_end(args);
}

static void svg_escape(SvgBuffer *buffer, const char *text, size_t size)
{
    size_t index;
    for (index = 0u; index < size; ++index) {
        char ch = text[index];
        if (ch == '&') svg_append(buffer, "&amp;");
        else if (ch == '<') svg_append(buffer, "&lt;");
        else if (ch == '>') svg_append(buffer, "&gt;");
        else if (ch == '"') svg_append(buffer, "&quot;");
        else if (ch == '\'') svg_append(buffer, "&apos;");
        else if (ch == '\n' || ch == '\r') svg_append(buffer, " ");
        else {
            char single[2] = {ch, '\0'};
            svg_append(buffer, single);
        }
    }
}

static uint32_t entity_color(const QwCadDocument *document,
                             const QwCadEntity *entity)
{
    uint32_t color;
    if (entity->has_color) color = entity->color;
    else if (entity->layer < document->layer_count)
        color = document->layers[entity->layer].color;
    else
        color = 0x00000000u;
    return color & 0x00FFFFFFu;
}

static void color_hex(uint32_t color, char *out, size_t capacity)
{
    snprintf(out, capacity, "#%02X%02X%02X", (unsigned)((color >> 16) & 0xFFu),
             (unsigned)((color >> 8) & 0xFFu), (unsigned)(color & 0xFFu));
}

static void put_pixel(QwCadImage *image, int x, int y, uint32_t color)
{
    uint8_t *pixel;
    if (x < 0 || y < 0 || (uint32_t)x >= image->width ||
        (uint32_t)y >= image->height)
        return;
    pixel = image->pixels + (size_t)y * image->stride + (size_t)x * 4u;
    pixel[0] = (uint8_t)((color >> 16) & 0xFFu);
    pixel[1] = (uint8_t)((color >> 8) & 0xFFu);
    pixel[2] = (uint8_t)(color & 0xFFu);
    pixel[3] = 0xFFu;
}

static void draw_line(QwCadImage *image, double x0, double y0, double x1,
                      double y1, uint32_t color)
{
    int ix0 = (int)lround(x0);
    int iy0 = (int)lround(y0);
    int ix1 = (int)lround(x1);
    int iy1 = (int)lround(y1);
    int dx = abs(ix1 - ix0);
    int sx = ix0 < ix1 ? 1 : -1;
    int dy = -abs(iy1 - iy0);
    int sy = iy0 < iy1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        put_pixel(image, ix0, iy0, color);
        if (ix0 == ix1 && iy0 == iy1) break;
        {
            int twice = 2 * error;
            if (twice >= dy) {
                error += dy;
                ix0 += sx;
            }
            if (twice <= dx) {
                error += dx;
                iy0 += sy;
            }
        }
    }
}

static void draw_circle(QwCadImage *image, double cx, double cy, double radius,
                        uint32_t color)
{
    int step;
    double previous_x = cx + radius;
    double previous_y = cy;
    for (step = 1; step <= 96; ++step) {
        double angle = 2.0 * M_PI * step / 96.0;
        double x = cx + radius * cos(angle);
        double y = cy + radius * sin(angle);
        draw_line(image, previous_x, previous_y, x, y, color);
        previous_x = x;
        previous_y = y;
    }
}

static void draw_arc(QwCadImage *image, double cx, double cy, double radius,
                     double start_degrees, double end_degrees, uint32_t color)
{
    double sweep = end_degrees - start_degrees;
    int step;
    double previous_x;
    double previous_y;
    while (sweep < 0.0) sweep += 360.0;
    if (sweep < 1e-9) sweep = 360.0;
    previous_x = cx + radius * cos(start_degrees * M_PI / 180.0);
    previous_y = cy + radius * sin(start_degrees * M_PI / 180.0);
    for (step = 1; step <= 96; ++step) {
        double degrees = start_degrees + sweep * step / 96.0;
        double x = cx + radius * cos(degrees * M_PI / 180.0);
        double y = cy + radius * sin(degrees * M_PI / 180.0);
        draw_line(image, previous_x, previous_y, x, y, color);
        previous_x = x;
        previous_y = y;
    }
}

static void draw_ellipse(QwCadImage *image, const QwCadEntity *entity,
                         uint32_t color)
{
    double major_x = entity->p[1].x;
    double major_y = entity->p[1].y;
    double major = sqrt(major_x * major_x + major_y * major_y);
    double angle = atan2(major_y, major_x);
    double minor = major * (entity->ratio > 0.0 ? entity->ratio : 1.0);
    double start = entity->start_angle;
    double sweep = entity->end_angle - start;
    int step;
    double previous_x = 0.0;
    double previous_y = 0.0;
    if (sweep <= 0.0) sweep = 2.0 * M_PI;
    for (step = 0; step <= 96; ++step) {
        double t = start + sweep * step / 96.0;
        double x = major * cos(t);
        double y = minor * sin(t);
        double rx = x * cos(angle) - y * sin(angle);
        double ry = x * sin(angle) + y * cos(angle);
        double px = entity->p[0].x + rx;
        double py = entity->p[0].y + ry;
        if (step > 0) draw_line(image, previous_x, previous_y, px, py, color);
        previous_x = px;
        previous_y = py;
    }
}

static void fill_polygon(QwCadImage *image, const QwCadPoint *points,
                         size_t count, double scale, double min_x,
                         double min_y, double top, double margin,
                         uint32_t color)
{
    double xs[4];
    int min_y_px;
    int max_y_px;
    int y;
    size_t index;
    if (count < 3u) return;
    min_y_px = 32767;
    max_y_px = -32768;
    for (index = 0u; index < count; ++index) {
        double py = top - (margin + (points[index].y - min_y) * scale);
        int pyi = (int)floor(py);
        if (pyi < min_y_px) min_y_px = pyi;
        if (pyi > max_y_px) max_y_px = pyi;
    }
    if (min_y_px < 0) min_y_px = 0;
    if (max_y_px >= (int)image->height) max_y_px = (int)image->height - 1;
    for (y = min_y_px; y <= max_y_px; ++y) {
        size_t intersections = 0u;
        for (index = 0u; index < count; ++index) {
            const QwCadPoint *a = &points[index];
            const QwCadPoint *b = &points[(index + 1u) % count];
            double ay = a->y;
            double by = b->y;
            double ax;
            double bx;
            if ((ay <= (double)y && by > (double)y) ||
                (by <= (double)y && ay > (double)y)) {
                double t = ((double)y - ay) / (by - ay);
                ax = margin + (a->x - min_x) * scale;
                bx = margin + (b->x - min_x) * scale;
                if (intersections < 4u)
                    xs[intersections] = ax + t * (bx - ax);
                ++intersections;
            }
        }
        if (intersections >= 2u) {
            size_t left;
            size_t right;
            if (xs[0] > xs[1]) {
                double swap = xs[0];
                xs[0] = xs[1];
                xs[1] = swap;
            }
            left = (size_t)(xs[0] < 0.0 ? 0.0 : xs[0]);
            right = (size_t)(xs[1] > (double)image->width
                                 ? (double)image->width - 1.0
                                 : xs[1]);
            for (; left <= right && left < image->width; ++left)
                put_pixel(image, (int)left, y, color);
        }
    }
}

static void fill_polygon_generic(QwCadImage *image, const QwCadPoint *points,
                                 size_t count, double scale, double min_x,
                                 double min_y, double top, double margin,
                                 uint32_t color)
{
    double *xs;
    int min_y_px = 32767;
    int max_y_px = -32768;
    int y;
    size_t index;
    if (count < 3u || count > 100000u) return;
    xs = (double *)malloc(count * sizeof(*xs));
    if (!xs) return;
    for (index = 0u; index < count; ++index) {
        double py = top - (margin + (points[index].y - min_y) * scale);
        int pyi = (int)floor(py);
        if (pyi < min_y_px) min_y_px = pyi;
        if (pyi > max_y_px) max_y_px = pyi;
    }
    if (min_y_px < 0) min_y_px = 0;
    if (max_y_px >= (int)image->height) max_y_px = (int)image->height - 1;
    for (y = min_y_px; y <= max_y_px; ++y) {
        size_t intersections = 0u;
        size_t left_index;
        for (index = 0u; index < count; ++index) {
            const QwCadPoint *a = &points[index];
            const QwCadPoint *b = &points[(index + 1u) % count];
            double ay = top - (margin + (a->y - min_y) * scale);
            double by = top - (margin + (b->y - min_y) * scale);
            if ((ay <= (double)y && by > (double)y) ||
                (by <= (double)y && ay > (double)y)) {
                double t = ((double)y - ay) / (by - ay);
                double ax = margin + (a->x - min_x) * scale;
                double bx = margin + (b->x - min_x) * scale;
                xs[intersections++] = ax + t * (bx - ax);
            }
        }
        for (left_index = 0u; left_index + 1u < intersections; ++left_index) {
            size_t right_index = left_index + 1u;
            while (right_index > 0u &&
                   xs[right_index - 1u] > xs[right_index]) {
                double swap = xs[right_index - 1u];
                xs[right_index - 1u] = xs[right_index];
                xs[right_index] = swap;
                --right_index;
            }
        }
        for (index = 0u; index + 1u < intersections; index += 2u) {
            double left = xs[index];
            double right = xs[index + 1u];
            int left_px;
            int right_px;
            if (left < 0.0) left = 0.0;
            if (right > (double)image->width - 1.0)
                right = (double)image->width - 1.0;
            left_px = (int)ceil(left);
            right_px = (int)floor(right);
            for (; left_px <= right_px; ++left_px)
                put_pixel(image, left_px, y, color);
        }
    }
    free(xs);
}
static void draw_text_placeholder(QwCadImage *image, const QwCadEntity *entity,
                                  double x, double y, double scale,
                                  uint32_t color)
{
    size_t characters = 0u;
    size_t index;
    double width;
    if (entity->text) {
        for (index = 0u; entity->text[index]; ++index) {
            if (((unsigned char)entity->text[index] & 0xC0u) != 0x80u)
                ++characters;
        }
    }
    width = (double)(characters ? characters : 1u) * entity->height *
            0.6 * scale;
    draw_line(image, x, y, x + width, y, color);
    draw_line(image, x, y, x, y - entity->height * scale, color);
}

QwCadStatus qw_cad_render_rgba(const QwCadDocument *document, uint32_t width,
                               QwCadImage *out_image)
{
    QwCadBounds bounds;
    double range_x;
    double range_y;
    double scale;
    double margin = 8.0;
    uint32_t height;
    size_t index;
    uint8_t *pixels;
    QwCadImage image;
    if (!document || !out_image) return QW_CAD_ERR_INVALID_ARGUMENT;
    if (width < 16u || width > 16384u) return QW_CAD_ERR_INVALID_ARGUMENT;
    memset(out_image, 0, sizeof(*out_image));
    qw_cad_bounds(document, &bounds);
    range_x = bounds.max_x - bounds.min_x;
    range_y = bounds.max_y - bounds.min_y;
    if (range_x <= 0.0) range_x = 1.0;
    if (range_y <= 0.0) range_y = 1.0;
    scale = ((double)width - 2.0 * margin) / range_x;
    height = (uint32_t)ceil(range_y * scale + 2.0 * margin);
    if (height < 16u) height = 16u;
    if (height > 16384u) {
        height = 16384u;
        scale = ((double)height - 2.0 * margin) / range_y;
        width = (uint32_t)ceil(range_x * scale + 2.0 * margin);
        if (width < 16u) width = 16u;
        if (width > 16384u) width = 16384u;
    }
    if ((uint64_t)width * height > 20000000u) return QW_CAD_ERR_RESOURCE_LIMIT;
    {
        size_t stride = (size_t)width * 4u;
        pixels = (uint8_t *)malloc(stride * height);
        if (!pixels) return QW_CAD_ERR_OUT_OF_MEMORY;
        memset(pixels, 0xFF, stride * height);
        image.pixels = pixels;
        image.size = stride * height;
        image.stride = stride;
        image.width = width;
        image.height = height;
    }
    for (index = 0u; index < document->entity_count; ++index) {
        const QwCadEntity *entity = &document->entities[index];
        uint32_t color = entity_color(document, entity);
        if (entity->kind == QW_CAD_ENTITY_TEXT) {
            double x = margin + (entity->p[0].x - bounds.min_x) * scale;
            double y = (double)height -
                       (margin + (entity->p[0].y - bounds.min_y) * scale);
            draw_text_placeholder(&image, entity, x, y, scale, color);
            continue;
        }
        if (entity->kind == QW_CAD_ENTITY_LINE) {
            double x0 = margin + (entity->p[0].x - bounds.min_x) * scale;
            double y0 = (double)height -
                        (margin + (entity->p[0].y - bounds.min_y) * scale);
            double x1 = margin + (entity->p[1].x - bounds.min_x) * scale;
            double y1 = (double)height -
                        (margin + (entity->p[1].y - bounds.min_y) * scale);
            draw_line(&image, x0, y0, x1, y1, color);
        } else if (entity->kind == QW_CAD_ENTITY_CIRCLE) {
            double x = margin + (entity->p[0].x - bounds.min_x) * scale;
            double y = (double)height -
                       (margin + (entity->p[0].y - bounds.min_y) * scale);
            draw_circle(&image, x, y, entity->radius * scale, color);
        } else if (entity->kind == QW_CAD_ENTITY_ARC) {
            double x = margin + (entity->p[0].x - bounds.min_x) * scale;
            double y = (double)height -
                       (margin + (entity->p[0].y - bounds.min_y) * scale);
            draw_arc(&image, x, y, entity->radius * scale,
                     entity->start_angle, entity->end_angle, color);
        } else if (entity->kind == QW_CAD_ENTITY_ELLIPSE) {
            QwCadEntity scaled = *entity;
            scaled.p[0].x = margin + (entity->p[0].x - bounds.min_x) * scale;
            scaled.p[0].y = (double)height -
                            (margin + (entity->p[0].y - bounds.min_y) * scale);
            scaled.p[1].x = entity->p[1].x * scale;
            scaled.p[1].y = entity->p[1].y * scale;
            draw_ellipse(&image, &scaled, color);
        } else if (entity->kind == QW_CAD_ENTITY_POLYLINE ||
                   entity->kind == QW_CAD_ENTITY_SPLINE) {
            size_t point;
            if (entity->filled && entity->point_count >= 3u) {
                fill_polygon_generic(&image, entity->points,
                                     entity->point_count, scale, bounds.min_x,
                                     bounds.min_y, (double)height, margin,
                                     color);
            }
            if (entity->point_count == 1u) {
                double x = margin +
                           (entity->points[0].x - bounds.min_x) * scale;
                double y = (double)height -
                           (margin +
                            (entity->points[0].y - bounds.min_y) * scale);
                put_pixel(&image, (int)x, (int)y, color);
            }
            for (point = 1u; point < entity->point_count; ++point) {
                double x0 = margin +
                            (entity->points[point - 1u].x - bounds.min_x) *
                                scale;
                double y0 = (double)height -
                            (margin +
                             (entity->points[point - 1u].y - bounds.min_y) *
                                 scale);
                double x1 = margin +
                            (entity->points[point].x - bounds.min_x) * scale;
                double y1 = (double)height -
                            (margin +
                             (entity->points[point].y - bounds.min_y) * scale);
                draw_line(&image, x0, y0, x1, y1, color);
            }
            if (entity->closed && entity->point_count > 2u) {
                double x0 = margin +
                            (entity->points[entity->point_count - 1u].x -
                             bounds.min_x) * scale;
                double y0 = (double)height -
                            (margin +
                             (entity->points[entity->point_count - 1u].y -
                              bounds.min_y) * scale);
                double x1 =
                    margin + (entity->points[0].x - bounds.min_x) * scale;
                double y1 = (double)height -
                            (margin + (entity->points[0].y - bounds.min_y) *
                                          scale);
                draw_line(&image, x0, y0, x1, y1, color);
            }
        } else if (entity->kind == QW_CAD_ENTITY_SOLID) {
            QwCadPoint points[4];
            size_t point;
            points[0] = entity->p[0];
            points[1] = entity->p[1];
            points[2] = entity->p[3];
            points[3] = entity->p[2];
            fill_polygon(&image, points, 4u, scale, bounds.min_x, bounds.min_y,
                         (double)height, margin, color);
            for (point = 0u; point < 4u; ++point) {
                size_t next = (point + 1u) % 4u;
                draw_line(&image,
                          margin + (points[point].x - bounds.min_x) * scale,
                          (double)height -
                              (margin + (points[point].y - bounds.min_y) * scale),
                          margin + (points[next].x - bounds.min_x) * scale,
                          (double)height -
                              (margin + (points[next].y - bounds.min_y) * scale),
                          color);
            }
        } else if (entity->kind == QW_CAD_ENTITY_POINT) {
            double x = margin + (entity->p[0].x - bounds.min_x) * scale;
            double y = (double)height -
                       (margin + (entity->p[0].y - bounds.min_y) * scale);
            draw_line(&image, x - 2.0, y, x + 2.0, y, color);
            draw_line(&image, x, y - 2.0, x, y + 2.0, color);
        }
    }
    *out_image = image;
    return QW_CAD_OK;
}

static void svg_entity(SvgBuffer *buffer, const QwCadEntity *entity,
                       uint32_t color)
{
    char hex[16];
    size_t index;
    color_hex(color, hex, sizeof(hex));
    if (entity->kind == QW_CAD_ENTITY_LINE) {
        svg_printf(buffer,
                   "<line x1=\"%.6f\" y1=\"%.6f\" x2=\"%.6f\" y2=\"%.6f\" "
                   "stroke=\"%s\" stroke-width=\"1\" "
                   "vector-effect=\"non-scaling-stroke\"/>",
                   entity->p[0].x, entity->p[0].y, entity->p[1].x,
                   entity->p[1].y, hex);
    } else if (entity->kind == QW_CAD_ENTITY_CIRCLE) {
        svg_printf(buffer,
                   "<circle cx=\"%.6f\" cy=\"%.6f\" r=\"%.6f\" fill=\"none\" "
                   "stroke=\"%s\" vector-effect=\"non-scaling-stroke\"/>",
                   entity->p[0].x, entity->p[0].y, entity->radius, hex);
    } else if (entity->kind == QW_CAD_ENTITY_ARC) {
        double sweep = entity->end_angle - entity->start_angle;
        double start = entity->start_angle * M_PI / 180.0;
        double end;
        int large;
        double sx;
        double sy;
        double ex;
        double ey;
        while (sweep < 0.0) sweep += 360.0;
        if (sweep < 1e-9) sweep = 360.0;
        end = (entity->start_angle + sweep) * M_PI / 180.0;
        large = sweep > 180.0;
        sx = entity->p[0].x + entity->radius * cos(start);
        sy = entity->p[0].y + entity->radius * sin(start);
        ex = entity->p[0].x + entity->radius * cos(end);
        ey = entity->p[0].y + entity->radius * sin(end);
        svg_printf(buffer,
                   "<path d=\"M %.6f %.6f A %.6f %.6f 0 %d 1 %.6f %.6f\" "
                   "fill=\"none\" stroke=\"%s\" "
                   "vector-effect=\"non-scaling-stroke\"/>",
                   sx, sy, entity->radius, entity->radius, large, ex, ey, hex);
    } else if (entity->kind == QW_CAD_ENTITY_ELLIPSE) {
        double angle = atan2(entity->p[1].y, entity->p[1].x) * 180.0 / M_PI;
        double major = sqrt(entity->p[1].x * entity->p[1].x +
                            entity->p[1].y * entity->p[1].y);
        double minor = major * (entity->ratio > 0.0 ? entity->ratio : 1.0);
        svg_printf(buffer,
                   "<ellipse cx=\"%.6f\" cy=\"%.6f\" rx=\"%.6f\" ry=\"%.6f\" "
                   "transform=\"rotate(%.6f %.6f %.6f)\" fill=\"none\" "
                   "stroke=\"%s\" vector-effect=\"non-scaling-stroke\"/>",
                   entity->p[0].x, entity->p[0].y, major, minor, angle,
                   entity->p[0].x, entity->p[0].y, hex);
    } else if (entity->kind == QW_CAD_ENTITY_POLYLINE ||
               entity->kind == QW_CAD_ENTITY_SPLINE) {
        if (entity->filled && entity->closed && entity->point_count >= 3u) {
            svg_append(buffer, "<polygon points=\"");
            for (index = 0u; index < entity->point_count; ++index)
                svg_printf(buffer, "%.6f,%.6f ", entity->points[index].x,
                           entity->points[index].y);
            svg_printf(buffer, "\" fill=\"%s\" stroke=\"%s\"/>", hex, hex);
        } else {
            svg_append(buffer, "<polyline points=\"");
            for (index = 0u; index < entity->point_count; ++index)
                svg_printf(buffer, "%.6f,%.6f ", entity->points[index].x,
                           entity->points[index].y);
            if (entity->closed && entity->point_count > 0u)
                svg_printf(buffer, "%.6f,%.6f", entity->points[0].x,
                           entity->points[0].y);
            svg_printf(buffer,
                       "\" fill=\"none\" stroke=\"%s\" "
                       "vector-effect=\"non-scaling-stroke\"/>",
                       hex);
        }
    } else if (entity->kind == QW_CAD_ENTITY_SOLID) {
        svg_printf(buffer, "<polygon points=\"%.6f,%.6f %.6f,%.6f %.6f,%.6f "
                           "%.6f,%.6f\" fill=\"%s\" stroke=\"%s\"/>",
                   entity->p[0].x, entity->p[0].y, entity->p[1].x,
                   entity->p[1].y, entity->p[3].x, entity->p[3].y,
                   entity->p[2].x, entity->p[2].y, hex, hex);
    } else if (entity->kind == QW_CAD_ENTITY_POINT) {
        svg_printf(buffer,
                   "<circle cx=\"%.6f\" cy=\"%.6f\" r=\"1\" fill=\"%s\"/>",
                   entity->p[0].x, entity->p[0].y, hex);
    } else if (entity->kind == QW_CAD_ENTITY_TEXT) {
        svg_printf(buffer,
                   "<text x=\"%.6f\" y=\"%.6f\" font-size=\"%.6f\" "
                   "fill=\"%s\" transform=\"rotate(%.6f %.6f %.6f)\" "
                   "font-family=\"sans-serif\">",
                   entity->p[0].x, entity->p[0].y,
                   entity->height > 0.0 ? entity->height : 2.5, hex,
                   entity->rotation, entity->p[0].x, entity->p[0].y);
        svg_escape(buffer, entity->text ? entity->text : "",
                   entity->text ? entity->text_size : 0u);
        svg_append(buffer, "</text>");
    }
}

QwCadStatus qw_cad_render_svg(const QwCadDocument *document, char **out_svg,
                              size_t *out_size)
{
    QwCadBounds bounds;
    SvgBuffer buffer;
    size_t layer_index;
    double margin;
    double width;
    double height;
    if (!document || !out_svg || !out_size) return QW_CAD_ERR_INVALID_ARGUMENT;
    *out_svg = NULL;
    *out_size = 0u;
    qw_cad_bounds(document, &bounds);
    margin = (bounds.max_x - bounds.min_x) * 0.02;
    if (margin <= 0.0) margin = 1.0;
    width = bounds.max_x - bounds.min_x + 2.0 * margin;
    height = bounds.max_y - bounds.min_y + 2.0 * margin;
    if (width <= 0.0) width = 1.0;
    if (height <= 0.0) height = 1.0;
    memset(&buffer, 0, sizeof(buffer));
    svg_printf(&buffer,
               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"%.6f "
               "%.6f %.6f %.6f\" width=\"1024\" height=\"%d\">\n",
               bounds.min_x - margin, bounds.min_y - margin, width, height,
               (int)(1024.0 * height / width > 1.0
                         ? lround(1024.0 * height / width)
                         : 1));
    svg_printf(&buffer,
               "<rect x=\"%.6f\" y=\"%.6f\" width=\"%.6f\" height=\"%.6f\" "
               "fill=\"white\"/>\n",
               bounds.min_x - margin, bounds.min_y - margin, width, height);
    for (layer_index = 0u; layer_index < document->layer_count; ++layer_index) {
        const QwCadLayer *layer = &document->layers[layer_index];
        size_t index;
        int has_entity = 0;
        for (index = 0u; index < document->entity_count; ++index) {
            if (document->entities[index].layer == layer_index) {
                has_entity = 1;
                break;
            }
        }
        if (!has_entity) continue;
        svg_printf(&buffer, "<g id=\"layer-%zu\" data-layer=\"", layer_index);
        svg_escape(&buffer, layer->name ? layer->name : "",
                   layer->name_size);
        svg_append(&buffer, "\">\n");
        for (index = 0u; index < document->entity_count; ++index) {
            const QwCadEntity *entity = &document->entities[index];
            if (entity->layer != layer_index) continue;
            svg_entity(&buffer, entity,
                       entity_color(document, entity));
        }
        svg_append(&buffer, "</g>\n");
    }
    svg_append(&buffer, "</svg>\n");
    if (buffer.failed) {
        free(buffer.data);
        return QW_CAD_ERR_OUT_OF_MEMORY;
    }
    *out_svg = buffer.data;
    *out_size = buffer.size;
    return QW_CAD_OK;
}

void qw_cad_image_free(QwCadImage *image)
{
    if (!image) return;
    free(image->pixels);
    memset(image, 0, sizeof(*image));
}





