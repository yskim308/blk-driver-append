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
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/fs.h>

struct stale_block {
	u32 physical_sector;
	struct list_head list;
};

struct ramblk_dev {
	u8 *data;
	struct stale_block *stale_pool;
	struct gendisk *disk;
	struct xarray l2p_table;
	u32 next_free_sector;
	struct list_head stale_list;
	spinlock_t lock;
};

const int TOTAL_BYTES = 16 * 1024 * 1024;
const u32 MAX_SECTORS = (16 * 1024 * 1024) >> 9;

#define RAMBLK_SAVE_PATH "/var/lib/ramblk.bin"

static struct ramblk_dev *ramblk;
static int ramblk_major;

static int alloc_physical_sector(struct ramblk_dev *ramblk,
				 u32 *physical_sector)
{
	struct stale_block *blk;
	lockdep_assert_held(&ramblk->lock);

	if (!list_empty(&ramblk->stale_list)) {
		blk = list_first_entry(&ramblk->stale_list, struct stale_block,
				       list);

		list_del_init(&blk->list);
		*physical_sector = blk->physical_sector;
		return 0;
	}

	if (ramblk->next_free_sector >= MAX_SECTORS)
		return -ENOSPC;

	*physical_sector = ramblk->next_free_sector++;

	return 0;
}

static void add_stale_sector(struct ramblk_dev *ramblk, u32 physical_sector)
{
	lockdep_assert_held(&ramblk->lock);
	list_add_tail(&ramblk->stale_pool[physical_sector].list,
		      &ramblk->stale_list);
}

static bool ramblk_rw_bvec(struct ramblk_dev *ramblk, struct bio *bio)
{
	struct bio_vec bv = bio_iter_iovec(bio, bio->bi_iter);

	sector_t logical_sector = bio->bi_iter.bi_sector;

	u32 num_sectors;
	void *kaddr;

	if (bv.bv_len & (SECTOR_SIZE - 1)) {
		bio->bi_status = BLK_STS_IOERR;
		return false;
	}

	num_sectors = bv.bv_len >> SECTOR_SHIFT;

	kaddr = bvec_kmap_local(&bv);

	if (bio_data_dir(bio) == WRITE) {
		spin_lock(&ramblk->lock);
		for (u32 i = 0; i < num_sectors; i++) {
			sector_t cur_log_sec = logical_sector + i;

			u32 new_phys;
			void *old_entry;

			u8 *src;
			u8 *dst;

			if (alloc_physical_sector(ramblk, &new_phys)) {
				spin_unlock(&ramblk->lock);

				kunmap_local(kaddr);
				pr_err("ramblk: out of space\n");
				bio->bi_status = BLK_STS_NOSPC;

				return false;
			}

			old_entry = xa_load(&ramblk->l2p_table, cur_log_sec);

			if (old_entry) {
				u32 old_phys = xa_to_value(old_entry);

				add_stale_sector(ramblk, old_phys);
			}

			src = (u8 *)kaddr + (i << SECTOR_SHIFT);
			dst = ramblk->data + (new_phys << SECTOR_SHIFT);

			memcpy(dst, src, SECTOR_SIZE);

			if (xa_err(xa_store(&ramblk->l2p_table, cur_log_sec,
					    xa_mk_value(new_phys),
					    GFP_ATOMIC))) {
				add_stale_sector(ramblk, new_phys);
				spin_unlock(&ramblk->lock);
				kunmap_local(kaddr);
				bio->bi_status = BLK_STS_RESOURCE;

				return false;
			}
		}
		spin_unlock(&ramblk->lock);
	} else {
		spin_lock(&ramblk->lock);

		for (u32 i = 0; i < num_sectors; i++) {
			sector_t cur_log_sec = logical_sector + i;
			u8 *dst = (u8 *)kaddr + (i << SECTOR_SHIFT);
			void *entry;

			entry = xa_load(&ramblk->l2p_table, cur_log_sec);

			if (entry) {
				u32 phys = xa_to_value(entry);
				u8 *src = ramblk->data + (phys << SECTOR_SHIFT);
				memcpy(dst, src, SECTOR_SIZE);
			} else {
				memset(dst, 0, SECTOR_SIZE);
			}
		}

		spin_unlock(&ramblk->lock);
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

static void ramblk_save(struct ramblk_dev *ramblk)
{
	struct file *f;
	loff_t pos = 0;
	u32 l2p_entry;

	f = filp_open(RAMBLK_SAVE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (IS_ERR(f))
		return;

	kernel_write(f, &ramblk->next_free_sector,
		     sizeof(ramblk->next_free_sector), &pos);

	for (u32 i = 0; i < MAX_SECTORS; i++) {
		void *entry = xa_load(&ramblk->l2p_table, i);

		l2p_entry = entry ? xa_to_value(entry) : U32_MAX;
		kernel_write(f, &l2p_entry, sizeof(l2p_entry), &pos);
	}

	kernel_write(f, ramblk->data, TOTAL_BYTES, &pos);

	filp_close(f, NULL);
}

static void ramblk_load(struct ramblk_dev *ramblk)
{
	struct file *f;
	loff_t pos = 0;
	u32 l2p_entry;

	f = filp_open(RAMBLK_SAVE_PATH, O_RDONLY, 0);
	if (IS_ERR(f))
		return;

	kernel_read(f, &ramblk->next_free_sector,
		    sizeof(ramblk->next_free_sector), &pos);

	for (u32 i = 0; i < MAX_SECTORS; i++) {
		kernel_read(f, &l2p_entry, sizeof(l2p_entry), &pos);
		if (l2p_entry != U32_MAX)
			xa_store(&ramblk->l2p_table, i, xa_mk_value(l2p_entry),
				 GFP_KERNEL);
	}

	kernel_read(f, ramblk->data, TOTAL_BYTES, &pos);

	filp_close(f, NULL);
}

static int __init ramblk_init(void)
{
	struct queue_limits lim = {
		.logical_block_size = 512,
		.physical_block_size = 512,
		.features = BLK_FEAT_SYNCHRONOUS,
	};

	int err;

	ramblk_major = register_blkdev(0, "ramblk");
	if (ramblk_major < 0)
		return ramblk_major;

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

	ramblk->stale_pool =
		vmalloc(array_size(MAX_SECTORS, sizeof(struct stale_block)));
	if (!ramblk->stale_pool) {
		err = -ENOMEM;
		goto out_free_data;
	}

	for (u32 i = 0; i < MAX_SECTORS; i++) {
		ramblk->stale_pool[i].physical_sector = i;
		INIT_LIST_HEAD(&ramblk->stale_pool[i].list);
	}

	xa_init(&ramblk->l2p_table);

	ramblk->next_free_sector = 0;

	INIT_LIST_HEAD(&ramblk->stale_list);

	spin_lock_init(&ramblk->lock);

	ramblk_load(ramblk);

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
	if (err)
		goto out_cleanup_disk;

	pr_info("ramblk: loaded\n");

	return 0;

out_cleanup_disk:
	put_disk(ramblk->disk);
out_destroy_xa:
	xa_destroy(&ramblk->l2p_table);
	vfree(ramblk->stale_pool);
out_free_data:
	vfree(ramblk->data);
out_free_dev:
	kfree(ramblk);
	unregister_blkdev(ramblk_major, "ramblk");
	return err;
}

static void __exit ramblk_exit(void)
{
	if (!ramblk)
		return;

	if (ramblk->disk) {
		del_gendisk(ramblk->disk);
		put_disk(ramblk->disk);
	}

	ramblk_save(ramblk);

	xa_destroy(&ramblk->l2p_table);

	vfree(ramblk->stale_pool);

	if (ramblk->data)
		vfree(ramblk->data);

	kfree(ramblk);

	unregister_blkdev(ramblk_major, "ramblk");

	pr_info("ramblk: unloaded\n");
}

module_init(ramblk_init);
module_exit(ramblk_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Append-only RAM block device with stale block reuse");
