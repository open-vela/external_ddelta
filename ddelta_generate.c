/*-
 * Copyright 2003-2005 Colin Percival
 * Copyright 2017 Julian Andres Klode <jak@jak-linux.org>
 * All rights reserved
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

#define _POSIX_SOURCE
#include "ddelta.h"

#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include <divsufsort.h>

#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif

#ifndef MAX
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#endif

static uint32_t ddelta_htobe32(uint32_t host)
{
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(host);
#elif defined(__GNUC__) && defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return host;
#else
    uint32_t be32;
    unsigned char *buf = &be32;

    buf[0] = (host >> 24) & 0xFF;
    buf[1] = (host >> 16) & 0xFF;
    buf[2] = (host >> 8) & 0xFF;
    buf[3] = (host >> 0) & 0xFF;

    return be32;
#endif
}

static uint64_t ddelta_htobe64(uint64_t host)
{
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(host);
#elif defined(__GNUC__) && defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return host;
#else
    uint64_t be64;
    unsigned char *buf = &be64;

    buf[0] = (host >> 56) & 0xFF;
    buf[1] = (host >> 48) & 0xFF;
    buf[2] = (host >> 40) & 0xFF;
    buf[3] = (host >> 32) & 0xFF;
    buf[4] = (host >> 24) & 0xFF;
    buf[5] = (host >> 16) & 0xFF;
    buf[6] = (host >> 8) & 0xFF;
    buf[7] = (host >> 0) & 0xFF;

    return be64;
#endif
}

static uint32_t ddelta_to_unsigned(int32_t i)
{
    return i >= 0 ? (uint32_t) i : ~(uint32_t)(-i) + 1;
}

static int ddelta_header_write(struct ddelta_header *header, FILE *file)
{
    header->new_file_size = ddelta_htobe64(header->new_file_size);

    if (fwrite(header, sizeof(*header), 1, file) < 1)
        return -DDELTA_EPATCHIO;

    return 0;
}

static int ddelta_entry_header_write(struct ddelta_entry_header *entry,
                                     FILE *file)
{
    entry->diff = ddelta_htobe32(entry->diff);
    entry->extra = ddelta_htobe32(entry->extra);
    entry->seek.raw = ddelta_htobe32(ddelta_to_unsigned(entry->seek.value));

    if (fwrite(entry, sizeof(*entry), 1, file) < 1)
        return -DDELTA_EPATCHIO;

    return 0;
}

static off_t matchlen(unsigned char *old, off_t oldsize, unsigned char *new,
                      off_t newsize)
{
    off_t i;

    for (i = 0; (i < oldsize) && (i < newsize); i++)
        if (old[i] != new[i])
            break;

    return i;
}

/* This is a binary search of the string |new| of size |newsize| (or a
 * prefix of it) in the |old| string with size |oldsize| using the suffix array
 * |I|. |st| and |en| is the start and end of the search range (inclusive).
 * Returns the length of the longest prefix found and stores the position of the
 * string found in |*pos|. */
static off_t search(saidx_t *I, unsigned char *old, off_t oldsize,
                    unsigned char *new, off_t newsize, off_t st, off_t en,
                    off_t *pos)
{
    off_t x, y;

    if (en - st < 2) {
        x = matchlen(old + I[st], oldsize - I[st], new, newsize);
        y = matchlen(old + I[en], oldsize - I[en], new, newsize);

        if (x > y) {
            *pos = I[st];
            return x;
        } else {
            *pos = I[en];
            return y;
        }
    };

    x = st + (en - st) / 2;
    if (memcmp(old + I[x], new, MIN(oldsize - I[x], newsize)) <= 0) {
        return search(I, old, oldsize, new, newsize, x, en, pos);
    } else {
        return search(I, old, oldsize, new, newsize, st, x, pos);
    };
}

static off_t read_file(int fd, unsigned char **buf)
{
    off_t size;
    if (fd < 0)
        return -1;

    if (((size = lseek(fd, 0, SEEK_END)) == -1) ||
        ((*buf = malloc(size + 1)) == NULL) ||
        (lseek(fd, 0, SEEK_SET) != 0) ||
        (read(fd, *buf, size) != size) || (close(fd) == -1))
        return -1;

    return size;
}

int ddelta_generate(int oldfd, int newfd, int patchfd, int blocksize)
{
    struct ddelta_header file_header = {
        DDELTA_MAGIC,
        0};
    struct ddelta_entry_header header;
    unsigned char *old = NULL, *new = NULL;
    off_t scansize, oldsize, newsize;
    uint32_t oldcrc, newcrc;
    saidx_t *I = NULL;
    off_t scan, pos = 0, len;
    off_t lastscan, lastpos, lastoffset;
    off_t oldscore, scsc;
    off_t s, Sf, lenf, Sb, lenb;
    off_t overlap, Ss, lens;
    off_t i;
    FILE *pf = NULL;
    int result = 0;

    newsize = read_file(newfd, &new);
    if (newsize > INT32_MAX) {
        result = -DDELTA_ENEWIO;
        goto out;
    } else if (newsize < 0) {
        result = -DDELTA_ENEWIO;
        goto out;
    }

    oldsize = read_file(oldfd, &old);
    if (oldsize > INT32_MAX) {
        result = -DDELTA_EOLDIO;
        goto out;
    } else if (oldsize < 0) {
        result = -DDELTA_EOLDIO;
        goto out;
    }

    if (newsize > oldsize) {
        unsigned char *tmp = realloc(old, newsize);
        if (tmp == NULL) {
            result = -DDELTA_EOLDIO;
            goto out;
        }
        memset(tmp + oldsize, 0, newsize - oldsize);
        old = tmp;
    }

    I = malloc((MAX(oldsize, newsize) + 1) * sizeof(saidx_t));
    if (I == NULL) {
        result = -DDELTA_EALGO;
        goto out;
    }

    /* Create the patch file */
    if ((pf = fdopen(patchfd, "wb")) == NULL) {
        result = -DDELTA_EPATCHIO;
        goto out;
    }

    file_header.new_file_size = (uint64_t) newsize;
    if ((result = ddelta_header_write(&file_header, pf)) < 0)
        goto out;

    scan = 0;
    len = 0;
    lastscan = 0;
    lastpos = 0;
    lastoffset = 0;

    if (blocksize > 0)
        scansize = MIN(blocksize, newsize);
    else
        scansize = newsize;

next:
    len = 0;
    oldcrc = 0;
    newcrc = 0;

    if (divsufsort(old, I, (int32_t) oldsize)) {
        result = -DDELTA_EALGO;
        goto out;
    }

    while (scan < scansize) {
        /* If we come across a large block of data that only differs
         * by less than 8 bytes, this loop will take a long time to
         * go past that block of data. We need to track the number of
         * times we're stuck in the block and break out of it. */
        int num_less_than_eight = 0;
        off_t prev_len, prev_oldscore, prev_pos;

        oldscore = 0;
        for (scsc = scan += len; scan < scansize; scan++) {
            const off_t fuzz = 8;

            prev_len = len;
            prev_oldscore = oldscore;
            prev_pos = pos;

            len = search(I, old, oldsize, new + scan, scansize - scan,
                         0, oldsize - 1, &pos);

            for (; scsc < scan + len; scsc++)
                if ((scsc + lastoffset < oldsize) &&
                    (old[scsc + lastoffset] == new[scsc]))
                    oldscore++;

            if (((len == oldscore) && (len != 0)) || (len > oldscore + 8))
                break;

            if ((scan + lastoffset < oldsize) &&
                (old[scan + lastoffset] == new[scan]))
                oldscore--;

            if (prev_len - fuzz <= len && len <= prev_len &&
                prev_oldscore - fuzz <= oldscore &&
                oldscore <= prev_oldscore &&
                prev_pos <= pos && pos <= prev_pos + fuzz &&
                oldscore <= len && len <= oldscore + fuzz)
                ++num_less_than_eight;
            else
                num_less_than_eight = 0;
            if (num_less_than_eight > 100)
                break;
        };

        if ((len != oldscore) || (scan == scansize)) {
            s = 0;
            Sf = 0;
            lenf = 0;
            for (i = 0; (lastscan + i < scan) && (lastpos + i < oldsize);) {
                if (old[lastpos + i] == new[lastscan + i])
                    s++;
                i++;
                if (s * 2 - i > Sf * 2 - lenf) {
                    Sf = s;
                    lenf = i;
                };
            };

            lenb = 0;
            if (scan < scansize) {
                s = 0;
                Sb = 0;
                for (i = 1; (scan >= lastscan + i) && (pos >= i); i++) {
                    if (old[pos - i] == new[scan - i])
                        s++;
                    if (s * 2 - i > Sb * 2 - lenb) {
                        Sb = s;
                        lenb = i;
                    };
                };
            };

            if (lastscan + lenf > scan - lenb) {
                overlap = (lastscan + lenf) - (scan - lenb);
                s = 0;
                Ss = 0;
                lens = 0;
                for (i = 0; i < overlap; i++) {
                    if (new[lastscan + lenf - overlap + i] ==
                        old[lastpos + lenf - overlap + i])
                        s++;
                    if (new[scan - lenb + i] == old[pos - lenb + i])
                        s--;
                    if (s > Ss) {
                        Ss = s;
                        lens = i + 1;
                    };
                };

                lenf += lens - overlap;
                lenb -= lens;
            };

            if (lenf < 0 || (scan - lenb) - (lastscan + lenf) < 0) {
                result = -DDELTA_EALGO;
                goto out;
            }

            header.diff = (uint32_t) lenf;
            header.extra = (uint32_t)((scan - lenb) - (lastscan + lenf));
            header.seek.value = (pos - lenb) - (lastpos + lenf);

            if (header.diff != lenf ||
                header.extra != (scan - lenb) - (lastscan + lenf) ||
                header.seek.value == DDELTA_FLUSH ||
                header.seek.value != (pos - lenb) - (lastpos + lenf)) {
                result = -DDELTA_EALGO;
                goto out;
            }

            if ((result = ddelta_entry_header_write(&header, pf)) < 0)
                goto out;

            for (i = 0; i < lenf; i++) {
                if (fputc(new[lastscan + i] - old[lastpos + i], pf) == EOF) {
                    result = -DDELTA_EPATCHIO;
                    goto out;
                }
            }

            if ((scan - lenb) - (lastscan + lenf)) {
                if (fwrite(new + lastscan + lenf,
                           (scan - lenb) - (lastscan + lenf), 1, pf) < 1) {
                    result = -DDELTA_EPATCHIO;
                    goto out;
                }
            }

            oldcrc = crc32(oldcrc, old + lastpos, lenf);
            newcrc = crc32(newcrc, new + lastscan, scan - lenb - lastscan);

            lastscan = scan - lenb;
            lastpos = pos - lenb;
            lastoffset = pos - scan;
        };
    };

    header.oldcrc = oldcrc;
    header.newcrc = newcrc;
    header.seek.value = DDELTA_FLUSH;
    if ((result = ddelta_entry_header_write(&header, pf)) < 0)
        goto out;


    if (scan < newsize) {
        memcpy(old + scansize - blocksize, new + scansize - blocksize, blocksize);
        oldsize = MAX(oldsize, scansize);
        scansize = MIN(scansize + blocksize, newsize);
        goto next;
    }

    memset(&header, 0, sizeof(header));
    if ((result = ddelta_entry_header_write(&header, pf)) < 0)
        goto out;

out:
    if (pf != NULL) {
        int save_errno = errno;

        if (fclose(pf) && result == 0) {
            result = -DDELTA_EPATCHIO;
        } else {
            errno = save_errno;
        }
    }

    /* Free the memory we used */
    free(I);
    free(old);
    free(new);

    return result;
}

#ifndef DDELTA_NO_MAIN
int main(int argc, char *argv[])
{
    int oldfd;
    int newfd;
    int patchfd;
    int blocksize;
    int err;

    if (argc < 4) {
        fprintf(stderr, "usage: %s oldfile newfile patchfile [blocksize]\n", argv[0]);
        return 1;
    }

    oldfd = open(argv[1], O_RDONLY, 0);
    if (oldfd < 0) {
        perror(argv[1]);
        return 1;
    }
    newfd = open(argv[2], O_RDONLY, 0);
    if (newfd < 0) {
        perror(argv[2]);
        return 1;
    }

    patchfd = open(argv[3], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (patchfd < 0) {
        perror(argv[3]);
        return 1;
    }

    blocksize = argc >= 5 ? atoi(argv[4]) : 0;
    err = ddelta_generate(oldfd, newfd, patchfd, blocksize);
    if (err < 0) {
        fprintf(stderr, "An error %d occured: %s", -err, strerror(errno));
        return -err;
    }
    return 0;
}
#endif
