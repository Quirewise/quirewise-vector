#ifndef QUIREWISE_CAD_H
#define QUIREWISE_CAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QUIREWISE_CAD_ABI_VERSION 1u

typedef enum QwCadStatus {
    QW_CAD_OK = 0,
    QW_CAD_ERR_INVALID_ARGUMENT = -1,
    QW_CAD_ERR_OUT_OF_MEMORY = -2,
    QW_CAD_ERR_FORMAT = -3,
    QW_CAD_ERR_UNSUPPORTED = -4,
    QW_CAD_ERR_RESOURCE_LIMIT = -5
} QwCadStatus;

typedef enum QwCadFormat {
    QW_CAD_FORMAT_UNKNOWN = 0,
    QW_CAD_FORMAT_DXF_ASCII,
    QW_CAD_FORMAT_DXF_BINARY,
    QW_CAD_FORMAT_DWG
} QwCadFormat;

typedef struct QwCadPoint {
    double x;
    double y;
} QwCadPoint;

typedef struct QwCadBounds {
    double min_x;
    double min_y;
    double max_x;
    double max_y;
} QwCadBounds;

/* RGBA8, top-down, owned by the caller; release with qw_cad_image_free(). */
typedef struct QwCadImage {
    uint8_t *pixels;
    size_t size;
    size_t stride;
    uint32_t width;
    uint32_t height;
} QwCadImage;

/* One text entity. Strings are borrowed until qw_cad_document_free(). */
typedef struct QwCadTextInfo {
    const char *text;
    size_t text_size;
    double x;
    double y;
    double height;
    double rotation;
    const char *layer;
    size_t layer_size;
} QwCadTextInfo;

typedef struct QwCadDocument QwCadDocument;

uint32_t qw_cad_abi_version(void);

/* Returns 1 when the bytes look like DXF (ASCII or binary) or DWG. */
int qw_cad_probe(const void *data, size_t size);

/* Parses DXF (ASCII or binary) and DWG R13/R14/R2000/R2004/R2007
 * (AC1012/AC1014/AC1015/AC1018/AC1021, LINE/CIRCLE/ARC/LWPOLYLINE and TEXT
 * up to R2004 in this release). DWG R2010+ is recognised by qw_cad_probe()
 * but returns QW_CAD_ERR_UNSUPPORTED. */
QwCadStatus qw_cad_parse(const void *data, size_t size,
                         QwCadDocument **out_document);
QwCadFormat qw_cad_format(const QwCadDocument *document);
size_t qw_cad_entity_count(const QwCadDocument *document);
size_t qw_cad_layer_count(const QwCadDocument *document);
QwCadStatus qw_cad_bounds(const QwCadDocument *document, QwCadBounds *out_bounds);

size_t qw_cad_text_count(const QwCadDocument *document);
QwCadStatus qw_cad_text_at(const QwCadDocument *document, size_t index,
                           QwCadTextInfo *out_text);

/* Rasterise the model at the requested pixel width. Height follows the model
 * aspect ratio. Text is drawn with a placeholder bar because the vector
 * package has no glyph engine yet; use qw_cad_render_svg() when real text is
 * required. */
QwCadStatus qw_cad_render_rgba(const QwCadDocument *document, uint32_t width,
                               QwCadImage *out_image);

/* Emit SVG with one group per layer, vector strokes and real <text> elements. */
QwCadStatus qw_cad_render_svg(const QwCadDocument *document, char **out_svg,
                              size_t *out_size);

void qw_cad_document_free(QwCadDocument *document);
void qw_cad_image_free(QwCadImage *image);
void qw_cad_buffer_free(void *buffer);
const char *qw_cad_status_string(QwCadStatus status);

#ifdef __cplusplus
}
#endif

#endif
