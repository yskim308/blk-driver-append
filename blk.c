#include "linux/blk_types.h"
#include "linux/bvec.h"
#include "linux/highmem-internal.h"
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

static struct ramblk_dev *ramblk;

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

module_init(ramblk_init);
module_exit(ramblk_exit);

MODULE_LICENSE("GPL");
