#ifndef QUIREWISE_SVG_H
#define QUIREWISE_SVG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QW_SVG_ABI_VERSION 1

typedef enum QwSvgStatus {
    QW_SVG_OK = 0,
    QW_SVG_ERR_INVALID_ARGUMENT = 1,
    QW_SVG_ERR_OUT_OF_MEMORY = 2,
    QW_SVG_ERR_FORMAT = 3,
    QW_SVG_ERR_UNSUPPORTED = 4,
    QW_SVG_ERR_RESOURCE_LIMIT = 5
} QwSvgStatus;

typedef struct QwSvgImage {
    uint8_t *pixels; /* RGBA8; transparent areas retain zero alpha */
    size_t size;
    size_t stride;
    uint32_t width;
    uint32_t height;
} QwSvgImage;

typedef struct QwSvgError {
    QwSvgStatus status;
    char message[192];
} QwSvgError;

int qw_svg_abi_version(void);

/* Render the supported native SVG subset at 2x document units into RGBA8.
 * The input bytes are borrowed. Release output pixels with qw_svg_image_free. */
QwSvgStatus qw_svg_decode(const void *data, size_t size,
                          QwSvgImage *out_image, QwSvgError *out_error);
void qw_svg_image_free(QwSvgImage *image);
const char *qw_svg_status_string(QwSvgStatus status);

#ifdef __cplusplus
}
#endif

#endif
