#include <linux/blk_types.h>
#include <linux/blkdev.h>
#include <linux/bvec.h>
#include <linux/highmem-internal.h>
#include <linux/slab.h>
#include <linux/blk-mq.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/vmalloc.h>
#include <linux/bio.h>
#include <linux/xarray.h>
#include <linux/rcupdate.h> /* Required for rcu_read_lock */

struct ramblk_dev {
	u8 *data;
	struct gendisk *disk;
	struct xarray l2p_table;
	u32 next_free_sector;
};

const int TOTAL_BYTES = 16 * 1024 * 1024;
const u32 MAX_SECTORS = (16 * 1024 * 1024) >> 9;

static struct ramblk_dev *ramblk;
static int ramblk_major;

static bool ramblk_rw_bvec(struct ramblk_dev *ramblk, struct bio *bio)
{
	struct bio_vec bv = bio_iter_iovec(bio, bio->bi_iter);
	sector_t logical_sector = bio->bi_iter.bi_sector;
	u32 num_sectors = bv.bv_len >> SECTOR_SHIFT;

	void *kaddr;
	u8 *target_memory = NULL;

	kaddr = bvec_kmap_local(&bv);

	if (bio_data_dir(bio) == WRITE) {
		u32 physical_sector;

		xa_lock(&ramblk->l2p_table);

		if (ramblk->next_free_sector + num_sectors > MAX_SECTORS) {
			kunmap_local(kaddr);
			pr_err("ramblk: Out of physical space (No GC implemented)\n");
			bio->bi_status = BLK_STS_NOSPC;
			xa_unlock(&ramblk->l2p_table);
			return false;
		}

		physical_sector = ramblk->next_free_sector;

		target_memory =
			ramblk->data + (physical_sector << SECTOR_SHIFT);
		memcpy(target_memory, kaddr, bv.bv_len);

		for (u32 i = 0; i < num_sectors; i++) {
			u8 *sector_ptr = target_memory + (i << SECTOR_SHIFT);
			sector_t cur_log_sec = logical_sector + i;

			if (xa_err(__xa_store(&ramblk->l2p_table, cur_log_sec,
					      sector_ptr, GFP_ATOMIC))) {
				kunmap_local(kaddr);
				bio->bi_status = BLK_STS_RESOURCE;
				xa_unlock(&ramblk->l2p_table);
				return false;
			}
		}

		ramblk->next_free_sector += num_sectors;
		xa_unlock(&ramblk->l2p_table);

	} else {
		rcu_read_lock();

		for (u32 i = 0; i < num_sectors; i++) {
			sector_t cur_log_sec = logical_sector + i;
			u8 *buffer_offset = (u8 *)kaddr + (i << SECTOR_SHIFT);

			target_memory =
				xa_load(&ramblk->l2p_table, cur_log_sec);

			if (target_memory) {
				memcpy(buffer_offset, target_memory, 512);
			} else {
				memset(buffer_offset, 0, 512);
			}
		}

		rcu_read_unlock();
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
			bio_endio(bio);
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

	xa_init(&ramblk->l2p_table);
	ramblk->next_free_sector = 0;

	ramblk->disk = blk_alloc_disk(&lim, NUMA_NO_NODE);
	if (IS_ERR(ramblk->disk)) {
		err = PTR_ERR(ramblk->disk);
		goto out_destroy_xa;
	}

	ramblk->disk->major = ramblk_major;
	ramblk->disk->first_minor = 0;
	ramblk->disk->minors = 1;
	ramblk->disk->fops = &ramblk_fops;
	ramblk->disk->private_data = ramblk;
	strscpy(ramblk->disk->disk_name, "ramblk0", DISK_NAME_LEN);

	set_capacity(ramblk->disk, MAX_SECTORS);

	err = add_disk(ramblk->disk);
	if (err) {
		goto out_cleanup_disk;
	}

	pr_info("ramblk: append-only module loaded (RCU protected)\n");
	return 0;

out_cleanup_disk:
	put_disk(ramblk->disk);
out_destroy_xa:
	xa_destroy(&ramblk->l2p_table);
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

		xa_destroy(&ramblk->l2p_table);

		if (ramblk->data) {
			vfree(ramblk->data);
		}

		kfree(ramblk);
	}

	unregister_blkdev(ramblk_major, "ramblk");
	pr_info("ramblk: module unloaded\n");
}

module_init(ramblk_init);
module_exit(ramblk_exit);

MODULE_LICENSE("GPL");
