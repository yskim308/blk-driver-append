#include "linux/blk_types.h"
#include "linux/blkdev.h"
#include "linux/bvec.h"
#include "linux/highmem-internal.h"
#include "linux/slab.h"
#include <linux/blk-mq.h>
#include <linux/init.h> /* Needed for the macros */
#include <linux/module.h> /* Needed by all modules */
#include <linux/printk.h> /* Needed for pr_info() */
#include <linux/vmalloc.h>
#include <linux/bio.h>

struct ramblk_dev {
	u8 *data;
	struct gendisk *disk;
};

const int TOTAL_BYTES = 16 * 1024 * 1024;
static struct ramblk_dev *ramblk;
static int ramblk_major;

static bool ramblk_rw_bvec(struct ramblk_dev *ramblk, struct bio *bio)
{
	struct bio_vec bv = bio_iter_iovec(bio, bio->bi_iter);
	sector_t sector = bio->bi_iter.bi_sector;

	void *kaddr;
	u8 *target_memory;
	target_memory = ramblk->data + (sector << SECTOR_SHIFT);

	kaddr = bvec_kmap_local(&bv);
	if (bio_data_dir(bio) == WRITE) {
		memcpy(target_memory, kaddr, bv.bv_len);
	} else {
		memcpy(kaddr, target_memory, bv.bv_len);
	}
	kunmap_local(kaddr);
	bio_advance_iter_single(bio, &bio->bi_iter, bv.bv_len);

	return true;
}

static void ramblk_submit_bio(struct bio *bio)
{
	struct ramblk_dev *ramblk = bio->bi_bdev->bd_disk->private_data;

	do {
		if (!ramblk_rw_bvec(ramblk, bio)) {
			return;
		}
	} while (bio->bi_iter.bi_size);

	bio_endio(bio);
}

static const struct block_device_operations ramblk_fops = {
	.owner = THIS_MODULE,
	.submit_bio = ramblk_submit_bio,
};

static int __init ramblk_init(void)
{
	struct queue_limits lim = {
		.logical_block_size = 512,
		.physical_block_size = 512,
		.features = BLK_FEAT_SYNCHRONOUS,
	};
	int err;

	ramblk_major = register_blkdev(0, "ramblk");
	if (ramblk_major < 0) {
		return ramblk_major;
	}

	ramblk = kzalloc(sizeof(*ramblk), GFP_KERNEL);
	if (!ramblk) {
		unregister_blkdev(ramblk_major, "ramblk");
		return -ENOMEM;
	}

	ramblk->data = vmalloc(TOTAL_BYTES);
	if (!ramblk->data) {
		err = -ENOMEM;
		goto out_free_dev;
	}

	ramblk->disk = blk_alloc_disk(&lim, NUMA_NO_NODE);
	if (IS_ERR(ramblk->disk)) {
		err = PTR_ERR(ramblk->disk);
		goto out_free_vmalloc;
	}

	ramblk->disk->major = ramblk_major;
	ramblk->disk->first_minor = 0;
	ramblk->disk->minors = 1;
	ramblk->disk->fops = &ramblk_fops;
	ramblk->disk->private_data = ramblk;
	strscpy(ramblk->disk->disk_name, "ramblk0", DISK_NAME_LEN);

	set_capacity(ramblk->disk, 32768);

	err = add_disk(ramblk->disk);
	if (err) {
		goto out_cleanup_disk;
	}

	pr_info("ramblk: module loaded\n");
	return 0;
out_cleanup_disk:
	put_disk(ramblk->disk);
out_free_vmalloc:
	vfree(ramblk->data);
out_free_dev:
	kfree(ramblk);
	unregister_blkdev(ramblk_major, "ramblk");
	return err;
}

static void __exit ramblk_exit(void)
{
	if (ramblk) {
		if (ramblk->disk) {
			del_gendisk(ramblk->disk);
			put_disk(ramblk->disk);
		}

		if (ramblk->data) {
			vfree(ramblk->data);
		}

		kfree(ramblk);
	}

	unregister_blkdev(ramblk_major, "ramblk");
}

module_init(ramblk_init);
module_exit(ramblk_exit);

MODULE_LICENSE("GPL");
