#include "formats/cad/cad_internal.h"

#include <quirewise/charset.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef enum DxfValueType {
    DXF_VALUE_STRING = 1,
    DXF_VALUE_DOUBLE,
    DXF_VALUE_INT
} DxfValueType;

typedef struct DxfPair {
    int code;
    DxfValueType type;
    char *string;
    size_t string_size;
    double number;
    long long integer;
} DxfPair;

typedef struct DxfLexer {
    const uint8_t *data;
    size_t size;
    size_t position;
    int binary;
    int failed;
    char *buffer;
    size_t buffer_size;
    size_t buffer_capacity;
    DxfPair pending;
    char *pending_owned;
    int has_pending;
} DxfLexer;

typedef struct QwCadParser {
    DxfLexer lexer;
    QwCadDocument *document;
    int saw_section;
    size_t unsupported_entities;
} QwCadParser;

static char *cad_strdup(const char *text, size_t length)
{
    char *copy = (char *)malloc(length + 1u);
    if (!copy) return NULL;
    if (length) memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static int dxf_buffer_reserve(DxfLexer *lexer, size_t need)
{
    char *grown;
    size_t capacity = lexer->buffer_capacity ? lexer->buffer_capacity : 64u;
    if (need <= lexer->buffer_capacity) return 1;
    while (capacity < need) {
        if (capacity > QW_CAD_MAX_TEXT_BYTES) return 0;
        capacity *= 2u;
    }
    grown = (char *)realloc(lexer->buffer, capacity);
    if (!grown) return 0;
    lexer->buffer = grown;
    lexer->buffer_capacity = capacity;
    return 1;
}

static int dxf_read_line(DxfLexer *lexer, const char **out, size_t *out_size)
{
    size_t start;
    size_t end;
    if (lexer->position >= lexer->size) return 0;
    start = lexer->position;
    while (lexer->position < lexer->size && lexer->data[lexer->position] != '\n')
        ++lexer->position;
    end = lexer->position;
    if (lexer->position < lexer->size) ++lexer->position;
    if (end > start && lexer->data[end - 1u] == '\r') --end;
    *out = (const char *)lexer->data + start;
    *out_size = end - start;
    return 1;
}

static int dxf_read_le(const uint8_t *data, size_t size, size_t *position,
                       void *out, size_t count)
{
    if (*position > size || size - *position < count) return 0;
    memcpy(out, data + *position, count);
    *position += count;
    return 1;
}

static int dxf_binary_next(DxfLexer *lexer, DxfPair *pair)
{
    uint8_t code = 0;
    if (!dxf_read_le(lexer->data, lexer->size, &lexer->position, &code, 1u)) {
        lexer->failed = 1;
        return 0;
    }
    pair->code = code;
    if (code == 0xFFu) {
        uint16_t wide = 0;
        if (!dxf_read_le(lexer->data, lexer->size, &lexer->position, &wide,
                         2u)) {
            lexer->failed = 1;
            return 0;
        }
        pair->code = (int)wide;
    }
    if (pair->code <= 9 || (pair->code >= 100 && pair->code <= 109) ||
        pair->code == 102 || (pair->code >= 300 && pair->code <= 309) ||
        (pair->code >= 320 && pair->code <= 369) ||
        (pair->code >= 390 && pair->code <= 399) ||
        (pair->code >= 410 && pair->code <= 419) ||
        (pair->code >= 430 && pair->code <= 439) ||
        (pair->code >= 470 && pair->code <= 481) ||
        (pair->code >= 1000 && pair->code <= 1009)) {
        size_t length = 0;
        const uint8_t *start;
        pair->type = DXF_VALUE_STRING;
        start = lexer->data + lexer->position;
        while (lexer->position < lexer->size && lexer->data[lexer->position])
            ++lexer->position;
        if (lexer->position >= lexer->size) {
            lexer->failed = 1;
            return 0;
        }
        length = (size_t)(lexer->data + lexer->position - start);
        ++lexer->position;
        if (!dxf_buffer_reserve(lexer, length + 1u)) {
            lexer->failed = 1;
            return 0;
        }
        memcpy(lexer->buffer, start, length);
        lexer->buffer[length] = '\0';
        pair->string = lexer->buffer;
        pair->string_size = length;
        return 1;
    }
    if ((pair->code >= 10 && pair->code <= 59) ||
        (pair->code >= 110 && pair->code <= 149) ||
        (pair->code >= 210 && pair->code <= 239) ||
        (pair->code >= 460 && pair->code <= 469) ||
        (pair->code >= 1010 && pair->code <= 1059)) {
        double value = 0.0;
        if (!dxf_read_le(lexer->data, lexer->size, &lexer->position, &value,
                         8u)) {
            lexer->failed = 1;
            return 0;
        }
        pair->type = DXF_VALUE_DOUBLE;
        pair->number = value;
        return 1;
    }
    if ((pair->code >= 60 && pair->code <= 79) ||
        (pair->code >= 170 && pair->code <= 179) ||
        (pair->code >= 270 && pair->code <= 289) ||
        (pair->code >= 370 && pair->code <= 389) ||
        (pair->code >= 400 && pair->code <= 409) ||
        (pair->code >= 1060 && pair->code <= 1070)) {
        int16_t value = 0;
        if (!dxf_read_le(lexer->data, lexer->size, &lexer->position, &value,
                         2u)) {
            lexer->failed = 1;
            return 0;
        }
        pair->type = DXF_VALUE_INT;
        pair->integer = value;
        return 1;
    }
    if ((pair->code >= 90 && pair->code <= 99) ||
        (pair->code >= 420 && pair->code <= 459) || pair->code == 1071) {
        int32_t value = 0;
        if (!dxf_read_le(lexer->data, lexer->size, &lexer->position, &value,
                         4u)) {
            lexer->failed = 1;
            return 0;
        }
        pair->type = DXF_VALUE_INT;
        pair->integer = value;
        return 1;
    }
    if (pair->code >= 160 && pair->code <= 169) {
        int64_t value = 0;
        if (!dxf_read_le(lexer->data, lexer->size, &lexer->position, &value,
                         8u)) {
            lexer->failed = 1;
            return 0;
        }
        pair->type = DXF_VALUE_INT;
        pair->integer = value;
        return 1;
    }
    if (pair->code >= 290 && pair->code <= 299) {
        uint8_t value = 0;
        if (!dxf_read_le(lexer->data, lexer->size, &lexer->position, &value,
                         1u)) {
            lexer->failed = 1;
            return 0;
        }
        pair->type = DXF_VALUE_INT;
        pair->integer = value;
        return 1;
    }
    if (pair->code >= 310 && pair->code <= 319) {
        uint8_t length = 0;
        if (!dxf_read_le(lexer->data, lexer->size, &lexer->position, &length,
                         1u) ||
            lexer->position + length > lexer->size) {
            lexer->failed = 1;
            return 0;
        }
        lexer->position += length;
        pair->type = DXF_VALUE_STRING;
        pair->string = lexer->buffer;
        pair->string_size = 0;
        if (lexer->buffer) lexer->buffer[0] = '\0';
        return 1;
    }
    /* Unknown binary code: consume a NUL-terminated string as a safe default. */
    {
        const uint8_t *start = lexer->data + lexer->position;
        size_t length;
        while (lexer->position < lexer->size && lexer->data[lexer->position])
            ++lexer->position;
        if (lexer->position >= lexer->size) {
            lexer->failed = 1;
            return 0;
        }
        length = (size_t)(lexer->data + lexer->position - start);
        ++lexer->position;
        if (!dxf_buffer_reserve(lexer, length + 1u)) {
            lexer->failed = 1;
            return 0;
        }
        memcpy(lexer->buffer, start, length);
        lexer->buffer[length] = '\0';
        pair->type = DXF_VALUE_STRING;
        pair->string = lexer->buffer;
        pair->string_size = length;
        return 1;
    }
}

static int dxf_next(DxfLexer *lexer, DxfPair *pair)
{
    if (lexer->has_pending) {
        *pair = lexer->pending;
        lexer->has_pending = 0;
        return 1;
    }
    free(lexer->pending_owned);
    lexer->pending_owned = NULL;
    pair->string = NULL;
    if (lexer->binary) return dxf_binary_next(lexer, pair);
    for (;;) {
        const char *code_text;
        size_t code_size;
        const char *value_text;
        size_t value_size;
        char *end = NULL;
        long code;
        if (!dxf_read_line(lexer, &code_text, &code_size)) return 0;
        if (code_size == 0) continue;
        code = strtol(code_text, &end, 10);
        if (end == code_text) continue;
        if (!dxf_read_line(lexer, &value_text, &value_size)) {
            lexer->failed = 1;
            return 0;
        }
        pair->code = (int)code;
        pair->type = DXF_VALUE_STRING;
        pair->string_size = value_size;
        if (!dxf_buffer_reserve(lexer, value_size + 1u)) return 0;
        memcpy(lexer->buffer, value_text, value_size);
        lexer->buffer[value_size] = '\0';
        pair->string = lexer->buffer;
        if (pair->code == 999) continue;
        return 1;
    }
}

static void dxf_lexer_dispose(DxfLexer *lexer)
{
    free(lexer->buffer);
    lexer->buffer = NULL;
    lexer->buffer_capacity = 0u;
    free(lexer->pending_owned);
    lexer->pending_owned = NULL;
}

static void dxf_unread(DxfLexer *lexer, const DxfPair *pair)
{
    char *copy = NULL;
    if (pair->string)
        copy = cad_strdup(pair->string, pair->string_size);
    if (lexer->has_pending) {
        lexer->pending.string = NULL;
        lexer->pending.string_size = 0u;
    }
    free(lexer->pending_owned);
    lexer->pending_owned = copy;
    lexer->pending = *pair;
    lexer->pending.string = copy;
    lexer->has_pending = 1;
}

static int pair_string_is(const DxfPair *pair, const char *text)
{
    size_t length = strlen(text);
    return pair->type == DXF_VALUE_STRING && pair->string &&
           pair->string_size == length &&
           strncmp(pair->string, text, length) == 0;
}

static double pair_double(const DxfPair *pair)
{
    if (pair->type == DXF_VALUE_DOUBLE) return pair->number;
    if (pair->type == DXF_VALUE_INT) return (double)pair->integer;
    if (pair->type == DXF_VALUE_STRING && pair->string) {
        char *end = NULL;
        double value = strtod(pair->string, &end);
        return end == pair->string ? 0.0 : value;
    }
    return 0.0;
}

static long long pair_integer(const DxfPair *pair)
{
    if (pair->type == DXF_VALUE_INT) return pair->integer;
    if (pair->type == DXF_VALUE_DOUBLE) return (long long)pair->number;
    if (pair->type == DXF_VALUE_STRING && pair->string) {
        char *end = NULL;
        long long value = strtoll(pair->string, &end, 10);
        return end == pair->string ? 0 : value;
    }
    return 0;
}

static const char *pair_string(const DxfPair *pair, size_t *out_size)
{
    if (pair->type != DXF_VALUE_STRING || !pair->string) {
        if (out_size) *out_size = 0u;
        return "";
    }
    if (out_size) *out_size = pair->string_size;
    return pair->string;
}

static QwCadEntity *cad_add_entity_to(QwCadEntity **list, size_t *count,
                                      size_t *capacity)
{
    QwCadEntity *entity;
    if (*count >= QW_CAD_MAX_ENTITIES) return NULL;
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2u : 64u;
        QwCadEntity *grown =
            (QwCadEntity *)realloc(*list, next * sizeof(**list));
        if (!grown) return NULL;
        *list = grown;
        *capacity = next;
    }
    entity = &(*list)[(*count)++];
    memset(entity, 0, sizeof(*entity));
    entity->layer = 0u;
    entity->scale_x = 1.0;
    entity->scale_y = 1.0;
    return entity;
}

QwCadEntity *qw_cad_document_add_entity(QwCadDocument *document)
{
    return cad_add_entity_to(&document->entities, &document->entity_count,
                             &document->entity_capacity);
}

QwCadBlock *qw_cad_document_add_block(QwCadDocument *document, const char *name)
{
    QwCadBlock *block;
    if (document->block_count >= 65536u) return NULL;
    if (document->block_count == document->block_capacity) {
        size_t next = document->block_capacity ? document->block_capacity * 2u
                                               : 16u;
        QwCadBlock *grown = (QwCadBlock *)realloc(
            document->blocks, next * sizeof(*document->blocks));
        if (!grown) return NULL;
        document->blocks = grown;
        document->block_capacity = next;
    }
    block = &document->blocks[document->block_count++];
    memset(block, 0, sizeof(*block));
    block->name = cad_strdup(name ? name : "", name ? strlen(name) : 0u);
    if (!block->name) return NULL;
    block->name_size = name ? strlen(name) : 0u;
    return block;
}

QwCadBlock *qw_cad_document_find_block(QwCadDocument *document,
                                       const char *name)
{
    size_t index;
    for (index = 0u; index < document->block_count; ++index) {
        if (document->blocks[index].name &&
            strcmp(document->blocks[index].name, name) == 0)
            return &document->blocks[index];
    }
    return NULL;
}

size_t qw_cad_document_layer_index(QwCadDocument *document, const char *name,
                                   int create)
{
    size_t index;
    for (index = 0u; index < document->layer_count; ++index) {
        if (document->layers[index].name &&
            strcmp(document->layers[index].name, name) == 0)
            return index;
    }
    if (!create) return 0u;
    if (document->layer_count >= 4096u) return 0u;
    if (document->layer_count == document->layer_capacity) {
        size_t next = document->layer_capacity ? document->layer_capacity * 2u
                                               : 16u;
        QwCadLayer *grown = (QwCadLayer *)realloc(
            document->layers, next * sizeof(*document->layers));
        if (!grown) return 0u;
        document->layers = grown;
        document->layer_capacity = next;
    }
    index = document->layer_count++;
    memset(&document->layers[index], 0, sizeof(document->layers[index]));
    document->layers[index].name = cad_strdup(name, strlen(name));
    document->layers[index].name_size = strlen(name);
    document->layers[index].color = 0x00FFFFFFu;
    document->layers[index].visible = 1;
    return index;
}

uint32_t qw_cad_aci_color(int aci)
{
    static const uint32_t basic[10] = {
        0x00000000u, 0x00FF0000u, 0x00FFFF00u, 0x0000FF00u, 0x0000FFFFu,
        0x000000FFu, 0x00FF00FFu, 0x00FFFFFFu, 0x00808080u, 0x00C0C0C0u
    };
    if (aci < 0) aci = -aci;
    if (aci <= 9) {
        if (aci == 7) return 0x00000000u;
        return basic[aci];
    }
    if (aci >= 250 && aci <= 255) {
        int gray = 51 + (aci - 250) * 40;
        if (gray > 255) gray = 255;
        return ((uint32_t)gray << 16) | ((uint32_t)gray << 8) |
               (uint32_t)gray;
    }
    {
        double hue = ((aci - 10) * 15.0) / 180.0 * M_PI;
        double value = 0.65 + 0.35 * (((aci - 10) % 2) ? 0.7 : 1.0);
        double r = value * (0.5 + 0.5 * cos(hue));
        double g = value * (0.5 + 0.5 * cos(hue - 2.09439510239));
        double b = value * (0.5 + 0.5 * cos(hue + 2.09439510239));
        return ((uint32_t)(r * 255.0) << 16) |
               ((uint32_t)(g * 255.0) << 8) | (uint32_t)(b * 255.0);
    }
}

static const char *codepage_name(const char *codepage)
{
    if (!codepage) return "windows-1252";
    if (strstr(codepage, "936") || strstr(codepage, "GBK")) return "GBK";
    if (strstr(codepage, "950") || strstr(codepage, "Big5")) return "Big5";
    if (strstr(codepage, "932") || strstr(codepage, "Shift")) return "CP932";
    if (strstr(codepage, "949")) return "CP949";
    if (strstr(codepage, "874")) return "windows-874";
    if (strstr(codepage, "1250")) return "windows-1250";
    if (strstr(codepage, "1251")) return "windows-1251";
    if (strstr(codepage, "1252")) return "windows-1252";
    if (strstr(codepage, "1253")) return "windows-1253";
    if (strstr(codepage, "1254")) return "windows-1254";
    if (strstr(codepage, "1255")) return "windows-1255";
    if (strstr(codepage, "1256")) return "windows-1256";
    if (strstr(codepage, "1257")) return "windows-1257";
    if (strstr(codepage, "1258")) return "windows-1258";
    return "windows-1252";
}

char *qw_cad_text_utf8(const QwCadDocument *document, const char *input,
                       size_t length)
{
    size_t needed = 0u;
    char *output;
    QwCharsetStatus status;
    if (!input || !length) return cad_strdup("", 0u);
    if (document->utf8_strings || !document->codepage)
        return cad_strdup(input, length);
    status = qw_charset_convert(codepage_name(document->codepage), "UTF-8",
                                input, length, NULL, 0u, &needed);
    if (status != QW_CHARSET_OK || needed == 0u)
        return cad_strdup(input, length);
    output = (char *)malloc(needed + 1u);
    if (!output) return NULL;
    status = qw_charset_convert(codepage_name(document->codepage), "UTF-8",
                                input, length, output, needed, &needed);
    if (status != QW_CHARSET_OK) {
        free(output);
        return cad_strdup(input, length);
    }
    output[needed] = '\0';
    return output;
}

static void entity_clear(QwCadEntity *entity)
{
    free(entity->points);
    free(entity->text);
    free(entity->block_name);
    memset(entity, 0, sizeof(*entity));
}

static int entity_add_point(QwCadEntity *entity, QwCadPoint point)
{
    if (entity->point_count >= QW_CAD_MAX_POINTS) return 0;
    if ((entity->point_count & (entity->point_count - 1u)) == 0u) {
        size_t next = entity->point_count ? entity->point_count * 2u : 8u;
        QwCadPoint *grown = (QwCadPoint *)realloc(
            entity->points, next * sizeof(*entity->points));
        if (!grown) return 0;
        entity->points = grown;
    }
    entity->points[entity->point_count++] = point;
    return 1;
}

static double normalize_angle(double angle)
{
    while (angle < 0.0) angle += 360.0;
    while (angle >= 360.0) angle -= 360.0;
    return angle;
}

static int parse_lwpolyline(QwCadParser *parser, QwCadEntity *entity,
                            const DxfPair *first)
{
    DxfPair pair;
    long long vertex_count = 0;
    long long vertex_index = 0;
    (void)first;
    entity->kind = QW_CAD_ENTITY_POLYLINE;
    for (;;) {
        if (!dxf_next(&parser->lexer, &pair)) return 0;
        if (pair.code == 0) {
            dxf_unread(&parser->lexer, &pair);
            break;
        }
        switch (pair.code) {
        case 90: vertex_count = pair_integer(&pair); break;
        case 70: entity->closed = (pair_integer(&pair) & 1) != 0; break;
        case 43: entity->height = pair_double(&pair); break;
        case 10:
            if (entity_add_point(entity, (QwCadPoint){pair_double(&pair), 0.0}))
                ++vertex_index;
            break;
        case 20:
            if (entity->point_count > 0u) {
                entity->points[entity->point_count - 1u].y =
                    pair_double(&pair);
            }
            break;
        default: break;
        }
    }
    if (vertex_count > 0 && entity->point_count == 0u) return 0;
    (void)vertex_index;
    return 1;
}

static int parse_polyline(QwCadParser *parser, QwCadEntity *entity,
                          const DxfPair *first)
{
    DxfPair pair;
    (void)first;
    entity->kind = QW_CAD_ENTITY_POLYLINE;
    for (;;) {
        if (!dxf_next(&parser->lexer, &pair)) return 0;
        if (pair.code == 0) {
            dxf_unread(&parser->lexer, &pair);
            break;
        }
        if (pair.code == 70)
            entity->closed = (pair_integer(&pair) & 1) != 0;
        else if (pair.code == 10)
            entity_add_point(entity, (QwCadPoint){pair_double(&pair), 0.0});
        else if (pair.code == 20 && entity->point_count > 0u)
            entity->points[entity->point_count - 1u].y = pair_double(&pair);
    }
    /* Read VERTEX records until SEQEND. */
    for (;;) {
        if (!dxf_next(&parser->lexer, &pair)) return 0;
        if (pair.code != 0) continue;
        if (pair_string_is(&pair, "VERTEX")) {
            QwCadPoint point = {0.0, 0.0};
            for (;;) {
                if (!dxf_next(&parser->lexer, &pair)) return 0;
                if (pair.code == 0) {
                    dxf_unread(&parser->lexer, &pair);
                    break;
                }
                if (pair.code == 10) point.x = pair_double(&pair);
                else if (pair.code == 20) point.y = pair_double(&pair);
            }
            entity_add_point(entity, point);
        } else if (pair_string_is(&pair, "SEQEND")) {
            for (;;) {
                if (!dxf_next(&parser->lexer, &pair)) return 0;
                if (pair.code == 0) {
                    dxf_unread(&parser->lexer, &pair);
                    break;
                }
            }
            break;
        } else {
            dxf_unread(&parser->lexer, &pair);
            break;
        }
    }
    return 1;
}

static char *mtext_plain(const char *input, size_t length)
{
    char *out = (char *)malloc(length + 1u);
    size_t index;
    size_t used = 0u;
    if (!out) return NULL;
    for (index = 0u; index < length; ++index) {
        char ch = input[index];
        if (ch == '\\' && index + 1u < length) {
            char next = input[index + 1u];
            if (next == 'P' || next == 'p') {
                out[used++] = '\n';
                ++index;
                continue;
            }
            if (next == '~') {
                out[used++] = ' ';
                ++index;
                continue;
            }
            if (next == '\\' || next == '{' || next == '}') {
                out[used++] = next;
                ++index;
                continue;
            }
            if (next == 'U' && index + 6u < length && input[index + 2u] == '+') {
                unsigned code = 0u;
                size_t digit;
                for (digit = 0u; digit < 4u; ++digit) {
                    char hex = input[index + 3u + digit];
                    code <<= 4u;
                    if (hex >= '0' && hex <= '9') code |= (unsigned)(hex - '0');
                    else if (hex >= 'A' && hex <= 'F') code |= (unsigned)(hex - 'A' + 10);
                    else if (hex >= 'a' && hex <= 'f') code |= (unsigned)(hex - 'a' + 10);
                    else code = 0u;
                }
                if (code) {
                    char utf8[4];
                    size_t utf8_len = 0u;
                    if (code < 0x80u) {
                        utf8[utf8_len++] = (char)code;
                    } else if (code < 0x800u) {
                        utf8[utf8_len++] = (char)(0xC0u | (code >> 6u));
                        utf8[utf8_len++] = (char)(0x80u | (code & 0x3Fu));
                    } else {
                        utf8[utf8_len++] = (char)(0xE0u | (code >> 12u));
                        utf8[utf8_len++] = (char)(0x80u | ((code >> 6u) & 0x3Fu));
                        utf8[utf8_len++] = (char)(0x80u | (code & 0x3Fu));
                    }
                    memcpy(out + used, utf8, utf8_len);
                    used += utf8_len;
                    index += 6u;
                    continue;
                }
            }
            if (next == 'f' || next == 'F' || next == 'H' || next == 'h' ||
                next == 'C' || next == 'c' || next == 'W' || next == 'w' ||
                next == 'Q' || next == 'q' || next == 'T' || next == 't' ||
                next == 'A' || next == 'a') {
                size_t scan = index + 2u;
                while (scan < length && input[scan] != ';') ++scan;
                index = scan < length ? scan : length;
                continue;
            }
        }
        if (ch == '{' || ch == '}') continue;
        if (ch == '%' && index + 2u < length && input[index + 1u] == '%') {
            char code = input[index + 2u];
            if (code == 'd' || code == 'D') {
                out[used++] = (char)0xB0;
                index += 2u;
                continue;
            }
            if (code == 'p' || code == 'P') {
                out[used++] = '+';
                index += 2u;
                continue;
            }
            if (code == 'c' || code == 'C') {
                out[used++] = 'O';
                index += 2u;
                continue;
            }
        }
        out[used++] = ch;
    }
    out[used] = '\0';
    return out;
}

static void set_entity_layer(QwCadParser *parser, QwCadEntity *entity,
                             const char *name)
{
    if (name && *name)
        entity->layer =
            qw_cad_document_layer_index(parser->document, name, 1);
}

static void set_entity_color(QwCadEntity *entity, const DxfPair *pair)
{
    if (pair->code == 62) {
        long long color = pair_integer(pair);
        entity->has_color = color != 256;
        if (entity->has_color)
            entity->color = qw_cad_aci_color((int)color);
    } else if (pair->code == 420) {
        entity->has_color = 1;
        entity->color = (uint32_t)pair_integer(pair) & 0x00FFFFFFu;
    }
}

static int parse_text_entity(QwCadParser *parser, QwCadEntity *entity,
                             int mtext)
{
    DxfPair pair;
    char *text = NULL;
    size_t text_size = 0u;
    size_t text_capacity = 0u;
    double align_x = 0.0;
    double align_y = 0.0;
    int has_align = 0;
    entity->kind = QW_CAD_ENTITY_TEXT;
    for (;;) {
        if (!dxf_next(&parser->lexer, &pair)) {
            free(text);
            return 0;
        }
        if (pair.code == 0) {
            dxf_unread(&parser->lexer, &pair);
            break;
        }
        switch (pair.code) {
        case 8: {
            size_t length = 0u;
            const char *layer = pair_string(&pair, &length);
            set_entity_layer(parser, entity, layer);
            break;
        }
        case 62:
        case 420: set_entity_color(entity, &pair); break;
        case 10: entity->p[0].x = pair_double(&pair); break;
        case 20: entity->p[0].y = pair_double(&pair); break;
        case 11:
            align_x = pair_double(&pair);
            has_align = 1;
            break;
        case 21:
            align_y = pair_double(&pair);
            has_align = 1;
            break;
        case 40: entity->height = pair_double(&pair); break;
        case 50: entity->rotation = pair_double(&pair); break;
        case 1:
        case 3: {
            size_t length = 0u;
            const char *value = pair_string(&pair, &length);
            if (text_size + length > QW_CAD_MAX_TEXT_BYTES) {
                free(text);
                return 0;
            }
            if (text_size + length + 1u > text_capacity) {
                size_t next = text_capacity ? text_capacity : 64u;
                char *grown;
                while (next < text_size + length + 1u) next *= 2u;
                grown = (char *)realloc(text, next);
                if (!grown) {
                    free(text);
                    return 0;
                }
                text = grown;
                text_capacity = next;
            }
            memcpy(text + text_size, value, length);
            text_size += length;
            text[text_size] = '\0';
            break;
        }
        default: break;
        }
    }
    if (has_align) {
        entity->p[0].x = align_x;
        entity->p[0].y = align_y;
    }
    if (!text) text = cad_strdup("", 0u);
    if (!text) return 0;
    if (mtext) {
        char *plain = mtext_plain(text, text_size);
        free(text);
        if (!plain) return 0;
        text = plain;
        text_size = strlen(plain);
    }
    {
        char *utf8 = qw_cad_text_utf8(parser->document, text, text_size);
        free(text);
        if (!utf8) return 0;
        entity->text = utf8;
        entity->text_size = strlen(utf8);
    }
    if (entity->height <= 0.0) entity->height = 2.5;
    return 1;
}

static int parse_insert(QwCadParser *parser, QwCadEntity *entity)
{
    DxfPair pair;
    entity->kind = QW_CAD_ENTITY_INSERT;
    entity->scale_x = 1.0;
    entity->scale_y = 1.0;
    for (;;) {
        if (!dxf_next(&parser->lexer, &pair)) return 0;
        if (pair.code == 0) {
            dxf_unread(&parser->lexer, &pair);
            break;
        }
        switch (pair.code) {
        case 8: {
            size_t length = 0u;
            const char *layer = pair_string(&pair, &length);
            set_entity_layer(parser, entity, layer);
            break;
        }
        case 62:
        case 420: set_entity_color(entity, &pair); break;
        case 2: {
            size_t length = 0u;
            const char *name = pair_string(&pair, &length);
            free(entity->block_name);
            entity->block_name = cad_strdup(name, length);
            break;
        }
        case 10: entity->p[0].x = pair_double(&pair); break;
        case 20: entity->p[0].y = pair_double(&pair); break;
        case 41: entity->scale_x = pair_double(&pair); break;
        case 42: entity->scale_y = pair_double(&pair); break;
        case 50: entity->rotation = pair_double(&pair); break;
        default: break;
        }
    }
    return entity->block_name != NULL;
}

static int skip_until_zero(QwCadParser *parser)
{
    DxfPair pair;
    for (;;) {
        if (!dxf_next(&parser->lexer, &pair)) return 0;
        if (pair.code == 0) {
            dxf_unread(&parser->lexer, &pair);
            return 1;
        }
    }
}

static int parse_entity(QwCadParser *parser, const char *type, QwCadEntity **out_list, size_t *out_count, size_t *out_capacity)
{
    QwCadEntity parsed;
    QwCadEntity *entity = &parsed;
    DxfPair pair;
    int ok = 1;
    memset(&parsed, 0, sizeof(parsed));
    parsed.layer = 0u;
    parsed.scale_x = 1.0;
    parsed.scale_y = 1.0;

    if (strcmp(type, "LINE") == 0) {
        parsed.kind = QW_CAD_ENTITY_LINE;
        for (;;) {
            if (!dxf_next(&parser->lexer, &pair)) { ok = 0; break; }
            if (pair.code == 0) { dxf_unread(&parser->lexer, &pair); break; }
            if (pair.code == 8) {
                size_t length = 0u;
                set_entity_layer(parser, &parsed, pair_string(&pair, &length));
            } else if (pair.code == 62 || pair.code == 420) {
                set_entity_color(&parsed, &pair);
            } else if (pair.code == 10) parsed.p[0].x = pair_double(&pair);
            else if (pair.code == 20) parsed.p[0].y = pair_double(&pair);
            else if (pair.code == 11) parsed.p[1].x = pair_double(&pair);
            else if (pair.code == 21) parsed.p[1].y = pair_double(&pair);
        }
    } else if (strcmp(type, "CIRCLE") == 0) {
        parsed.kind = QW_CAD_ENTITY_CIRCLE;
        for (;;) {
            if (!dxf_next(&parser->lexer, &pair)) { ok = 0; break; }
            if (pair.code == 0) { dxf_unread(&parser->lexer, &pair); break; }
            if (pair.code == 8) {
                size_t length = 0u;
                set_entity_layer(parser, &parsed, pair_string(&pair, &length));
            } else if (pair.code == 62 || pair.code == 420) {
                set_entity_color(&parsed, &pair);
            } else if (pair.code == 10) parsed.p[0].x = pair_double(&pair);
            else if (pair.code == 20) parsed.p[0].y = pair_double(&pair);
            else if (pair.code == 40) parsed.radius = pair_double(&pair);
        }
    } else if (strcmp(type, "ARC") == 0) {
        parsed.kind = QW_CAD_ENTITY_ARC;
        for (;;) {
            if (!dxf_next(&parser->lexer, &pair)) { ok = 0; break; }
            if (pair.code == 0) { dxf_unread(&parser->lexer, &pair); break; }
            if (pair.code == 8) {
                size_t length = 0u;
                set_entity_layer(parser, &parsed, pair_string(&pair, &length));
            } else if (pair.code == 62 || pair.code == 420) {
                set_entity_color(&parsed, &pair);
            } else if (pair.code == 10) parsed.p[0].x = pair_double(&pair);
            else if (pair.code == 20) parsed.p[0].y = pair_double(&pair);
            else if (pair.code == 40) parsed.radius = pair_double(&pair);
            else if (pair.code == 50) parsed.start_angle = pair_double(&pair);
            else if (pair.code == 51) parsed.end_angle = pair_double(&pair);
        }
        parsed.start_angle = normalize_angle(parsed.start_angle);
        parsed.end_angle = normalize_angle(parsed.end_angle);
    } else if (strcmp(type, "ELLIPSE") == 0) {
        parsed.kind = QW_CAD_ENTITY_ELLIPSE;
        parsed.ratio = 1.0;
        parsed.end_angle = 2.0 * M_PI;
        for (;;) {
            if (!dxf_next(&parser->lexer, &pair)) { ok = 0; break; }
            if (pair.code == 0) { dxf_unread(&parser->lexer, &pair); break; }
            if (pair.code == 8) {
                size_t length = 0u;
                set_entity_layer(parser, &parsed, pair_string(&pair, &length));
            } else if (pair.code == 62 || pair.code == 420) {
                set_entity_color(&parsed, &pair);
            } else if (pair.code == 10) parsed.p[0].x = pair_double(&pair);
            else if (pair.code == 20) parsed.p[0].y = pair_double(&pair);
            else if (pair.code == 11) parsed.p[1].x = pair_double(&pair);
            else if (pair.code == 21) parsed.p[1].y = pair_double(&pair);
            else if (pair.code == 40) parsed.ratio = pair_double(&pair);
            else if (pair.code == 41) parsed.start_angle = pair_double(&pair);
            else if (pair.code == 42) parsed.end_angle = pair_double(&pair);
        }
    } else if (strcmp(type, "LWPOLYLINE") == 0) {
        ok = parse_lwpolyline(parser, &parsed, NULL);
    } else if (strcmp(type, "POLYLINE") == 0) {
        ok = parse_polyline(parser, &parsed, NULL);
    } else if (strcmp(type, "SPLINE") == 0) {
        parsed.kind = QW_CAD_ENTITY_SPLINE;
        for (;;) {
            if (!dxf_next(&parser->lexer, &pair)) { ok = 0; break; }
            if (pair.code == 0) { dxf_unread(&parser->lexer, &pair); break; }
            if (pair.code == 8) {
                size_t length = 0u;
                set_entity_layer(parser, &parsed, pair_string(&pair, &length));
            } else if (pair.code == 62 || pair.code == 420) {
                set_entity_color(&parsed, &pair);
            } else if (pair.code == 10) {
                entity_add_point(&parsed, (QwCadPoint){pair_double(&pair), 0.0});
            } else if (pair.code == 20 && parsed.point_count > 0u) {
                parsed.points[parsed.point_count - 1u].y =
                    pair_double(&pair);
            }
        }
    } else if (strcmp(type, "TEXT") == 0 || strcmp(type, "ATTRIB") == 0 ||
               strcmp(type, "ATTDEF") == 0) {
        ok = parse_text_entity(parser, &parsed, 0);
    } else if (strcmp(type, "MTEXT") == 0) {
        ok = parse_text_entity(parser, &parsed, 1);
    } else if (strcmp(type, "INSERT") == 0) {
        ok = parse_insert(parser, &parsed);
    } else if (strcmp(type, "POINT") == 0) {
        parsed.kind = QW_CAD_ENTITY_POINT;
        for (;;) {
            if (!dxf_next(&parser->lexer, &pair)) { ok = 0; break; }
            if (pair.code == 0) { dxf_unread(&parser->lexer, &pair); break; }
            if (pair.code == 8) {
                size_t length = 0u;
                set_entity_layer(parser, &parsed, pair_string(&pair, &length));
            } else if (pair.code == 10) parsed.p[0].x = pair_double(&pair);
            else if (pair.code == 20) parsed.p[0].y = pair_double(&pair);
        }
    } else if (strcmp(type, "SOLID") == 0 || strcmp(type, "TRACE") == 0 ||
               strcmp(type, "3DFACE") == 0) {
        parsed.kind = QW_CAD_ENTITY_SOLID;
        for (;;) {
            if (!dxf_next(&parser->lexer, &pair)) { ok = 0; break; }
            if (pair.code == 0) { dxf_unread(&parser->lexer, &pair); break; }
            if (pair.code == 8) {
                size_t length = 0u;
                set_entity_layer(parser, &parsed, pair_string(&pair, &length));
            } else if (pair.code == 62 || pair.code == 420) {
                set_entity_color(&parsed, &pair);
            } else if (pair.code >= 10 && pair.code <= 13) {
                parsed.p[pair.code - 10].x = pair_double(&pair);
            } else if (pair.code >= 20 && pair.code <= 23) {
                parsed.p[pair.code - 20].y = pair_double(&pair);
            }
        }
    } else if (strcmp(type, "LEADER") == 0) {
        parsed.kind = QW_CAD_ENTITY_POLYLINE;
        for (;;) {
            if (!dxf_next(&parser->lexer, &pair)) { ok = 0; break; }
            if (pair.code == 0) { dxf_unread(&parser->lexer, &pair); break; }
            if (pair.code == 8) {
                size_t length = 0u;
                set_entity_layer(parser, &parsed, pair_string(&pair, &length));
            } else if (pair.code == 10) {
                entity_add_point(&parsed, (QwCadPoint){pair_double(&pair), 0.0});
            } else if (pair.code == 20 && parsed.point_count > 0u) {
                parsed.points[parsed.point_count - 1u].y = pair_double(&pair);
            }
        }
    } else if (strcmp(type, "DIMENSION") == 0) {
        /* A dimension can carry an anonymous block; treat the block reference
         * as an insert when present, otherwise skip its geometry. */
        parsed.kind = QW_CAD_ENTITY_INSERT;
        for (;;) {
            if (!dxf_next(&parser->lexer, &pair)) { ok = 0; break; }
            if (pair.code == 0) { dxf_unread(&parser->lexer, &pair); break; }
            if (pair.code == 2) {
                size_t length = 0u;
                const char *name = pair_string(&pair, &length);
                free(parsed.block_name);
                parsed.block_name = cad_strdup(name, length);
            }
        }
        if (!parsed.block_name) {
            entity_clear(&parsed);
            return skip_until_zero(parser);
        }
    } else {
        ++parser->unsupported_entities;
        entity_clear(&parsed);
        return skip_until_zero(parser);
    }

    if (!ok || entity->point_count > QW_CAD_MAX_POINTS) {
        entity_clear(&parsed);
        return 0;
    }
    entity = cad_add_entity_to(out_list, out_count, out_capacity);
    if (!entity) {
        entity_clear(&parsed);
        return 0;
    }
    *entity = parsed;
    return 1;
}

static void parse_header_pair(QwCadParser *parser, const char *name,
                              const DxfPair *pair)
{
    if (strcmp(name, "$ACADVER") == 0 && pair->type == DXF_VALUE_STRING) {
        size_t length = 0u;
        const char *value = pair_string(pair, &length);
        free(parser->document->version);
        parser->document->version = cad_strdup(value, length);
        parser->document->utf8_strings =
            length >= 5u && strncmp(value, "AC1021", 6u) >= 0;
        if (length >= 5u && strncmp(value, "AC1021", 6u) >= 0)
            parser->document->utf8_strings = 1;
    } else if (strcmp(name, "$DWGCODEPAGE") == 0 &&
               pair->type == DXF_VALUE_STRING) {
        size_t length = 0u;
        const char *value = pair_string(pair, &length);
        free(parser->document->codepage);
        parser->document->codepage = cad_strdup(value, length);
        if (strstr(value, "UTF-8") || strstr(value, "UTF8"))
            parser->document->utf8_strings = 1;
    }
}

static int parse_header(QwCadParser *parser)
{
    DxfPair pair;
    char *variable = NULL;
    for (;;) {
        if (!dxf_next(&parser->lexer, &pair)) {
            free(variable);
            return 0;
        }
        if (pair.code == 0) {
            dxf_unread(&parser->lexer, &pair);
            free(variable);
            return 1;
        }
        if (pair.code == 9) {
            size_t length = 0u;
            const char *name = pair_string(&pair, &length);
            free(variable);
            variable = cad_strdup(name, length);
            continue;
        }
        if (variable) parse_header_pair(parser, variable, &pair);
    }
}

static int parse_layers(QwCadParser *parser)
{
    DxfPair pair;
    for (;;) {
        if (!dxf_next(&parser->lexer, &pair)) return 0;
        if (pair.code == 0 && pair_string_is(&pair, "ENDTAB")) return 1;
        if (pair.code == 0 && pair_string_is(&pair, "LAYER")) {
            char *name = NULL;
            uint32_t color = 0x00FFFFFFu;
            int visible = 1;
            int has_true_color = 0;
            uint32_t true_color = 0;
            for (;;) {
                if (!dxf_next(&parser->lexer, &pair)) {
                    free(name);
                    return 0;
                }
                if (pair.code == 0) {
                    dxf_unread(&parser->lexer, &pair);
                    break;
                }
                if (pair.code == 2) {
                    size_t length = 0u;
                    const char *value = pair_string(&pair, &length);
                    free(name);
                    name = cad_strdup(value, length);
                } else if (pair.code == 62) {
                    long long aci = pair_integer(&pair);
                    visible = aci >= 0;
                    color = qw_cad_aci_color((int)aci);
                } else if (pair.code == 420) {
                    has_true_color = 1;
                    true_color = (uint32_t)pair_integer(&pair);
                }
            }
            if (name) {
                size_t index = qw_cad_document_layer_index(
                    parser->document, name, 1);
                parser->document->layers[index].color =
                    has_true_color ? (true_color & 0x00FFFFFFu) : color;
                parser->document->layers[index].visible = visible;
            }
            free(name);
        } else {
            if (pair.code == 0 && pair_string_is(&pair, "TABLE")) continue;
            /* Ignore unknown table records until ENDTAB. */
        }
    }
}

static int parse_tables(QwCadParser *parser)
{
    DxfPair pair;
    for (;;) {
        if (!dxf_next(&parser->lexer, &pair)) return 0;
        if (pair.code == 0 && pair_string_is(&pair, "ENDSEC")) return 1;
        if (pair.code == 0 && pair_string_is(&pair, "TABLE")) {
            for (;;) {
                if (!dxf_next(&parser->lexer, &pair)) return 0;
                if (pair.code == 2) {
                    size_t length = 0u;
                    const char *name = pair_string(&pair, &length);
                    if (strcmp(name, "LAYER") == 0) {
                        if (!parse_layers(parser)) return 0;
                    }
                    break;
                }
                if (pair.code == 0) {
                    dxf_unread(&parser->lexer, &pair);
                    break;
                }
            }
        }
    }
}

static int parse_entities_into(QwCadParser *parser, QwCadEntity **out,
                               size_t *out_count, size_t *out_capacity,
                               const char *terminator);

static int parse_blocks(QwCadParser *parser)
{
    DxfPair pair;
    for (;;) {
        if (!dxf_next(&parser->lexer, &pair)) return 0;
        if (pair.code == 0 && pair_string_is(&pair, "ENDSEC")) return 1;
        if (pair.code == 0 && pair_string_is(&pair, "BLOCK")) {
            QwCadBlock *block = NULL;
            char *name = NULL;
            QwCadPoint base = {0.0, 0.0};
            for (;;) {
                if (!dxf_next(&parser->lexer, &pair)) {
                    free(name);
                    return 0;
                }
                if (pair.code == 0) {
                    dxf_unread(&parser->lexer, &pair);
                    break;
                }
                if (pair.code == 2) {
                    size_t length = 0u;
                    const char *value = pair_string(&pair, &length);
                    free(name);
                    name = cad_strdup(value, length);
                } else if (pair.code == 10) base.x = pair_double(&pair);
                else if (pair.code == 20) base.y = pair_double(&pair);
            }
            if (!name) name = cad_strdup("", 0u);
            block = qw_cad_document_add_block(parser->document, name);
            free(name);
            if (!block) return 0;
            block->base = base;
            if (!parse_entities_into(parser, &block->entities,
                                     &block->entity_count,
                                     &block->entity_capacity, "ENDBLK"))
                return 0;
        }
    }
}

static int parse_entities_into(QwCadParser *parser, QwCadEntity **out,
                               size_t *out_count, size_t *out_capacity,
                               const char *terminator)
{
    DxfPair pair;
    for (;;) {
        if (!dxf_next(&parser->lexer, &pair)) return 0;
        if (pair.code == 0) {
            size_t length = 0u;
            const char *type = pair_string(&pair, &length);
            if ((terminator && strcmp(type, terminator) == 0) ||
                strcmp(type, "ENDSEC") == 0) {
                if (strcmp(type, "ENDSEC") == 0)
                    dxf_unread(&parser->lexer, &pair);
                return 1;
            }
            if (strcmp(type, "SECTION") == 0) {
                dxf_unread(&parser->lexer, &pair);
                return 1;
            }
            if (strcmp(type, "BLOCK") == 0 || strcmp(type, "ENDBLK") == 0) {
                dxf_unread(&parser->lexer, &pair);
                return 1;
            }
            if (!parse_entity(parser, type, out, out_count, out_capacity)) return 0;
        }
    }
}

static int parse_entities(QwCadParser *parser)
{
    return parse_entities_into(parser, &parser->document->entities,
                               &parser->document->entity_count,
                               &parser->document->entity_capacity, NULL);
}

static int dxf_parse_document(QwCadParser *parser)
{
    DxfPair pair;
    for (;;) {
        if (!dxf_next(&parser->lexer, &pair)) break;
        if (pair.code != 0) continue;
        if (pair_string_is(&pair, "EOF")) break;
        if (pair_string_is(&pair, "SECTION")) {
            size_t length = 0u;
            const char *name;
            parser->saw_section = 1;
            if (!dxf_next(&parser->lexer, &pair)) return 0;
            name = pair_string(&pair, &length);
            if (pair.code != 2) {
                dxf_unread(&parser->lexer, &pair);
                continue;
            }
            if (strcmp(name, "HEADER") == 0) {
                if (!parse_header(parser)) return 0;
            } else if (strcmp(name, "TABLES") == 0) {
                if (!parse_tables(parser)) return 0;
            } else if (strcmp(name, "BLOCKS") == 0) {
                if (!parse_blocks(parser)) return 0;
            } else if (strcmp(name, "ENTITIES") == 0) {
                if (!parse_entities(parser)) return 0;
            } else {
                for (;;) {
                    if (!dxf_next(&parser->lexer, &pair)) return 0;
                    if (pair.code == 0 && pair_string_is(&pair, "ENDSEC"))
                        break;
                }
            }
        }
    }
    return parser->saw_section;
}

static void document_init(QwCadDocument *document)
{
    memset(document, 0, sizeof(*document));
    document->format = QW_CAD_FORMAT_UNKNOWN;
    qw_cad_document_layer_index(document, "0", 1);
    if (document->layer_count > 0u)
        document->layers[0].color = 0x00000000u;
}

uint32_t qw_cad_abi_version(void)
{
    return QUIREWISE_CAD_ABI_VERSION;
}

QwCadStatus qw_cad_parse(const void *data, size_t size,
                         QwCadDocument **out_document)
{
    QwCadParser parser;
    QwCadDocument *document;
    const uint8_t *bytes = (const uint8_t *)data;
    static const uint8_t binary_magic[] = {
        'A', 'u', 't', 'o', 'C', 'A', 'D', ' ', 'B', 'i', 'n', 'a', 'r', 'y',
        ' ', 'D', 'X', 'F', '\r', '\n', 0x1A, 0x00
    };
    if (!out_document) return QW_CAD_ERR_INVALID_ARGUMENT;
    *out_document = NULL;
    if (!bytes || size < 4u) return QW_CAD_ERR_INVALID_ARGUMENT;
    if (size > QW_CAD_MAX_INPUT) return QW_CAD_ERR_RESOURCE_LIMIT;
    if (size >= 4u && memcmp(bytes, "AC10", 4u) == 0)
        return qw_cad_parse_dwg(bytes, size, out_document);
    document = (QwCadDocument *)calloc(1u, sizeof(*document));
    if (!document) return QW_CAD_ERR_OUT_OF_MEMORY;
    document_init(document);
    memset(&parser, 0, sizeof(parser));
    parser.document = document;
    parser.lexer.data = bytes;
    parser.lexer.size = size;
    parser.lexer.binary =
        size >= sizeof(binary_magic) &&
        memcmp(bytes, binary_magic, sizeof(binary_magic)) == 0;
    if (parser.lexer.binary) parser.lexer.position = sizeof(binary_magic);
    document->format = parser.lexer.binary ? QW_CAD_FORMAT_DXF_BINARY
                                           : QW_CAD_FORMAT_DXF_ASCII;
    if (!dxf_parse_document(&parser)) {
        int failed = parser.lexer.failed;
        qw_cad_document_free(document);
        dxf_lexer_dispose(&parser.lexer);
        if (!parser.saw_section) return QW_CAD_ERR_FORMAT;
        return failed ? QW_CAD_ERR_FORMAT : QW_CAD_ERR_RESOURCE_LIMIT;
    }
    dxf_lexer_dispose(&parser.lexer);
    qw_cad_document_expand_inserts(document);
    qw_cad_document_compute_bounds(document);
    *out_document = document;
    return QW_CAD_OK;
}

int qw_cad_probe(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t index;
    if (!bytes || size < 4u) return 0;
    if (memcmp(bytes, "AC10", 4u) == 0) return 1;
    if (size >= 22u &&
        memcmp(bytes, "AutoCAD Binary DXF\r\n\x1a\x00", 22u) == 0)
        return 1;
    for (index = 0u; index + 7u <= size && index < 4096u; ++index) {
        if (memcmp(bytes + index, "SECTION", 7u) == 0) return 1;
    }
    return 0;
}

QwCadFormat qw_cad_format(const QwCadDocument *document)
{
    return document ? document->format : QW_CAD_FORMAT_UNKNOWN;
}

size_t qw_cad_entity_count(const QwCadDocument *document)
{
    return document ? document->entity_count : 0u;
}

size_t qw_cad_layer_count(const QwCadDocument *document)
{
    return document ? document->layer_count : 0u;
}

QwCadStatus qw_cad_bounds(const QwCadDocument *document, QwCadBounds *out_bounds)
{
    if (!document || !out_bounds) return QW_CAD_ERR_INVALID_ARGUMENT;
    if (!document->has_bounds) {
        out_bounds->min_x = 0.0;
        out_bounds->min_y = 0.0;
        out_bounds->max_x = 1.0;
        out_bounds->max_y = 1.0;
    } else {
        *out_bounds = document->bounds;
    }
    return QW_CAD_OK;
}

size_t qw_cad_text_count(const QwCadDocument *document)
{
    size_t index;
    size_t count = 0u;
    if (!document) return 0u;
    for (index = 0u; index < document->entity_count; ++index) {
        if (document->entities[index].kind == QW_CAD_ENTITY_TEXT) ++count;
    }
    return count;
}

QwCadStatus qw_cad_text_at(const QwCadDocument *document, size_t index,
                           QwCadTextInfo *out_text)
{
    size_t cursor = 0u;
    size_t entity_index;
    if (!document || !out_text) return QW_CAD_ERR_INVALID_ARGUMENT;
    for (entity_index = 0u; entity_index < document->entity_count;
         ++entity_index) {
        const QwCadEntity *entity = &document->entities[entity_index];
        if (entity->kind != QW_CAD_ENTITY_TEXT) continue;
        if (cursor++ != index) continue;
        out_text->text = entity->text ? entity->text : "";
        out_text->text_size = entity->text ? entity->text_size : 0u;
        out_text->x = entity->p[0].x;
        out_text->y = entity->p[0].y;
        out_text->height = entity->height;
        out_text->rotation = entity->rotation;
        out_text->layer = document->layers[entity->layer].name;
        out_text->layer_size = document->layers[entity->layer].name_size;
        return QW_CAD_OK;
    }
    return QW_CAD_ERR_INVALID_ARGUMENT;
}

const char *qw_cad_status_string(QwCadStatus status)
{
    switch (status) {
    case QW_CAD_OK: return "ok";
    case QW_CAD_ERR_INVALID_ARGUMENT: return "invalid argument";
    case QW_CAD_ERR_OUT_OF_MEMORY: return "out of memory";
    case QW_CAD_ERR_FORMAT: return "invalid DXF";
    case QW_CAD_ERR_UNSUPPORTED: return "unsupported CAD format";
    case QW_CAD_ERR_RESOURCE_LIMIT: return "resource limit";
    default: return "unknown CAD status";
    }
}

void qw_cad_buffer_free(void *buffer)
{
    free(buffer);
}





