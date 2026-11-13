#include "formats/cad/cad_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct R21 {
    const uint8_t *src;
    size_t src_size;
    size_t at;
    uint8_t *dst;
    size_t dst_size;
    size_t put;
} R21;

static int r21_u8(R21 *r, int *out)
{
    if (r->at >= r->src_size) return 0;
    *out = r->src[r->at++];
    return 1;
}

static const uint8_t r21_plan_n[32][5] = {
    {0, 0, 0, 0, 0}, {1, 0, 0, 0, 0}, {2, 0, 0, 0, 0}, {3, 0, 0, 0, 0},
    {4, 0, 0, 0, 0}, {1, 4, 0, 0, 0}, {1, 4, 1, 0, 0}, {2, 4, 1, 0, 0},
    {4, 4, 0, 0, 0}, {1, 8, 0, 0, 0}, {1, 8, 1, 0, 0}, {2, 8, 1, 0, 0},
    {4, 8, 0, 0, 0}, {1, 4, 8, 0, 0}, {1, 4, 8, 1, 0}, {2, 4, 8, 1, 0},
    {16, 0, 0, 0, 0}, {8, 1, 8, 0, 0}, {1, 16, 1, 0, 0}, {3, 16, 0, 0, 0},
    {4, 16, 0, 0, 0}, {1, 4, 16, 0, 0}, {2, 4, 16, 0, 0},
    {3, 4, 16, 0, 0}, {8, 16, 0, 0, 0}, {8, 1, 16, 0, 0},
    {1, 8, 1, 16, 0}, {2, 8, 1, 16, 0}, {4, 8, 16, 0, 0},
    {1, 4, 8, 16, 0}, {2, 4, 8, 16, 0}, {1, 4, 8, 16, 2}
};

static const uint8_t r21_plan_at[32][5] = {
    {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0}, {4, 0, 0, 0, 0}, {5, 1, 0, 0, 0}, {5, 1, 0, 0, 0},
    {0, 4, 0, 0, 0}, {8, 0, 0, 0, 0}, {9, 1, 0, 0, 0}, {9, 1, 0, 0, 0},
    {8, 0, 0, 0, 0}, {12, 8, 0, 0, 0}, {13, 9, 1, 0, 0}, {13, 9, 1, 0, 0},
    {0, 0, 0, 0, 0}, {9, 8, 0, 0, 0}, {17, 1, 0, 0, 0}, {16, 0, 0, 0, 0},
    {16, 0, 0, 0, 0}, {20, 16, 0, 0, 0}, {20, 16, 0, 0, 0},
    {20, 16, 0, 0, 0}, {16, 0, 0, 0, 0}, {17, 16, 0, 0, 0},
    {25, 17, 16, 0, 0}, {25, 17, 16, 0, 0}, {24, 16, 0, 0, 0},
    {28, 24, 16, 0, 0}, {28, 24, 16, 0, 0}, {30, 26, 18, 2, 0}
};

static const uint8_t r21_plan_count[32] = {
    0, 1, 1, 1, 1, 2, 3, 3, 2, 2, 3, 3, 2, 3, 4, 4,
    1, 3, 3, 2, 2, 3, 3, 3, 2, 3, 4, 4, 3, 4, 4, 5
};

static int r21_literal_run(R21 *r, int opcode, int *length)
{
    int value = opcode + 8;
    int n;
    if (value != 0x17) {
        *length = value;
        return 1;
    }
    if (!r21_u8(r, &n)) return 0;
    value += n;
    if (n != 0xFF) {
        *length = value;
        return 1;
    }
    for (;;) {
        int lo, hi;
        if (r->at + 2u > r->src_size) return 0;
        lo = r->src[r->at++];
        hi = r->src[r->at++];
        n = lo | (hi << 8);
        value += n;
        if (n != 0xFFFF) {
            *length = value;
            return 1;
        }
    }
}

static int r21_instr(R21 *r, int op, int *next_op, int *offset, int *length)
{
    switch (op >> 4) {
    case 0: {
        int b, second;
        if (!r21_u8(r, &b) || !r21_u8(r, &second)) return 0;
        *next_op = second;
        *length = (op & 0x0F) + 0x13 + ((second >> 3) & 0x10);
        *offset = ((((second & 0x78) << 5) + 1) + b);
        return 1;
    }
    case 1: {
        int b, second;
        if (!r21_u8(r, &b) || !r21_u8(r, &second)) return 0;
        *next_op = second;
        *length = (op & 0x0F) + 0x03;
        *offset = ((((second & 0xF8) << 5) + 1) + b);
        return 1;
    }
    case 2: {
        int b, second;
        int value;
        if (!r21_u8(r, &b) || !r21_u8(r, &second)) return 0;
        value = b | ((second << 8) & 0xFF00);
        *length = op & 0x07;
        if ((op & 0x08) == 0) {
            int tail;
            if (!r21_u8(r, &tail)) return 0;
            *next_op = tail;
            *length += tail & 0xF8;
            *offset = value;
            return 1;
        }
        value++;
        if (!r21_u8(r, &b)) return 0;
        *length += b << 3;
        {
            int tail;
            if (!r21_u8(r, &tail)) return 0;
            *next_op = tail;
            *length = (((tail & 0xF8) << 8) + *length) + 0x100;
            *offset = value;
        }
        return 1;
    }
    default: {
        int b;
        if (!r21_u8(r, &b)) return 0;
        *next_op = b;
        *length = op >> 4;
        *offset = ((((b & 0xF8) << 1) + (op & 0x0F)) + 1);
        return 1;
    }
    }
}

static int r21_copy_run(R21 *r, int offset, int length)
{
    size_t source;
    int index;
    if (offset <= 0 || length < 0 || (size_t)offset > r->put) return 0;
    source = r->put - (size_t)offset;
    if (r->put + (size_t)length > r->dst_size) return 0;
    for (index = 0; index < length; ++index)
        r->dst[r->put + (size_t)index] = r->dst[source + (size_t)index];
    r->put += (size_t)length;
    return 1;
}

static int r21_copy_runs(R21 *r, int *literal_pending, int *literal_opcode)
{
    int op;
    int ins_next, ins_offset, ins_length;
    if (!r21_u8(r, &op)) return 0;
    if (!r21_instr(r, op, &ins_next, &ins_offset, &ins_length)) return 0;
    for (;;) {
        if (!r21_copy_run(r, ins_offset, ins_length)) return 0;
        *literal_pending = ins_next & 0x07;
        if (*literal_pending != 0 || r->at >= r->src_size) {
            *literal_opcode = op;
            return 1;
        }
        if (!r21_u8(r, &op)) return 0;
        if ((op >> 4) == 0) {
            *literal_pending = 0;
            *literal_opcode = op;
            return 1;
        }
        if ((op >> 4) == 15) op &= 0x0F;
        if (!r21_instr(r, op, &ins_next, &ins_offset, &ins_length)) return 0;
    }
}

static int r21_permute(int n, int j)
{
    if (n == 2) return 1 - j;
    if (n == 3) return 2 - j;
    if (n == 16) return j < 8 ? j + 8 : j - 8;
    return j;
}

static int r21_emit_block(R21 *r, int source_at, int n)
{
    int j;
    for (j = 0; j < n; ++j) {
        int source = source_at + r21_permute(n, j);
        size_t out = r->put + (size_t)j;
        if (source < 0 || (size_t)source >= r->src_size || out >= r->dst_size)
            return 0;
        r->dst[out] = r->src[source];
    }
    r->put += (size_t)n;
    return 1;
}

static int r21_emit_chunk(R21 *r, int source_at, int length)
{
    while (length >= 32) {
        if (!r21_emit_block(r, source_at + 16, 16) ||
            !r21_emit_block(r, source_at, 16))
            return 0;
        source_at += 32;
        length -= 32;
    }
    {
        int step;
        int count = r21_plan_count[length];
        for (step = 0; step < count; ++step) {
            if (!r21_emit_block(r, source_at + r21_plan_at[length][step],
                                r21_plan_n[length][step]))
                return 0;
        }
    }
    return 1;
}

int qw_dwg_r21_decompress(const uint8_t *source, size_t source_size,
                          uint8_t **out, size_t target_size)
{
    R21 r;
    int lead;
    int literal_opcode;
    int literal_pending = 0;
    if (!out) return 0;
    *out = NULL;
    if (target_size == 0u) {
        *out = (uint8_t *)calloc(1u, 1u);
        return *out != NULL;
    }
    if (!source || source_size == 0u) return 0;
    memset(&r, 0, sizeof(r));
    r.src = source;
    r.src_size = source_size;
    r.dst = (uint8_t *)calloc(1u, target_size);
    if (!r.dst) return 0;
    r.dst_size = target_size;
    if (!r21_u8(&r, &lead)) goto fail;
    literal_opcode = lead;
    if ((lead & 0xF0) == 0x20) {
        r.at += 2u;
        if (r.at >= r.src_size) goto fail;
        literal_pending = r.src[r.at] & 0x07;
        ++r.at;
    }
    while (r.at < r.src_size) {
        int length = literal_pending;
        if (length == 0) {
            if (!r21_literal_run(&r, literal_opcode, &length)) goto fail;
        }
        if (r.put + (size_t)length > target_size) break;
        if (!r21_emit_chunk(&r, (int)r.at, length)) goto fail;
        r.at += (size_t)length;
        if (r.at >= r.src_size) break;
        if (!r21_copy_runs(&r, &literal_pending, &literal_opcode)) goto fail;
    }
    *out = r.dst;
    return 1;
fail:
    free(r.dst);
    return 0;
}
