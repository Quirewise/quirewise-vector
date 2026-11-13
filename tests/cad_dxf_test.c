#include <quirewise/cad.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char kAsciiDxf[] =
    "0\nSECTION\n2\nHEADER\n9\n$ACADVER\n1\nAC1015\n9\n$DWGCODEPAGE\n3\n"
    "ANSI_1252\n0\nENDSEC\n"
    "0\nSECTION\n2\nTABLES\n0\nTABLE\n2\nLAYER\n0\nLAYER\n2\nWALLS\n62\n1\n"
    "70\n0\n0\nENDTAB\n0\nENDSEC\n"
    "0\nSECTION\n2\nBLOCKS\n0\nBLOCK\n2\nDOOR\n10\n0\n20\n0\n0\nLINE\n8\n"
    "WALLS\n10\n0\n20\n0\n11\n1\n21\n0\n0\nENDBLK\n0\nENDSEC\n"
    "0\nSECTION\n2\nENTITIES\n"
    "0\nLINE\n8\nWALLS\n10\n0\n20\n0\n11\n10\n21\n0\n"
    "0\nCIRCLE\n10\n5\n20\n5\n40\n2\n"
    "0\nLWPOLYLINE\n90\n3\n70\n1\n10\n1\n20\n1\n10\n2\n20\n1\n10\n2\n20\n2\n"
    "0\nTEXT\n10\n1\n20\n2\n40\n1\n1\nHello CAD\n"
    "0\nINSERT\n2\nDOOR\n10\n10\n20\n0\n50\n90\n"
    "0\nSOLID\n10\n0\n20\n0\n11\n1\n21\n0\n12\n1\n22\n1\n13\n0\n23\n1\n"
    "0\nENDSEC\n0\nEOF\n";

static void put_code(unsigned char *out, size_t *position, unsigned value)
{
    out[(*position)++] = (unsigned char)(value & 0xFFu);
}



static void put_double(unsigned char *out, size_t *position, double value)
{
    unsigned char bytes[8];
    memcpy(bytes, &value, 8u);
    memcpy(out + *position, bytes, 8u);
    *position += 8u;
}

static void put_string(unsigned char *out, size_t *position, const char *text)
{
    size_t length = strlen(text) + 1u;
    memcpy(out + *position, text, length);
    *position += length;
}

static size_t build_binary_dxf(unsigned char *out)
{
    static const char magic[] = "AutoCAD Binary DXF\r\n\x1a\x00";
    size_t position = 0u;
    memcpy(out, magic, 22u);
    position = 22u;
    put_code(out, &position, 0u); put_string(out, &position, "SECTION");
    put_code(out, &position, 2u); put_string(out, &position, "ENTITIES");
    put_code(out, &position, 0u); put_string(out, &position, "LINE");
    put_code(out, &position, 10u); put_double(out, &position, 1.0);
    put_code(out, &position, 20u); put_double(out, &position, 1.0);
    put_code(out, &position, 11u); put_double(out, &position, 3.0);
    put_code(out, &position, 21u); put_double(out, &position, 4.0);
    put_code(out, &position, 0u); put_string(out, &position, "ENDSEC");
    put_code(out, &position, 0u); put_string(out, &position, "EOF");
    return position;
}

static int check(int condition, const char *message)
{
    if (!condition) fprintf(stderr, "FAIL: %s\n", message);
    return condition ? 1 : 0;
}

int main(void)
{
    QwCadDocument *document = NULL;
    QwCadBounds bounds;
    QwCadTextInfo text;
    QwCadImage image;
    char *svg = NULL;
    size_t svg_size = 0u;
    int ok = 1;
    size_t index;
    int non_white = 0;
    unsigned char binary[512];
    size_t binary_size;

    ok &= check(qw_cad_probe(kAsciiDxf, sizeof(kAsciiDxf) - 1u),
                "ASCII probe");
    ok &= check(qw_cad_parse(kAsciiDxf, sizeof(kAsciiDxf) - 1u, &document) ==
                    QW_CAD_OK,
                "ASCII parse");
    if (!document) return 1;
    ok &= check(qw_cad_format(document) == QW_CAD_FORMAT_DXF_ASCII,
                "ASCII format");
    ok &= check(qw_cad_entity_count(document) == 6u, "entity count");
    ok &= check(qw_cad_layer_count(document) >= 2u, "layer count");
    ok &= check(qw_cad_text_count(document) == 1u, "text count");
    ok &= check(qw_cad_bounds(document, &bounds) == QW_CAD_OK, "bounds");
    ok &= check(bounds.min_x <= 0.001 && bounds.max_x >= 9.999, "bounds x");
    ok &= check(bounds.min_y <= 0.001 && bounds.max_y >= 6.999, "bounds y");
    ok &= check(qw_cad_text_at(document, 0u, &text) == QW_CAD_OK, "text at");
    ok &= check(text.text && strcmp(text.text, "Hello CAD") == 0, "text");
    ok &= check(text.height > 0.9 && text.height < 1.1, "text height");
    ok &= check(qw_cad_render_rgba(document, 256u, &image) == QW_CAD_OK,
                "raster");
    if (image.pixels) {
        for (index = 0u; index < image.size; index += 4u) {
            if (image.pixels[index] != 0xFFu ||
                image.pixels[index + 1u] != 0xFFu ||
                image.pixels[index + 2u] != 0xFFu) {
                non_white = 1;
                break;
            }
        }
    }
    ok &= check(non_white, "raster has ink");
    ok &= check(qw_cad_render_svg(document, &svg, &svg_size) == QW_CAD_OK,
                "svg");
    ok &= check(svg && svg_size > 0u && strstr(svg, "Hello CAD") != NULL,
                "svg text");
    ok &= check(svg && strstr(svg, "data-layer=\"WALLS\"") != NULL,
                "svg layer");
    qw_cad_buffer_free(svg);
    qw_cad_image_free(&image);
    qw_cad_document_free(document);
    document = NULL;

    binary_size = build_binary_dxf(binary);
    ok &= check(qw_cad_probe(binary, binary_size), "binary probe");
    ok &= check(qw_cad_parse(binary, binary_size, &document) == QW_CAD_OK,
                "binary parse");
    if (document) {
        ok &= check(qw_cad_format(document) == QW_CAD_FORMAT_DXF_BINARY,
                    "binary format");
        ok &= check(qw_cad_entity_count(document) == 1u, "binary entity");
        ok &= check(qw_cad_bounds(document, &bounds) == QW_CAD_OK,
                    "binary bounds");
        ok &= check(fabs(bounds.max_x - 3.0) < 1e-9 &&
                        fabs(bounds.max_y - 4.0) < 1e-9,
                    "binary coordinates");
        qw_cad_document_free(document);
    }
    ok &= check(qw_cad_probe("AC1015", 6u) == 1, "DWG probe");
    ok &= check(qw_cad_parse("AC1015", 6u, &document) ==
                    QW_CAD_ERR_UNSUPPORTED,
                "DWG explicit unsupported");
    {
        size_t cut;
        unsigned char random_bytes[128];
        for (cut = 0u; cut < sizeof(kAsciiDxf) - 1u; cut += 37u) {
            document = NULL;
            (void)qw_cad_parse(kAsciiDxf, cut, &document);
            qw_cad_document_free(document);
        }
        for (cut = 0u; cut < sizeof(random_bytes); ++cut)
            random_bytes[cut] = (unsigned char)(cut * 37u + 11u);
        document = NULL;
        (void)qw_cad_parse(random_bytes, sizeof(random_bytes), &document);
        qw_cad_document_free(document);
    }
    if (!ok) return 1;
    printf("cad dxf tests passed\n");
    return 0;
}

