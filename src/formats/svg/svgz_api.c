#include "quirewise/svgz.h"

#include "svg_limits.h"
#include "svgz_parser_internal.h"

#include "quirewise/status.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <zlib.h>

static int svgz_error(QwConvertReport *report, const char *message, int status)
{
    if (report) (void)snprintf(report->note, sizeof(report->note), "%s", message);
    return status;
}

int qw_svgz_decompress_internal(const void *data, size_t size,
                                uint8_t **out_svg, size_t *out_svg_size,
                                QwConvertReport *report)
{
    z_stream stream;
    uint8_t *output = NULL;
    size_t capacity = 0u;
    size_t output_size = 0u;
    const size_t output_limit = QW_SVG_MAX_INPUT_BYTES;
    int initialized = 0;
    int zrc;
    int rc = QW_STATUS_ERR_DECODE;

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
    if (size < 2u || ((const uint8_t *)data)[0] != 0x1fu ||
        ((const uint8_t *)data)[1] != 0x8bu) {
        return svgz_error(report, "SVGZ input has no gzip header", QW_STATUS_ERR_FORMAT);
    }

    memset(&stream, 0, sizeof(stream));
    zrc = inflateInit2(&stream, MAX_WBITS + 16);
    if (zrc != Z_OK) {
        return svgz_error(report, "Could not initialize SVGZ decompressor",
                          zrc == Z_MEM_ERROR ? QW_STATUS_ERR_OUT_OF_MEMORY
                                             : QW_STATUS_ERR_DECODE);
    }
    initialized = 1;
    stream.next_in = (Bytef *)(void *)data;
    stream.avail_in = (uInt)size;

    for (;;) {
        uInt input_before;
        uInt output_before;
        size_t capacity_limit = output_limit + 1u;
        if (capacity == output_size) {
            size_t next_capacity = capacity
                ? (capacity > capacity_limit / 2u
                    ? capacity_limit : capacity * 2u)
                : 64u * 1024u;
            uint8_t *grown;
            if (next_capacity > capacity_limit) next_capacity = capacity_limit;
            grown = (uint8_t *)realloc(output, next_capacity);
            if (!grown) {
                rc = svgz_error(report, "Could not allocate SVGZ output buffer",
                                QW_STATUS_ERR_OUT_OF_MEMORY);
                break;
            }
            output = grown;
            capacity = next_capacity;
        }
        input_before = stream.avail_in;
        stream.next_out = output + output_size;
        stream.avail_out = (uInt)(capacity - output_size);
        output_before = stream.avail_out;
        zrc = inflate(&stream, Z_NO_FLUSH);
        output_size += (size_t)(output_before - stream.avail_out);
        if (output_size > output_limit) {
            rc = svgz_error(report, "SVGZ decompressed data exceeds 32 MiB",
                            QW_STATUS_ERR_RESOURCE_LIMIT);
            break;
        }
        if (zrc == Z_STREAM_END) {
            if (stream.avail_in != 0u) {
                rc = svgz_error(report,
                    "SVGZ contains trailing data or multiple gzip members",
                    QW_STATUS_ERR_FORMAT);
                break;
            }
            if (!output_size) {
                rc = svgz_error(report, "SVGZ contains no SVG document",
                                QW_STATUS_ERR_FORMAT);
                break;
            }
            *out_svg = output;
            *out_svg_size = output_size;
            output = NULL;
            rc = QW_STATUS_OK;
            break;
        }
        if (zrc != Z_OK) {
            rc = svgz_error(report, "SVGZ decompression failed",
                            zrc == Z_MEM_ERROR ? QW_STATUS_ERR_OUT_OF_MEMORY
                                               : QW_STATUS_ERR_DECODE);
            break;
        }
        if (input_before == stream.avail_in && output_before == stream.avail_out) {
            rc = svgz_error(report, "SVGZ decompressor made no progress",
                            QW_STATUS_ERR_DECODE);
            break;
        }
    }

    if (initialized) (void)inflateEnd(&stream);
    free(output);
    return rc;
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

