#include <quirewise/svg.h>
#include <quirewise/svgz.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FUZZ_SEED UINT32_C(0x51f07733)
#define FUZZ_RUNS 250u

static uint32_t rng_state = FUZZ_SEED;

static const uint8_t svgz_seed[] = {
    0x1fu, 0x8bu, 0x08u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xffu,
    0x01u, 0x06u, 0x00u, 0xf9u, 0xffu, '<', 's', 'v', 'g', '/', '>',
    0x49u, 0xfbu, 0xb9u, 0xacu, 0x06u, 0x00u, 0x00u, 0x00u
};

static uint32_t next_random(void)
{
    uint32_t value = rng_state;
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    rng_state = value;
    return value;
}

static size_t mutate(const uint8_t *seed, size_t seed_size, uint8_t *buffer,
                     size_t capacity)
{
    size_t length = seed_size > capacity ? capacity : seed_size;
    unsigned int flips = 1u + next_random() % 4u;
    size_t index;
    memcpy(buffer, seed, length);
    if (next_random() % 8u == 0u && length > 1u)
        length = 1u + next_random() % length;
    for (index = 0u; index < flips && length; ++index) {
        size_t position = next_random() % length;
        buffer[position] = (uint8_t)(buffer[position] ^
                                     (uint8_t)(1u << (next_random() % 8u)));
    }
    return length;
}

int main(void)
{
    static const char svg_seed[] =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4\" height=\"4\">"
        "<rect width=\"4\" height=\"4\" fill=\"#336699\"/>"
        "<circle cx=\"2\" cy=\"2\" r=\"1\"/></svg>";
    uint8_t buffer[1024];
    size_t run;
    size_t ok = 0u;
    size_t rejected = 0u;
    for (run = 0u; run < FUZZ_RUNS; ++run) {
        QwSvgImage image = {0};
        size_t size = mutate((const uint8_t *)svg_seed, sizeof(svg_seed) - 1u,
                             buffer, sizeof(buffer));
        if (qw_svg_decode((const char *)buffer, size, &image, NULL) ==
            QW_SVG_OK) {
            ++ok;
            qw_svg_image_free(&image);
        } else {
            ++rejected;
        }
    }
    for (run = 0u; run < FUZZ_RUNS; ++run) {
        QwSvgzDocument document = {0};
        size_t size = mutate(svgz_seed, sizeof(svgz_seed), buffer,
                             sizeof(buffer));
        if (qw_svgz_decode(buffer, size, &document) == QW_SVGZ_OK) {
            ++ok;
            qw_svgz_document_free(&document);
        } else {
            ++rejected;
        }
    }
    printf("Vector fuzz smoke: %zu ok, %zu rejected, seed=0x%08x runs=%u\n", ok,
           rejected, FUZZ_SEED, FUZZ_RUNS);
    return 0;
}
