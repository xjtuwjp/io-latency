#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kallsyms.h>

#include <linux/blkdev.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/time.h>
#include <scsi/scsi_device.h>

#include "hotfixes.h"
#include "hash_table.h"
#include "latency_stats.h"

#define HOTFIX_GET_REQUEST	0
#define HOTFIX_PEEK_REQUEST	1
#define HOTFIX_FINISH_REQUEST	2

#define MAX_REQUEST_QUEUE	100
#define MAX_REQUESTS		10000

/* /proc/io-latency/sda/
 * /proc/io-latency/sda/io_latency_s
 * /proc/io-latency/sda/io_latency_ms
 * /proc/io-latency/sda/io_latency_us
 * /proc/io-latency/sda/soft_io_latency_s
 * /proc/io-latency/sda/soft_io_latency_ms
 * /proc/io-latency/sda/soft_io_latency_us
 * /proc/io-latency/sda/io_latency_reset
 */
#define NR_PROC_TYPE		8

static struct proc_dir_entry *proc_io_latency;
static struct class *sd_disk_class;
static struct hash_table *request_queue_table;

struct proc_entry_name {
	struct proc_dir_entry *entry;
	struct proc_dir_entry *parent;
	char name[64];
};

static struct proc_entry_name *dir_proc_list;
static int nr_dir_proc;

static void add_proc_node(const char *name, struct proc_dir_entry *node,
			struct proc_dir_entry *parent)
{
	dir_proc_list[nr_dir_proc].entry = node;
	dir_proc_list[nr_dir_proc].parent = parent;
	strncpy(dir_proc_list[nr_dir_proc].name, name, 64);
	nr_dir_proc++;
}

static void delete_procfs(void);

/* every request_queue has an instance of this struct */
struct request_queue_aux {
	struct latency_stats *lstats;
	struct hash_table *hash_table;
};
static struct kmem_cache *request_table_aux_cache;

static struct request* (*p_get_request_wait)(struct request_queue *q,
		int rw_flags, struct bio *bio);
static struct request* (*p_blk_peek_request)(struct request_queue *q);
static void (*p_blk_finish_request)(struct request *req, int error);

struct scsi_disk {
	struct scsi_driver *driver;	/* always &sd_template */
	struct scsi_device *device;
	struct device	dev;
	struct gendisk	*disk;
	atomic_t	openers;
	sector_t	capacity;	/* size in 512-byte sectors */
	u32		max_ws_blocks;
	u32		max_unmap_blocks;
	u32		unmap_granularity;
	u32		unmap_alignment;
	u32		index;
	unsigned int	physical_block_size;
	unsigned int	max_medium_access_timeouts;
	unsigned int	medium_access_timed_out;
	u8		media_present;
	u8		write_prot;
	u8		protection_type;/* Data Integrity Field */
	u8		provisioning_mode;
	unsigned	ATO : 1;	/* state of disk ATO bit */
	unsigned	cache_override : 1; /* temp override of WCE,RCD */
	unsigned	WCE : 1;	/* state of disk WCE bit */
	unsigned	RCD : 1;	/* state of disk RCD bit, unused */
	unsigned	DPOFUA : 1;	/* state of disk DPOFUA bit */
	unsigned	first_scan : 1;
	unsigned	lbpme : 1;
	unsigned	lbprz : 1;
	unsigned	lbpu : 1;
	unsigned	lbpws : 1;
	unsigned	lbpws10 : 1;
	unsigned	lbpvpd : 1;
	unsigned	ws10 : 1;
	unsigned	ws16 : 1;
};

static struct ali_sym_addr io_latency_sym_addr_list[] = {
	ALI_DEFINE_SYM_ADDR(get_request_wait),
	ALI_DEFINE_SYM_ADDR(blk_peek_request),
	ALI_DEFINE_SYM_ADDR(blk_finish_request),
	{},
};
static struct request *overwrite_get_request_wait(struct request_queue *q,
		int rw_flags, struct bio *bio);
static struct request *overwrite_blk_peek_request(struct request_queue *q);
static void overwrite_blk_finish_request(struct request *req, int error);

static struct ali_hotfix_desc io_latency_hotfix_list[] = {

	[HOTFIX_GET_REQUEST] = ALI_DEFINE_HOTFIX( \
			"block: get_request_wait", \
			"get_request_wait", \
			overwrite_get_request_wait),

	[HOTFIX_PEEK_REQUEST] = ALI_DEFINE_HOTFIX( \
			"block: blk_peek_request", \
			"blk_peek_request", \
			overwrite_blk_peek_request),

	[HOTFIX_FINISH_REQUEST] = ALI_DEFINE_HOTFIX( \
			"block: blk_finish_request", \
			"blk_finish_request", \
			overwrite_blk_finish_request),

	{},
};

static struct request *(*orig_get_request_wait)(struct request_queue *q,
		int rw_flags, struct bio *bio);
static struct request *overwrite_get_request_wait(struct request_queue *q,
		int rw_flags, struct bio *bio)
{
	struct request *req;
	struct hash_node *queue_nd;
	struct request_queue_aux *aux;
	ktime_t ts;

	orig_get_request_wait = ali_hotfix_orig_func(
			&io_latency_hotfix_list[HOTFIX_GET_REQUEST]);
	req = orig_get_request_wait(q, rw_flags, bio);
	if (!req || !req->q)
		goto out;

	queue_nd = hash_table_find(request_queue_table,
						(unsigned long)req->q);
	if (!queue_nd)
		goto out;

	aux = (struct request_queue_aux *)(queue_nd->value);
	if (!aux || !aux->hash_table || !aux->lstats)
		goto out;

	/* insert request to hash table */
	ts = ktime_get();
	hash_table_insert(aux->hash_table, (unsigned long)req,
				(unsigned long)ktime_to_us(ts));
out:
	return req;
}

static struct request *(*orig_blk_peek_request)(struct request_queue *q);
static struct request *overwrite_blk_peek_request(struct request_queue *q)
{
	struct request *req;
	struct hash_node *req_nd, *queue_nd;
	struct request_queue_aux *aux;
	unsigned long stime, now;

	orig_blk_peek_request = ali_hotfix_orig_func(
			&io_latency_hotfix_list[HOTFIX_PEEK_REQUEST]);
	req = orig_blk_peek_request(q);
	if (!req || !req->q)
		goto out;

	queue_nd = hash_table_find(request_queue_table,
						(unsigned long)req->q);
	if (!queue_nd)
		goto out;

	aux = (struct request_queue_aux *)(queue_nd->value);
	if (!aux || !aux->hash_table || !aux->lstats)
		goto out;

	/* find request in request hash table */
	req_nd = hash_table_find(aux->hash_table, (unsigned long)req);
	if (!req_nd)
		goto out;

	now = ktime_to_us(ktime_get());
	stime = req_nd->value;
	req_nd->value = now;
	update_latency_stats(aux->lstats, stime, now, 1);
	update_io_size_stats(aux->lstats, blk_rq_bytes(req));
out:
	return req;
}

static void (*orig_blk_finish_request)(struct request *req, int error);
static void overwrite_blk_finish_request(struct request *req, int error)
{
	struct hash_node *queue_nd;
	struct request_queue_aux *aux;
	unsigned long stime, now;
	int res;

	orig_blk_finish_request =
		ali_hotfix_orig_func(
			&io_latency_hotfix_list[HOTFIX_FINISH_REQUEST]);
	if (!req || !req->q)
		goto out;

	queue_nd = hash_table_find(request_queue_table,
						(unsigned long)(req->q));
	if (!queue_nd)
		goto out;

	aux = (struct request_queue_aux *)(queue_nd->value);
	if (!aux || !aux->hash_table || !aux->lstats)
		goto out;

	/* find and remove request in request hash table */
	res = hash_table_find_and_remove(aux->hash_table,
					(unsigned long)req, &stime);
	if (res)
		goto out;

	now = ktime_to_us(ktime_get());
	update_latency_stats(aux->lstats, stime, now, 0);
out:
	orig_blk_finish_request(req, error);
}

#define PROC_SHOW(_name, _unit, _nr, _grain, _member)			\
static void _name##_show(struct seq_file *seq,				\
				struct latency_stats *lstats)		\
{									\
	int slot_base = 0;						\
	int i;								\
									\
	for (i = 0; i < _nr; i++) {					\
		seq_printf(seq,						\
			"%d-%d(%s):%d\n",				\
			slot_base,					\
			slot_base + _grain - 1,				\
			_unit,						\
			atomic_read(&(lstats->_member[i])));		\
		slot_base += _grain;					\
	}								\
}

#define PROC_FOPS(_name) 						\
static int _name##_seq_show(struct seq_file *seq, void *v)		\
{									\
	struct request_queue *q = seq->private;				\
	struct request_queue_aux *aux;					\
	struct hash_node *nd;						\
									\
	nd = hash_table_find(request_queue_table, (unsigned long)q);	\
	if (!nd)							\
		seq_puts(seq, "none");					\
	else {								\
		aux = (struct request_queue_aux *)nd->value;		\
		_name##_show(seq, aux->lstats);				\
	}								\
	return 0;							\
}									\
									\
static const struct seq_operations _name##_seq_ops = {			\
	.start  = io_latency_seq_start,					\
	.next   = io_latency_seq_next,					\
	.stop   = io_latency_seq_stop,					\
	.show   = _name##_seq_show,					\
};									\
									\
static int proc_##_name##_open(struct inode *inode, struct file *file)	\
{									\
	int res;							\
	res = seq_open(file, &_name##_seq_ops);				\
	if (res == 0) {							\
		struct seq_file *m = file->private_data;		\
		m->private = PDE_DATA(inode);				\
	}								\
	return res;							\
}									\
									\
static const struct file_operations proc_##_name##_fops = {		\
	.owner		= THIS_MODULE,					\
	.open		= proc_##_name##_open,				\
	.read		= seq_read,					\
	.llseek		= seq_lseek,					\
	.release	= seq_release,					\
}

static void *PDE_DATA(const struct inode *inode)
{
	return container_of(inode, struct proc_inode, vfs_inode)->pde->data;
}

static void *io_latency_seq_start(struct seq_file *seq, loff_t *pos)
{
	return *pos ? NULL : SEQ_START_TOKEN;
}

static void *io_latency_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	return NULL;
}

static void io_latency_seq_stop(struct seq_file *seq, void *v)
{
}

#define KB (1024)
static void io_size_show(struct seq_file *seq,
				struct latency_stats *lstats)
{
	int slot_base = 0;
	int i;

	for (i = 0; i < IO_SIZE_STATS_NR; i++) {
		seq_printf(seq,
			"%d-%d(KB):%d\n",
			(slot_base / KB),
			(slot_base + IO_SIZE_STATS_GRAINSIZE - 1) / KB,
			atomic_read(&(lstats->io_size_stats[i])));
		slot_base += IO_SIZE_STATS_GRAINSIZE;
	}
}

PROC_SHOW(soft_io_latency_us, "us", IO_LATENCY_STATS_US_NR,
		IO_LATENCY_STATS_US_GRAINSIZE, soft_latency_stats_us);
PROC_SHOW(soft_io_latency_ms, "ms", IO_LATENCY_STATS_MS_NR,
		IO_LATENCY_STATS_MS_GRAINSIZE, soft_latency_stats_ms);
PROC_SHOW(soft_io_latency_s, "s", IO_LATENCY_STATS_S_NR,
		IO_LATENCY_STATS_S_GRAINSIZE, soft_latency_stats_s);
PROC_SHOW(io_latency_us, "us", IO_LATENCY_STATS_US_NR,
		IO_LATENCY_STATS_US_GRAINSIZE, latency_stats_us);
PROC_SHOW(io_latency_ms, "ms", IO_LATENCY_STATS_MS_NR,
		IO_LATENCY_STATS_MS_GRAINSIZE, latency_stats_ms);
PROC_SHOW(io_latency_s, "s", IO_LATENCY_STATS_S_NR,
		IO_LATENCY_STATS_S_GRAINSIZE, latency_stats_s);

PROC_FOPS(io_size);
PROC_FOPS(soft_io_latency_us);
PROC_FOPS(soft_io_latency_ms);
PROC_FOPS(soft_io_latency_s);
PROC_FOPS(io_latency_us);
PROC_FOPS(io_latency_ms);
PROC_FOPS(io_latency_s);

static int show_io_stats_reset(char *page, char **start, off_t offset,
					int count, int *eof, void *data)
{
	return snprintf(page, count, "0\n");
}

static int store_io_stats_reset(struct file *file, const char __user *buffer,
					unsigned long count, void *data)
{
	struct hash_node *nd;
	struct request_queue_aux *aux;
	int i;

	if (count <= 0)
		goto out;

	nd = hash_table_find(request_queue_table, (unsigned long)data);
	if (!nd)
		goto out;
	aux = (struct request_queue_aux *)nd->value;
	if (!aux)
		goto out;

	for (i = 0; i < IO_LATENCY_STATS_MS_NR; i++)
		atomic_set(&aux->lstats->latency_stats_us[i], 0);
	for (i = 0; i < IO_LATENCY_STATS_MS_NR; i++)
		atomic_set(&aux->lstats->latency_stats_ms[i], 0);
	for (i = 0; i < IO_LATENCY_STATS_S_NR; i++)
		atomic_set(&aux->lstats->latency_stats_s[i], 0);
	for (i = 0; i < IO_LATENCY_STATS_MS_NR; i++)
		atomic_set(&aux->lstats->soft_latency_stats_us[i], 0);
	for (i = 0; i < IO_LATENCY_STATS_MS_NR; i++)
		atomic_set(&aux->lstats->soft_latency_stats_ms[i], 0);
	for (i = 0; i < IO_LATENCY_STATS_S_NR; i++)
		atomic_set(&aux->lstats->soft_latency_stats_s[i], 0);
	for (i = 0; i < IO_SIZE_STATS_NR; i++)
		atomic_set(&aux->lstats->io_size_stats[i], 0);

out:
	return count;
}

static int create_procfs(void)
{
	struct class_dev_iter iter;
	struct device *dev;
	struct scsi_disk *sd;
	struct proc_dir_entry *proc_node, *proc_dir;
	struct latency_stats *lstats;
	struct hash_table *request_table = NULL;
	struct request_queue_aux *aux;
	char table_name[MAX_HASH_TABLE_NAME_LEN];

	proc_io_latency = proc_mkdir("io-latency", NULL);
	if (!proc_io_latency)
		goto err;

	dir_proc_list = kzalloc(sizeof(struct proc_entry_name) *
			MAX_REQUEST_QUEUE * NR_PROC_TYPE, GFP_KERNEL);
	if (!dir_proc_list)
		goto err;

	class_dev_iter_init(&iter, sd_disk_class, NULL, NULL);
	while ((dev = class_dev_iter_next(&iter))) {
		sd = container_of(dev, struct scsi_disk, dev);
		proc_dir = proc_mkdir(sd->disk->disk_name, proc_io_latency);
		if (!proc_dir)
			goto err;
		add_proc_node(sd->disk->disk_name, proc_dir, proc_io_latency);
		/* create io_latency_us */
		proc_node = proc_create_data("io_latency_us", S_IFREG, proc_dir,
					&proc_io_latency_us_fops,
					sd->device->request_queue);
		if (!proc_node)
			goto err;
		add_proc_node("io_latency_us", proc_node, proc_dir);
		/* create io_latency_ms */
		proc_node = proc_create_data("io_latency_ms", S_IFREG, proc_dir,
					&proc_io_latency_ms_fops,
					sd->device->request_queue);
		if (!proc_node)
			goto err;
		add_proc_node("io_latency_ms", proc_node, proc_dir);
		/* create io_latency_s */
		proc_node = proc_create_data("io_latency_s", S_IFREG, proc_dir,
					&proc_io_latency_s_fops,
					sd->device->request_queue);
		if (!proc_node)
			goto err;
		add_proc_node("io_latency_s", proc_node, proc_dir);
		/* create soft_io_latency_us */
		proc_node = proc_create_data("soft_io_latency_us", S_IFREG,
					proc_dir, &proc_soft_io_latency_us_fops,
					sd->device->request_queue);
		if (!proc_node)
			goto err;
		add_proc_node("soft_io_latency_us", proc_node, proc_dir);
		/* create soft_io_latency_ms */
		proc_node = proc_create_data("soft_io_latency_ms", S_IFREG,
					proc_dir, &proc_soft_io_latency_ms_fops,
					sd->device->request_queue);
		if (!proc_node)
			goto err;
		add_proc_node("soft_io_latency_ms", proc_node, proc_dir);
		/* create soft_io_latency_s */
		proc_node = proc_create_data("soft_io_latency_s", S_IFREG,
					proc_dir, &proc_soft_io_latency_s_fops,
					sd->device->request_queue);
		if (!proc_node)
			goto err;
		add_proc_node("soft_io_latency_s", proc_node, proc_dir);
		/* create io_size */
		proc_node = proc_create_data("io_size", S_IFREG, proc_dir,
					&proc_io_size_fops,
					sd->device->request_queue);
		if (!proc_node)
			goto err;
		add_proc_node("io_size", proc_node, proc_dir);
		/* create io_stats_reset */
		proc_node = proc_create_data("io_stats_reset", S_IFREG,
					proc_dir, NULL,
					sd->device->request_queue);
		if (!proc_node)
			goto err;
		proc_node->read_proc = show_io_stats_reset;
		proc_node->write_proc = store_io_stats_reset;
		add_proc_node("io_stats_reset", proc_node, proc_dir);

		lstats = create_latency_stats();
		if (!lstats)
			goto err;
		sprintf(table_name, "htable-%s", sd->disk->disk_name);
		request_table = create_hash_table(table_name, MAX_REQUESTS);
		if (!request_table) {
			destroy_latency_stats(lstats);
			goto err;
		}
		aux = (struct request_queue_aux *)kmem_cache_zalloc(
				request_table_aux_cache, GFP_KERNEL);
		if (!aux) {
			destroy_hash_table(request_table);
			destroy_latency_stats(lstats);
			goto err;
		}
		aux->lstats = lstats;
		aux->hash_table = request_table;
		hash_table_insert(request_queue_table,
				(unsigned long)(sd->device->request_queue),
				(unsigned long)aux);
	}
	class_dev_iter_exit(&iter);

	return 0;
err:
	delete_procfs();
	return -ENOMEM;
}

static int free_aux(struct hash_node *nd)
{
	struct request_queue_aux *aux = (struct request_queue_aux *)(nd->value);
	if (aux) {
		if (aux->hash_table)
			destroy_hash_table(aux->hash_table);
		if (aux->lstats)
			destroy_latency_stats(aux->lstats);
		kmem_cache_free(request_table_aux_cache, aux);
	}
	nd->value = 0;
	return 0;
}

static void delete_procfs(void)
{
	struct proc_dir_entry *proc_node;
	int i;

	if (dir_proc_list) {
		for (i = nr_dir_proc - 1; i >= 0; i--) {
			proc_node = dir_proc_list[i].entry;
			if (proc_node) {
				remove_proc_entry(dir_proc_list[i].name,
						dir_proc_list[i].parent);
			}
		}
		kfree(dir_proc_list);
		dir_proc_list = NULL;
		nr_dir_proc = 0;
	}
	if (proc_io_latency) {
		remove_proc_entry("io-latency", NULL);
		proc_io_latency = NULL;
	}
	if (request_queue_table)
		call_for_each_hash_node(request_queue_table,
				free_aux);
}

static int __init io_latency_init(void)
{
	int res;

	request_queue_table = create_hash_table("request-queue-table",
						MAX_REQUEST_QUEUE);
	if (!request_queue_table) {
		res = -ENOMEM;
		goto err;
	}

	request_table_aux_cache = kmem_cache_create("request-queue-aux",
					sizeof(struct request_queue_aux),
					0, 0, NULL);

	sd_disk_class = (struct class *)ali_get_symbol_address("sd_disk_class");
	if (!sd_disk_class) {
		res = -EINVAL;
		goto err;
	}

	res = init_latency_stats();
	if (res)
		goto err;

	/* create /proc/io-latency/ */
	res = create_procfs();
	if (res) {
		exit_latency_stats();
		goto err;
	}

	if (ali_get_symbol_address_list(io_latency_sym_addr_list, &res)) {
		printk(KERN_ERR "Can't get address of %s\n",
				io_latency_sym_addr_list[res].name);
		res = -EINVAL;
		goto hotfix_err;
	}

	res = ali_hotfix_register_list(io_latency_hotfix_list);
	if (res)
		goto hotfix_err;

	if (!ali_hotfix_orig_func(
			&io_latency_hotfix_list[HOTFIX_FINISH_REQUEST])) {
		printk(KERN_ERR "Register fail\n");
		ali_hotfix_unregister_list(io_latency_hotfix_list);
		res = -ENODEV;
		goto hotfix_err;
	}

	return 0;

hotfix_err:
	delete_procfs();
	exit_latency_stats();
err:
	return res;
}

static void __exit io_latency_exit(void)
{
	ali_hotfix_unregister_list(io_latency_hotfix_list);
	delete_procfs();
	exit_latency_stats();
	kmem_cache_destroy(request_table_aux_cache);
	destroy_hash_table(request_queue_table);
}

module_init(io_latency_init)
module_exit(io_latency_exit)
MODULE_AUTHOR("Robin Dong <sanbai@taobao.com>");
MODULE_DESCRIPTION("Collect statistics about disk io");
MODULE_LICENSE("GPL");
