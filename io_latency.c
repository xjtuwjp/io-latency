#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kallsyms.h>

#include <linux/blkdev.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/time.h>
#include <linux/async.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_cmnd.h>

#include "hotfixes.h"
#include "hash_table.h"
#include "latency_stats.h"
#include "config.h"

#define IO_LATENCY_VERSION	"1.1.3"

#define HOTFIX_GET_REQUEST	0
#define HOTFIX_SCSI_DISPATCH	1
#define HOTFIX_FINISH_REQUEST	2
#define HOTFIX_SD_PROBE_ASYNC	3

#define MAX_REQUEST_QUEUE	97
#define MAX_REQUESTS		9973

#ifdef USE_HASH_TABLE
#define this_cpu_ptr(ptr) per_cpu_ptr(ptr, smp_processor_id())
#endif

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

/* every request_queue has an instance of this struct */
struct request_queue_aux {
	struct latency_stats __percpu *lstats;
#ifdef USE_HASH_TABLE
	struct hash_table *hash_table;
#endif
	short enable_latency;
	short enable_soft_latency;
};
static struct kmem_cache *request_table_aux_cache;

static struct request* (*p_get_request_wait)(struct request_queue *q,
		int rw_flags, struct bio *bio);
static struct request* (*p_scsi_dispatch_cmd)(struct request_queue *q);
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

static struct request_queue_aux *insert_aux(struct scsi_disk *sd);
static int insert_procfs(struct scsi_disk *sd);

static struct ali_sym_addr io_latency_sym_addr_list[] = {
	ALI_DEFINE_SYM_ADDR(get_request_wait),
	ALI_DEFINE_SYM_ADDR(scsi_dispatch_cmd),
	ALI_DEFINE_SYM_ADDR(blk_finish_request),
	{},
};
static struct request *overwrite_get_request_wait(struct request_queue *q,
		int rw_flags, struct bio *bio);
static int overwrite_scsi_dispatch_cmd(struct scsi_cmnd *cmd);
static void overwrite_blk_finish_request(struct request *req, int error);
static void overwrite_sd_probe_async(void *data, async_cookie_t cookie);

static struct ali_hotfix_desc io_latency_hotfix_list[] = {

	[HOTFIX_GET_REQUEST] = ALI_DEFINE_HOTFIX( \
			"block: get_request_wait", \
			"get_request_wait", \
			overwrite_get_request_wait),

	[HOTFIX_SCSI_DISPATCH] = ALI_DEFINE_HOTFIX( \
			"block: scsi_dispatch_cmd", \
			"scsi_dispatch_cmd", \
			overwrite_scsi_dispatch_cmd),

	[HOTFIX_FINISH_REQUEST] = ALI_DEFINE_HOTFIX( \
			"block: blk_finish_request", \
			"blk_finish_request", \
			overwrite_blk_finish_request),

	[HOTFIX_SD_PROBE_ASYNC] = ALI_DEFINE_HOTFIX( \
			"scsi: sd_probe_async", \
			"sd_probe_async", \
			overwrite_sd_probe_async),
	{},
};

static void (*orig_sd_probe_async)(void *data, async_cookie_t cookie);
static void overwrite_sd_probe_async(void *data, async_cookie_t cookie)
{
	struct scsi_disk *sdkp = data;
	struct hash_node *queue_nd;

	queue_nd = hash_table_find(request_queue_table,
			(unsigned long)sdkp->device->request_queue);
	if (!queue_nd) {
		insert_procfs(sdkp);
		insert_aux(sdkp);
	}
	orig_sd_probe_async = ali_hotfix_orig_func(
			&io_latency_hotfix_list[HOTFIX_GET_REQUEST]);
	orig_sd_probe_async(data, cookie);
}

static struct request *(*orig_get_request_wait)(struct request_queue *q,
		int rw_flags, struct bio *bio);
static struct request *overwrite_get_request_wait(struct request_queue *q,
		int rw_flags, struct bio *bio)
{
	struct request *req;
	struct request_queue_aux *aux;
	unsigned long now;
#ifdef USE_HASH_TABLE
	struct hash_node *queue_nd, *req_nd;
#endif

	orig_get_request_wait = ali_hotfix_orig_func(
			&io_latency_hotfix_list[HOTFIX_GET_REQUEST]);
	req = orig_get_request_wait(q, rw_flags, bio);
	if (!req || !req->q)
		goto out;

#ifdef USE_HASH_TABLE
	queue_nd = hash_table_find(request_queue_table, (unsigned long)req->q);
	if (!queue_nd)
		goto out;
	aux = (struct request_queue_aux *)(queue_nd->value);
	if (!aux->hash_table)
		goto out;
#else
	aux = (struct request_queue_aux *)q->pad;
#endif
	if (!aux || !aux->lstats)
		goto out;

	if (!aux->enable_latency && !aux->enable_soft_latency)
		goto out;

	/* put time into 'pad' now */
#ifdef USE_US
	now = ktime_to_us(ktime_get());
#else
	now = jiffies;
#endif

#ifdef USE_HASH_TABLE
	req_nd = hash_table_find(aux->hash_table, (unsigned long)req);
	if (req_nd)
		req_nd->value = now;
	else
		hash_table_insert(aux->hash_table, (unsigned long)req, now);
#else
	req->pad = (void *)now;
#endif
out:
	return req;
}

static int (*orig_scsi_dispatch_cmd)(struct scsi_cmnd *cmd);
static int overwrite_scsi_dispatch_cmd(struct scsi_cmnd *cmd)
{
	struct request *req;
	struct request_queue_aux *aux;
	unsigned long stime, now;
	int bytes;
#ifdef USE_HASH_TABLE
	struct hash_node *req_nd, *queue_nd;
#endif

	orig_scsi_dispatch_cmd = ali_hotfix_orig_func(
			&io_latency_hotfix_list[HOTFIX_SCSI_DISPATCH]);
	req = cmd->request;
	if (!req || !req->q)
		goto out;

#ifdef USE_HASH_TABLE
	queue_nd = hash_table_find(request_queue_table, (unsigned long)req->q);
	if (!queue_nd)
		goto out;

	aux = (struct request_queue_aux *)(queue_nd->value);
	if (!aux->hash_table)
		goto out;
#else
	if (!req->pad)
		goto out;

	aux = (struct request_queue_aux *)req->q->pad;
#endif
	if (!aux || !aux->lstats)
		goto out;

	bytes = blk_rq_bytes(req);
	if (bytes <= 0) {
#ifdef USE_HASH_TABLE
		hash_table_remove(aux->hash_table, (unsigned long)req);
#endif
		goto out;
	}

#ifdef USE_US
	now = ktime_to_us(ktime_get());
#else
	now = jiffies;
#endif

#ifdef USE_HASH_TABLE
	/* find request in request hash table */
	req_nd = hash_table_find(aux->hash_table, (unsigned long)req);
	if (!req_nd)
		goto out;

	stime = req_nd->value;
	req_nd->value = now;
	if (aux->enable_soft_latency)
		update_latency_stats(this_cpu_ptr(aux->lstats),
				stime, now, 1, rq_data_dir(req));
	if (aux->enable_latency)
		update_io_size_stats(this_cpu_ptr(aux->lstats),
				blk_rq_bytes(req), rq_data_dir(req));

#else
	if (aux->enable_soft_latency) {
		stime = (unsigned long)req->pad;
		update_latency_stats(this_cpu_ptr(aux->lstats),
				stime, now, 1, rq_data_dir(req));
	}
	if (aux->enable_latency) {
		req->pad = (void *)now;
		update_io_size_stats(this_cpu_ptr(aux->lstats),
					bytes, rq_data_dir(req));
	}
#endif
out:
	return orig_scsi_dispatch_cmd(cmd);
}

static void (*orig_blk_finish_request)(struct request *req, int error);
static void overwrite_blk_finish_request(struct request *req, int error)
{
	struct request_queue_aux *aux;
	unsigned long stime, now;
#ifdef USE_HASH_TABLE
	struct hash_node *queue_nd, *req_nd;
#endif

	orig_blk_finish_request = ali_hotfix_orig_func(
			&io_latency_hotfix_list[HOTFIX_FINISH_REQUEST]);
	if (!req || !req->q)
		goto out;

#ifdef USE_HASH_TABLE
	queue_nd = hash_table_find(request_queue_table,
				(unsigned long)(req->q));
	if (!queue_nd)
		goto out;

	aux = (struct request_queue_aux *)(queue_nd->value);
	if (!aux->hash_table)
		goto out;
#else
	if (!req->pad)
		goto out;

	aux = (struct request_queue_aux *)req->q->pad;
#endif
	if (!aux || !aux->lstats)
		goto out;

	if (!aux->enable_latency)
		goto out;

#ifdef USE_US
	now = ktime_to_us(ktime_get());
#else
	now = jiffies;
#endif

#ifdef USE_HASH_TABLE
	/* find request in request hash table and update it's value */
	req_nd = hash_table_find(aux->hash_table, (unsigned long)req);
	if (!req_nd)
		goto out;

	stime = req_nd->value;
	update_latency_stats(this_cpu_ptr(aux->lstats),
				stime, now, 0, rq_data_dir(req));
#else
	stime = (unsigned long)req->pad;
	req->pad = NULL;
	update_latency_stats(this_cpu_ptr(aux->lstats),
				stime, now, 0, rq_data_dir(req));
#endif
out:
	orig_blk_finish_request(req, error);
}

inline struct request_queue_aux *get_aux(void *request_queue)
{
	struct request_queue_aux *aux = NULL;

#ifdef USE_HASH_TABLE
	struct hash_node *nd;

	if (!request_queue)
		return NULL;
	nd = hash_table_find(request_queue_table, (unsigned long)request_queue);
	if (!nd)
		return NULL;
	aux = (struct request_queue_aux *)nd->value;
#else
	aux = (struct request_queue_aux *)
		((struct request_queue *)request_queue)->pad;
#endif
	return aux;
}

#define PROC_SHOW(_name, _unit, _nr, _grain, _member)			\
static void _name##_show(struct seq_file *seq,				\
				struct latency_stats __percpu *lstats)	\
{									\
	int slot_base = 0;						\
	int i, cpu;							\
	unsigned long sum;						\
									\
	for (i = 0; i < _nr; i++) {					\
		sum = 0;						\
		for_each_possible_cpu(cpu)				\
			sum += per_cpu_ptr(lstats, cpu)->_member[i];	\
									\
		seq_printf(seq,						\
			"%d-%d(%s):%lu\n",				\
			slot_base,					\
			slot_base + _grain - 1,				\
			_unit,						\
			sum);						\
		slot_base += _grain;					\
	}								\
}

#define PROC_FOPS(_name) 						\
static int _name##_seq_show(struct seq_file *seq, void *v)		\
{									\
	struct request_queue *q = seq->private;				\
	struct request_queue_aux *aux;					\
									\
	if (!q)								\
		seq_puts(seq, "none");					\
	else {								\
		aux = get_aux(q);					\
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
				struct latency_stats __percpu *lstats)
{
	int slot_base = 0;
	int i, cpu;
	unsigned long sum;

	for (i = 0; i < IO_SIZE_STATS_NR; i++) {
		sum = 0;
		for_each_possible_cpu(cpu)
			sum += per_cpu_ptr(lstats, cpu)->io_size_stats[i];

		seq_printf(seq,
			"%d-%d(KB):%lu\n",
			(slot_base / KB),
			(slot_base + IO_SIZE_STATS_GRAINSIZE - 1) / KB,
			sum);
		slot_base += IO_SIZE_STATS_GRAINSIZE;
	}
}

static void io_read_size_show(struct seq_file *seq,
				struct latency_stats __percpu *lstats)
{
	int slot_base = 0;
	int i, cpu;
	unsigned long sum;

	for (i = 0; i < IO_SIZE_STATS_NR; i++) {
		sum = 0;
		for_each_possible_cpu(cpu)
			sum += per_cpu_ptr(lstats, cpu)->io_read_size_stats[i];

		seq_printf(seq,
			"%d-%d(KB):%lu\n",
			(slot_base / KB),
			(slot_base + IO_SIZE_STATS_GRAINSIZE - 1) / KB,
			sum);
		slot_base += IO_SIZE_STATS_GRAINSIZE;
	}
}

static void io_write_size_show(struct seq_file *seq,
				struct latency_stats __percpu *lstats)
{
	int slot_base = 0;
	int i, cpu;
	unsigned long sum;

	for (i = 0; i < IO_SIZE_STATS_NR; i++) {
		sum = 0;
		for_each_possible_cpu(cpu)
			sum += per_cpu_ptr(lstats, cpu)->io_write_size_stats[i];

		seq_printf(seq,
			"%d-%d(KB):%lu\n",
			(slot_base / KB),
			(slot_base + IO_SIZE_STATS_GRAINSIZE - 1) / KB,
			sum);
		slot_base += IO_SIZE_STATS_GRAINSIZE;
	}
}

PROC_SHOW(soft_io_latency_us, "us", IO_LATENCY_STATS_US_NR,
		IO_LATENCY_STATS_US_GRAINSIZE, soft_latency_stats_us);
PROC_SHOW(soft_io_latency_ms, "ms", IO_LATENCY_STATS_MS_NR,
		IO_LATENCY_STATS_MS_GRAINSIZE, soft_latency_stats_ms);
PROC_SHOW(soft_io_latency_s, "s", IO_LATENCY_STATS_S_NR,
		IO_LATENCY_STATS_S_GRAINSIZE, soft_latency_stats_s);

PROC_SHOW(soft_read_io_latency_us, "us", IO_LATENCY_STATS_US_NR,
		IO_LATENCY_STATS_US_GRAINSIZE, soft_latency_read_stats_us);
PROC_SHOW(soft_read_io_latency_ms, "ms", IO_LATENCY_STATS_MS_NR,
		IO_LATENCY_STATS_MS_GRAINSIZE, soft_latency_read_stats_ms);
PROC_SHOW(soft_read_io_latency_s, "s", IO_LATENCY_STATS_S_NR,
		IO_LATENCY_STATS_S_GRAINSIZE, soft_latency_read_stats_s);

PROC_SHOW(soft_write_io_latency_us, "us", IO_LATENCY_STATS_US_NR,
		IO_LATENCY_STATS_US_GRAINSIZE, soft_latency_write_stats_us);
PROC_SHOW(soft_write_io_latency_ms, "ms", IO_LATENCY_STATS_MS_NR,
		IO_LATENCY_STATS_MS_GRAINSIZE, soft_latency_write_stats_ms);
PROC_SHOW(soft_write_io_latency_s, "s", IO_LATENCY_STATS_S_NR,
		IO_LATENCY_STATS_S_GRAINSIZE, soft_latency_write_stats_s);

PROC_SHOW(io_latency_us, "us", IO_LATENCY_STATS_US_NR,
		IO_LATENCY_STATS_US_GRAINSIZE, latency_stats_us);
PROC_SHOW(io_latency_ms, "ms", IO_LATENCY_STATS_MS_NR,
		IO_LATENCY_STATS_MS_GRAINSIZE, latency_stats_ms);
PROC_SHOW(io_latency_s, "s", IO_LATENCY_STATS_S_NR,
		IO_LATENCY_STATS_S_GRAINSIZE, latency_stats_s);

PROC_SHOW(read_io_latency_us, "us", IO_LATENCY_STATS_US_NR,
		IO_LATENCY_STATS_US_GRAINSIZE, latency_read_stats_us);
PROC_SHOW(read_io_latency_ms, "ms", IO_LATENCY_STATS_MS_NR,
		IO_LATENCY_STATS_MS_GRAINSIZE, latency_read_stats_ms);
PROC_SHOW(read_io_latency_s, "s", IO_LATENCY_STATS_S_NR,
		IO_LATENCY_STATS_S_GRAINSIZE, latency_read_stats_s);

PROC_SHOW(write_io_latency_us, "us", IO_LATENCY_STATS_US_NR,
		IO_LATENCY_STATS_US_GRAINSIZE, latency_write_stats_us);
PROC_SHOW(write_io_latency_ms, "ms", IO_LATENCY_STATS_MS_NR,
		IO_LATENCY_STATS_MS_GRAINSIZE, latency_write_stats_ms);
PROC_SHOW(write_io_latency_s, "s", IO_LATENCY_STATS_S_NR,
		IO_LATENCY_STATS_S_GRAINSIZE, latency_write_stats_s);

PROC_FOPS(io_size);
PROC_FOPS(io_read_size);
PROC_FOPS(io_write_size);

PROC_FOPS(soft_io_latency_us);
PROC_FOPS(soft_io_latency_ms);
PROC_FOPS(soft_io_latency_s);
PROC_FOPS(soft_read_io_latency_us);
PROC_FOPS(soft_read_io_latency_ms);
PROC_FOPS(soft_read_io_latency_s);
PROC_FOPS(soft_write_io_latency_us);
PROC_FOPS(soft_write_io_latency_ms);
PROC_FOPS(soft_write_io_latency_s);

PROC_FOPS(io_latency_us);
PROC_FOPS(io_latency_ms);
PROC_FOPS(io_latency_s);
PROC_FOPS(read_io_latency_us);
PROC_FOPS(read_io_latency_ms);
PROC_FOPS(read_io_latency_s);
PROC_FOPS(write_io_latency_us);
PROC_FOPS(write_io_latency_ms);
PROC_FOPS(write_io_latency_s);

#define ENABLE_ATTR(_name)						\
static int show_##_name(char *page, char **start, off_t offset,		\
					int count, int *eof, void *data)\
{									\
	struct request_queue_aux *aux;					\
	int res = 0;							\
									\
	if (!data)							\
		goto out;						\
	aux = get_aux(data);						\
	if (!aux)							\
		goto out;						\
	if (aux->_name)							\
		res = snprintf(page, count, "1\n");			\
	else								\
		res = snprintf(page, count, "0\n");			\
out:									\
	return res;							\
}									\
									\
static int store_##_name(struct file *file, const char __user *buffer,	\
					unsigned long count, void *data)\
{									\
	struct request_queue_aux *aux;					\
	char *page = NULL;						\
									\
	if (count <= 0 || count > PAGE_SIZE)				\
		goto out;						\
	if (!data)							\
		goto out;						\
	aux = get_aux(data);						\
	if (!aux)							\
		goto out;						\
	page = (char *)__get_free_page(GFP_KERNEL);			\
	if (!page)							\
		goto out;						\
	if (copy_from_user(page, buffer, count))			\
		goto out;						\
	if (page[0] == '1')						\
		aux->_name = 1;						\
	else if (page[0] == '0')					\
		aux->_name = 0;						\
out:									\
	if (page)							\
		free_page((unsigned long)page);				\
	return count;							\
}

ENABLE_ATTR(enable_latency);
ENABLE_ATTR(enable_soft_latency);

static int show_io_stats_reset(char *page, char **start, off_t offset,
					int count, int *eof, void *data)
{
	return snprintf(page, count, "0\n");
}

static int store_io_stats_reset(struct file *file, const char __user *buffer,
					unsigned long count, void *data)
{
	struct request_queue_aux *aux;

	if (count <= 0)
		goto out;

	aux = get_aux(data);
	if (!aux)
		goto out;

	reset_latency_stats(aux->lstats);

out:
	return count;
}

struct io_latency_proc_node {
	char *name;
	const struct file_operations *fops;
};

static const struct io_latency_proc_node proc_node_list[] = {
	{ "io_latency_ms", &proc_io_latency_ms_fops},
	{ "io_latency_s", &proc_io_latency_s_fops},

	{ "read_io_latency_ms", &proc_read_io_latency_ms_fops},
	{ "read_io_latency_s", &proc_read_io_latency_s_fops},

	{ "write_io_latency_ms", &proc_write_io_latency_ms_fops},
	{ "write_io_latency_s", &proc_write_io_latency_s_fops},

	{ "soft_io_latency_ms", &proc_soft_io_latency_ms_fops},
	{ "soft_io_latency_s", &proc_soft_io_latency_s_fops},

	{ "soft_read_io_latency_ms", &proc_soft_read_io_latency_ms_fops},
	{ "soft_read_io_latency_s", &proc_soft_read_io_latency_s_fops},

	{ "soft_write_io_latency_ms", &proc_soft_write_io_latency_ms_fops},
	{ "soft_write_io_latency_s", &proc_soft_write_io_latency_s_fops},

	{ "io_size", &proc_io_size_fops},
	{ "io_read_size", &proc_io_read_size_fops},
	{ "io_write_size", &proc_io_write_size_fops},
#ifdef USE_US
	{ "io_latency_us", &proc_io_latency_us_fops},
	{ "read_io_latency_us", &proc_read_io_latency_us_fops},
	{ "write_io_latency_us", &proc_write_io_latency_us_fops},
	{ "soft_io_latency_us", &proc_soft_io_latency_us_fops},
	{ "soft_read_io_latency_us", &proc_soft_read_io_latency_us_fops},
	{ "soft_write_io_latency_us", &proc_soft_write_io_latency_us_fops},
#endif
};

#define PROC_NUM (sizeof(proc_node_list) / sizeof(struct io_latency_proc_node))
#define DIR_PROC_NUM (MAX_REQUEST_QUEUE * (PROC_NUM + 3))

static void add_proc_node(const char *name, struct proc_dir_entry *node,
			struct proc_dir_entry *parent)
{
	if (nr_dir_proc < DIR_PROC_NUM) {
		dir_proc_list[nr_dir_proc].entry = node;
		dir_proc_list[nr_dir_proc].parent = parent;
		strncpy(dir_proc_list[nr_dir_proc].name, name, 64);
		nr_dir_proc++;
	}
}

static void delete_procfs(void);

static int insert_procfs(struct scsi_disk *sd)
{
	struct proc_dir_entry *proc_node, *proc_dir;
	int i;

	proc_dir = proc_mkdir(sd->disk->disk_name, proc_io_latency);
	if (!proc_dir)
		goto err;
	add_proc_node(sd->disk->disk_name, proc_dir, proc_io_latency);

	for (i = 0; i < PROC_NUM; i++) {
		proc_node = proc_create_data(proc_node_list[i].name,
					S_IFREG, proc_dir,
					proc_node_list[i].fops,
					sd->device->request_queue);
		if (!proc_node)
			goto err;
		add_proc_node(proc_node_list[i].name, proc_node,
				proc_dir);
	}
	/* create io_stats_reset */
	proc_node = proc_create_data("io_stats_reset", S_IFREG,
				proc_dir, NULL,
				sd->device->request_queue);
	if (!proc_node)
		goto err;
	proc_node->read_proc = show_io_stats_reset;
	proc_node->write_proc = store_io_stats_reset;
	add_proc_node("io_stats_reset", proc_node, proc_dir);
	/* create enable_latency */
	proc_node = proc_create_data("enable_latency", S_IFREG,
				proc_dir, NULL,
				sd->device->request_queue);
	if (!proc_node)
		goto err;
	proc_node->read_proc = show_enable_latency;
	proc_node->write_proc = store_enable_latency;
	add_proc_node("enable_latency", proc_node, proc_dir);
	/* create enable_soft_latency */
	proc_node = proc_create_data("enable_soft_latency", S_IFREG,
				proc_dir, NULL,
				sd->device->request_queue);
	if (!proc_node)
		goto err;
	proc_node->read_proc = show_enable_soft_latency;
	proc_node->write_proc = store_enable_soft_latency;
	add_proc_node("enable_soft_latency", proc_node, proc_dir);
	return 0;
err:
	return -1;
}

static struct request_queue_aux *insert_aux(struct scsi_disk *sd)
{
	struct request_queue_aux *aux;
	struct latency_stats __percpu *lstats;
#ifdef USE_HASH_TABLE
	char table_name[MAX_HASH_TABLE_NAME_LEN];
	struct hash_table *request_table = NULL;
#endif

	lstats = create_latency_stats();
	if (!lstats)
		goto err;

#ifdef USE_HASH_TABLE
	sprintf(table_name, "htable-%s", sd->disk->sdev_gendev->disk_name);
	request_table = create_hash_table(table_name, MAX_REQUESTS);
	if (!request_table)
		goto err;
	aux = (struct request_queue_aux *)kmem_cache_zalloc(
			request_table_aux_cache, GFP_KERNEL);
	if (!aux) {
		destroy_hash_table(request_table);
		goto err;
	}
	aux->hash_table = request_table;
#else
	aux = (struct request_queue_aux *)kmem_cache_zalloc(
			request_table_aux_cache, GFP_KERNEL);
	if (!aux)
		goto err;
	sd->device->request_queue->pad = aux;
#endif
	aux->lstats = lstats;
	aux->enable_latency = 1;
	aux->enable_soft_latency = 1;
	hash_table_insert(request_queue_table,
			(unsigned long)(sd->device->request_queue),
			(unsigned long)aux);
	return aux;
err:
	if (lstats)
		destroy_latency_stats(lstats);
	return NULL;
}

static int create_procfs(void)
{
	struct class_dev_iter iter;
	struct device *dev;
	struct scsi_disk *sd;
	struct request_queue_aux *aux;

	proc_io_latency = proc_mkdir("io-latency", NULL);
	if (!proc_io_latency)
		goto err;

	/* proc_node in proc_node_list and
	 * 'io_stats_reset' 'enable_latency' 'enable_soft_latency'
	 */
	dir_proc_list = kzalloc(sizeof(struct proc_entry_name) * DIR_PROC_NUM,
			GFP_KERNEL);
	if (!dir_proc_list)
		goto err;

	class_dev_iter_init(&iter, sd_disk_class, NULL, NULL);
	while ((dev = class_dev_iter_next(&iter))) {
		sd = container_of(dev, struct scsi_disk, dev);
		if (insert_procfs(sd))
			goto err;
		aux = insert_aux(sd);
		if (!aux)
			goto err;
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
#ifdef USE_HASH_TABLE
		if (aux->hash_table)
			destroy_hash_table(aux->hash_table);
#endif
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
MODULE_VERSION(IO_LATENCY_VERSION);
