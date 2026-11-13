#include <quirewise/cad.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QW_CAD_TEST_DATA_DIR
#define QW_CAD_TEST_DATA_DIR "."
#endif

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

static unsigned char *load_file(const char *path, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    long length;
    unsigned char *data;
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = (unsigned char *)malloc((size_t)length);
    if (!data) {
        fclose(file);
        return NULL;
    }
    if (fread(data, 1u, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_size = (size_t)length;
    return data;
}

static int check_dwg(const char *path, QwCadStatus expected,
                     size_t expected_entities, double min_x, double min_y,
                     double max_x, double max_y)
{
    size_t size = 0u;
    unsigned char *data;
    QwCadDocument *document = NULL;
    QwCadBounds bounds;
    QwCadStatus status;
    QwCadImage image;
    int ok = 1;
    char full[512];
    snprintf(full, sizeof(full), "%s/%s", QW_CAD_TEST_DATA_DIR, path);
    data = load_file(full, &size);
    if (!data) return check(0, path);
    status = qw_cad_parse(data, size, &document);
    ok &= check(status == expected, path);
    if (status == QW_CAD_OK && document) {
        ok &= check(qw_cad_format(document) == QW_CAD_FORMAT_DWG, path);
        ok &= check(qw_cad_entity_count(document) == expected_entities, path);
        ok &= check(qw_cad_bounds(document, &bounds) == QW_CAD_OK, path);
        if (expected == QW_CAD_OK) {
            ok &= check(fabs(bounds.min_x - min_x) < 1e-6 &&
                            fabs(bounds.min_y - min_y) < 1e-6 &&
                            fabs(bounds.max_x - max_x) < 1e-6 &&
                            fabs(bounds.max_y - max_y) < 1e-6,
                        path);
        }
        memset(&image, 0, sizeof(image));
        ok &= check(qw_cad_render_rgba(document, 128u, &image) == QW_CAD_OK,
                    path);
        qw_cad_image_free(&image);
    }
    qw_cad_document_free(document);
    free(data);
    return ok;
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

    {
        static const char hatch_dxf[] =
            "0\nSECTION\n2\nENTITIES\n"
            "0\nHATCH\n8\nHATCH\n62\n1\n70\n1\n91\n1\n93\n4\n"
            "10\n0\n20\n0\n10\n100\n20\n0\n"
            "10\n100\n20\n100\n10\n0\n20\n100\n"
            "0\nENDSEC\n0\nEOF\n";
        QwCadDocument *hatch = NULL;
        QwCadBounds hatch_bounds;
        ok &= check(qw_cad_parse(hatch_dxf, sizeof(hatch_dxf) - 1u, &hatch) ==
                        QW_CAD_OK &&
                    hatch && qw_cad_entity_count(hatch) == 1u,
                    "hatch parse");
        ok &= check(hatch &&
                        qw_cad_bounds(hatch, &hatch_bounds) == QW_CAD_OK &&
                        hatch_bounds.max_x > 99.0 && hatch_bounds.max_y > 99.0,
                    "hatch bounds");
        if (hatch) {
            char *hatch_svg = NULL;
            size_t hatch_svg_size = 0u;
            ok &= check(qw_cad_render_svg(hatch, &hatch_svg,
                                          &hatch_svg_size) == QW_CAD_OK &&
                            hatch_svg && strstr(hatch_svg, "<polygon") != NULL,
                        "hatch svg fill");
            qw_cad_buffer_free(hatch_svg);
        }
        qw_cad_document_free(hatch);
    }
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
    {
        unsigned char unsupported[0x19];
        memset(unsupported, 0, sizeof(unsupported));
        memcpy(unsupported, "AC1004", 6u);
        ok &= check(qw_cad_parse(unsupported, sizeof(unsupported), &document) ==
                        QW_CAD_ERR_UNSUPPORTED,
                    "DWG explicit unsupported");
    }
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
    ok &= check_dwg("line_2000.dwg", QW_CAD_OK, 1u, 50.0, 50.0, 100.0,
                         100.0);
    ok &= check_dwg("line_R14.dwg", QW_CAD_OK, 1u, 50.0, 50.0, 100.0,
                         100.0);
    ok &= check_dwg("circle_R14.dwg", QW_CAD_OK, 1u, 0.0, 0.0, 100.0, 100.0);
    ok &= check_dwg("arc_R14.dwg", QW_CAD_OK, 1u, 50.0, 50.0, 100.0, 75.0);
    ok &= check_dwg("lw_entities-2d.dwg", QW_CAD_OK, 9u, 0.0, 0.0, 8.1272793,
                    9.1272793);
    {
        size_t size = 0u;
        unsigned char *data = load_file(
            QW_CAD_TEST_DATA_DIR "/lw_entities-2d.dwg", &size);
        if (data) {
            QwCadDocument *drawing = NULL;
            QwCadTextInfo info;
            if (qw_cad_parse(data, size, &drawing) == QW_CAD_OK && drawing) {
                ok &= check(qw_cad_text_count(drawing) == 1u, "DWG text count");
                if (qw_cad_text_at(drawing, 0u, &info) == QW_CAD_OK) {
                    ok &= check(info.text_size == 3u &&
                                    memcmp(info.text, "FOO", 3u) == 0,
                                "DWG text value");
                    ok &= check(fabs(info.height - 0.75) < 1e-6,
                                "DWG text height");
                }
            }
            qw_cad_document_free(drawing);
            free(data);
        }
    }
    ok &= check_dwg("line_2004.dwg", QW_CAD_OK, 1u, 50.0, 50.0, 100.0,
                         100.0);
    ok &= check_dwg("text_2004.dwg", QW_CAD_OK, 1u, 50.0, 50.0, 80.0, 55.0);
    {
        size_t size = 0u;
        unsigned char *data =
            load_file(QW_CAD_TEST_DATA_DIR "/text_2004.dwg", &size);
        if (data) {
            QwCadDocument *drawing = NULL;
            QwCadTextInfo info;
            if (qw_cad_parse(data, size, &drawing) == QW_CAD_OK && drawing &&
                qw_cad_text_at(drawing, 0u, &info) == QW_CAD_OK) {
                ok &= check(info.text_size == 10u &&
                                memcmp(info.text, "Hello TEXT", 10u) == 0,
                            "R2004 TEXT value");
            }
            qw_cad_document_free(drawing);
            free(data);
        }
    }
    ok &= check_dwg("circle_2004.dwg", QW_CAD_OK, 1u, 0.0, 0.0, 100.0, 100.0);
    ok &= check_dwg("arc_2004.dwg", QW_CAD_OK, 1u, 50.0, 50.0, 100.0, 75.0);
    ok &= check_dwg("line_2007.dwg", QW_CAD_OK, 1u, 50.0, 50.0, 100.0,
                         100.0);
    ok &= check_dwg("circle_2007.dwg", QW_CAD_OK, 1u, 0.0, 0.0, 100.0, 100.0);
    ok &= check_dwg("arc_2007.dwg", QW_CAD_OK, 1u, 50.0, 50.0, 100.0, 75.0);
    ok &= check_dwg("line_2010.dwg", QW_CAD_OK, 1u, 50.0, 50.0, 100.0,
                         100.0);
    ok &= check_dwg("line_2013.dwg", QW_CAD_OK, 1u, 50.0, 50.0, 100.0,
                         100.0);
    ok &= check_dwg("circle_2013.dwg", QW_CAD_OK, 1u, 0.0, 0.0, 100.0, 100.0);
    ok &= check_dwg("arc_2013.dwg", QW_CAD_OK, 1u, 50.0, 50.0, 100.0, 75.0);
    {
        const char *names[2] = {"lw_example2013.dwg", "lw_example2018.dwg"};
        size_t name_index;
        for (name_index = 0u; name_index < 2u; ++name_index) {
            char path[256];
            size_t size = 0u;
            unsigned char *data;
            snprintf(path, sizeof(path), "%s/%s", QW_CAD_TEST_DATA_DIR,
                     names[name_index]);
            data = load_file(path, &size);
            if (data) {
                QwCadDocument *drawing = NULL;
                if (qw_cad_parse(data, size, &drawing) == QW_CAD_OK && drawing)
                    ok &= check(qw_cad_entity_count(drawing) > 0u,
                                names[name_index]);
                qw_cad_document_free(drawing);
                free(data);
            }
        }
    }
    {
        size_t size = 0u;
        unsigned char *data = load_file(
            QW_CAD_TEST_DATA_DIR "/lw_example2007.dwg", &size);
        if (data) {
            QwCadDocument *drawing = NULL;
            if (qw_cad_parse(data, size, &drawing) == QW_CAD_OK && drawing)
                ok &= check(qw_cad_entity_count(drawing) > 0u,
                            "R2007 mixed entities");
            qw_cad_document_free(drawing);
            free(data);
        }
    }
    {
        size_t size = 0u;
        unsigned char *data = load_file(
            QW_CAD_TEST_DATA_DIR "/lw_example2004.dwg", &size);
        if (data) {
            QwCadDocument *drawing = NULL;
            QwCadBounds shape;
            if (qw_cad_parse(data, size, &drawing) == QW_CAD_OK && drawing) {
                ok &= check(qw_cad_entity_count(drawing) > 0u,
                            "R2004 mixed entities");
                ok &= check(qw_cad_bounds(drawing, &shape) == QW_CAD_OK,
                            "R2004 mixed bounds");
                ok &= check(fabs(shape.min_x) < 1e6 &&
                                fabs(shape.max_x) < 1e6,
                            "R2004 mixed sane bounds");
            }
            qw_cad_document_free(drawing);
            free(data);
        }
    }
    {
        size_t size = 0u;
        unsigned char *data =
            load_file(QW_CAD_TEST_DATA_DIR "/line_2000.dwg", &size);
        if (data) {
            size_t cut;
            for (cut = 0u; cut < size; cut += 4096u) {
                QwCadDocument *drawing = NULL;
                (void)qw_cad_parse(data, cut, &drawing);
                qw_cad_document_free(drawing);
            }
            free(data);
        }
    }
    if (!ok) return 1;
    printf("cad dxf tests passed\n");
    return 0;
}












