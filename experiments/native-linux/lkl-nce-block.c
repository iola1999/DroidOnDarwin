/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/module.h>
#include <asm/host_ops.h>

static struct gendisk *nce_disk;
static int nce_major;

static void nce_submit_bio(struct bio *bio)
{
    struct bio_vec vector;
    struct bvec_iter iter;
    u64 offset = bio->bi_iter.bi_sector << SECTOR_SHIFT;
    unsigned int operation = bio_op(bio);
    int error = 0;

    if (operation == REQ_OP_FLUSH || bio->bi_opf & REQ_PREFLUSH)
        error = lkl_ops->disk_io(2, 0, NULL, 0);
    if (error || operation == REQ_OP_FLUSH)
        goto complete;
    if (operation != REQ_OP_READ && operation != REQ_OP_WRITE) {
        bio->bi_status = BLK_STS_NOTSUPP;
        bio_endio(bio);
        return;
    }
    bio_for_each_segment(vector, bio, iter) {
        void *buffer = bvec_kmap_local(&vector);
        error = lkl_ops->disk_io(operation == REQ_OP_WRITE, offset, buffer, vector.bv_len);
        kunmap_local(buffer);
        if (error)
            goto complete;
        offset += vector.bv_len;
    }
    if (bio->bi_opf & REQ_FUA)
        error = lkl_ops->disk_io(2, 0, NULL, 0);
complete:
    if (error)
        bio->bi_status = BLK_STS_IOERR;
    bio_endio(bio);
}

static const struct block_device_operations nce_block_ops = {
    .owner = THIS_MODULE,
    .submit_bio = nce_submit_bio,
};

static int __init nce_block_init(void)
{
    struct queue_limits limits = {
        .logical_block_size = 512,
        .physical_block_size = PAGE_SIZE,
        .features = BLK_FEAT_SYNCHRONOUS | BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA,
    };
    u64 bytes = lkl_ops->disk_size ? lkl_ops->disk_size() : 0;
    int error;
    if (!bytes)
        return 0;
    if (!lkl_ops->disk_io || bytes & 511)
        return -EINVAL;
    nce_major = register_blkdev(0, "ncedisk");
    if (nce_major < 0)
        return nce_major;
    nce_disk = blk_alloc_disk(&limits, NUMA_NO_NODE);
    if (IS_ERR(nce_disk)) {
        error = PTR_ERR(nce_disk);
        goto unregister;
    }
    nce_disk->major = nce_major;
    nce_disk->minors = 1;
    nce_disk->flags = GENHD_FL_NO_PART;
    nce_disk->fops = &nce_block_ops;
    strscpy(nce_disk->disk_name, "ncedisk", DISK_NAME_LEN);
    set_capacity(nce_disk, bytes >> SECTOR_SHIFT);
    error = add_disk(nce_disk);
    if (!error)
        return 0;
    put_disk(nce_disk);
unregister:
    unregister_blkdev(nce_major, "ncedisk");
    return error;
}
device_initcall(nce_block_init);
