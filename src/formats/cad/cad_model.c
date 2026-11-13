#include "formats/cad/cad_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void qw_cad_entity_release(QwCadEntity *entity)
{
    if (!entity) return;
    free(entity->points);
    free(entity->bulges);
    free(entity->contour_starts);
    free(entity->text);
    free(entity->block_name);
    memset(entity, 0, sizeof(*entity));
}

void qw_cad_document_free(QwCadDocument *document)
{
    size_t index;
    if (!document) return;
    for (index = 0u; index < document->entity_count; ++index)
        qw_cad_entity_release(&document->entities[index]);
    free(document->entities);
    for (index = 0u; index < document->block_count; ++index) {
        QwCadBlock *block = &document->blocks[index];
        size_t entity_index;
        for (entity_index = 0u; entity_index < block->entity_count;
             ++entity_index)
            qw_cad_entity_release(&block->entities[entity_index]);
        free(block->entities);
        free(block->name);
    }
    free(document->blocks);
    for (index = 0u; index < document->layer_count; ++index)
        free(document->layers[index].name);
    free(document->layers);
    free(document->version);
    free(document->codepage);
    free(document);
}

static void bounds_point(QwCadBounds *bounds, int *has, double x, double y)
{
    if (!*has) {
        bounds->min_x = bounds->max_x = x;
        bounds->min_y = bounds->max_y = y;
        *has = 1;
        return;
    }
    if (x < bounds->min_x) bounds->min_x = x;
    if (x > bounds->max_x) bounds->max_x = x;
    if (y < bounds->min_y) bounds->min_y = y;
    if (y > bounds->max_y) bounds->max_y = y;
}

static size_t utf8_count(const char *text)
{
    size_t count = 0u;
    if (!text) return 0u;
    while (*text) {
        unsigned char ch = (unsigned char)*text;
        if ((ch & 0xC0u) != 0x80u) ++count;
        ++text;
    }
    return count;
}

static void arc_bounds(const QwCadEntity *entity, QwCadBounds *bounds,
                       int *has)
{
    double start = entity->start_angle;
    double end = entity->end_angle;
    double sweep;
    int step;
    if (entity->kind == QW_CAD_ENTITY_ELLIPSE) {
        double major_x = entity->p[1].x;
        double major_y = entity->p[1].y;
        double major = sqrt(major_x * major_x + major_y * major_y);
        double angle = atan2(major_y, major_x);
        double minor = major * (entity->ratio > 0.0 ? entity->ratio : 1.0);
        start = entity->start_angle;
        end = entity->end_angle;
        if (end <= start) end = start + 2.0 * M_PI;
        sweep = end - start;
        for (step = 0; step <= 64; ++step) {
            double t = start + sweep * step / 64.0;
            double x = major * cos(t);
            double y = minor * sin(t);
            double rx = x * cos(angle) - y * sin(angle);
            double ry = x * sin(angle) + y * cos(angle);
            bounds_point(bounds, has, entity->p[0].x + rx,
                         entity->p[0].y + ry);
        }
        return;
    }
    sweep = end - start;
    while (sweep < 0.0) sweep += 360.0;
    if (sweep < 1e-9) sweep = 360.0;
    for (step = 0; step <= 64; ++step) {
        double degrees = start + sweep * step / 64.0;
        double radians = degrees * M_PI / 180.0;
        bounds_point(bounds, has,
                     entity->p[0].x + entity->radius * cos(radians),
                     entity->p[0].y + entity->radius * sin(radians));
    }
}

void qw_cad_entity_bounds(const QwCadEntity *entity, QwCadBounds *bounds,
                          int *has)
{
    size_t index;
    switch (entity->kind) {
    case QW_CAD_ENTITY_LINE:
        bounds_point(bounds, has, entity->p[0].x, entity->p[0].y);
        bounds_point(bounds, has, entity->p[1].x, entity->p[1].y);
        break;
    case QW_CAD_ENTITY_POLYLINE:
    case QW_CAD_ENTITY_SPLINE:
        for (index = 0u; index < entity->point_count; ++index)
            bounds_point(bounds, has, entity->points[index].x,
                         entity->points[index].y);
        break;
    case QW_CAD_ENTITY_CIRCLE:
        bounds_point(bounds, has, entity->p[0].x - entity->radius,
                     entity->p[0].y - entity->radius);
        bounds_point(bounds, has, entity->p[0].x + entity->radius,
                     entity->p[0].y + entity->radius);
        break;
    case QW_CAD_ENTITY_ARC:
    case QW_CAD_ENTITY_ELLIPSE:
        arc_bounds(entity, bounds, has);
        break;
    case QW_CAD_ENTITY_TEXT: {
        double width = entity->height * 0.6 * (double)utf8_count(entity->text);
        if (width < entity->height) width = entity->height;
        bounds_point(bounds, has, entity->p[0].x, entity->p[0].y);
        bounds_point(bounds, has, entity->p[0].x + width,
                     entity->p[0].y + entity->height);
        break;
    }
    case QW_CAD_ENTITY_SOLID:
        for (index = 0u; index < 4u; ++index)
            bounds_point(bounds, has, entity->p[index].x, entity->p[index].y);
        break;
    case QW_CAD_ENTITY_POINT:
        bounds_point(bounds, has, entity->p[0].x, entity->p[0].y);
        break;
    default:
        break;
    }
}

void qw_cad_document_compute_bounds(QwCadDocument *document)
{
    QwCadBounds bounds = {0.0, 0.0, 0.0, 0.0};
    int has = 0;
    size_t index;
    for (index = 0u; index < document->entity_count; ++index)
        qw_cad_entity_bounds(&document->entities[index], &bounds, &has);
    if (!has) {
        bounds.min_x = 0.0;
        bounds.min_y = 0.0;
        bounds.max_x = 1.0;
        bounds.max_y = 1.0;
    }
    document->bounds = bounds;
    document->has_bounds = 1;
}

typedef struct CadTransform {
    double a;
    double b;
    double tx;
    double ty;
    double rotation;
    double scale;
    size_t layer_override;
    int has_layer_override;
} CadTransform;

static void transform_point(const CadTransform *transform, double *x,
                            double *y)
{
    double source_x = *x;
    double source_y = *y;
    *x = transform->a * source_x - transform->b * source_y + transform->tx;
    *y = transform->b * source_x + transform->a * source_y + transform->ty;
}

static int append_owned(QwCadEntity **output, size_t *count,
                        size_t *capacity, QwCadEntity *entity)
{
    if (*count >= QW_CAD_MAX_ENTITIES) return 0;
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2u : 64u;
        QwCadEntity *grown = (QwCadEntity *)realloc(
            *output, next * sizeof(**output));
        if (!grown) return 0;
        *output = grown;
        *capacity = next;
    }
    (*output)[(*count)++] = *entity;
    return 1;
}

static int expand_entity(QwCadDocument *document, const QwCadEntity *source,
                         CadTransform transform, size_t depth,
                         QwCadEntity **output, size_t *count,
                         size_t *capacity);

static int expand_insert(QwCadDocument *document, const QwCadEntity *insert,
                         CadTransform transform, size_t depth,
                         QwCadEntity **output, size_t *count,
                         size_t *capacity)
{
    QwCadBlock *block;
    size_t index;
    CadTransform next;
    double x = insert->p[0].x;
    double y = insert->p[0].y;
    double rotation;
    double scale_x = insert->scale_x != 0.0 ? insert->scale_x : 1.0;
    double scale_y = insert->scale_y != 0.0 ? insert->scale_y : 1.0;
    double uniform = (fabs(scale_x) + fabs(scale_y)) * 0.5;
    double base_x;
    double base_y;
    if (depth > QW_CAD_MAX_INSERT_DEPTH || !insert->block_name) return 1;
    block = qw_cad_document_find_block(document, insert->block_name);
    if (!block) return 1;
    transform_point(&transform, &x, &y);
    rotation = (insert->rotation + transform.rotation) * M_PI / 180.0;
    next.a = cos(rotation) * uniform * transform.scale;
    next.b = sin(rotation) * uniform * transform.scale;
    next.rotation = insert->rotation + transform.rotation;
    next.scale = transform.scale * uniform;
    base_x = block->base.x;
    base_y = block->base.y;
    next.tx = x - (next.a * base_x - next.b * base_y);
    next.ty = y - (next.b * base_x + next.a * base_y);
    next.layer_override = insert->layer;
    next.has_layer_override = insert->layer != 0u;
    for (index = 0u; index < block->entity_count; ++index) {
        if (!expand_entity(document, &block->entities[index], next, depth + 1u,
                           output, count, capacity))
            return 0;
    }
    return 1;
}

static int expand_entity(QwCadDocument *document, const QwCadEntity *source,
                         CadTransform transform, size_t depth,
                         QwCadEntity **output, size_t *count,
                         size_t *capacity)
{
    QwCadEntity copy;
    size_t index;
    if (source->kind == QW_CAD_ENTITY_INSERT)
        return expand_insert(document, source, transform, depth, output, count,
                             capacity);
    copy = *source;
    copy.points = NULL;
    copy.bulges = NULL;
    copy.contour_starts = NULL;
    copy.contour_count = 0u;
    copy.text = NULL;
    copy.block_name = NULL;
    copy.point_count = 0u;
    if (transform.has_layer_override && source->layer == 0u)
        copy.layer = transform.layer_override;
    if (source->point_count) {
        copy.points = (QwCadPoint *)malloc(source->point_count *
                                            sizeof(*copy.points));
        if (!copy.points) return 0;
        memcpy(copy.points, source->points,
               source->point_count * sizeof(*copy.points));
        copy.point_count = source->point_count;
        for (index = 0u; index < copy.point_count; ++index)
            transform_point(&transform, &copy.points[index].x,
                            &copy.points[index].y);
    }
    if (source->bulges && source->point_count) {
        copy.bulges =
            (double *)malloc(source->point_count * sizeof(*copy.bulges));
        if (!copy.bulges) {
            free(copy.points);
            return 0;
        }
        memcpy(copy.bulges, source->bulges,
               source->point_count * sizeof(*copy.bulges));
    }    if (source->contour_count) {
        copy.contour_starts =
            (size_t *)malloc(source->contour_count * sizeof(*copy.contour_starts));
        if (!copy.contour_starts) {
            free(copy.points);
            return 0;
        }
        memcpy(copy.contour_starts, source->contour_starts,
               source->contour_count * sizeof(*copy.contour_starts));
        copy.contour_count = source->contour_count;
    }
    for (index = 0u; index < 4u; ++index)
        transform_point(&transform, &copy.p[index].x, &copy.p[index].y);
    copy.radius *= transform.scale;
    copy.height *= transform.scale;
    copy.rotation += transform.rotation;
    if (source->text) {
        copy.text = (char *)malloc(source->text_size + 1u);
        if (!copy.text) {
            free(copy.points);
            return 0;
        }
        memcpy(copy.text, source->text, source->text_size + 1u);
        copy.text_size = source->text_size;
    }
    if (!append_owned(output, count, capacity, &copy)) {
        free(copy.points);
        free(copy.text);
        return 0;
    }
    return 1;
}

void qw_cad_document_expand_inserts(QwCadDocument *document)
{
    QwCadEntity *output = NULL;
    size_t count = 0u;
    size_t capacity = 0u;
    CadTransform identity;
    size_t index;
    identity.a = 1.0;
    identity.b = 0.0;
    identity.tx = 0.0;
    identity.ty = 0.0;
    identity.rotation = 0.0;
    identity.scale = 1.0;
    identity.layer_override = 0u;
    identity.has_layer_override = 0;
    for (index = 0u; index < document->entity_count; ++index) {
        if (!expand_entity(document, &document->entities[index], identity, 0u,
                           &output, &count, &capacity)) {
            size_t cleanup;
            for (cleanup = 0u; cleanup < count; ++cleanup)
                qw_cad_entity_release(&output[cleanup]);
            free(output);
            return;
        }
    }
    for (index = 0u; index < document->entity_count; ++index)
        qw_cad_entity_release(&document->entities[index]);
    free(document->entities);
    document->entities = output;
    document->entity_count = count;
    document->entity_capacity = capacity;
    for (index = 0u; index < document->block_count; ++index) {
        QwCadBlock *block = &document->blocks[index];
        size_t entity_index;
        for (entity_index = 0u; entity_index < block->entity_count;
             ++entity_index)
            qw_cad_entity_release(&block->entities[entity_index]);
        free(block->entities);
        block->entities = NULL;
        block->entity_count = 0u;
        block->entity_capacity = 0u;
    }
}






