#ifndef QW_CAD_INTERNAL_H
#define QW_CAD_INTERNAL_H

#include "quirewise/cad.h"

#include <stdint.h>

#define QW_CAD_MAX_ENTITIES 200000u
#define QW_CAD_MAX_POINTS 2000000u
#define QW_CAD_MAX_TEXT_BYTES (1u * 1024u * 1024u)
#define QW_CAD_MAX_INPUT (256u * 1024u * 1024u)
#define QW_CAD_MAX_INSERT_DEPTH 16u

typedef enum QwCadEntityKind {
    QW_CAD_ENTITY_LINE = 1,
    QW_CAD_ENTITY_POLYLINE,
    QW_CAD_ENTITY_CIRCLE,
    QW_CAD_ENTITY_ARC,
    QW_CAD_ENTITY_ELLIPSE,
    QW_CAD_ENTITY_SPLINE,
    QW_CAD_ENTITY_TEXT,
    QW_CAD_ENTITY_INSERT,
    QW_CAD_ENTITY_POINT,
    QW_CAD_ENTITY_SOLID,
    QW_CAD_ENTITY_RAY
} QwCadEntityKind;

typedef struct QwCadEntity {
    QwCadEntityKind kind;
    size_t layer;
    int has_color;
    uint32_t color;
    QwCadPoint p[4];
    double radius;
    double start_angle;
    double end_angle;
    double ratio;
    double rotation;
    double height;
    double scale_x;
    double scale_y;
    QwCadPoint *points;
    size_t point_count;
    int closed;
    char *text;
    size_t text_size;
    char *block_name;
} QwCadEntity;

typedef struct QwCadLayer {
    char *name;
    size_t name_size;
    uint32_t color;
    int visible;
} QwCadLayer;

typedef struct QwCadBlock {
    char *name;
    size_t name_size;
    QwCadPoint base;
    QwCadEntity *entities;
    size_t entity_count;
    size_t entity_capacity;
} QwCadBlock;

struct QwCadDocument {
    QwCadFormat format;
    QwCadEntity *entities;
    size_t entity_count;
    size_t entity_capacity;
    QwCadLayer *layers;
    size_t layer_count;
    size_t layer_capacity;
    QwCadBlock *blocks;
    size_t block_count;
    size_t block_capacity;
    char *version;
    char *codepage;
    int utf8_strings;
    QwCadBounds bounds;
    int has_bounds;
};

typedef struct QwCadParser QwCadParser;

QwCadEntity *qw_cad_document_add_entity(QwCadDocument *document);
QwCadBlock *qw_cad_document_add_block(QwCadDocument *document, const char *name);
QwCadBlock *qw_cad_document_find_block(QwCadDocument *document, const char *name);
size_t qw_cad_document_layer_index(QwCadDocument *document, const char *name,
                                   int create);
uint32_t qw_cad_aci_color(int aci);
char *qw_cad_text_utf8(const QwCadDocument *document, const char *input,
                       size_t length);
void qw_cad_entity_release(QwCadEntity *entity);
void qw_cad_document_compute_bounds(QwCadDocument *document);
void qw_cad_document_expand_inserts(QwCadDocument *document);
void qw_cad_entity_bounds(const QwCadEntity *entity, QwCadBounds *bounds,
                          int *has_bounds);

#endif
