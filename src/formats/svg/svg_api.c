#include "quirewise/svg.h"

#include "quirewise/status.h"
#include "svg_native.h"
#include "byte_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int qw_svg_abi_version(void)
{
    return QW_SVG_ABI_VERSION;
}

static QwSvgStatus qw_svg_map_status(int status)
{
    switch (status) {
    case QW_STATUS_OK:
        return QW_SVG_OK;
    case QW_STATUS_ERR_INVALID_ARGUMENT:
        return QW_SVG_ERR_INVALID_ARGUMENT;
    case QW_STATUS_ERR_OUT_OF_MEMORY:
        return QW_SVG_ERR_OUT_OF_MEMORY;
    case QW_STATUS_ERR_UNSUPPORTED:
        return QW_SVG_ERR_UNSUPPORTED;
    case QW_STATUS_ERR_RESOURCE_LIMIT:
        return QW_SVG_ERR_RESOURCE_LIMIT;
    default:
        return QW_SVG_ERR_FORMAT;
    }
}

static void qw_svg_set_error(QwSvgError *out_error, QwSvgStatus status,
                             const char *message)
{
    if (!out_error) return;
    memset(out_error, 0, sizeof(*out_error));
    out_error->status = status;
    if (message) {
        (void)snprintf(out_error->message, sizeof(out_error->message), "%s",
                       message);
    }
}

QwSvgStatus qw_svg_decode(const void *data, size_t size,
                          QwSvgImage *out_image, QwSvgError *out_error)
{
    QwConvertReport report = {0};
    QwSvgScene *scene = NULL;
    uint8_t *pixels = NULL;
    uint32_t width = 0u;
    uint32_t height = 0u;
    size_t stride;
    size_t image_size;
    QwSvgStatus status;
    int rc;

    if (out_image) memset(out_image, 0, sizeof(*out_image));
    if (out_error) memset(out_error, 0, sizeof(*out_error));
    if (!data || !size || !out_image) {
        qw_svg_set_error(out_error, QW_SVG_ERR_INVALID_ARGUMENT,
                         "SVG input and output image are required");
        return QW_SVG_ERR_INVALID_ARGUMENT;
    }

    rc = qw_svg_parse_native((const uint8_t *)data, size, &scene, &report);
    if (rc == QW_STATUS_OK) {
        rc = qw_svg_render_native(scene, &pixels, &width, &height, &report);
    }
    qw_svg_scene_drop(scene);
    if (rc != QW_STATUS_OK) {
        status = qw_svg_map_status(rc);
        qw_svg_set_error(out_error, status, report.note);
        free(pixels);
        return status;
    }

    if (!qw_checked_size_mul((size_t)width, 4u, &stride) ||
        !qw_checked_size_mul(stride, (size_t)height, &image_size)) {
        free(pixels);
        qw_svg_set_error(out_error, QW_SVG_ERR_RESOURCE_LIMIT,
                         "Rendered SVG image dimensions overflow");
        return QW_SVG_ERR_RESOURCE_LIMIT;
    }
    out_image->pixels = pixels;
    out_image->size = image_size;
    out_image->stride = stride;
    out_image->width = width;
    out_image->height = height;
    qw_svg_set_error(out_error, QW_SVG_OK, NULL);
    return QW_SVG_OK;
}

void qw_svg_image_free(QwSvgImage *image)
{
    if (!image) return;
    free(image->pixels);
    memset(image, 0, sizeof(*image));
}

const char *qw_svg_status_string(QwSvgStatus status)
{
    switch (status) {
    case QW_SVG_OK:
        return "success";
    case QW_SVG_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case QW_SVG_ERR_OUT_OF_MEMORY:
        return "out of memory";
    case QW_SVG_ERR_FORMAT:
        return "invalid SVG";
    case QW_SVG_ERR_UNSUPPORTED:
        return "unsupported SVG feature";
    case QW_SVG_ERR_RESOURCE_LIMIT:
        return "SVG resource limit exceeded";
    default:
        return "unknown SVG status";
    }
}
