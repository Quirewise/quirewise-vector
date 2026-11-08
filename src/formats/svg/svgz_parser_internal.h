#ifndef QUIREWISE_SVGZ_PARSER_INTERNAL_H
#define QUIREWISE_SVGZ_PARSER_INTERNAL_H

#include "quirewise/format.h"

int qw_svgz_decompress_internal(const void *data, size_t size,
                                uint8_t **out_svg, size_t *out_svg_size,
                                QwConvertReport *report);

#endif
