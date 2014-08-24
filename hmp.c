/*
 * Human Monitor Interface
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include "hmp.h"
#include "qmp-commands.h"

static void hmp_handle_error(Monitor *mon, Error **errp)
{
    if (error_is_set(errp)) {
        monitor_printf(mon, "%s\n", error_get_pretty(*errp));
        error_free(*errp);
    }
}

#ifdef CONFIG_LIVE_SNAPSHOTS
void hmp_drive_mirror(Monitor *mon, const QDict *qdict)
{
    const char *device = qdict_get_str(qdict, "device");
    const char *filename = qdict_get_try_str(qdict, "target");
    const char *format = qdict_get_try_str(qdict, "format");
    int reuse = qdict_get_try_bool(qdict, "reuse", 0);
    int full = qdict_get_try_bool(qdict, "full", 0);
    int speed = qdict_get_try_int(qdict, "speed", 0);
    enum NewImageMode mode;
    Error *errp = NULL;

    if (!filename) {
        error_set(&errp, QERR_MISSING_PARAMETER, "target");
        hmp_handle_error(mon, &errp);
        return;
    }

    if (reuse) {
        mode = NEW_IMAGE_MODE_EXISTING;
    } else {
        mode = NEW_IMAGE_MODE_ABSOLUTE_PATHS;
    }

    qmp___com_redhat_drive_mirror(device, filename, !!format, format,
				  qdict_haskey(qdict, "speed"), speed, true, full, true, mode, &errp);
    hmp_handle_error(mon, &errp);
}

void hmp_snapshot_blkdev(Monitor *mon, const QDict *qdict)
{
    const char *device = qdict_get_str(qdict, "device");
    const char *filename = qdict_get_try_str(qdict, "snapshot-file");
    const char *format = qdict_get_try_str(qdict, "format");
    int reuse = qdict_get_try_bool(qdict, "reuse", 0);
    enum NewImageMode mode;
    Error *errp = NULL;

    if (!filename) {
        /* In the future, if 'snapshot-file' is not specified, the snapshot
           will be taken internally. Today it's actually required. */
        error_set(&errp, QERR_MISSING_PARAMETER, "snapshot-file");
        hmp_handle_error(mon, &errp);
        return;
    }

    mode = reuse ? NEW_IMAGE_MODE_EXISTING : NEW_IMAGE_MODE_ABSOLUTE_PATHS;
    qmp_blockdev_snapshot_sync(device, filename, !!format, format,
                               true, mode, &errp);
    hmp_handle_error(mon, &errp);
}

void hmp_drive_reopen(Monitor *mon, const QDict *qdict)
{
    const char *device = qdict_get_str(qdict, "device");
    const char *filename = qdict_get_str(qdict, "new-image-file");
    const char *format = qdict_get_try_str(qdict, "format");
    Error *errp = NULL;

    qmp___com_redhat_drive_reopen(device, filename, !!format, format, false, NULL, &errp);
    hmp_handle_error(mon, &errp);
}
#endif

void hmp_dump_guest_memory(Monitor *mon, const QDict *qdict)
{
    Error *errp = NULL;
    int paging = qdict_get_try_bool(qdict, "paging", 0);
    const char *file = qdict_get_str(qdict, "filename");
    bool has_begin = qdict_haskey(qdict, "begin");
    bool has_length = qdict_haskey(qdict, "length");
    int64_t begin = 0;
    int64_t length = 0;
    char *prot;

    if (has_begin) {
        begin = qdict_get_int(qdict, "begin");
    }
    if (has_length) {
        length = qdict_get_int(qdict, "length");
    }

    prot = g_strconcat("file:", file, NULL);
    qmp_dump_guest_memory(paging, prot, has_begin, begin, has_length, length,
                          &errp);
    hmp_handle_error(mon, &errp);
    g_free(prot);
}
