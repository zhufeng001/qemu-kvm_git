/*
 * Virtio Block Device
 *
 * Copyright IBM, Corp. 2007
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include <qemu-common.h>
#include "qemu-error.h"
#include "trace.h"
#include "virtio-blk.h"
#ifdef CONFIG_VIRTIO_BLK_DATA_PLANE
#include "hw/dataplane/virtio-blk.h"
#endif
#include "scsi-defs.h"
#ifdef __linux__
# include <scsi/sg.h>
#endif

typedef struct VirtIOBlock
{
    VirtIODevice vdev;
    BlockDriverState *bs;
    VirtQueue *vq;
    void *rq;
    QEMUBH *bh;
    BlockConf *conf;
    VirtIOBlkConf *blk;
    unsigned short sector_mask;
    DeviceState *qdev;
    VMChangeStateEntry *change;
#ifdef CONFIG_VIRTIO_BLK_DATA_PLANE
    VirtIOBlockDataPlane *dataplane;
#endif
} VirtIOBlock;

static VirtIOBlock *to_virtio_blk(VirtIODevice *vdev)
{
    return (VirtIOBlock *)vdev;
}

typedef struct VirtIOBlockReq
{
    VirtIOBlock *dev;
    VirtQueueElement elem;
    struct virtio_blk_inhdr *in;
    struct virtio_blk_outhdr *out;
    struct virtio_scsi_inhdr *scsi;
    QEMUIOVector qiov;
    struct VirtIOBlockReq *next;
    BlockAcctCookie acct;
} VirtIOBlockReq;

static void virtio_blk_req_complete(VirtIOBlockReq *req, int status)
{
    VirtIOBlock *s = req->dev;

    trace_virtio_blk_req_complete(req, status);

    req->in->status = status;
    virtqueue_push(s->vq, &req->elem, req->qiov.size + sizeof(*req->in));
    virtio_notify(&s->vdev, s->vq);
}

static int virtio_blk_handle_rw_error(VirtIOBlockReq *req, int error,
    int is_read)
{
    BlockErrorAction action = bdrv_get_on_error(req->dev->bs, is_read);
    VirtIOBlock *s = req->dev;

    if (action == BLOCK_ERR_IGNORE) {
        bdrv_emit_qmp_error_event(s->bs, BDRV_ACTION_IGNORE, error, is_read);
        return 0;
    }

    if ((error == ENOSPC && action == BLOCK_ERR_STOP_ENOSPC)
            || action == BLOCK_ERR_STOP_ANY) {
        req->next = s->rq;
        s->rq = req;
        bdrv_emit_qmp_error_event(s->bs, BDRV_ACTION_STOP, error, is_read);
        vm_stop(RUN_STATE_IO_ERROR);
        bdrv_iostatus_set_err(s->bs, error);
    } else {
        virtio_blk_req_complete(req, VIRTIO_BLK_S_IOERR);
        bdrv_acct_done(s->bs, &req->acct);
        qemu_free(req);
        bdrv_emit_qmp_error_event(s->bs, BDRV_ACTION_REPORT, error, is_read);
    }

    return 1;
}

static void virtio_blk_rw_complete(void *opaque, int ret)
{
    VirtIOBlockReq *req = opaque;

    trace_virtio_blk_rw_complete(req, ret);

    if (ret) {
        int is_read = !(req->out->type & VIRTIO_BLK_T_OUT);
        if (virtio_blk_handle_rw_error(req, -ret, is_read))
            return;
    }

    virtio_blk_req_complete(req, VIRTIO_BLK_S_OK);
    bdrv_acct_done(req->dev->bs, &req->acct);
    qemu_free(req);
}

static void virtio_blk_flush_complete(void *opaque, int ret)
{
    VirtIOBlockReq *req = opaque;

    if (ret) {
        if (virtio_blk_handle_rw_error(req, -ret, 0)) {
            return;
        }
    }

    virtio_blk_req_complete(req, VIRTIO_BLK_S_OK);
    bdrv_acct_done(req->dev->bs, &req->acct);
    qemu_free(req);
}

static VirtIOBlockReq *virtio_blk_alloc_request(VirtIOBlock *s)
{
    VirtIOBlockReq *req = qemu_malloc(sizeof(*req));
    req->dev = s;
    req->qiov.size = 0;
    req->next = NULL;
    return req;
}

static VirtIOBlockReq *virtio_blk_get_request(VirtIOBlock *s)
{
    VirtIOBlockReq *req = virtio_blk_alloc_request(s);

    if (req != NULL) {
        if (!virtqueue_pop(s->vq, &req->elem)) {
            qemu_free(req);
            return NULL;
        }
    }

    return req;
}

#ifdef __linux__
static void virtio_blk_handle_scsi(VirtIOBlockReq *req)
{
    struct sg_io_hdr hdr;
    int ret, size = 0;
    int status;
    int i;

    if ((req->dev->vdev.guest_features & (1 << VIRTIO_BLK_F_SCSI)) == 0) {
        virtio_blk_req_complete(req, VIRTIO_BLK_S_UNSUPP);
        qemu_free(req);
        return;
    }

    /*
     * We require at least one output segment each for the virtio_blk_outhdr
     * and the SCSI command block.
     *
     * We also at least require the virtio_blk_inhdr, the virtio_scsi_inhdr
     * and the sense buffer pointer in the input segments.
     */
    if (req->elem.out_num < 2 || req->elem.in_num < 3) {
        virtio_blk_req_complete(req, VIRTIO_BLK_S_IOERR);
        qemu_free(req);
        return;
    }

    /*
     * No support for bidirection commands yet.
     */
    if (req->elem.out_num > 2 && req->elem.in_num > 3) {
        virtio_blk_req_complete(req, VIRTIO_BLK_S_UNSUPP);
        qemu_free(req);
        return;
    }

    /*
     * The scsi inhdr is placed in the second-to-last input segment, just
     * before the regular inhdr.
     */
    req->scsi = (void *)req->elem.in_sg[req->elem.in_num - 2].iov_base;
    size = sizeof(*req->in) + sizeof(*req->scsi);

    memset(&hdr, 0, sizeof(struct sg_io_hdr));
    hdr.interface_id = 'S';
    hdr.cmd_len = req->elem.out_sg[1].iov_len;
    hdr.cmdp = req->elem.out_sg[1].iov_base;
    hdr.dxfer_len = 0;

    if (req->elem.out_num > 2) {
        /*
         * If there are more than the minimally required 2 output segments
         * there is write payload starting from the third iovec.
         */
        hdr.dxfer_direction = SG_DXFER_TO_DEV;
        hdr.iovec_count = req->elem.out_num - 2;

        for (i = 0; i < hdr.iovec_count; i++)
            hdr.dxfer_len += req->elem.out_sg[i + 2].iov_len;

        hdr.dxferp = req->elem.out_sg + 2;

    } else if (req->elem.in_num > 3) {
        /*
         * If we have more than 3 input segments the guest wants to actually
         * read data.
         */
        hdr.dxfer_direction = SG_DXFER_FROM_DEV;
        hdr.iovec_count = req->elem.in_num - 3;
        for (i = 0; i < hdr.iovec_count; i++)
            hdr.dxfer_len += req->elem.in_sg[i].iov_len;

        hdr.dxferp = req->elem.in_sg;
        size += hdr.dxfer_len;
    } else {
        /*
         * Some SCSI commands don't actually transfer any data.
         */
        hdr.dxfer_direction = SG_DXFER_NONE;
    }

    hdr.sbp = req->elem.in_sg[req->elem.in_num - 3].iov_base;
    hdr.mx_sb_len = req->elem.in_sg[req->elem.in_num - 3].iov_len;
    size += hdr.mx_sb_len;

    ret = bdrv_ioctl(req->dev->bs, SG_IO, &hdr);
    if (ret) {
        status = VIRTIO_BLK_S_UNSUPP;
        hdr.status = ret;
        hdr.resid = hdr.dxfer_len;
    } else if (hdr.status) {
        status = VIRTIO_BLK_S_IOERR;
    } else {
        status = VIRTIO_BLK_S_OK;
    }

    /*
     * From SCSI-Generic-HOWTO: "Some lower level drivers (e.g. ide-scsi)
     * clear the masked_status field [hence status gets cleared too, see
     * block/scsi_ioctl.c] even when a CHECK_CONDITION or COMMAND_TERMINATED
     * status has occurred.  However they do set DRIVER_SENSE in driver_status
     * field. Also a (sb_len_wr > 0) indicates there is a sense buffer.
     */
    if (hdr.status == 0 && hdr.sb_len_wr > 0) {
        hdr.status = CHECK_CONDITION;
    }

    req->scsi->errors = hdr.status | (hdr.msg_status << 8) |
        (hdr.host_status << 16) | (hdr.driver_status << 24);
    req->scsi->residual = hdr.resid;
    req->scsi->sense_len = hdr.sb_len_wr;
    req->scsi->data_len = hdr.dxfer_len;

    virtio_blk_req_complete(req, status);
    qemu_free(req);
}
#else
static void virtio_blk_handle_scsi(VirtIOBlockReq *req)
{
    virtio_blk_req_complete(req, VIRTIO_BLK_S_UNSUPP);
    qemu_free(req);
}
#endif /* __linux__ */

static void do_multiwrite(BlockDriverState *bs, BlockRequest *blkreq,
    int num_writes)
{
    int i, ret;
    ret = bdrv_aio_multiwrite(bs, blkreq, num_writes);

    if (ret != 0) {
        for (i = 0; i < num_writes; i++) {
            if (blkreq[i].error) {
                virtio_blk_rw_complete(blkreq[i].opaque, -EIO);
            }
        }
    }
}

static void virtio_blk_handle_flush(BlockRequest *blkreq, int *num_writes,
    VirtIOBlockReq *req, BlockDriverState **old_bs)
{
    BlockDriverAIOCB *acb;

    bdrv_acct_start(req->dev->bs, &req->acct, 0, BDRV_ACCT_FLUSH);

    /*
     * Make sure all outstanding writes are posted to the backing device.
     */
    if (*old_bs != NULL) {
        do_multiwrite(*old_bs, blkreq, *num_writes);
    }
    *num_writes = 0;
    *old_bs = req->dev->bs;

    acb = bdrv_aio_flush(req->dev->bs, virtio_blk_flush_complete, req);
    if (!acb) {
        virtio_blk_flush_complete(req, -EIO);
    }
}

static void virtio_blk_handle_write(BlockRequest *blkreq, int *num_writes,
    VirtIOBlockReq *req, BlockDriverState **old_bs)
{
    trace_virtio_blk_handle_write(req, req->out->sector, req->qiov.size / 512);

    bdrv_acct_start(req->dev->bs, &req->acct, req->qiov.size, BDRV_ACCT_WRITE);

    if (req->out->sector & req->dev->sector_mask) {
        virtio_blk_rw_complete(req, -EIO);
        return;
    }

    if (req->dev->bs != *old_bs || *num_writes == 32) {
        if (*old_bs != NULL) {
            do_multiwrite(*old_bs, blkreq, *num_writes);
        }
        *num_writes = 0;
        *old_bs = req->dev->bs;
    }
    if (req->qiov.size % req->dev->conf->logical_block_size) {
        virtio_blk_rw_complete(req, -EIO);
        return;
    }

    blkreq[*num_writes].sector = req->out->sector;
    blkreq[*num_writes].nb_sectors = req->qiov.size / 512;
    blkreq[*num_writes].qiov = &req->qiov;
    blkreq[*num_writes].cb = virtio_blk_rw_complete;
    blkreq[*num_writes].opaque = req;
    blkreq[*num_writes].error = 0;

    (*num_writes)++;
}

static void virtio_blk_handle_read(VirtIOBlockReq *req)
{
    BlockDriverAIOCB *acb;

    bdrv_acct_start(req->dev->bs, &req->acct, req->qiov.size, BDRV_ACCT_READ);

    if (req->out->sector & req->dev->sector_mask) {
        virtio_blk_rw_complete(req, -EIO);
        return;
    }
    if (req->qiov.size % req->dev->conf->logical_block_size) {
        virtio_blk_rw_complete(req, -EIO);
        return;
    }

    acb = bdrv_aio_readv(req->dev->bs, req->out->sector, &req->qiov,
                         req->qiov.size / 512, virtio_blk_rw_complete, req);
    if (!acb) {
        virtio_blk_rw_complete(req, -EIO);
    }
}

typedef struct MultiReqBuffer {
    BlockRequest        blkreq[32];
    int                 num_writes;
    BlockDriverState    *old_bs;
} MultiReqBuffer;

static void virtio_blk_handle_request(VirtIOBlockReq *req,
    MultiReqBuffer *mrb)
{
    if (req->elem.out_num < 1 || req->elem.in_num < 1) {
        fprintf(stderr, "virtio-blk missing headers\n");
        exit(1);
    }

    if (req->elem.out_sg[0].iov_len < sizeof(*req->out) ||
        req->elem.in_sg[req->elem.in_num - 1].iov_len < sizeof(*req->in)) {
        fprintf(stderr, "virtio-blk header not in correct element\n");
        exit(1);
    }

    req->out = (void *)req->elem.out_sg[0].iov_base;
    req->in = (void *)req->elem.in_sg[req->elem.in_num - 1].iov_base;

    if (req->out->type & VIRTIO_BLK_T_FLUSH) {
        virtio_blk_handle_flush(mrb->blkreq, &mrb->num_writes,
            req, &mrb->old_bs);
    } else if (req->out->type & VIRTIO_BLK_T_SCSI_CMD) {
        virtio_blk_handle_scsi(req);
    } else if (req->out->type & VIRTIO_BLK_T_GET_ID) {
        VirtIOBlock *s = req->dev;

        /*
         * NB: per existing s/n string convention the string is
         * terminated by '\0' only when shorter than buffer.
         */
        strncpy(req->elem.in_sg[0].iov_base,
                s->blk->serial ? s->blk->serial : "",
                MIN(req->elem.in_sg[0].iov_len, VIRTIO_BLK_ID_BYTES));
        virtio_blk_req_complete(req, VIRTIO_BLK_S_OK);
        qemu_free(req);
    } else if (req->out->type & VIRTIO_BLK_T_OUT) {
        qemu_iovec_init_external(&req->qiov, &req->elem.out_sg[1],
                                 req->elem.out_num - 1);
        virtio_blk_handle_write(mrb->blkreq, &mrb->num_writes,
            req, &mrb->old_bs);
    } else {
        qemu_iovec_init_external(&req->qiov, &req->elem.in_sg[0],
                                 req->elem.in_num - 1);
        virtio_blk_handle_read(req);
    }
}

static void virtio_blk_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOBlock *s = to_virtio_blk(vdev);
    VirtIOBlockReq *req;
    MultiReqBuffer mrb = {
        .num_writes = 0,
        .old_bs = NULL,
    };

#ifdef CONFIG_VIRTIO_BLK_DATA_PLANE
    /* Some guests kick before setting VIRTIO_CONFIG_S_DRIVER_OK so start
     * dataplane here instead of waiting for .set_status().
     */
    if (s->dataplane) {
        virtio_blk_data_plane_start(s->dataplane);
        return;
    }
#endif

    while ((req = virtio_blk_get_request(s))) {
        virtio_blk_handle_request(req, &mrb);
    }

    if (mrb.num_writes > 0) {
        do_multiwrite(mrb.old_bs, mrb.blkreq, mrb.num_writes);
    }

    /*
     * FIXME: Want to check for completions before returning to guest mode,
     * so cached reads and writes are reported as quickly as possible. But
     * that should be done in the generic block layer.
     */
}

static void virtio_blk_dma_restart_bh(void *opaque)
{
    VirtIOBlock *s = opaque;
    VirtIOBlockReq *req = s->rq;
    MultiReqBuffer mrb = {
        .num_writes = 0,
        .old_bs = NULL,
    };

    qemu_bh_delete(s->bh);
    s->bh = NULL;

    s->rq = NULL;

    while (req) {
        virtio_blk_handle_request(req, &mrb);
        req = req->next;
    }

    if (mrb.num_writes > 0) {
        do_multiwrite(mrb.old_bs, mrb.blkreq, mrb.num_writes);
    }
}

static void virtio_blk_dma_restart_cb(void *opaque, int running, RunState state)
{
    VirtIOBlock *s = opaque;

    if (!running)
        return;

    if (!s->bh) {
        s->bh = qemu_bh_new(virtio_blk_dma_restart_bh, s);
        qemu_bh_schedule(s->bh);
    }
}

static void virtio_blk_reset(VirtIODevice *vdev)
{
#ifdef CONFIG_VIRTIO_BLK_DATA_PLANE
    VirtIOBlock *s = to_virtio_blk(vdev);

    if (s->dataplane) {
        virtio_blk_data_plane_stop(s->dataplane);
    }
#endif

    /*
     * This should cancel pending requests, but can't do nicely until there
     * are per-device request lists.
     */
    bdrv_drain_all();
}

/* coalesce internal state, copy to pci i/o region 0
 */
static void virtio_blk_update_config(VirtIODevice *vdev, uint8_t *config)
{
    VirtIOBlock *s = to_virtio_blk(vdev);
    struct virtio_blk_config blkcfg;
    uint64_t capacity;
    int cylinders, heads, secs;

    bdrv_get_geometry(s->bs, &capacity);
    bdrv_get_geometry_hint(s->bs, &cylinders, &heads, &secs);
    memset(&blkcfg, 0, sizeof(blkcfg));
    stq_raw(&blkcfg.capacity, capacity);
    stl_raw(&blkcfg.seg_max, 128 - 2);
    stw_raw(&blkcfg.cylinders, cylinders);
    blkcfg.heads = heads;
    blkcfg.sectors = secs & ~s->sector_mask;
    blkcfg.blk_size = s->conf->logical_block_size;
    blkcfg.size_max = 0;
    blkcfg.physical_block_exp = get_physical_block_exp(s->conf);
    blkcfg.alignment_offset = 0;
    blkcfg.min_io_size = s->conf->min_io_size / blkcfg.blk_size;
    blkcfg.opt_io_size = s->conf->opt_io_size / blkcfg.blk_size;
    memcpy(config, &blkcfg, sizeof(struct virtio_blk_config));
}

static uint32_t virtio_blk_get_features(VirtIODevice *vdev, uint32_t features)
{
    VirtIOBlock *s = to_virtio_blk(vdev);

    features |= (1 << VIRTIO_BLK_F_SEG_MAX);
    features |= (1 << VIRTIO_BLK_F_GEOMETRY);
    features |= (1 << VIRTIO_BLK_F_TOPOLOGY);
    features |= (1 << VIRTIO_BLK_F_BLK_SIZE);

    if (bdrv_enable_write_cache(s->bs))
        features |= (1 << VIRTIO_BLK_F_WCACHE);
    
    if (bdrv_is_read_only(s->bs))
        features |= 1 << VIRTIO_BLK_F_RO;

    return features;
}

#ifdef CONFIG_VIRTIO_BLK_DATA_PLANE
static void virtio_blk_set_status(VirtIODevice *vdev, uint8_t status)
{
    VirtIOBlock *s = to_virtio_blk(vdev);

    if (s->dataplane && !(status & (VIRTIO_CONFIG_S_DRIVER |
                                    VIRTIO_CONFIG_S_DRIVER_OK))) {
        virtio_blk_data_plane_stop(s->dataplane);
    }
}
#endif

static void virtio_blk_save(QEMUFile *f, void *opaque)
{
    VirtIOBlock *s = opaque;
    VirtIOBlockReq *req = s->rq;

    virtio_save(&s->vdev, f);
    
    while (req) {
        qemu_put_sbyte(f, 1);
        qemu_put_buffer(f, (unsigned char*)&req->elem, sizeof(req->elem));
        req = req->next;
    }
    qemu_put_sbyte(f, 0);
}

static int virtio_blk_load(QEMUFile *f, void *opaque, int version_id)
{
    VirtIOBlock *s = opaque;
    int ret;

    if (version_id != 2)
        return -EINVAL;

    /* RHEL only.  Upstream we will fix this by ensuring that VIRTIO_BLK_F_SCSI
     * is always set.  This however will cause migration from new QEMU to old
     * QEMU to fail if both have scsi=off.  We cannot use compatibility
     * properties because the scsi property is being overridden by management
     * (libvirt).
     *
     * If RHEL7->RHEL6 migration will be supported, we'll have to disable
     * VIRTIO_BLK_F_SCSI in the migration stream when running on a rhel6.x.0
     * machine type with scsi=off.  Otherwise SG_IO will magically start
     * working on the destination.
     */
    ret = virtio_load_with_features(&s->vdev, f, 1 << VIRTIO_BLK_F_SCSI);
    if (ret) {
        return ret;
    }
    while (qemu_get_sbyte(f)) {
        VirtIOBlockReq *req = virtio_blk_alloc_request(s);
        qemu_get_buffer(f, (unsigned char*)&req->elem, sizeof(req->elem));
        req->next = s->rq;
        s->rq = req;

        virtqueue_map_sg(req->elem.in_sg, req->elem.in_addr,
            req->elem.in_num, 1);
        virtqueue_map_sg(req->elem.out_sg, req->elem.out_addr,
            req->elem.out_num, 0);
    }

    return 0;
}

static void virtio_blk_resize(void *opaque)
{
    VirtIOBlock *s = opaque;

    virtio_notify_config(&s->vdev);
}

static const BlockDevOps virtio_block_ops = {
    .resize_cb = virtio_blk_resize,
};

VirtIODevice *virtio_blk_init(DeviceState *dev, VirtIOBlkConf *blk)
{
    VirtIOBlock *s;
    int cylinders, heads, secs;
    static int virtio_blk_id;
    DriveInfo *dinfo;

    if (!blk->conf.bs) {
        error_report("drive property not set");
        return NULL;
    }
    if (!bdrv_is_inserted(blk->conf.bs)) {
        error_report("Device needs media, but drive is empty");
        return NULL;
    }

    if (!blk->serial) {
        /* try to fall back to value set with legacy -drive serial=... */
        dinfo = drive_get_by_blockdev(blk->conf.bs);
        if (*dinfo->serial) {
            blk->serial = strdup(dinfo->serial);
        }
    }

    s = (VirtIOBlock *)virtio_common_init("virtio-blk", VIRTIO_ID_BLOCK,
                                          sizeof(struct virtio_blk_config),
                                          sizeof(VirtIOBlock));

    s->vdev.get_config = virtio_blk_update_config;
    s->vdev.get_features = virtio_blk_get_features;
#ifdef CONFIG_VIRTIO_BLK_DATA_PLANE
    s->vdev.set_status = virtio_blk_set_status;
#endif
    s->vdev.reset = virtio_blk_reset;
    s->bs = blk->conf.bs;
    s->conf = &blk->conf;
    s->blk = blk;
    s->rq = NULL;
    s->sector_mask = (s->conf->logical_block_size / 512) - 1;
    bdrv_guess_geometry(s->bs, &cylinders, &heads, &secs);
    bdrv_set_geometry_hint(s->bs, cylinders, heads, secs);

    s->vq = virtio_add_queue(&s->vdev, 128, virtio_blk_handle_output);
#ifdef CONFIG_VIRTIO_BLK_DATA_PLANE
    if (!virtio_blk_data_plane_create(&s->vdev, blk, &s->dataplane)) {
        virtio_cleanup(&s->vdev);
        return NULL;
    }
#endif

    s->change = qemu_add_vm_change_state_handler(virtio_blk_dma_restart_cb, s);
    s->qdev = dev;
    register_savevm(dev, "virtio-blk", virtio_blk_id++, 2,
                    virtio_blk_save, virtio_blk_load, s);
#ifdef CONFIG_VIRTIO_BLK_DATA_PLANE
    if (s->dataplane) {
        register_device_unmigratable(dev, "virtio-blk", s);
    }
#endif
    bdrv_set_dev_ops(s->bs, &virtio_block_ops, s);
    s->bs->buffer_alignment = s->conf->logical_block_size;

    bdrv_iostatus_enable(s->bs);
    add_boot_device_path(s->conf->bootindex, dev, "/disk@0,0");

    return &s->vdev;
}

void virtio_blk_exit(VirtIODevice *vdev)
{
    VirtIOBlock *s = to_virtio_blk(vdev);

#ifdef CONFIG_VIRTIO_BLK_DATA_PLANE
    virtio_blk_data_plane_destroy(s->dataplane);
    s->dataplane = NULL;
#endif
    qemu_del_vm_change_state_handler(s->change);
    unregister_savevm(s->qdev, "virtio-blk", s);
    virtio_cleanup(vdev);
}
