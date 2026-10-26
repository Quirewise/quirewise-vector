#include "svg_native.h"

#include "../../core/xml_parser.h"
#include "quirewise/status.h"
#include "svg_limits.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    QW_SVG_NATIVE_MAX_SHAPES = 8192,
    QW_SVG_NATIVE_MAX_POINTS = 65536,
    QW_SVG_NATIVE_MAX_TOTAL_POINTS = 1000000,
    QW_SVG_NATIVE_MAX_ARC_STEPS = 4096
};

typedef struct QwSvgViewport {
    double min_x;
    double min_y;
    double width;
    double height;
    double scale_x;
    double scale_y;
    double offset_x;
    double offset_y;
} QwSvgViewport;

typedef struct QwSvgStyle {
    QwSvgColor fill;
    QwSvgColor stroke;
    QwSvgFillRule fill_rule;
    double stroke_width;
    int has_fill;
    int has_stroke;
} QwSvgStyle;

typedef struct QwSvgTransform {
    /* x'=a*x+c*y+e, y'=b*x+d*y+f */
    double a;
    double b;
    double c;
    double d;
    double e;
    double f;
} QwSvgTransform;

static int svg_parser_error(QwConvertReport *report, const char *message,
                            int code)
{
    if (report) (void)snprintf(report->note, sizeof(report->note), "%s", message);
    return code;
}

static int svg_space(unsigned char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f';
}

static void svg_skip_space(const char **cursor)
{
    while (**cursor && svg_space((unsigned char)**cursor)) ++*cursor;
}

static int svg_parse_number(const char **cursor, double *out_value)
{
    char *end = NULL;
    double value;
    if (!cursor || !*cursor || !out_value) return 0;
    svg_skip_space(cursor);
    errno = 0;
    value = strtod(*cursor, &end);
    if (end == *cursor || errno == ERANGE || !isfinite(value)) return 0;
    *cursor = end;
    *out_value = value;
    return 1;
}

static int svg_parse_length(const char *text, double *out_value)
{
    const char *cursor = text;
    char unit[4] = {0, 0, 0, 0};
    size_t unit_size = 0u;
    double value;
    double factor = 1.0;
    if (!text || !out_value || !svg_parse_number(&cursor, &value)) return 0;
    svg_skip_space(&cursor);
    while (*cursor && unit_size < sizeof(unit) - 1u) {
        if (!isalpha((unsigned char)*cursor)) return 0;
        unit[unit_size++] = (char)tolower((unsigned char)*cursor++);
    }
    svg_skip_space(&cursor);
    if (*cursor) return 0;
    if (unit_size == 0u || strcmp(unit, "px") == 0) factor = 1.0;
    else if (strcmp(unit, "pt") == 0) factor = 96.0 / 72.0;
    else if (strcmp(unit, "pc") == 0) factor = 16.0;
    else if (strcmp(unit, "in") == 0) factor = 96.0;
    else if (strcmp(unit, "cm") == 0) factor = 96.0 / 2.54;
    else if (strcmp(unit, "mm") == 0) factor = 96.0 / 25.4;
    else if (strcmp(unit, "q") == 0) factor = 96.0 / 101.6;
    else return 0;
    value *= factor;
    if (!isfinite(value)) return 0;
    *out_value = value;
    return 1;
}

static void svg_transform_identity(QwSvgTransform *transform)
{
    transform->a = 1.0;
    transform->b = 0.0;
    transform->c = 0.0;
    transform->d = 1.0;
    transform->e = 0.0;
    transform->f = 0.0;
}

static int svg_transform_multiply(const QwSvgTransform *left,
                                  const QwSvgTransform *right,
                                  QwSvgTransform *out)
{
    QwSvgTransform value;
    if (!left || !right || !out) return 0;
    /* Transform lists and nested groups compose in SVG matrix order. */
    value.a = left->a * right->a + left->c * right->b;
    value.b = left->b * right->a + left->d * right->b;
    value.c = left->a * right->c + left->c * right->d;
    value.d = left->b * right->c + left->d * right->d;
    value.e = left->a * right->e + left->c * right->f + left->e;
    value.f = left->b * right->e + left->d * right->f + left->f;
    if (!isfinite(value.a) || !isfinite(value.b) || !isfinite(value.c) ||
        !isfinite(value.d) || !isfinite(value.e) || !isfinite(value.f))
        return 0;
    *out = value;
    return 1;
}

static int svg_transform_arguments(const char **cursor, double *values,
                                   size_t *out_count, size_t max_count)
{
    size_t count = 0u;
    for (;;) {
        const char *before_space;
        double value;
        svg_skip_space(cursor);
        if (**cursor == ')') {
            ++*cursor;
            break;
        }
        if (**cursor == ',') {
            ++*cursor;
            svg_skip_space(cursor);
            if (!**cursor || **cursor == ',' || **cursor == ')') return 0;
        }
        if (count >= max_count || !svg_parse_number(cursor, &value)) return 0;
        values[count++] = value;
        before_space = *cursor;
        svg_skip_space(cursor);
        if (**cursor == ')') {
            ++*cursor;
            break;
        }
        if (**cursor != ',' && *cursor == before_space) return 0;
    }
    *out_count = count;
    return 1;
}

static int svg_parse_transform(const char *text, QwSvgTransform *out)
{
    const char *cursor = text;
    QwSvgTransform result;
    if (!out) return 0;
    svg_transform_identity(&result);
    if (!text || !*text) {
        *out = result;
        return 1;
    }
    for (;;) {
        char name[16];
        size_t name_size = 0u;
        double values[6];
        size_t count = 0u;
        QwSvgTransform operation;
        QwSvgTransform combined;
        svg_skip_space(&cursor);
        if (*cursor == ',') {
            ++cursor;
            svg_skip_space(&cursor);
        }
        if (!*cursor) break;
        while (isalpha((unsigned char)*cursor) && name_size < sizeof(name) - 1u)
            name[name_size++] = (char)*cursor++;
        name[name_size] = '\0';
        if (!name_size) return 0;
        svg_skip_space(&cursor);
        if (*cursor++ != '(' ||
            !svg_transform_arguments(&cursor, values, &count,
                                     sizeof(values) / sizeof(values[0]))) return 0;
        svg_transform_identity(&operation);
        if (strcmp(name, "translate") == 0 && (count == 1u || count == 2u)) {
            operation.e = values[0];
            operation.f = count == 2u ? values[1] : 0.0;
        } else if (strcmp(name, "scale") == 0 && (count == 1u || count == 2u)) {
            operation.a = values[0];
            operation.d = count == 2u ? values[1] : values[0];
        } else if (strcmp(name, "matrix") == 0 && count == 6u) {
            operation.a = values[0];
            operation.b = values[1];
            operation.c = values[2];
            operation.d = values[3];
            operation.e = values[4];
            operation.f = values[5];
        } else if (strcmp(name, "rotate") == 0 &&
                   (count == 1u || count == 3u)) {
            double radians = values[0] * (acos(-1.0) / 180.0);
            double cosine = cos(radians);
            double sine = sin(radians);
            operation.a = cosine;
            operation.b = sine;
            operation.c = -sine;
            operation.d = cosine;
            if (count == 3u) {
                operation.e = values[1] -
                              (cosine * values[1] - sine * values[2]);
                operation.f = values[2] -
                              (sine * values[1] + cosine * values[2]);
            }
        } else if ((strcmp(name, "skewX") == 0 ||
                    strcmp(name, "skewY") == 0) && count == 1u) {
            double radians = values[0] * (acos(-1.0) / 180.0);
            double tangent = tan(radians);
            if (!isfinite(tangent)) return 0;
            if (strcmp(name, "skewX") == 0) operation.c = tangent;
            else operation.b = tangent;
        } else {
            return 0;
        }
        if (!svg_transform_multiply(&result, &operation, &combined)) return 0;
        result = combined;
    }
    *out = result;
    return 1;
}

static int svg_parse_viewbox(const char *text, double values[4])
{
    const char *cursor = text;
    size_t i;
    if (!text || !values) return 0;
    for (i = 0u; i < 4u; ++i) {
        const char *before_space;
        if (!svg_parse_number(&cursor, &values[i])) return 0;
        before_space = cursor;
        svg_skip_space(&cursor);
        if (i == 3u) return *cursor == '\0';
        if (*cursor == ',') {
            ++cursor;
            svg_skip_space(&cursor);
            if (!*cursor || *cursor == ',') return 0;
        } else if (cursor == before_space) {
            return 0;
        }
    }
    return 0;
}

static int svg_parse_points(const char *text, double **out_points,
                            size_t *out_point_count)
{
    const char *cursor = text;
    double *points = NULL;
    size_t value_count = 0u;
    size_t capacity = 0u;
    if (out_points) *out_points = NULL;
    if (out_point_count) *out_point_count = 0u;
    if (!text || !out_points || !out_point_count) return 0;
    for (;;) {
        double value;
        const char *before_space;
        if (!svg_parse_number(&cursor, &value)) goto failure;
        if (value_count >= QW_SVG_NATIVE_MAX_POINTS * 2u) goto failure;
        if (value_count == capacity) {
            size_t next_capacity = capacity ? capacity * 2u : 16u;
            double *grown;
            if (next_capacity > QW_SVG_NATIVE_MAX_POINTS * 2u)
                next_capacity = QW_SVG_NATIVE_MAX_POINTS * 2u;
            if (next_capacity > SIZE_MAX / sizeof(*points)) goto failure;
            grown = (double *)realloc(points, next_capacity * sizeof(*points));
            if (!grown) goto failure;
            points = grown;
            capacity = next_capacity;
        }
        points[value_count++] = value;
        before_space = cursor;
        svg_skip_space(&cursor);
        if (!*cursor) break;
        if (*cursor == ',') {
            ++cursor;
            svg_skip_space(&cursor);
            if (!*cursor || *cursor == ',') goto failure;
        } else if (cursor == before_space) {
            goto failure;
        }
    }
    if (value_count < 4u || (value_count & 1u) != 0u) goto failure;
    *out_points = points;
    *out_point_count = value_count / 2u;
    return 1;

failure:
    free(points);
    return 0;
}

static int svg_path_append_point(double **points, size_t *point_count,
                                 size_t *capacity, double x, double y)
{
    if (*point_count >= QW_SVG_NATIVE_MAX_POINTS) return 0;
    if (*point_count == *capacity) {
        size_t next_capacity = *capacity ? *capacity * 2u : 32u;
        double *grown;
        if (next_capacity > QW_SVG_NATIVE_MAX_POINTS)
            next_capacity = QW_SVG_NATIVE_MAX_POINTS;
        if (next_capacity > SIZE_MAX / (2u * sizeof(*points[0]))) return 0;
        grown = (double *)realloc(*points, next_capacity * 2u * sizeof(*points[0]));
        if (!grown) return 0;
        *points = grown;
        *capacity = next_capacity;
    }
    (*points)[*point_count * 2u] = x;
    (*points)[*point_count * 2u + 1u] = y;
    ++*point_count;
    return 1;
}

static int svg_path_append_contour(QwSvgContour **contours,
                                   size_t *contour_count,
                                   size_t *capacity,
                                   size_t first_point,
                                   size_t point_count,
                                   int is_closed)
{
    if (*contour_count >= QW_SVG_NATIVE_MAX_POINTS) return 0;
    if (*contour_count == *capacity) {
        size_t next_capacity = *capacity ? *capacity * 2u : 8u;
        QwSvgContour *grown;
        if (next_capacity > QW_SVG_NATIVE_MAX_POINTS)
            next_capacity = QW_SVG_NATIVE_MAX_POINTS;
        if (next_capacity > SIZE_MAX / sizeof(**contours)) return 0;
        grown = (QwSvgContour *)realloc(*contours,
                                        next_capacity * sizeof(**contours));
        if (!grown) return 0;
        *contours = grown;
        *capacity = next_capacity;
    }
    (*contours)[*contour_count].first_point = first_point;
    (*contours)[*contour_count].point_count = point_count;
    (*contours)[*contour_count].is_closed = is_closed;
    ++*contour_count;
    return 1;
}

static int svg_shape_set_single_contour(QwSvgShape *shape,
                                        size_t point_count,
                                        int is_closed)
{
    QwSvgContour *contour;
    if (!shape || !point_count) return 0;
    contour = (QwSvgContour *)malloc(sizeof(*contour));
    if (!contour) return 0;
    contour->first_point = 0u;
    contour->point_count = point_count;
    contour->is_closed = is_closed;
    free(shape->contours);
    shape->contours = contour;
    shape->contour_count = 1u;
    return 1;
}

static void svg_shape_drop_data(QwSvgShape *shape)
{
    if (!shape) return;
    free(shape->points);
    free(shape->contours);
    shape->points = NULL;
    shape->contours = NULL;
    shape->point_count = 0u;
    shape->contour_count = 0u;
}

static int svg_path_number(const char **cursor, double *value)
{
    if (**cursor == ',') {
        ++*cursor;
        svg_skip_space(cursor);
        if (!**cursor || **cursor == ',') return 0;
    }
    return svg_parse_number(cursor, value);
}

static int svg_path_flag(const char **cursor, int *value)
{
    if (**cursor == ',') {
        ++*cursor;
        svg_skip_space(cursor);
    } else {
        svg_skip_space(cursor);
    }
    if (**cursor == '0') *value = 0;
    else if (**cursor == '1') *value = 1;
    else return 0;
    ++*cursor;
    return 1;
}

static int svg_path_pair(const char **cursor, double *x, double *y)
{
    return svg_path_number(cursor, x) && svg_path_number(cursor, y);
}

static int svg_path_curve_cubic(double **points, size_t *point_count,
                                size_t *capacity, double x0, double y0,
                                double x1, double y1, double x2, double y2,
                                double x3, double y3)
{
    unsigned step;
    for (step = 1u; step <= 12u; ++step) {
        double t = (double)step / 12.0;
        double u = 1.0 - t;
        double x = u * u * u * x0 + 3.0 * u * u * t * x1 +
                   3.0 * u * t * t * x2 + t * t * t * x3;
        double y = u * u * u * y0 + 3.0 * u * u * t * y1 +
                   3.0 * u * t * t * y2 + t * t * t * y3;
        if (!svg_path_append_point(points, point_count, capacity, x, y)) return 0;
    }
    return 1;
}

static int svg_path_curve_quadratic(double **points, size_t *point_count,
                                    size_t *capacity, double x0, double y0,
                                    double x1, double y1, double x2, double y2)
{
    unsigned step;
    for (step = 1u; step <= 12u; ++step) {
        double t = (double)step / 12.0;
        double u = 1.0 - t;
        double x = u * u * x0 + 2.0 * u * t * x1 + t * t * x2;
        double y = u * u * y0 + 2.0 * u * t * y1 + t * t * y2;
        if (!svg_path_append_point(points, point_count, capacity, x, y)) return 0;
    }
    return 1;
}

static int svg_path_arc(double **points, size_t *point_count, size_t *capacity,
                        double x0, double y0, double rx, double ry,
                        double rotation_degrees, int large_arc, int sweep,
                        double x1, double y1)
{
    const double pi = 3.14159265358979323846;
    double phi = rotation_degrees * (pi / 180.0);
    double cosine = cos(phi);
    double sine = sin(phi);
    double dx = (x0 - x1) * 0.5;
    double dy = (y0 - y1) * 0.5;
    double x_prime = cosine * dx + sine * dy;
    double y_prime = -sine * dx + cosine * dy;
    double rx_squared;
    double ry_squared;
    double lambda;
    double numerator;
    double denominator;
    double coefficient;
    double center_x_prime;
    double center_y_prime;
    double center_x;
    double center_y;
    double ux;
    double uy;
    double vx;
    double vy;
    double theta;
    double delta;
    double max_radius;
    double max_step;
    unsigned steps;
    unsigned step;
    if (!isfinite(phi) || !isfinite(x_prime) || !isfinite(y_prime)) return 0;
    rx = fabs(rx);
    ry = fabs(ry);
    if (!(rx > 0.0) || !(ry > 0.0))
        return svg_path_append_point(points, point_count, capacity, x1, y1);
    if (x0 == x1 && y0 == y1) return 1;
    rx_squared = rx * rx;
    ry_squared = ry * ry;
    lambda = (x_prime * x_prime) / rx_squared +
             (y_prime * y_prime) / ry_squared;
    if (!isfinite(lambda)) return 0;
    if (lambda > 1.0) {
        double scale = sqrt(lambda);
        rx *= scale;
        ry *= scale;
        rx_squared = rx * rx;
        ry_squared = ry * ry;
    }
    numerator = rx_squared * ry_squared -
                rx_squared * y_prime * y_prime -
                ry_squared * x_prime * x_prime;
    denominator = rx_squared * y_prime * y_prime +
                  ry_squared * x_prime * x_prime;
    if (!(denominator > 0.0) || !isfinite(numerator) || !isfinite(denominator))
        return svg_path_append_point(points, point_count, capacity, x1, y1);
    coefficient = sqrt(fmax(0.0, numerator / denominator));
    if (large_arc == sweep) coefficient = -coefficient;
    center_x_prime = coefficient * rx * y_prime / ry;
    center_y_prime = -coefficient * ry * x_prime / rx;
    center_x = cosine * center_x_prime - sine * center_y_prime + (x0 + x1) * 0.5;
    center_y = sine * center_x_prime + cosine * center_y_prime + (y0 + y1) * 0.5;
    ux = (x_prime - center_x_prime) / rx;
    uy = (y_prime - center_y_prime) / ry;
    vx = (-x_prime - center_x_prime) / rx;
    vy = (-y_prime - center_y_prime) / ry;
    theta = atan2(uy, ux);
    delta = atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    if (!sweep && delta > 0.0) delta -= 2.0 * pi;
    else if (sweep && delta < 0.0) delta += 2.0 * pi;
    max_radius = fmax(rx, ry);
    max_step = max_radius > 1.0 ? sqrt(1.0 / max_radius) : 0.25;
    if (max_step > 0.25) max_step = 0.25;
    if (!(max_step > 0.0) || !isfinite(max_step)) return 0;
    steps = (unsigned)ceil(fabs(delta) / max_step);
    if (!steps) steps = 1u;
    if (steps > QW_SVG_NATIVE_MAX_ARC_STEPS) return 0;
    for (step = 1u; step <= steps; ++step) {
        double angle = theta + delta * (double)step / (double)steps;
        double ellipse_x = rx * cos(angle);
        double ellipse_y = ry * sin(angle);
        double px = center_x + cosine * ellipse_x - sine * ellipse_y;
        double py = center_y + sine * ellipse_x + cosine * ellipse_y;
        if (step == steps) {
            px = x1;
            py = y1;
        }
        if (!svg_path_append_point(points, point_count, capacity, px, py)) return 0;
    }
    return 1;
}

static int svg_parse_path(const char *text, double **out_points,
                          size_t *out_point_count,
                          QwSvgContour **out_contours,
                          size_t *out_contour_count)
{
    const char *cursor = text;
    double *points = NULL;
    QwSvgContour *contours = NULL;
    size_t point_count = 0u;
    size_t capacity = 0u;
    size_t contour_count = 0u;
    size_t contour_capacity = 0u;
    size_t current_contour = SIZE_MAX;
    double x = 0.0;
    double y = 0.0;
    double start_x = 0.0;
    double start_y = 0.0;
    double cubic_x = 0.0;
    double cubic_y = 0.0;
    double quadratic_x = 0.0;
    double quadratic_y = 0.0;
    char command = 0;
    char previous_command = 0;
    int rc = 0;
    if (out_points) *out_points = NULL;
    if (out_point_count) *out_point_count = 0u;
    if (out_contours) *out_contours = NULL;
    if (out_contour_count) *out_contour_count = 0u;
    if (!text || !out_points || !out_point_count || !out_contours ||
        !out_contour_count) return 0;
    for (;;) {
        double values[7];
        char operation;
        int relative;
        size_t value_count = 0u;
        svg_skip_space(&cursor);
        if (!*cursor) break;
        if (*cursor == ',') goto done;
        if (isalpha((unsigned char)*cursor)) command = *cursor++;
        else if (!command) goto done;
        operation = (char)toupper((unsigned char)command);
        relative = islower((unsigned char)command);
        if (operation == 'Z') {
            if (current_contour == SIZE_MAX) {
                if (!contour_count || !contours[contour_count - 1u].is_closed)
                    goto done;
            } else {
                contours[current_contour].is_closed = 1;
                current_contour = SIZE_MAX;
            }
            x = start_x;
            y = start_y;
            previous_command = command;
            command = 0;
            continue;
        }
        if (operation == 'M' || operation == 'L' || operation == 'T')
            value_count = 2u;
        else if (operation == 'H' || operation == 'V')
            value_count = 1u;
        else if (operation == 'Q' || operation == 'S')
            value_count = 4u;
        else if (operation == 'C')
            value_count = 6u;
        else if (operation == 'A')
            value_count = 7u;
        else goto done;
        if (operation != 'M' && current_contour == SIZE_MAX) goto done;
        if (operation == 'A') {
            int large_arc;
            int sweep;
            if (!svg_path_number(&cursor, &values[0]) ||
                !svg_path_number(&cursor, &values[1]) ||
                !svg_path_number(&cursor, &values[2]) ||
                !svg_path_flag(&cursor, &large_arc) ||
                !svg_path_flag(&cursor, &sweep) ||
                !svg_path_pair(&cursor, &values[5], &values[6])) goto done;
            values[3] = (double)large_arc;
            values[4] = (double)sweep;
        } else {
            for (size_t i = 0u; i < value_count; ++i) {
                if (!svg_path_number(&cursor, &values[i])) goto done;
            }
        }
        if (operation == 'M') {
            double next_x = relative ? x + values[0] : values[0];
            double next_y = relative ? y + values[1] : values[1];
            if (!svg_path_append_point(&points, &point_count, &capacity,
                                       next_x, next_y) ||
                !svg_path_append_contour(&contours, &contour_count,
                                         &contour_capacity, point_count - 1u,
                                         1u, 0)) goto done;
            current_contour = contour_count - 1u;
            x = start_x = next_x;
            y = start_y = next_y;
            previous_command = command;
            command = relative ? 'l' : 'L';
            continue;
        }
        if (operation == 'L' || operation == 'T') {
            double next_x = relative ? x + values[0] : values[0];
            double next_y = relative ? y + values[1] : values[1];
            if (operation == 'T' &&
                (toupper((unsigned char)previous_command) == 'Q' ||
                 toupper((unsigned char)previous_command) == 'T')) {
                double control_x = 2.0 * x - quadratic_x;
                double control_y = 2.0 * y - quadratic_y;
                if (!svg_path_curve_quadratic(&points, &point_count, &capacity,
                                              x, y, control_x, control_y,
                                              next_x, next_y)) goto done;
                quadratic_x = control_x;
                quadratic_y = control_y;
            } else if (operation == 'T') {
                if (!svg_path_curve_quadratic(&points, &point_count, &capacity,
                                              x, y, x, y, next_x, next_y)) goto done;
                quadratic_x = x;
                quadratic_y = y;
            } else if (!svg_path_append_point(&points, &point_count, &capacity,
                                              next_x, next_y)) {
                goto done;
            }
            x = next_x;
            y = next_y;
        } else if (operation == 'H') {
            x = relative ? x + values[0] : values[0];
            if (!svg_path_append_point(&points, &point_count, &capacity, x, y))
                goto done;
        } else if (operation == 'V') {
            y = relative ? y + values[0] : values[0];
            if (!svg_path_append_point(&points, &point_count, &capacity, x, y))
                goto done;
        } else if (operation == 'C' || operation == 'S') {
            double control1_x;
            double control1_y;
            double control2_x;
            double control2_y;
            double next_x;
            double next_y;
            if (operation == 'C') {
                control1_x = relative ? x + values[0] : values[0];
                control1_y = relative ? y + values[1] : values[1];
                control2_x = relative ? x + values[2] : values[2];
                control2_y = relative ? y + values[3] : values[3];
                next_x = relative ? x + values[4] : values[4];
                next_y = relative ? y + values[5] : values[5];
            } else {
                control1_x = (toupper((unsigned char)previous_command) == 'C' ||
                              toupper((unsigned char)previous_command) == 'S')
                                 ? 2.0 * x - cubic_x : x;
                control1_y = (toupper((unsigned char)previous_command) == 'C' ||
                              toupper((unsigned char)previous_command) == 'S')
                                 ? 2.0 * y - cubic_y : y;
                control2_x = relative ? x + values[0] : values[0];
                control2_y = relative ? y + values[1] : values[1];
                next_x = relative ? x + values[2] : values[2];
                next_y = relative ? y + values[3] : values[3];
            }
            if (!svg_path_curve_cubic(&points, &point_count, &capacity,
                                      x, y, control1_x, control1_y,
                                      control2_x, control2_y, next_x, next_y))
                goto done;
            cubic_x = control2_x;
            cubic_y = control2_y;
            x = next_x;
            y = next_y;
        } else if (operation == 'A') {
            double next_x = relative ? x + values[5] : values[5];
            double next_y = relative ? y + values[6] : values[6];
            if (!svg_path_arc(&points, &point_count, &capacity,
                              x, y, values[0], values[1], values[2],
                              (int)values[3], (int)values[4], next_x, next_y))
                goto done;
            x = next_x;
            y = next_y;
        } else if (operation == 'Q') {
            double control_x = relative ? x + values[0] : values[0];
            double control_y = relative ? y + values[1] : values[1];
            double next_x = relative ? x + values[2] : values[2];
            double next_y = relative ? y + values[3] : values[3];
            if (!svg_path_curve_quadratic(&points, &point_count, &capacity,
                                          x, y, control_x, control_y,
                                          next_x, next_y)) goto done;
            quadratic_x = control_x;
            quadratic_y = control_y;
            x = next_x;
            y = next_y;
        }
        previous_command = command;
        contours[current_contour].point_count =
            point_count - contours[current_contour].first_point;
    }
    if (!contour_count) goto done;
    *out_points = points;
    *out_point_count = point_count;
    *out_contours = contours;
    *out_contour_count = contour_count;
    points = NULL;
    contours = NULL;
    rc = 1;

done:
    free(points);
    free(contours);
    return rc;
}

static int svg_hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int svg_parse_color(const char *text, QwSvgColor *out_color,
                           int *out_none)
{
    size_t i;
    if (!text || !out_color || !out_none) return 0;
    *out_none = 0;
    if (strcmp(text, "none") == 0) {
        *out_none = 1;
        memset(out_color, 0, sizeof(*out_color));
        return 1;
    }
    out_color->alpha = 255u;
    if (text[0] == '#') {
        size_t length = strlen(text + 1u);
        if (length == 3u) {
            int r = svg_hex_digit(text[1]);
            int g = svg_hex_digit(text[2]);
            int b = svg_hex_digit(text[3]);
            if (r < 0 || g < 0 || b < 0) return 0;
            out_color->red = (uint8_t)(r * 17);
            out_color->green = (uint8_t)(g * 17);
            out_color->blue = (uint8_t)(b * 17);
            return 1;
        }
        if (length == 6u) {
            unsigned channels[3] = {0u, 0u, 0u};
            for (i = 0u; i < 3u; ++i) {
                int hi = svg_hex_digit(text[1u + i * 2u]);
                int lo = svg_hex_digit(text[2u + i * 2u]);
                if (hi < 0 || lo < 0) return 0;
                channels[i] = (unsigned)(hi * 16 + lo);
            }
            out_color->red = (uint8_t)channels[0];
            out_color->green = (uint8_t)channels[1];
            out_color->blue = (uint8_t)channels[2];
            return 1;
        }
        return 0;
    }
#define QW_SVG_COLOR(name, r, g, b) \
    if (strcmp(text, name) == 0) { \
        out_color->red = (r); out_color->green = (g); out_color->blue = (b); \
        return 1; \
    }
    QW_SVG_COLOR("black", 0u, 0u, 0u)
    QW_SVG_COLOR("white", 255u, 255u, 255u)
    QW_SVG_COLOR("red", 255u, 0u, 0u)
    QW_SVG_COLOR("green", 0u, 128u, 0u)
    QW_SVG_COLOR("blue", 0u, 0u, 255u)
    QW_SVG_COLOR("yellow", 255u, 255u, 0u)
    QW_SVG_COLOR("cyan", 0u, 255u, 255u)
    QW_SVG_COLOR("magenta", 255u, 0u, 255u)
    QW_SVG_COLOR("gray", 128u, 128u, 128u)
    QW_SVG_COLOR("grey", 128u, 128u, 128u)
    QW_SVG_COLOR("orange", 255u, 165u, 0u)
    QW_SVG_COLOR("purple", 128u, 0u, 128u)
    QW_SVG_COLOR("lime", 0u, 255u, 0u)
    QW_SVG_COLOR("navy", 0u, 0u, 128u)
    QW_SVG_COLOR("teal", 0u, 128u, 128u)
    QW_SVG_COLOR("silver", 192u, 192u, 192u)
    QW_SVG_COLOR("maroon", 128u, 0u, 0u)
    QW_SVG_COLOR("olive", 128u, 128u, 0u)
    QW_SVG_COLOR("aqua", 0u, 255u, 255u)
    QW_SVG_COLOR("fuchsia", 255u, 0u, 255u)
#undef QW_SVG_COLOR
    return 0;
}

static int svg_is_xmlns(const char *name)
{
    return name && (strcmp(name, "xmlns") == 0 ||
                    strncmp(name, "xmlns:", 6u) == 0);
}

static int svg_root_attributes_supported(const QwXmlNode *root)
{
    size_t i;
    for (i = 0u; i < qw_xml_attribute_count(root); ++i) {
        const char *name = qw_xml_attribute_name(root, i);
        if (!name) return 0;
        if (svg_is_xmlns(name) || strcmp(name, "width") == 0 ||
            strcmp(name, "height") == 0 || strcmp(name, "viewBox") == 0 ||
            strcmp(name, "preserveAspectRatio") == 0 ||
            strcmp(name, "version") == 0 || strcmp(name, "baseProfile") == 0 ||
            strcmp(name, "fill") == 0 || strcmp(name, "stroke") == 0 ||
            strcmp(name, "fill-rule") == 0 ||
            strcmp(name, "stroke-width") == 0 ||
            strcmp(name, "transform") == 0)
            continue;
        return 0;
    }
    return 1;
}

static int svg_group_attributes_supported(const QwXmlNode *group)
{
    size_t i;
    for (i = 0u; i < qw_xml_attribute_count(group); ++i) {
        const char *name = qw_xml_attribute_name(group, i);
        if (!name) return 0;
        if (svg_is_xmlns(name) || strcmp(name, "fill") == 0 ||
            strcmp(name, "stroke") == 0 || strcmp(name, "stroke-width") == 0 ||
            strcmp(name, "fill-rule") == 0 || strcmp(name, "transform") == 0)
            continue;
        return 0;
    }
    return 1;
}

static int svg_parse_fill_rule(const char *text, QwSvgFillRule *out_rule)
{
    if (!text || !out_rule) return 0;
    if (strcmp(text, "nonzero") == 0)
        *out_rule = QW_SVG_FILL_RULE_NONZERO;
    else if (strcmp(text, "evenodd") == 0)
        *out_rule = QW_SVG_FILL_RULE_EVENODD;
    else
        return 0;
    return 1;
}

static int svg_apply_style_attributes(const QwXmlNode *node, QwSvgStyle *style)
{
    const char *text;
    int is_none;
    if (!node || !style) return 0;
    text = qw_xml_att(node, "fill");
    if (text) {
        if (!svg_parse_color(text, &style->fill, &is_none)) return 0;
        style->has_fill = !is_none;
    }
    text = qw_xml_att(node, "stroke");
    if (text) {
        if (!svg_parse_color(text, &style->stroke, &is_none)) return 0;
        style->has_stroke = !is_none;
    }
    text = qw_xml_att(node, "fill-rule");
    if (text && !svg_parse_fill_rule(text, &style->fill_rule)) return 0;
    text = qw_xml_att(node, "stroke-width");
    if (text && (!svg_parse_length(text, &style->stroke_width) ||
                 style->stroke_width < 0.0)) return 0;
    if (style->stroke_width == 0.0) style->has_stroke = 0;
    return 1;
}

static int svg_shape_attribute_supported(const QwXmlNode *node,
                                        QwSvgShapeKind kind,
                                        const char *tag)
{
    size_t i;
    for (i = 0u; i < qw_xml_attribute_count(node); ++i) {
        const char *name = qw_xml_attribute_name(node, i);
        int accepted = 0;
        if (!name) return 0;
        if (svg_is_xmlns(name)) continue;
        if (strcmp(name, "fill") == 0 || strcmp(name, "stroke") == 0 ||
            strcmp(name, "fill-rule") == 0 ||
            strcmp(name, "stroke-width") == 0 ||
            strcmp(name, "transform") == 0) {
            accepted = 1;
        } else if (kind == QW_SVG_SHAPE_RECT &&
                   (strcmp(name, "x") == 0 || strcmp(name, "y") == 0 ||
                    strcmp(name, "width") == 0 || strcmp(name, "height") == 0)) {
            accepted = 1;
        } else if (kind == QW_SVG_SHAPE_ELLIPSE) {
            if (strcmp(name, "cx") == 0 || strcmp(name, "cy") == 0)
                accepted = 1;
            else if (strcmp(tag, "circle") == 0 && strcmp(name, "r") == 0)
                accepted = 1;
            else if (strcmp(tag, "ellipse") == 0 &&
                     (strcmp(name, "rx") == 0 || strcmp(name, "ry") == 0))
                accepted = 1;
        } else if (kind == QW_SVG_SHAPE_LINE &&
                   (strcmp(name, "x1") == 0 || strcmp(name, "y1") == 0 ||
                    strcmp(name, "x2") == 0 || strcmp(name, "y2") == 0)) {
            accepted = 1;
        } else if ((kind == QW_SVG_SHAPE_POLYLINE ||
                    kind == QW_SVG_SHAPE_POLYGON) &&
                   strcmp(name, "points") == 0) {
            accepted = 1;
        } else if (kind == QW_SVG_SHAPE_PATH && strcmp(name, "d") == 0) {
            accepted = 1;
        }
        if (!accepted) return 0;
    }
    return 1;
}

static int svg_parse_optional_length(const QwXmlNode *node, const char *name,
                                     double default_value, double *out_value)
{
    const char *text = qw_xml_att(node, name);
    if (!text) {
        *out_value = default_value;
        return 1;
    }
    return svg_parse_length(text, out_value);
}

static int svg_parse_shape(const QwXmlNode *node, const QwSvgStyle *inherited,
                           QwSvgShape *shape)
{
    const char *tag = qw_xml_local_name(node);
    const char *text;
    int is_none;
    if (!tag || !inherited || !shape) return 0;
    memset(shape, 0, sizeof(*shape));
    shape->fill = inherited->fill;
    shape->stroke = inherited->stroke;
    shape->fill_rule = inherited->fill_rule;
    shape->has_fill = inherited->has_fill;
    shape->has_stroke = inherited->has_stroke;
    shape->stroke_width = inherited->stroke_width;
    if (strcmp(tag, "rect") == 0) shape->kind = QW_SVG_SHAPE_RECT;
    else if (strcmp(tag, "circle") == 0 || strcmp(tag, "ellipse") == 0)
        shape->kind = QW_SVG_SHAPE_ELLIPSE;
    else if (strcmp(tag, "line") == 0) shape->kind = QW_SVG_SHAPE_LINE;
    else if (strcmp(tag, "polyline") == 0) shape->kind = QW_SVG_SHAPE_POLYLINE;
    else if (strcmp(tag, "polygon") == 0) shape->kind = QW_SVG_SHAPE_POLYGON;
    else if (strcmp(tag, "path") == 0) shape->kind = QW_SVG_SHAPE_PATH;
    else return 0;

    if (!svg_shape_attribute_supported(node, shape->kind, tag)) return 0;
    if (qw_xml_down((QwXmlNode *)node)) return 0;

    if (shape->kind == QW_SVG_SHAPE_RECT) {
        if (!svg_parse_optional_length(node, "x", 0.0, &shape->x) ||
            !svg_parse_optional_length(node, "y", 0.0, &shape->y) ||
            !svg_parse_optional_length(node, "width", -1.0, &shape->width) ||
            !svg_parse_optional_length(node, "height", -1.0, &shape->height) ||
            !(shape->width > 0.0) || !(shape->height > 0.0)) return 0;
    } else if (shape->kind == QW_SVG_SHAPE_ELLIPSE) {
        if (strcmp(tag, "circle") == 0) {
            if (!svg_parse_optional_length(node, "cx", 0.0, &shape->x) ||
                !svg_parse_optional_length(node, "cy", 0.0, &shape->y) ||
                !svg_parse_optional_length(node, "r", -1.0, &shape->width) ||
                !(shape->width > 0.0)) return 0;
            shape->height = shape->width;
        } else if (!svg_parse_optional_length(node, "cx", 0.0, &shape->x) ||
                   !svg_parse_optional_length(node, "cy", 0.0, &shape->y) ||
                   !svg_parse_optional_length(node, "rx", -1.0, &shape->width) ||
                   !svg_parse_optional_length(node, "ry", -1.0, &shape->height) ||
                   !(shape->width > 0.0) || !(shape->height > 0.0)) {
            return 0;
        }
    } else if (shape->kind == QW_SVG_SHAPE_LINE) {
        shape->has_fill = 0;
        shape->point_count = 2u;
        shape->points = (double *)malloc(4u * sizeof(*shape->points));
        if (!shape->points) return -1;
        if (!svg_parse_optional_length(node, "x1", 0.0, &shape->points[0]) ||
            !svg_parse_optional_length(node, "y1", 0.0, &shape->points[1]) ||
            !svg_parse_optional_length(node, "x2", 0.0, &shape->points[2]) ||
            !svg_parse_optional_length(node, "y2", 0.0, &shape->points[3])) return 0;
        if (!svg_shape_set_single_contour(shape, shape->point_count, 0))
            return -1;
    } else if (shape->kind == QW_SVG_SHAPE_PATH) {
        text = qw_xml_att(node, "d");
        if (!svg_parse_path(text, &shape->points, &shape->point_count,
                            &shape->contours, &shape->contour_count)) return 0;
    } else {
        text = qw_xml_att(node, "points");
        if (!svg_parse_points(text, &shape->points, &shape->point_count) ||
            (shape->kind == QW_SVG_SHAPE_POLYLINE && shape->point_count < 2u) ||
            (shape->kind == QW_SVG_SHAPE_POLYGON && shape->point_count < 3u)) return 0;
        if (!svg_shape_set_single_contour(
                shape, shape->point_count,
                shape->kind == QW_SVG_SHAPE_POLYGON)) return -1;
    }

    text = qw_xml_att(node, "fill-rule");
    if (text && !svg_parse_fill_rule(text, &shape->fill_rule)) return 0;
    text = qw_xml_att(node, "fill");
    if (text) {
        if (!svg_parse_color(text, &shape->fill, &is_none)) return 0;
        shape->has_fill = !is_none;
    }
    text = qw_xml_att(node, "stroke");
    if (text) {
        if (!svg_parse_color(text, &shape->stroke, &is_none)) return 0;
        shape->has_stroke = !is_none;
    }
    if (!svg_parse_optional_length(node, "stroke-width",
                                   inherited->stroke_width,
                                   &shape->stroke_width) ||
        shape->stroke_width < 0.0) return 0;
    if (shape->stroke_width == 0.0) shape->has_stroke = 0;
    if (shape->kind == QW_SVG_SHAPE_LINE) shape->has_fill = 0;
    return 1;
}

static int svg_transform_shape(QwSvgShape *shape,
                               const QwSvgTransform *transform)
{
    const double pi = acos(-1.0);
    const double axis_tolerance = 1e-12;
    double scale_x;
    double scale_y;
    double dot;
    double stroke_scale;
    int axis_aligned;
    size_t i;
    if (!shape || !transform) return 0;
    scale_x = hypot(transform->a, transform->b);
    scale_y = hypot(transform->c, transform->d);
    dot = transform->a * transform->c + transform->b * transform->d;
    stroke_scale = (scale_x + scale_y) * 0.5;
    if (!isfinite(scale_x) || !isfinite(scale_y) || !isfinite(dot)) return 0;
    if (shape->has_stroke &&
        (fabs(scale_x - scale_y) > 1e-9 * fmax(1.0, fmax(scale_x, scale_y)) ||
         fabs(dot) > 1e-9 * fmax(1.0, scale_x * scale_y))) return 0;
    axis_aligned = fabs(transform->b) <= axis_tolerance &&
                   fabs(transform->c) <= axis_tolerance;
    if (shape->kind == QW_SVG_SHAPE_RECT && !axis_aligned) {
        static const unsigned char corners[8] = {0u, 0u, 1u, 0u,
                                                  1u, 1u, 0u, 1u};
        double *points = (double *)malloc(8u * sizeof(*points));
        if (!points) return -1;
        for (i = 0u; i < 4u; ++i) {
            double x = shape->x + (double)corners[i * 2u] * shape->width;
            double y = shape->y + (double)corners[i * 2u + 1u] * shape->height;
            points[i * 2u] = transform->a * x + transform->c * y + transform->e;
            points[i * 2u + 1u] = transform->b * x + transform->d * y + transform->f;
        }
        if (!svg_shape_set_single_contour(shape, 4u, 1)) {
            free(points);
            return -1;
        }
        free(shape->points);
        shape->points = points;
        shape->point_count = 4u;
        shape->kind = QW_SVG_SHAPE_POLYGON;
    } else if (shape->kind == QW_SVG_SHAPE_ELLIPSE &&
               (!axis_aligned || shape->has_stroke)) {
        enum { SVG_ELLIPSE_SEGMENTS = 128 };
        double *points = (double *)malloc(
            SVG_ELLIPSE_SEGMENTS * 2u * sizeof(*points));
        if (!points) return -1;
        for (i = 0u; i < SVG_ELLIPSE_SEGMENTS; ++i) {
            double angle = 2.0 * pi * (double)i /
                           (double)SVG_ELLIPSE_SEGMENTS;
            double x = shape->x + cos(angle) * shape->width;
            double y = shape->y + sin(angle) * shape->height;
            points[i * 2u] = transform->a * x + transform->c * y + transform->e;
            points[i * 2u + 1u] = transform->b * x + transform->d * y + transform->f;
        }
        if (!svg_shape_set_single_contour(shape, SVG_ELLIPSE_SEGMENTS, 1)) {
            free(points);
            return -1;
        }
        free(shape->points);
        shape->points = points;
        shape->point_count = SVG_ELLIPSE_SEGMENTS;
        shape->kind = QW_SVG_SHAPE_POLYGON;
    } else if (shape->kind == QW_SVG_SHAPE_RECT) {
        double x0 = shape->x * transform->a + transform->e;
        double x1 = (shape->x + shape->width) * transform->a + transform->e;
        double y0 = shape->y * transform->d + transform->f;
        double y1 = (shape->y + shape->height) * transform->d + transform->f;
        shape->x = fmin(x0, x1);
        shape->y = fmin(y0, y1);
        shape->width = fabs(x1 - x0);
        shape->height = fabs(y1 - y0);
    } else if (shape->kind == QW_SVG_SHAPE_ELLIPSE) {
        shape->x = shape->x * transform->a + transform->e;
        shape->y = shape->y * transform->d + transform->f;
        shape->width *= fabs(transform->a);
        shape->height *= fabs(transform->d);
    } else {
        for (i = 0u; i < shape->point_count; ++i) {
            double x = shape->points[i * 2u];
            double y = shape->points[i * 2u + 1u];
            shape->points[i * 2u] = transform->a * x + transform->c * y +
                                    transform->e;
            shape->points[i * 2u + 1u] = transform->b * x + transform->d * y +
                                         transform->f;
            if (!isfinite(shape->points[i * 2u]) ||
                !isfinite(shape->points[i * 2u + 1u])) return 0;
        }
    }
    if (shape->has_stroke) shape->stroke_width *= stroke_scale;
    for (i = 0u; i < shape->point_count; ++i) {
        if (!isfinite(shape->points[i * 2u]) ||
            !isfinite(shape->points[i * 2u + 1u])) return 0;
    }
    return isfinite(shape->x) && isfinite(shape->y) &&
           isfinite(shape->width) && isfinite(shape->height) &&
           isfinite(shape->stroke_width);
}

static int svg_node_is_svg_namespace(const QwXmlNode *node)
{
    const char *namespace_uri = qw_xml_namespace_uri(node);
    return !namespace_uri || !*namespace_uri ||
           strcmp(namespace_uri, "http://www.w3.org/2000/svg") == 0;
}

static int svg_parse_children(QwSvgScene *scene, QwXmlNode *root,
                              const QwSvgStyle *inherited, size_t depth,
                              const QwSvgTransform *inherited_transform,
                              QwConvertReport *report)
{
    QwXmlNode *child;
    if (depth > 64u) {
        return svg_parser_error(report, "SVG group nesting limit exceeded",
                                QW_STATUS_ERR_RESOURCE_LIMIT);
    }
    for (child = qw_xml_down(root); child; child = qw_xml_next(child)) {
        QwXmlNodeKind kind = qw_xml_node_kind(child);
        if (kind == QW_XML_NODE_COMMENT || kind == QW_XML_NODE_PROCESSING_INSTRUCTION)
            continue;
        if (kind == QW_XML_NODE_TEXT) {
            const unsigned char *text = (const unsigned char *)qw_xml_text(child);
            if (text) while (*text && svg_space(*text)) ++text;
            if (text && *text) return QW_STATUS_ERR_UNSUPPORTED;
            continue;
        }
        if (kind != QW_XML_NODE_ELEMENT || !svg_node_is_svg_namespace(child))
            return QW_STATUS_ERR_UNSUPPORTED;
        if (qw_xml_is_tag(child, "g")) {
            QwSvgStyle group_style = *inherited;
            QwSvgTransform group_transform = *inherited_transform;
            QwSvgTransform local_transform;
            QwSvgTransform combined_transform;
            int rc;
            if (!svg_group_attributes_supported(child) ||
                !svg_apply_style_attributes(child, &group_style) ||
                !svg_parse_transform(qw_xml_att(child, "transform"),
                                     &local_transform) ||
                !svg_transform_multiply(inherited_transform, &local_transform,
                                        &combined_transform))
                return QW_STATUS_ERR_UNSUPPORTED;
            group_transform = combined_transform;
            rc = svg_parse_children(scene, child, &group_style, depth + 1u,
                                    &group_transform, report);
            if (rc != QW_STATUS_OK) return rc;
            continue;
        }
        if (scene->shape_count >= QW_SVG_NATIVE_MAX_SHAPES)
            return svg_parser_error(report, "SVG native shape limit exceeded",
                                   QW_STATUS_ERR_RESOURCE_LIMIT);
        {
            QwSvgShape shape;
            QwSvgTransform local_transform;
            QwSvgTransform combined_transform;
            int parsed = svg_parse_shape(child, inherited, &shape);
            int transformed;
            if (parsed < 0) {
                svg_shape_drop_data(&shape);
                return svg_parser_error(report, "Could not allocate SVG shape data",
                                       QW_STATUS_ERR_OUT_OF_MEMORY);
            }
            if (!parsed) {
                svg_shape_drop_data(&shape);
                return QW_STATUS_ERR_UNSUPPORTED;
            }
            if (!svg_parse_transform(qw_xml_att(child, "transform"),
                                     &local_transform) ||
                !svg_transform_multiply(inherited_transform, &local_transform,
                                        &combined_transform)) {
                svg_shape_drop_data(&shape);
                return QW_STATUS_ERR_UNSUPPORTED;
            }
            transformed = svg_transform_shape(&shape, &combined_transform);
            if (transformed < 0) {
                svg_shape_drop_data(&shape);
                return svg_parser_error(report,
                                       "Could not allocate transformed SVG shape data",
                                       QW_STATUS_ERR_OUT_OF_MEMORY);
            }
            if (!transformed) {
                svg_shape_drop_data(&shape);
                return QW_STATUS_ERR_UNSUPPORTED;
            }
            if (shape.point_count >
                QW_SVG_NATIVE_MAX_TOTAL_POINTS - scene->point_count) {
                svg_shape_drop_data(&shape);
                return svg_parser_error(report,
                                       "SVG native point limit exceeded",
                                       QW_STATUS_ERR_RESOURCE_LIMIT);
            }
            scene->point_count += shape.point_count;
            scene->shapes[scene->shape_count++] = shape;
        }
    }
    return QW_STATUS_OK;
}

void qw_svg_scene_drop(QwSvgScene *scene)
{
    size_t i;
    if (!scene) return;
    for (i = 0u; i < scene->shape_count; ++i)
        svg_shape_drop_data(&scene->shapes[i]);
    free(scene->shapes);
    free(scene);
}

int qw_svg_parse_native(const uint8_t *data, size_t size,
                        QwSvgScene **out_scene,
                        QwConvertReport *report)
{
    QwXmlError error;
    QwXmlNode *document = NULL;
    QwXmlNode *root;
    QwSvgScene *scene = NULL;
    QwSvgViewport viewport;
    QwSvgStyle root_style = {0};
    QwSvgTransform root_transform;
    QwSvgTransform viewport_transform;
    QwSvgTransform scene_transform;
    double viewbox[4];
    const char *text;
    const char *preserve;
    int has_viewbox = 0;
    int rc = QW_STATUS_ERR_FORMAT;
    if (out_scene) *out_scene = NULL;
    if (!data || !size || !out_scene)
        return svg_parser_error(report, "SVG parser input and output are required",
                                QW_STATUS_ERR_INVALID_ARGUMENT);
    if (size > QW_SVG_MAX_INPUT_BYTES)
        return svg_parser_error(report, "SVG input exceeds the byte limit",
                                QW_STATUS_ERR_RESOURCE_LIMIT);
    if (memchr(data, '\0', size) != NULL)
        return svg_parser_error(report, "SVG input contains a NUL byte",
                                QW_STATUS_ERR_FORMAT);
    document = qw_xml_parse(data, size, &error);
    if (!document) {
        if (report) {
            (void)snprintf(report->note, sizeof(report->note),
                           "SVG XML parse failed: %.120s", error.message);
        }
        if (error.code == QW_XML_ERROR_RESOURCE_LIMIT) rc = QW_STATUS_ERR_RESOURCE_LIMIT;
        else if (error.code == QW_XML_ERROR_OUT_OF_MEMORY) rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        else rc = QW_STATUS_ERR_FORMAT;
        return rc;
    }
    root = qw_xml_root(document);
    if (!root || !qw_xml_is_tag(root, "svg")) {
        rc = svg_parser_error(report, "SVG document root element is not <svg>",
                              QW_STATUS_ERR_FORMAT);
        goto done;
    }
    if (!svg_node_is_svg_namespace(root) || !svg_root_attributes_supported(root)) {
        rc = QW_STATUS_ERR_UNSUPPORTED;
        goto done;
    }
    root_style.fill.alpha = 255u;
    root_style.stroke.alpha = 255u;
    root_style.stroke_width = 1.0;
    root_style.has_fill = 1;
    svg_transform_identity(&root_transform);
    if (!svg_apply_style_attributes(root, &root_style) ||
        !svg_parse_transform(qw_xml_att(root, "transform"), &root_transform)) {
        rc = QW_STATUS_ERR_UNSUPPORTED;
        goto done;
    }
    scene = (QwSvgScene *)calloc(1u, sizeof(*scene));
    if (!scene) {
        rc = svg_parser_error(report, "Could not allocate SVG scene",
                              QW_STATUS_ERR_OUT_OF_MEMORY);
        goto done;
    }
    if (!svg_parse_length(qw_xml_att(root, "width"), &scene->width) ||
        !svg_parse_length(qw_xml_att(root, "height"), &scene->height) ||
        !(scene->width > 0.0) || !(scene->height > 0.0)) {
        rc = QW_STATUS_ERR_UNSUPPORTED;
        goto done;
    }
    if (scene->width > 100000.0 || scene->height > 100000.0 ||
        scene->width * scene->height > (double)QW_SVG_MAX_PIXELS / 4.0) {
        rc = svg_parser_error(report, "SVG dimensions exceed the raster pixel limit",
                              QW_STATUS_ERR_RESOURCE_LIMIT);
        goto done;
    }
    text = qw_xml_att(root, "viewBox");
    if (text) {
        if (!svg_parse_viewbox(text, viewbox) || !(viewbox[2] > 0.0) ||
            !(viewbox[3] > 0.0)) {
            rc = QW_STATUS_ERR_UNSUPPORTED;
            goto done;
        }
        has_viewbox = 1;
    }
    viewport.min_x = has_viewbox ? viewbox[0] : 0.0;
    viewport.min_y = has_viewbox ? viewbox[1] : 0.0;
    viewport.width = has_viewbox ? viewbox[2] : scene->width;
    viewport.height = has_viewbox ? viewbox[3] : scene->height;
    viewport.scale_x = scene->width / viewport.width;
    viewport.scale_y = scene->height / viewport.height;
    viewport.offset_x = 0.0;
    viewport.offset_y = 0.0;
    preserve = qw_xml_att(root, "preserveAspectRatio");
    if (preserve && strcmp(preserve, "none") != 0 &&
        strcmp(preserve, "xMidYMid meet") != 0 &&
        strcmp(preserve, "xMidYMid") != 0) {
        rc = QW_STATUS_ERR_UNSUPPORTED;
        goto done;
    }
    if (has_viewbox && (!preserve || strcmp(preserve, "none") != 0)) {
        double scale = fmin(viewport.scale_x, viewport.scale_y);
        viewport.scale_x = scale;
        viewport.scale_y = scale;
        viewport.offset_x = (scene->width - viewport.width * scale) * 0.5;
        viewport.offset_y = (scene->height - viewport.height * scale) * 0.5;
    }
    svg_transform_identity(&viewport_transform);
    viewport_transform.a = viewport.scale_x;
    viewport_transform.d = viewport.scale_y;
    viewport_transform.e = viewport.offset_x -
                           viewport.min_x * viewport.scale_x;
    viewport_transform.f = viewport.offset_y -
                           viewport.min_y * viewport.scale_y;
    if (!svg_transform_multiply(&root_transform, &viewport_transform,
                                &scene_transform)) {
        rc = QW_STATUS_ERR_UNSUPPORTED;
        goto done;
    }
    scene->shapes = (QwSvgShape *)calloc(QW_SVG_NATIVE_MAX_SHAPES,
                                         sizeof(*scene->shapes));
    if (!scene->shapes) {
        rc = svg_parser_error(report, "Could not allocate SVG shape list",
                              QW_STATUS_ERR_OUT_OF_MEMORY);
        goto done;
    }
    rc = svg_parse_children(scene, root, &root_style, 0u, &scene_transform,
                            report);
    if (rc != QW_STATUS_OK) goto done;
    *out_scene = scene;
    scene = NULL;
    rc = QW_STATUS_OK;

done:
    qw_xml_drop(document);
    qw_svg_scene_drop(scene);
    return rc;
}
