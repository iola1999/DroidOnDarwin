/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/page_reporting.h>
#include <asm/host_ops.h>

static int report_free_pages(struct page_reporting_dev_info *device,
                             struct scatterlist *pages, unsigned int count)
{
    struct scatterlist *entry;
    unsigned int index;
    /* page_reporting isolates these buddy blocks until the callback returns. */
    for_each_sg(pages, entry, count, index) {
        unsigned long offset = (unsigned long)sg_virt(entry) - memory_start;
        int error = lkl_ops->shmem_reclaim(offset, entry->length);
        if (error) return error;
    }
    return 0;
}

static struct page_reporting_dev_info reporter = {
    .report = report_free_pages,
    .order = 4,
};

static int __init nce_reclaim_init(void)
{
    return lkl_ops->shmem_reclaim ? page_reporting_register(&reporter) : 0;
}
device_initcall(nce_reclaim_init);
