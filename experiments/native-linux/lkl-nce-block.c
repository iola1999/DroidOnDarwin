/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/module.h>
#include <asm/host_ops.h>

static struct gendisk *nce_disks[2];
static int nce_major;

static void nce_submit_bio(struct bio *bio)
{
    struct bio_vec vector;
    struct bvec_iter iter;
    u64 offset = bio->bi_iter.bi_sector << SECTOR_SHIFT;
    unsigned int operation = bio_op(bio);
    unsigned int index = (unsigned long)bio->bi_bdev->bd_disk->private_data;
    int error = 0;

    if (operation == REQ_OP_FLUSH || bio->bi_opf & REQ_PREFLUSH)
        error = lkl_ops->disk_io(index, 2, 0, NULL, 0);
    if (error || operation == REQ_OP_FLUSH)
        goto complete;
    if (operation != REQ_OP_READ && operation != REQ_OP_WRITE) {
        bio->bi_status = BLK_STS_NOTSUPP;
        bio_endio(bio);
        return;
    }
    bio_for_each_segment(vector, bio, iter) {
        void *buffer = bvec_kmap_local(&vector);
        error = lkl_ops->disk_io(index, operation == REQ_OP_WRITE, offset, buffer, vector.bv_len);
        kunmap_local(buffer);
        if (error)
            goto complete;
        offset += vector.bv_len;
    }
    if (bio->bi_opf & REQ_FUA)
        error = lkl_ops->disk_io(index, 2, 0, NULL, 0);
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
    int error = 0;
    if (!lkl_ops->disk_size || !lkl_ops->disk_io)
        return 0;
    nce_major = register_blkdev(0, "ncedisk");
    if (nce_major < 0)
        return nce_major;
    for (unsigned int index = 0; index < ARRAY_SIZE(nce_disks); ++index) {
        u64 bytes = lkl_ops->disk_size(index);
        struct gendisk *disk;
        if (!bytes) continue;
        if (bytes & 511) { error = -EINVAL; goto fail; }
        disk = blk_alloc_disk(&limits, NUMA_NO_NODE);
        if (IS_ERR(disk)) { error = PTR_ERR(disk); goto fail; }
        disk->major = nce_major;
        disk->first_minor = index;
        disk->minors = 1;
        disk->flags = GENHD_FL_NO_PART;
        disk->fops = &nce_block_ops;
        disk->private_data = (void *)(unsigned long)index;
        snprintf(disk->disk_name, DISK_NAME_LEN, "ncedisk%u", index);
        set_capacity(disk, bytes >> SECTOR_SHIFT);
        set_disk_ro(disk, index == 1);
        error = add_disk(disk);
        if (error) { put_disk(disk); goto fail; }
        nce_disks[index] = disk;
    }
    return 0;
fail:
    for (unsigned int index = 0; index < ARRAY_SIZE(nce_disks); ++index) {
        if (!nce_disks[index]) continue;
        del_gendisk(nce_disks[index]);
        put_disk(nce_disks[index]);
        nce_disks[index] = NULL;
    }
    unregister_blkdev(nce_major, "ncedisk");
    return error;
}
device_initcall(nce_block_init);
