#ifndef QUIREWISE_SVGZ_H
#define QUIREWISE_SVGZ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QW_SVGZ_ABI_VERSION 1

typedef enum QwSvgzStatus {
    QW_SVGZ_OK = 0,
    QW_SVGZ_ERR_INVALID_ARGUMENT = 1,
    QW_SVGZ_ERR_OUT_OF_MEMORY = 2,
    QW_SVGZ_ERR_FORMAT = 3,
    QW_SVGZ_ERR_RESOURCE_LIMIT = 4,
    QW_SVGZ_ERR_DECODE = 5
} QwSvgzStatus;

typedef struct QwSvgzDocument {
    uint8_t *svg_utf8; /* Decompressed SVG bytes, owned by this structure */
    size_t svg_size;
} QwSvgzDocument;

int qw_svgz_abi_version(void);

/* Decompress one bounded gzip member containing an SVG document. */
QwSvgzStatus qw_svgz_decode(const void *data, size_t size,
                            QwSvgzDocument *out_document);
void qw_svgz_document_free(QwSvgzDocument *document);
const char *qw_svgz_status_string(QwSvgzStatus status);

#ifdef __cplusplus
}
#endif

#endif
