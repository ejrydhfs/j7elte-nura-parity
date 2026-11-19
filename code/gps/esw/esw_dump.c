/*
 * esw_dump.c - extract the BCM4752 ESW / RAM-patch image out of the stock
 *              Android glgps daemon ("/system/bin/gpsd") without any external
 *              dependency (no libcrypto, no zlib).
 *
 * The daemon keeps its 8051 patch in a small table of 16-byte records inside
 * .data.rel.ro.local:
 *
 *     struct esw_record {           // ARM32 build (32-bit pointers)
 *         uint8_t  flags;           // bank number used by the 0x61 16 switch
 *         uint32_t addr;            // 16-bit load address inside that bank
 *         uint32_t ptr;             // VA of the data, relocated (R_ARM_RELATIVE)
 *         uint32_t len;             // byte count
 *     };
 *
 * This is the public "BCM4752 init" container layout {u8 flags; u32 addr;
 * u64 ptr; u32 len} (24 bytes on x86-64, where ptr is 64-bit) narrowed to the
 * 32-bit pointer this build uses, i.e. exactly 16 bytes.
 *
 * Table selection was reversed from the daemon:
 *   GlMeSrdAsicInitPatchLoader::SetData()      @0x2a9184
 *     -> revision name via GlMeSrdAsicVersion   @0x2a9080  (reads obj+0)
 *     -> record lookup                          @0x2da0fc
 *   lookup("4752_B2", flag=0, index)
 *       index 0..2 -> table @0x3c6d58 + 16*index
 *       index 3    -> synthetic 4-byte block {addr 0x7fd2, ptr 0x3a6e78,
 *                                             len 4} built at 0x2da196
 *     lookup("4752_B3", flag=0, index)
 *       index 0..1 -> table @0x3c6d88 + 16*index
 *       index 2    -> same synthetic 4-byte block
 *     lookup("47521_A0", flag=1, index)
 *       index 0..4 -> table @0x3c6d08 + 16*index
 *
 * The same records are consumed by the send path:
 *   PatchLoader::GetAddress()  @0x2a8d70  = record.addr + offset
 *   PatchLoader::GetData()     @0x2a8dcc  = record.ptr  + offset
 *   PatchLoader::GetBlockSize()@0x2a8e24  = min(remaining, 4096)
 *   flags getter               @0x2a8d2c  -> bank switch P5 (0x61 16 <bank>)
 *
 * Usage:
 *   ./esw_dump /path/to/gpsd [--rev b2|b3|a0] [--all] [--dump PREFIX]
 *   (--dump writes PREFIX-<rev>-<idx>.bin; the directory must exist)
 *
 * Output is one table per revision: index, flags, load address, file offset,
 * size, CRC-32 and SHA-256 of each block, then the concatenated image digest.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;

static u8    *img;
static size_t img_len;

/* ------------------------------------------------------------------ */
/* little-endian readers                                              */
/* ------------------------------------------------------------------ */
static u16 rd16(size_t o) { return (u16)(img[o] | (img[o + 1] << 8)); }
static u32 rd32(size_t o)
{
    return (u32)img[o] | ((u32)img[o + 1] << 8) |
           ((u32)img[o + 2] << 16) | ((u32)img[o + 3] << 24);
}

/* ------------------------------------------------------------------ */
/* SHA-256 (FIPS 180-4), self-contained                               */
/* ------------------------------------------------------------------ */
typedef struct {
    u32 h[8];
    uint64_t bits;
    u8  buf[64];
    size_t used;
} sha256_ctx;

static const u32 K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static u32 ror32(u32 x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(sha256_ctx *c, const u8 *p)
{
    u32 w[64], a, b, d, e, f, g, h, t1, t2, cc;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((u32)p[4*i] << 24) | ((u32)p[4*i+1] << 16) |
               ((u32)p[4*i+2] << 8) | p[4*i+3];
    for (i = 16; i < 64; i++) {
        u32 s0 = ror32(w[i-15], 7) ^ ror32(w[i-15], 18) ^ (w[i-15] >> 3);
        u32 s1 = ror32(w[i-2], 17) ^ ror32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        u32 S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        u32 mj = (a & b) ^ (a & cc) ^ (b & cc);
        t1 = h + S1 + ch + K256[i] + w[i];
        t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(sha256_ctx *c)
{
    c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85;
    c->h[2] = 0x3c6ef372; c->h[3] = 0xa54ff53a;
    c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c;
    c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
    c->bits = 0; c->used = 0;
}

static void sha256_update(sha256_ctx *c, const void *data, size_t n)
{
    const u8 *p = data;
    c->bits += (uint64_t)n * 8;
    while (n) {
        size_t take = 64 - c->used;
        if (take > n) take = n;
        memcpy(c->buf + c->used, p, take);
        c->used += take; p += take; n -= take;
        if (c->used == 64) { sha256_block(c, c->buf); c->used = 0; }
    }
}

static void sha256_final(sha256_ctx *c, u8 out[32])
{
    u8 pad[72];
    size_t padlen = (c->used < 56) ? 56 - c->used : 120 - c->used;
    uint64_t bits = c->bits;
    int i;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (i = 0; i < 8; i++)
        pad[padlen + i] = (u8)(bits >> (56 - 8 * i));
    sha256_update(c, pad, padlen + 8);
    for (i = 0; i < 8; i++) {
        out[4*i]     = (u8)(c->h[i] >> 24);
        out[4*i + 1] = (u8)(c->h[i] >> 16);
        out[4*i + 2] = (u8)(c->h[i] >> 8);
        out[4*i + 3] = (u8)(c->h[i]);
    }
}

/* ------------------------------------------------------------------ */
/* CRC-32 (IEEE 802.3 / zlib: poly 0xEDB88320, init/xorout 0xFFFFFFFF) */
/* ------------------------------------------------------------------ */
static u32 crc_table[256];

static void crc32_init(void)
{
    u32 i, j, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static u32 crc32_update(u32 c, const u8 *p, size_t n)
{
    while (n--)
        c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c;
}

static u32 crc32_buf(const u8 *p, size_t n)
{
    return crc32_update(0xffffffffu, p, n) ^ 0xffffffffu;
}

/* ------------------------------------------------------------------ */
/* ELF32 LE section helper                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *name;
    u32 type, flags, addr, off, size;
} Sect;

static Sect *sects;
static int   nsects;

static int load_elf_sections(const u8 *d, size_t n)
{
    u32 shoff, shentsize, shnum, shstrndx, i;
    const char *strtab;

    if (n < 52 || d[0] != 0x7f || d[1] != 'E' || d[2] != 'L' || d[3] != 'F')
        return -1;
    if (d[4] != 1 /*ELFCLASS32*/ || d[5] != 1 /*LSB*/)
        return -1;

    shoff     = rd32(0x20);
    shentsize = rd16(0x2e);
    shnum     = rd16(0x30);
    shstrndx  = rd16(0x32);
    if (!shoff || !shnum || shentsize < 40 || shoff + (size_t)shnum * shentsize > n)
        return -1;
    if (shstrndx >= shnum)
        return -1;

    strtab = (const char *)d + rd32(shoff + (size_t)shstrndx * shentsize + 0x10);

    sects = calloc(shnum, sizeof *sects);
    if (!sects)
        return -1;
    nsects = (int)shnum;
    for (i = 0; i < shnum; i++) {
        size_t o = shoff + (size_t)i * shentsize;
        Sect *s = &sects[i];
        s->name  = strtab + rd32(o + 0x00);
        s->type  = rd32(o + 0x04);
        s->flags = rd32(o + 0x08);
        s->addr  = rd32(o + 0x0c);
        s->off   = rd32(o + 0x10);
        s->size  = rd32(o + 0x14);
    }
    return 0;
}

static const Sect *find_sect(const char *name)
{
    int i;
    for (i = 0; i < nsects; i++)
        if (!strcmp(sects[i].name, name))
            return &sects[i];
    return NULL;
}

/* VA -> file offset using the section table (SHT_NOBITS has no bytes) */
static long va_to_off(u32 va, u32 len)
{
    int i;
    for (i = 0; i < nsects; i++) {
        const Sect *s = &sects[i];
        if (s->type == 8 /*SHT_NOBITS*/ || !s->size)
            continue;
        if (va >= s->addr && (uint64_t)va + len <= (uint64_t)s->addr + s->size)
            return (long)(s->off + (va - s->addr));
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* table model                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    int      index;
    int      synthetic;   /* record built in code, not stored in the table */
    u32      flags, addr, ptr, len;
} Rec;

typedef struct {
    const char *rev;
    u32 table_va;         /* 0 = no table (records synthesised only) */
    int nrec;
    int synth_index;      /* index of the synthetic block, -1 if none */
} RevDesc;

static const RevDesc revs[] = {
    { "4752_B2",   0x3c6d58, 3, 3 },
    { "4752_B3",   0x3c6d88, 2, 2 },
    { "47521_A0",  0x3c6d08, 5, -1 },
};

#define NREVS ((int)(sizeof revs / sizeof revs[0]))

/* the synthetic block built at 0x2da196: addr 0x7fd2, 4 bytes of zeros */
#define SYNTH_ADDR 0x7fd2u
#define SYNTH_PTR  0x3a6e78u
#define SYNTH_LEN  4u

/* ------------------------------------------------------------------ */
/* R_ARM_RELATIVE relocation check (proves the ptr field is a pointer) */
/* ------------------------------------------------------------------ */
static int reloc_relative(u32 va)
{
    const Sect *r = find_sect(".rel.dyn");
    size_t i;
    if (!r)
        return 0;
    for (i = 0; i + 8 <= r->size; i += 8) {
        size_t o = r->off + i;
        u32 roff = rd32(o), info = rd32(o + 4);
        if (roff == va && (info & 0xff) == 23 /*R_ARM_RELATIVE*/)
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* digest helpers                                                     */
/* ------------------------------------------------------------------ */
static void sha256_hex(const u8 *p, size_t n, char out[65])
{
    sha256_ctx c;
    u8 dg[32];
    int i;
    sha256_init(&c);
    sha256_update(&c, p, n);
    sha256_final(&c, dg);
    for (i = 0; i < 32; i++)
        sprintf(out + 2 * i, "%02x", dg[i]);
    out[64] = 0;
}

/* ------------------------------------------------------------------ */
static int dump_rev(const RevDesc *r, int dump_files, const char *prefix)
{
    Rec rec[8];
    int n = 0, i;
    char hex[65];
    sha256_ctx whole;
    u8 wdg[32];
    size_t total = 0;
    u32 whole_crc;
    long tbl_off = 0;

    printf("\n== revision %s ==\n", r->rev);
    if (r->table_va) {
        tbl_off = va_to_off(r->table_va, (u32)(r->nrec * 16));
        if (tbl_off < 0) {
            printf("   error: table vaddr 0x%08x is not backed by file data\n",
                   r->table_va);
            return 1;
        }
        printf("   table: vaddr 0x%08x  file offset 0x%08zx  records %d\n",
               r->table_va, (size_t)tbl_off, r->nrec);
        for (i = 0; i < r->nrec; i++) {
            size_t o = (size_t)tbl_off + (size_t)i * 16;
            rec[n].index = i;
            rec[n].synthetic = 0;
            rec[n].flags = img[o];
            rec[n].addr  = rd32(o + 4);
            rec[n].ptr   = rd32(o + 8);
            rec[n].len   = rd32(o + 12);
            if (!reloc_relative(r->table_va + (u32)i * 16 + 8))
                printf("   note: no R_ARM_RELATIVE at record %d ptr field\n", i);
            n++;
        }
    } else {
        printf("   table: none (records are synthesised in code)\n");
    }
    if (r->synth_index >= 0) {
        rec[n].index = r->synth_index;
        rec[n].synthetic = 1;
        rec[n].flags = 0;
        rec[n].addr  = SYNTH_ADDR;
        rec[n].ptr   = SYNTH_PTR;
        rec[n].len   = SYNTH_LEN;
        n++;
    }

    printf("   idx  bnk  load addr  file off   size    crc32       sha256\n");
    sha256_init(&whole);
    whole_crc = 0xffffffffu;
    for (i = 0; i < n; i++) {
        long off = va_to_off(rec[i].ptr, rec[i].len);
        if (off < 0) {
            printf("   %3d  0x%x  0x%04x      <outside file: ptr 0x%08x len %u>\n",
                   rec[i].index, rec[i].flags, rec[i].addr, rec[i].ptr, rec[i].len);
            return 1;
        }
        sha256_hex(img + off, rec[i].len, hex);
        printf("   %3d   %3u  0x%04x     0x%08zx  %6u  0x%08x  %s%s\n",
               rec[i].index, rec[i].flags, rec[i].addr, (size_t)off,
               rec[i].len, crc32_buf(img + off, rec[i].len), hex,
               rec[i].synthetic ? "  (synthetic 4-byte zero block @0x2da196)" : "");
        sha256_update(&whole, img + off, rec[i].len);
        whole_crc = crc32_update(whole_crc, img + off, rec[i].len);
        total += rec[i].len;
        if (dump_files) {
            char path[512];
            FILE *f;
            snprintf(path, sizeof path, "%s-%s-%d.bin", prefix, r->rev,
                     rec[i].index);
            f = fopen(path, "wb");
            if (!f || fwrite(img + off, 1, rec[i].len, f) != rec[i].len) {
                printf("   error: cannot write %s (does the directory exist?)\n", path);
                if (f) fclose(f);
                return 1;
            }
            fclose(f);
            printf("        -> %s\n", path);
        }
    }
    sha256_final(&whole, wdg);
    {
        char whex[65];
        int k;
        for (k = 0; k < 32; k++) sprintf(whex + 2 * k, "%02x", wdg[k]);
        whex[64] = 0;
        printf("   image: %d blocks, %zu bytes, crc32 0x%08x, sha256(concat) %s\n",
               n, total, whole_crc ^ 0xffffffffu, whex);
    }

    /* covered ranges vs the holes between them, in 16-bit load space */
    printf("   ranges:");
    for (i = 0; i < n; i++) {
        long off = va_to_off(rec[i].ptr, rec[i].len);
        (void)off;
        printf(" bank%u:0x%04x-0x%04x", rec[i].flags, rec[i].addr,
               rec[i].addr + rec[i].len);
    }
    printf("\n");
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = NULL, *only = NULL, *dump_prefix = NULL;
    int all = 0, dump = 0, i, rc = 0;
    FILE *f;
    long sz;
    const Sect *ro;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--all")) all = 1;
        else if (!strcmp(argv[i], "--rev") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
            dump = 1;
            dump_prefix = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: %s <gpsd> [--rev b2|b3|a0] [--all] "
                            "[--dump PREFIX]\n"
                            "        (PREFIX directory must already exist)\n", argv[0]);
            return 2;
        } else if (!path) {
            path = argv[i];
        } else {
            fprintf(stderr, "unexpected argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!path) {
        fprintf(stderr, "usage: %s <gpsd> [--rev b2|b3|a0] [--all] "
                        "[--dump PREFIX]\n"
                            "        (PREFIX directory must already exist)\n", argv[0]);
        return 2;
    }

    f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fprintf(stderr, "%s: empty\n", path); fclose(f); return 1; }
    img_len = (size_t)sz;
    img = malloc(img_len);
    if (!img || fread(img, 1, img_len, f) != img_len) {
        fprintf(stderr, "%s: read error\n", path);
        fclose(f);
        return 1;
    }
    fclose(f);

    if (load_elf_sections(img, img_len) != 0) {
        fprintf(stderr, "%s: not a little-endian ELF32 file\n", path);
        free(img);
        return 1;
    }
    ro = find_sect(".rodata");
    {
        char hex[65];
        printf("file: %s (%zu bytes)\n", path, img_len);
        sha256_hex(img, img_len, hex);
        printf("sha256(file): %s\n", hex);
        if (ro)
            printf(".rodata: vaddr 0x%08x  file offset 0x%08x  size 0x%08x\n",
                   ro->addr, ro->off, ro->size);
        if (!find_sect(".data.rel.ro.local"))
            printf("warning: no .data.rel.ro.local section\n");
    }

    crc32_init();

    if (only || !all) {
        int found = 0;
        const char *want = only ? only : "b2";
        for (i = 0; i < NREVS; i++) {
            const char *sh = revs[i].rev;
            if ((!strcmp(want, "b2") && strstr(sh, "B2")) ||
                (!strcmp(want, "b3") && strstr(sh, "B3")) ||
                (!strcmp(want, "a0") && strstr(sh, "A0")) ||
                !strcmp(want, sh)) {
                rc |= dump_rev(&revs[i], dump, dump_prefix ? dump_prefix : "esw");
                found = 1;
            }
        }
        if (!found) {
            fprintf(stderr, "unknown revision '%s' (use b2, b3 or a0)\n", want);
            rc = 2;
        }
    } else {
        for (i = 0; i < NREVS; i++)
            rc |= dump_rev(&revs[i], dump, dump_prefix ? dump_prefix : "esw");
    }

    free(sects);
    free(img);
    return rc;
}
