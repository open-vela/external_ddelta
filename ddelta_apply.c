/* ddelta_apply.c - A sane reimplementation of bspatch
 * 
 * Copyright (C) 2017 Julian Andres Klode <jak@debian.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted providing that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "ddelta.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <zlib.h>

#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif

/* Size of blocks to work on at once */
#ifndef DDELTA_BLOCK_SIZE
#define DDELTA_BLOCK_SIZE (32 * 1024)
#endif

static uint32_t ddelta_be32toh(uint32_t be32)
{
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(be32);
#elif defined(__GNUC__) && defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return be32;
#else
    unsigned char *buf = (unsigned char *) &be32;

    return (uint32_t) buf[0] << 24 |
           (uint32_t) buf[1] << 16 |
           (uint32_t) buf[2] << 8 |
           (uint32_t) buf[3] << 0;
#endif
}

static uint64_t ddelta_be64toh(uint64_t be64)
{
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(be64);
#elif defined(__GNUC__) && defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return be64;
#else
    unsigned char *buf = (unsigned char *) &be64;

    return (uint64_t) buf[0] << 56 |
           (uint64_t) buf[1] << 48 |
           (uint64_t) buf[2] << 40 |
           (uint64_t) buf[3] << 32 |
           (uint64_t) buf[4] << 24 |
           (uint64_t) buf[5] << 16 |
           (uint64_t) buf[6] << 8 |
           (uint64_t) buf[7] << 0;
#endif
}

static int32_t ddelta_from_unsigned(uint32_t u)
{
    return u & 0x80000000 ? -(int32_t) ~(u - 1) : (int32_t) u;
}

int ddelta_header_read(struct ddelta_header *header, FILE *file)
{
    if (fread(header, sizeof(*header), 1, file) < 1)
        return -DDELTA_EPATCHIO;
    if (memcmp(DDELTA_MAGIC, header->magic, sizeof(header->magic)) != 0)
        return -DDELTA_EMAGIC;

    header->new_file_size = ddelta_be64toh(header->new_file_size);
    return 0;
}

static int ddelta_entry_header_read(struct ddelta_entry_header *entry,
                                    FILE *file)
{
    if (fread(entry, sizeof(*entry), 1, file) < 1)
        return -DDELTA_EPATCHIO;

    entry->diff = ddelta_be32toh(entry->diff);
    entry->extra = ddelta_be32toh(entry->extra);
    entry->seek.value = ddelta_from_unsigned(ddelta_be32toh(entry->seek.raw));
    return 0;
}

static int apply_diff(FILE *patchfd, FILE *oldfd, FILE *newfd, uint32_t size, uint32_t *oldcrc)
{
#ifdef __GNUC__
    typedef unsigned char uchar_vector __attribute__((vector_size(16)));
#else
    typedef unsigned char uchar_vector;
#endif
    uchar_vector old[DDELTA_BLOCK_SIZE / sizeof(uchar_vector)];
    uchar_vector patch[DDELTA_BLOCK_SIZE / sizeof(uchar_vector)];

    /* Apply the diff */
    while (size > 0) {
        unsigned int i;
        const uint32_t toread = MIN(sizeof(old), size);
        const uint32_t items_to_add = MIN(sizeof(uchar_vector) + toread,
                                          sizeof(old)) /
                                      sizeof(uchar_vector);

        if (fread(&patch, 1, toread, patchfd) < toread)
            return -DDELTA_EPATCHIO;
        if (fread(&old, 1, toread, oldfd) < toread)
            return -DDELTA_EOLDIO;

        *oldcrc = crc32(*oldcrc, (const unsigned char *)old, toread);
        for (i = 0; i < items_to_add; i++)
            old[i] += patch[i];

        if (fwrite(&old, 1, toread, newfd) < toread)
            return -DDELTA_ENEWIO;

        size -= toread;
    }

    return 0;
}

static int copy_bytes(FILE *a, FILE *b, uint32_t bytes)
{
    char buf[DDELTA_BLOCK_SIZE];
    while (bytes > 0) {
        uint32_t toread = MIN(sizeof(buf), bytes);

        if (fread(&buf, toread, 1, a) < 1)
            return -DDELTA_EPATCHIO;
        if (fwrite(&buf, toread, 1, b) < 1)
            return -DDELTA_ENEWIO;

        bytes -= toread;
    }
    return 0;
}

static int copy_file(const char *a, FILE *b, off_t start, off_t end, uint32_t *crc)
{
    unsigned char buf[DDELTA_BLOCK_SIZE];
    off_t origin = ftell(b);
    int err = 0;
    FILE *af;

    if (fseek(b, start, SEEK_SET) < 0)
        return -DDELTA_EOLDIO;

    af = fopen(a, "rb");
    if (af == NULL)
        return -DDELTA_ENEWIO;

    while (start < end && err >= 0) {
        uint32_t toread = MIN(sizeof(buf), end - start);

        if (fread(&buf, toread, 1, af) < 1)
            err = -DDELTA_ENEWIO;
        else if (fwrite(&buf, toread, 1, b) < 1)
            err = -DDELTA_EOLDIO;

        *crc = crc32(*crc, buf, toread);
        start += toread;
    }

    fclose(af);
    if (err < 0)
        return err;

    if (fflush(b) < 0 || fsync(fileno(b)) < 0)
        return -DDELTA_EOLDIO;

    if (fseek(b, origin, SEEK_SET) < 0)
        return -DDELTA_EOLDIO;

    return 0;
}

/**
 * Apply a ddelta_apply in patchfd to oldfd, writing to newfd.
 *
 * The oldfd must be seekable, the patchfd and newfd are read/written
 * sequentially.
 */
int ddelta_apply(struct ddelta_header *header, FILE *patchfd, FILE *oldfd, const char *new)
{
    struct ddelta_entry_header entry;
    struct stat st;
    char tmpname[PATH_MAX];
    uint32_t oldcrc = 0;
    FILE *tmpfd;
    FILE *newfd;
    int err;
    uint64_t bytes_written = 0;

    if (stat(new, &st) >= 0 && S_ISDIR(st.st_mode)) {
        snprintf(tmpname, sizeof(tmpname), "%s/%s", new, "ddelta.tmp");
        unlink(tmpname);
        newfd = tmpfd = fopen(tmpname, "wb");
    } else {
        newfd = fopen(new, "wb");
        tmpfd = NULL;
    }

    if (newfd == NULL)
        return -DDELTA_ENEWIO;

    while (ddelta_entry_header_read(&entry, patchfd) == 0) {
        if (entry.diff == 0 && entry.extra == 0 && entry.seek.value == 0) {
            fflush(newfd);
            fsync(fileno(newfd));
            fclose(newfd);

            if (tmpfd)
                unlink(tmpname);

            return bytes_written == header->new_file_size ? 0 : -DDELTA_EPATCHSHORT;
        }

        if (entry.seek.value == DDELTA_FLUSH) {
            char bakname[PATH_MAX];
            uint32_t newcrc = 0;
            off_t start;

            if (tmpfd == NULL)
                continue;

            start = bytes_written - ftell(tmpfd);

            fflush(tmpfd);
            fsync(fileno(tmpfd));
            fclose(tmpfd);

            snprintf(bakname, sizeof(bakname), "%s/%" PRIu32 ".tmp", new, entry.newcrc);

            if (oldcrc == entry.oldcrc) {
                unlink(bakname);
                if (rename(tmpname, bakname) < 0)
                    return -DDELTA_ENEWIO;
            }

            if (access(bakname, F_OK) >= 0) {
                err = copy_file(bakname, oldfd, start, bytes_written, &newcrc);
                if (err < 0)
                    return err;
                if (newcrc != entry.newcrc)
                    return -DDELTA_ENEWIO;
                unlink(bakname);
            }

            unlink(tmpname);
            newfd = tmpfd = fopen(tmpname, "wb");
            if (newfd == NULL)
                return -DDELTA_ENEWIO;

            oldcrc = 0;
            continue;
        }

        if ((err = apply_diff(patchfd, oldfd, newfd, entry.diff, &oldcrc)) < 0)
            return err;

        /* Copy the bytes over */
        if ((err = copy_bytes(patchfd, newfd, entry.extra)) < 0)
            return err;

        /* Skip remaining bytes */
        if (fseek(oldfd, entry.seek.value, SEEK_CUR) < 0) {
            return -DDELTA_EOLDIO;
        }

        bytes_written += entry.diff + entry.extra;
    }

    return -DDELTA_EPATCHIO;
}

#ifndef DDELTA_NO_MAIN
int main(int argc, char *argv[])
{
    int ret;
    FILE *old;
    FILE *patch;
    struct ddelta_header header;

    if (argc != 4) {
        fprintf(stderr, "usage: %s oldfile newfile|tmpdir patchfile\n", argv[0]);
        return 1;
    }

    old = fopen(argv[1], "r+b");
    patch = fopen(argv[3], "rb");

    if (old == NULL)
        return perror("Cannot open old"), 1;
    if (patch == NULL)
        return perror("Cannot open patch"), 1;

    ret = ddelta_header_read(&header, patch);
    if (ret < 0)
        return fprintf(stderr, "Not a ddelta file: %d(%d)", ret, errno), 1;

    ret = ddelta_apply(&header, patch, old, argv[2]);
    if (ret < 0)
        return fprintf(stderr, "Cannot apply patch: %d(%d)\n", ret, errno), 1;

    return 0;
}
#endif
