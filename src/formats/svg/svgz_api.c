#include "quirewise/svgz.h"

#include "svg_limits.h"
#include "svgz_parser_internal.h"

#include "quirewise/status.h"

#include <quirewise/compression.h>

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>

static int svgz_error(QwConvertReport *report, const char *message, int status)
{
    if (report) (void)snprintf(report->note, sizeof(report->note), "%s", message);
    return status;
}

int qw_svgz_decompress_internal(const void *data, size_t size,
                                uint8_t **out_svg, size_t *out_svg_size,
                                QwConvertReport *report)
{
    const uint8_t *input = (const uint8_t *)data;
    uint8_t *output;
    size_t output_size = 0u;
    const size_t output_limit = QW_SVG_MAX_INPUT_BYTES;

    if (out_svg) *out_svg = NULL;
    if (out_svg_size) *out_svg_size = 0u;
    if (!data || !size || !out_svg || !out_svg_size) {
        return svgz_error(report, "SVGZ input and output pointers are required",
                          QW_STATUS_ERR_INVALID_ARGUMENT);
    }
    if (size > QW_SVG_MAX_INPUT_BYTES || size > UINT_MAX) {
        return svgz_error(report, "SVGZ input exceeds the byte limit",
                          QW_STATUS_ERR_RESOURCE_LIMIT);
    }
    if (size < 18u || input[0] != 0x1fu || input[1] != 0x8bu) {
        return svgz_error(report, "SVGZ input has no gzip header", QW_STATUS_ERR_FORMAT);
    }
    output_size = (size_t)input[size - 4u] |
                  ((size_t)input[size - 3u] << 8) |
                  ((size_t)input[size - 2u] << 16) |
                  ((size_t)input[size - 1u] << 24);
    if (output_size > output_limit) {
        return svgz_error(report, "SVGZ decompressed data exceeds 32 MiB",
                          QW_STATUS_ERR_RESOURCE_LIMIT);
    }
    output = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!output) {
        return svgz_error(report, "Could not allocate SVGZ output buffer",
                          QW_STATUS_ERR_OUT_OF_MEMORY);
    }
    if (!qw_compression_inflate_gzip(input, size, output, output_size)) {
        free(output);
        return svgz_error(report, "SVGZ decompression failed",
                          QW_STATUS_ERR_DECODE);
    }
    if (!output_size) {
        free(output);
        return svgz_error(report, "SVGZ contains no SVG document",
                          QW_STATUS_ERR_FORMAT);
    }
    *out_svg = output;
    *out_svg_size = output_size;
    return QW_STATUS_OK;
}

static QwSvgzStatus svgz_status_from_parser(int status)
{
    switch (status) {
    case QW_STATUS_OK: return QW_SVGZ_OK;
    case QW_STATUS_ERR_INVALID_ARGUMENT: return QW_SVGZ_ERR_INVALID_ARGUMENT;
    case QW_STATUS_ERR_OUT_OF_MEMORY: return QW_SVGZ_ERR_OUT_OF_MEMORY;
    case QW_STATUS_ERR_RESOURCE_LIMIT: return QW_SVGZ_ERR_RESOURCE_LIMIT;
    case QW_STATUS_ERR_DECODE: return QW_SVGZ_ERR_DECODE;
    default: return QW_SVGZ_ERR_FORMAT;
    }
}

int qw_svgz_abi_version(void)
{
    return QW_SVGZ_ABI_VERSION;
}

QwSvgzStatus qw_svgz_decode(const void *data, size_t size,
                            QwSvgzDocument *out_document)
{
    uint8_t *svg = NULL;
    size_t svg_size = 0u;
    int status;
    if (!out_document) return QW_SVGZ_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    status = qw_svgz_decompress_internal(data, size, &svg, &svg_size, NULL);
    if (status != QW_STATUS_OK) return svgz_status_from_parser(status);
    out_document->svg_utf8 = svg;
    out_document->svg_size = svg_size;
    return QW_SVGZ_OK;
}

void qw_svgz_document_free(QwSvgzDocument *document)
{
    if (!document) return;
    free(document->svg_utf8);
    memset(document, 0, sizeof(*document));
}

const char *qw_svgz_status_string(QwSvgzStatus status)
{
    switch (status) {
    case QW_SVGZ_OK: return "ok";
    case QW_SVGZ_ERR_INVALID_ARGUMENT: return "invalid argument";
    case QW_SVGZ_ERR_OUT_OF_MEMORY: return "out of memory";
    case QW_SVGZ_ERR_FORMAT: return "invalid SVGZ gzip stream";
    case QW_SVGZ_ERR_RESOURCE_LIMIT: return "SVGZ resource limit exceeded";
    case QW_SVGZ_ERR_DECODE: return "SVGZ decompression failed";
    default: return "unknown status";
    }
}

