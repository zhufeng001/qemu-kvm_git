/*
 * QEMU dump
 *
 * Copyright Fujitsu, Corp. 2011, 2012
 *
 * Authors:
 *     Wen Congyang <wency@cn.fujitsu.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2. See
 * the COPYING file in the top-level directory.
 *
 */

#ifndef DUMP_H
#define DUMP_H

typedef struct ArchDumpInfo {
    int d_machine;  /* Architecture */
    int d_endian;   /* ELFDATA2LSB or ELFDATA2MSB */
    int d_class;    /* ELFCLASS32 or ELFCLASS64 */
} ArchDumpInfo;

#endif
