/* Copyright (C) 2006 Coraid, Inc.  See COPYING for GPL terms. */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/workqueue.h>
#include <linux/blkdev.h>
#include <linux/netdevice.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/ata.h>
#include <linux/ctype.h>
#include <linux/kern_levels.h>
#include <linux/tree.h>
#include <linux/delay.h>
#include "if_aoe.h"
#include "clydeinterface.h"

typedef enum {
    INCOMING = 1,
    OUTGOING,
} frame_direction_t;
struct aoedev;

#define xprintk(L, fmt, arg...) printk(L "kvblade: " "%s: " fmt, __func__, ## arg)
#define iprintk(fmt, arg...) xprintk(KERN_INFO, fmt, ## arg)
#define eprintk(fmt, arg...) xprintk(KERN_ERR, fmt, ## arg)
#define wprintk(fmt, arg...) xprintk(KERN_WARN, fmt, ## arg)
#define dprintk(fmt, arg...) if(0);else xprintk(KERN_DEBUG, fmt, ## arg)

struct tree_work {
    struct work_struct work;
    struct aoedev *d;
    struct sk_buff *rskb;
};

static struct workqueue_struct *tree_wq = NULL;
static struct kmem_cache *tw_pool = NULL;

struct counter {
    /*rollover every 1k*/
    unsigned long count;
    /*incremented at every rollover*/
    unsigned long kcount;
};

struct counter mycounter;

static __always_inline void count_inc(struct counter *c)
{
    if ( unlikely(++c->count == 1000) ){
        c->count = 0;
        c->kcount++;
    }
}

static struct timer_list tmr;
static void tmr_cb(unsigned long data) {
    printk("kvblade (inc packets): %luk, %lu\n", mycounter.kcount, mycounter.count);
    /*restart in 10 secs*/
    if ( mod_timer(&tmr, jiffies + msecs_to_jiffies(10000)) ) {
        printk("error initialising timer (mod_timer)\n");
    }
}


//#define DEBUGGING 0

#ifdef DEBUGGING
	static __always_inline char *treecmd_name(unsigned char cmd)
	{
	    switch (cmd) {
	    case AOECMD_CREATETREE:
	        return "AOECMD_CREATETREE";
	    case AOECMD_REMOVETREE:
	        return "AOECMD_REMOVETREE";
	    case AOECMD_READNODE:
	        return "AOECMD_READNODE";
	    case AOECMD_INSERTNODE:
	        return "AOECMD_INSERTNODE";
	    case AOECMD_UPDATENODE:
	        return "AOECMD_UPDATENODE";
	    case AOECMD_REMOVENODE:
	        return "AOECMD_REMOVENODE";
	    default:
	        return "UNKNOWN_TREE_CMD";
	    }
	}

	static void __dbg_print_treecmd(frame_direction_t dir, struct aoe_hdr *ah, struct aoe_datahdr *dh)
	{
	    pdbg(
	        "%s cmd(%u),tid(%llu),nid(%llu),off(%llu),len(%llu),err(%u) ", 
	        (dir == INCOMING ? "=>" : "<="), ah->cmd, dh->tree.tid, dh->tree.nid, 
	        dh->tree.off, dh->tree.len, dh->tree.err
	    );
	    if (ah->cmd == AOECMD_UPDATENODE && dir == INCOMING) {
	        printk("[%llu]\n", *((u64*)dh->data));
	    } else if (ah->cmd == AOECMD_READNODE && dir == OUTGOING) {
	        printk("[%llu]\n", *((u64*)dh->data));
	    } else {
	        printk("\n");
	    }
	}

	#define pdbg(fmt, arg...) xprintk(KERN_INFO, fmt, ## arg)
#else
	static __always_inline char *treecmd_name(unsigned char cmd) {
		return "";
	}

	#define pdbg(fmt, arg...) {}
#endif

#define nelem(A) (sizeof (A) / sizeof (A)[0])
#define MAXSECTORS(mtu) (((mtu) - sizeof (struct aoe_hdr) - sizeof (struct aoe_datahdr)) / 512)

static struct kobject kvblade_kobj;



enum {
	ATA_MODEL_LEN =	40,
	ATA_LBA28MAX = 0x0fffffff,
};

struct aoereq {
	struct bio *bio;
	struct sk_buff *skb;
	struct aoedev *d;	/* blech.  I'm blind to a cleaner solution. */
};

struct aoedev {
	struct kobject kobj;
	struct aoedev *next;
	struct net_device *netdev;
	struct block_device *blkdev;
	struct aoereq reqs[16];
	atomic_t busy;
	unsigned char config[1024];
	int nconfig;
	int major;
	int minor;

	char path[256];
	loff_t scnt;
	char model[ATA_MODEL_LEN];
	char sn[ATA_ID_SERNO_LEN];
};

struct kvblade_sysfs_entry {
	struct attribute attr;
	ssize_t (*show)(struct aoedev *, char *);
	ssize_t (*store)(struct aoedev *, const char *, size_t);
};

static struct sk_buff_head skb_outq, skb_inq;
static spinlock_t lock;
static struct aoedev *devlist;
static struct completion ktrendez;
static struct task_struct *task;
static wait_queue_head_t ktwaitq;

static struct sk_buff *treecmd(struct aoedev *d, struct sk_buff *skb);

/** 
 * Processes the actual work request and manipulates the 
 * backend. 
 * @param w the work structure containing the request 
 *  
 */ 
static void do_tree_work(struct work_struct *w)
{
    struct sk_buff *rskb;
    struct tree_work *tw = container_of(w, struct tree_work, work);

    rskb = treecmd(tw->d, tw->rskb);

    if (unlikely(!rskb)) {
        printk("do_tree_work: treecmd(d,rskb) failed\n");
        kmem_cache_free(tw_pool, tw);
        return; /*err*/
    }

    atomic_dec(&tw->d->busy);

    kmem_cache_free(tw_pool,tw);
    skb_queue_tail(&skb_outq, rskb);
    wake_up(&ktwaitq);
}

/** 
 * Initialise the work structure before first use.
 * @param data a reference to the work struct to initialise 
 * @note as all tree_work items go to the same queue, 
 *       initialising a tree_work struct just once suffices.
 * @note subsequent calls re-using the work structure should 
 *       call PREPARE_WORK()
 */ 
static void tree_work_init_once(void *data)
{
	struct tree_work *w = data;
	INIT_WORK(&w->work, do_tree_work);
}

static struct kobj_type kvblade_ktype;

static void kvblade_release(struct kobject *kobj)
{
}

static ssize_t kvblade_sysfs_args(char *p, char *argv[], int argv_max)
{
	int argc = 0;

	while (*p) {
		while (*p && isspace(*p))
			++p;
		if (*p == '\0')
			break;
		if (argc < argv_max)
			argv[argc++] = p;
		else {
			printk(KERN_ERR "too many args!\n");
			return -1;
		}
		while (*p && !isspace(*p))
			++p;
		if (*p)
			*p++ = '\0';
	}
	return argc;
}

static struct sk_buff * skb_new(struct net_device *dev, ulong len) 
{
	struct sk_buff *skb;

	if (len < ETH_ZLEN)
		len = ETH_ZLEN;

	skb = alloc_skb(len, GFP_ATOMIC);
	if (skb) {
		memset(skb->data, 0, len);

		skb_reset_network_header(skb);
		skb_reset_mac_header(skb);
		skb->dev = dev;
		skb->protocol = __constant_htons(ETH_P_AOE);
		skb->priority = 0;
		skb->next = skb->prev = NULL;
		skb->ip_summed = CHECKSUM_NONE;
		skb_put(skb, len);
	}
	return skb;
}

static char* spncpy(char *d, const char *s, int n)
{
	char *r = d;

	memset(d, ' ', n);
	while (n-- > 0) {
		if (*s == '\0')
			break;
		*d++ = *s++;
	}
	return r;
}


static void kvblade_announce(struct aoedev *d)
{
	struct sk_buff *skb;
	struct aoe_hdr *aoe;
	struct aoe_cfghdr *cfg;
	int len = sizeof *aoe + sizeof *cfg + d->nconfig;

	skb = skb_new(d->netdev, len);
	if (skb == NULL)
		return;

	aoe = (struct aoe_hdr *) skb_mac_header(skb);
	cfg = (struct aoe_cfghdr *) aoe->data;

	memset(aoe, 0, sizeof *aoe);
	memcpy(aoe->src, d->netdev->dev_addr, ETH_ALEN);
	memset(aoe->dst, 0xFF, ETH_ALEN);

	aoe->type = __constant_htons(ETH_P_AOE);
	aoe->verfl = AOE_HVER | AOEFL_RSP;
	aoe->major = cpu_to_be16(d->major);
	aoe->minor = d->minor;
	aoe->cmd = AOECMD_CFG;

	memset(cfg, 0, sizeof *cfg);
	cfg->bufcnt = cpu_to_be16(nelem(d->reqs));
	cfg->fwver = __constant_htons(0x0002);
	cfg->scnt = MAXSECTORS(d->netdev->mtu);
	cfg->aoeccmd = AOE_HVER;

	if (d->nconfig) {
		cfg->cslen = cpu_to_be16(d->nconfig);
		memcpy(cfg->data, d->config, d->nconfig);
	}
	skb_queue_tail(&skb_outq, skb);
	wake_up(&ktwaitq);
}


static ssize_t kvblade_add(u32 major, u32 minor, char *ifname, char *path)
{
	struct net_device *nd;
	struct block_device *bd;
	struct aoedev *d, *td;
	int ret = 0;

	printk("kvblade_add\n");
	nd = dev_get_by_name(&init_net, ifname);
	if (nd == NULL) {
		eprintk("add failed: interface %s not found.\n", ifname);
		return -ENOENT;
	}
	dev_put(nd);

	bd = blkdev_get_by_path(path, FMODE_READ|FMODE_WRITE, NULL);
	if (!bd || IS_ERR(bd)) {
		printk(KERN_ERR "add failed: can't open block device %s: %ld\n", path, PTR_ERR(bd));
		return -ENOENT;
	}

	if (get_capacity(bd->bd_disk) == 0) {
		printk(KERN_ERR "add failed: zero sized block device.\n");
		ret = -ENOENT;
		goto err;
	}

	spin_lock(&lock);
	
	for (td = devlist; td; td = td->next)
		if (td->major == major &&
			td->minor == minor &&
			td->netdev == nd) {

			spin_unlock(&lock);

			printk(KERN_ERR "add failed: device %d.%d already exists on %s.\n",
				major, minor, ifname);

			ret = -EEXIST;
			goto err;
		}
	
	d = kmalloc(sizeof(struct aoedev), GFP_KERNEL);
	if (!d) {
		printk(KERN_ERR "add failed: kmalloc error for %d.%d\n", major, minor);
		ret = -ENOMEM;
		goto err;
	}

	memset(d, 0, sizeof(struct aoedev));
	atomic_set(&d->busy, 0);
	d->blkdev = bd;
	d->netdev = nd;
	d->major = major;
	d->minor = minor;
	d->scnt = get_capacity(bd->bd_disk);
	strncpy(d->path, path, nelem(d->path)-1);
	spncpy(d->model, "EtherDrive(R) kvblade", nelem(d->model));
	spncpy(d->sn, "SN HERE", nelem(d->sn));
	
	kobject_init_and_add(&d->kobj, &kvblade_ktype, &kvblade_kobj, "%d.%d@%s", major, minor, ifname);

	d->next = devlist;
	devlist = d;
	spin_unlock(&lock);

	dprintk("added %s as %d.%d@%s: %Lu sectors.\n",
		path, major, minor, ifname, d->scnt);
	kvblade_announce(d);
	return 0;
err:
	blkdev_put(bd, FMODE_READ|FMODE_WRITE);
	return ret;
}

static ssize_t kvblade_del(u32 major, u32 minor, char *ifname)
{
	struct aoedev *d, **b;
	int ret;

	b = &devlist;
	d = devlist;
	spin_lock(&lock);
	
	for (; d; b = &d->next, d = *b)
		if (d->major == major &&
			d->minor == minor &&
			strcmp(d->netdev->name, ifname) == 0)
			break;

	if (d == NULL) {
		printk(KERN_ERR "del failed: device %d.%d@%s not found.\n", 
			major, minor, ifname);
		ret = -ENOENT;
		goto err;
	} else if (atomic_read(&d->busy)) {
		printk(KERN_ERR "del failed: device %d.%d@%s is busy.\n",
			major, minor, ifname);
		ret = -EBUSY;
		goto err;
	}

	*b = d->next;
	
	spin_unlock(&lock);
	
	blkdev_put(d->blkdev, FMODE_READ|FMODE_WRITE);
	
	kobject_del(&d->kobj);
	kobject_put(&d->kobj);
	
	return 0;
err:
	spin_unlock(&lock);
	return ret;
}


static ssize_t store_add(struct aoedev *dev, const char *page, size_t len)
{
	int error = 0;
	char *argv[16];
	char *p;

	p = kmalloc(len+1, GFP_KERNEL);
	memcpy(p, page, len);
	p[len] = '\0';
	
	if (kvblade_sysfs_args(p, argv, nelem(argv)) != 4) {
		printk(KERN_ERR "bad arg count for add\n");
		error = -EINVAL;
	} else
		error = kvblade_add(simple_strtoul(argv[0], NULL, 0),
			simple_strtoul(argv[1], NULL, 0),
			argv[2], argv[3]);

	kfree(p);
	return error ? error : len;
}


static struct kvblade_sysfs_entry kvblade_sysfs_add = __ATTR(add, 0644, NULL, store_add);

static ssize_t store_del(struct aoedev *dev, const char *page, size_t len)
{
	int error = 0;
	char *argv[16];
	char *p;

	p = kmalloc(len+1, GFP_KERNEL);
	memcpy(p, page, len);
	p[len] = '\0';

	if (kvblade_sysfs_args(p, argv, nelem(argv)) != 3) {
		printk(KERN_ERR "bad arg count for del\n");
		error = -EINVAL;
	} else
		error = kvblade_del(simple_strtoul(argv[0], NULL, 0),
			simple_strtoul(argv[1], NULL, 0),
			argv[2]);

	kfree(p);
	return error ? error : len;
}

static struct kvblade_sysfs_entry kvblade_sysfs_del = __ATTR(del, 0644, NULL, store_del);

static ssize_t show_scnt(struct aoedev *dev, char *page)
{
	return sprintf(page, "%Ld\n", dev->scnt);
}

static struct kvblade_sysfs_entry kvblade_sysfs_scnt = __ATTR(scst, 0644, show_scnt, NULL);

static ssize_t show_bdev(struct aoedev *dev, char *page)
{
	return print_dev_t(page, dev->blkdev->bd_dev);
}

static struct kvblade_sysfs_entry kvblade_sysfs_bdev = __ATTR(bdev, 0644, show_bdev, NULL);

static ssize_t show_bpath(struct aoedev *dev, char *page)
{
	return sprintf(page, "%.*s\n", (int) nelem(dev->path), dev->path);
}

static struct kvblade_sysfs_entry kvblade_sysfs_bpath = __ATTR(bpath, 0644, show_bpath, NULL);

static ssize_t show_model(struct aoedev *dev, char *page)
{
	return sprintf(page, "%.*s\n", (int) nelem(dev->model), dev->model);
}

static ssize_t store_model(struct aoedev *dev, const char *page, size_t len)
{
	spncpy(dev->model, page, nelem(dev->model));
	return 0;
}

static struct kvblade_sysfs_entry kvblade_sysfs_model = __ATTR(model, 0644, show_model, store_model);

static ssize_t show_sn(struct aoedev *dev, char *page)
{
	return sprintf(page, "%.*s\n", (int) nelem(dev->sn), dev->sn);
}

static ssize_t store_sn(struct aoedev *dev, const char *page, size_t len)
{
	spncpy(dev->sn, page, nelem(dev->sn));
	return 0; 
}

static struct kvblade_sysfs_entry kvblade_sysfs_sn = __ATTR(sn, 0644, show_sn, store_sn);

static struct attribute *kvblade_ktype_attrs[] = {
	&kvblade_sysfs_scnt.attr,
	&kvblade_sysfs_bdev.attr,
	&kvblade_sysfs_bpath.attr,
	&kvblade_sysfs_model.attr,
	&kvblade_sysfs_sn.attr,
	NULL,
};

static struct attribute *kvblade_ktype_ops_attrs[] = {
	&kvblade_sysfs_add.attr,
	&kvblade_sysfs_del.attr,
	NULL,
};

static ssize_t kvblade_attr_show(struct kobject *kobj, struct attribute *attr, char *page)
{
	struct kvblade_sysfs_entry *entry;
	struct aoedev *dev;

	entry = container_of(attr, struct kvblade_sysfs_entry, attr);
	dev = container_of(kobj, struct aoedev, kobj);

	if (!entry->show)
		return -EIO;

	return entry->show(dev, page);
}

static ssize_t kvblade_attr_store(struct kobject *kobj, struct attribute *attr,
			const char *page, size_t length)
{
	ssize_t ret;
	struct kvblade_sysfs_entry *entry;

	entry = container_of(attr, struct kvblade_sysfs_entry, attr);

	if (kobj == &kvblade_kobj)
		ret = entry->store(NULL, page, length);
	else {
		struct aoedev *dev = container_of(kobj, struct aoedev, kobj);

		if (!entry->store)
			return -EIO;

		ret = entry->store(dev, page, length);
	}

	return ret;
}

static const struct sysfs_ops kvblade_sysfs_ops = {
	.show		= kvblade_attr_show,
	.store		= kvblade_attr_store,
};

static struct kobj_type kvblade_ktype = {
	.default_attrs	= kvblade_ktype_attrs,
	.sysfs_ops		= &kvblade_sysfs_ops,
	.release		= kvblade_release,
};

static struct kobj_type kvblade_ktype_ops = {
	.default_attrs	= kvblade_ktype_ops_attrs,
	.sysfs_ops		= &kvblade_sysfs_ops,
	.release		= kvblade_release,
};


static void setfld(u16 *a, int idx, int len, char *str)
{
	u8 *p;

	for (p = (u8*)(a + idx); len; p += 2, len -= 2) {
		p[1] = *str ? *str++ : ' ';
		p[0] = *str ? *str++ : ' ';
	}
}

static int ata_identify(struct aoedev *d, struct aoe_datahdr *dh)
{
	char 	buf[64];
	u16     *words  = (u16 *)dh->data;
	u8      *cp;
	loff_t  scnt;


	memset(words, 0, 512);

	words[47] = 0x8000;
	words[49] = 0x0200;
	words[50] = 0x4000;
	words[83] = 0x5400;
	words[84] = 0x4000;
	words[86] = 0x1400;
	words[87] = 0x4000;
	words[93] = 0x400b;

	sprintf(buf, "V%d.%d\n", 0, 2);
	setfld(words, 23,  8, buf);
	setfld(words, 27, nelem(d->model), d->model);
	setfld(words, 10, nelem(d->sn), d->sn);

	scnt = d->scnt;
	cp = (u8 *)&words[100];
	*cp++ = scnt;
	*cp++ = (scnt >>= 8);
	*cp++ = (scnt >>= 8);
	*cp++ = (scnt >>= 8);
	*cp++ = (scnt >>= 8);
	*cp++ = (scnt >>= 8);

	scnt = d->scnt;
	cp = (u8 *)&words[60];

	if (scnt & ~ATA_LBA28MAX)
		scnt = ATA_LBA28MAX;
	*cp++ = scnt;
	*cp++ = (scnt >>= 8);
	*cp++ = (scnt >>= 8);
	*cp++ = (scnt >>= 8) & 0xf;

	return 512;
}

static void ata_io_complete(struct bio *bio, int error)
{
	struct aoereq *rq;
	struct aoedev *d;
	struct sk_buff *skb;
	struct aoe_hdr *aoe;
    struct aoe_datahdr *dh;
	int len;
	unsigned int bytes = 0;

	if (!error)
		bytes = bio->bi_io_vec[0].bv_len;

	rq = bio->bi_private;
	d = rq->d;
	skb = rq->skb;

	aoe = (struct aoe_hdr *) skb_mac_header(skb);
	dh = (struct aoe_datahdr *) aoe->data;

	len = sizeof *aoe + sizeof *dh;
	if (bio_flagged(bio, BIO_UPTODATE)) {
		if (bio_data_dir(bio) == READ)
			len += bytes;
		dh->ata.scnt = 0;
		dh->ata.cmdstat = ATA_DRDY;
		dh->ata.errfeat = 0;
		// should increment lba here, too
	} else {
		dprintk(KERN_ERR "I/O error %d on %s\n", error, d->kobj.name);
		dh->ata.cmdstat = ATA_ERR | ATA_DF;
		dh->ata.errfeat = ATA_UNC | ATA_ABORTED;
	}

	bio_put(bio);
	rq->skb = NULL;
	atomic_dec(&d->busy);

	skb_trim(skb, len);
	skb_queue_tail(&skb_outq, skb);

	wake_up(&ktwaitq);
}

static inline loff_t readlba(u8 *lba)
{
	loff_t n = 0ULL;
	int i;

	for (i=5; i>=0; i--) {
		n <<= 8;
		n |= lba[i];
	}
	return n;
}

static struct sk_buff * ata(struct aoedev *d, struct sk_buff *skb)
{
	struct aoe_hdr *aoe;
    struct aoe_datahdr *dh;
	struct aoereq *rq, *e;
	struct bio *bio;
	sector_t lba;
	int len, rw;
	struct page *page;
	ulong bcnt, offset;

	aoe = (struct aoe_hdr *) skb_mac_header(skb);
	dh = (struct aoe_datahdr *) aoe->data;
	lba = readlba(dh->ata.lba);
	len = sizeof *aoe + sizeof *dh;
	switch (dh->ata.cmdstat) {
	do {
	case ATA_CMD_PIO_READ:
		lba &= ATA_LBA28MAX;
	case ATA_CMD_PIO_READ_EXT:
		lba &= 0x0000FFFFFFFFFFFFULL;
		rw = READ;
		break;
	case ATA_CMD_PIO_WRITE:
		lba &= ATA_LBA28MAX;
	case ATA_CMD_PIO_WRITE_EXT:
		lba &= 0x0000FFFFFFFFFFFFULL;
		rw = WRITE;
	} while (0);
		if ((lba + dh->ata.scnt) > d->scnt) {
			printk(KERN_ERR "sector I/O is out of range: %Lu (%d), max %Lu\n",
				(long long) lba, dh->ata.scnt, d->scnt);
			dh->ata.cmdstat = ATA_ERR;
			dh->ata.errfeat = ATA_IDNF;
			break;
		}
		rq = d->reqs;
		e = rq + nelem(d->reqs);
		for (; rq<e; rq++)
			if (rq->skb == NULL)
				break;
		if (rq == e)
			goto drop;
		
		bio = bio_alloc(GFP_ATOMIC, 1);
		if (bio == NULL) {
			eprintk("can't alloc bio\n");
			goto drop;
		}
		rq->bio = bio;
		rq->d = d;

		bio->bi_sector = lba;
		bio->bi_bdev = d->blkdev;
		bio->bi_end_io = ata_io_complete;
		bio->bi_private = rq;

		page = virt_to_page(dh->data);
		bcnt = dh->ata.scnt << 9;
		offset = offset_in_page(dh->data);

		if (bio_add_page(bio, page, bcnt, offset) < bcnt) {
			eprintk(KERN_ERR "Can't bio_add_page for %d sectors\n", dh->ata.scnt);
			bio_put(bio);
			goto drop;
		}

		rq->skb = skb;
		atomic_inc(&d->busy);
		submit_bio(rw, bio);
		return NULL;
	default:
		eprintk(KERN_ERR "Unknown ATA command 0x%02X\n", dh->ata.cmdstat);
		dh->ata.cmdstat = ATA_ERR;
		dh->ata.errfeat = ATA_ABORTED;
		break;
	case ATA_CMD_ID_ATA:
		len += ata_identify(d, dh);
	case ATA_CMD_FLUSH:
		dh->ata.cmdstat = ATA_DRDY;
		dh->ata.errfeat = 0;
		break;
	}
	skb_trim(skb, len);
	return skb;
drop:
	dev_kfree_skb(skb);
	return NULL;
}

static struct sk_buff* cfg(struct aoedev *d, struct sk_buff *skb)
{
	struct aoe_hdr *aoe;
	struct aoe_cfghdr *cfg;
	int len, cslen, ccmd;

	aoe = (struct aoe_hdr *) skb_mac_header(skb);
	cfg = (struct aoe_cfghdr *) aoe->data;
	cslen = ntohs(cfg->cslen);
	ccmd = cfg->aoeccmd & 0xf;
	len = sizeof *aoe;

	cfg->bufcnt = htons(nelem(d->reqs));
	cfg->scnt = MAXSECTORS(d->netdev->mtu);
	cfg->fwver = __constant_htons(0x0002);
	cfg->aoeccmd = AOE_HVER;

	if (cslen > nelem(d->config))
		goto drop;

	switch (ccmd) {
	case AOECCMD_TEST:
		if (d->nconfig != cslen)
			goto drop;
		// fall thru
	case AOECCMD_PTEST:
		if (cslen > d->nconfig)
			goto drop;
		if (memcmp(cfg->data, d->config, cslen) != 0)
			goto drop;
		// fall thru
	case AOECCMD_READ:
		cfg->cslen = cpu_to_be16(d->nconfig);
		memcpy(cfg->data, d->config, d->nconfig);
		len += sizeof *cfg + d->nconfig;
		break;
	case AOECCMD_SET:
		if (d->nconfig)
		if (d->nconfig != cslen || memcmp(cfg->data, d->config, cslen) != 0) {
			aoe->verfl |= AOEFL_ERR;
			aoe->err = AOEERR_CFG;
			break;
		}
		// fall thru
	case AOECCMD_FSET:
		d->nconfig = cslen;
		memcpy(d->config, cfg->data, cslen);
		len += sizeof *cfg + cslen;
		break;
	default:
		aoe->verfl |= AOEFL_ERR;
		aoe->err = AOEERR_ARG;
	}
	skb_trim(skb, len);
	return skb;
drop:
	dev_kfree_skb(skb);
	return NULL;
}



static __always_inline void set_errcode(struct aoe_datahdr *dh, int errcode)
{
    memcpy(dh->data, &errcode, sizeof(int));
}


static struct sk_buff *treecmd(struct aoedev *d, struct sk_buff *skb)
{
    struct aoe_hdr *ah;
    struct aoe_datahdr *dh;
    u64 tree_ret;

    ah = (struct aoe_hdr *) skb_mac_header(skb);
	dh = (struct aoe_datahdr *) ah->data;

    /*__dbg_print_treecmd(INCOMING,ah,dh);*/

    switch(ah->cmd) {
    case AOECMD_CREATETREE:
        /*FIXME: guard against retval==0 which indicates a failure to make a tree*/
        tree_ret = clydefscore_tree_create(10);
        if(likely(tree_ret)){ /*success*/
            dh->tree.tid = tree_ret;
            dh->tree.err = 0;
        } else {
            dh->tree.err = TERR_ALLOC_FAILED;
        }
        skb_trim(skb, sizeof(*ah) + sizeof(*dh));
        break;
    case AOECMD_REMOVETREE:
        dh->tree.err = clydefscore_tree_remove(dh->tree.tid);
        skb_trim(skb, sizeof(*ah) + sizeof(*dh));
        break;
    case AOECMD_UPDATENODE:
        pdbg("AOECMD_UPDATENODE: writing %llu bytes of data at offset(%llu)\n", dh->tree.len, dh->tree.off);
        dh->tree.err = clydefscore_node_write(dh->tree.tid, dh->tree.nid, dh->tree.off, dh->tree.len, dh->data);
        pdbg("data written:\n");
#ifdef DEBUGGING
        print_hex_dump(KERN_EMERG, "", DUMP_PREFIX_NONE, 16, 1, dh->data, dh->tree.len, 0);
#endif
        skb_trim(skb, sizeof(*ah)+sizeof(*dh));
        break;
    case AOECMD_INSERTNODE:
        dh->tree.err = clydefscore_node_insert(dh->tree.tid, &dh->tree.nid);
        skb_trim(skb, sizeof(*ah) + sizeof(*dh));
        break;
    case AOECMD_READNODE:
        pdbg("AOECMD_READNODE: reading %llu(uint:%u) bytes of data at offset(%llu)\n", dh->tree.len, (dh->tree.len & 0xFFFFFFFF), dh->tree.off);
        dh->tree.err = clydefscore_node_read(dh->tree.tid, dh->tree.nid, dh->tree.off, dh->tree.len, dh->data);
        if (unlikely(dh->tree.err)) {
            pdbg("\t\t AOECMD_READNODE err'ed out!\n");
            skb_trim(skb, sizeof(*ah) + sizeof(*dh)); /*no additional data on error*/
        } else {
            /*dh->tree.len -- cannot use u64 across our ethernet interface 
              anyway, but it's technically breaking the interface*/
            skb_trim(skb, sizeof(*ah) + sizeof(*dh) + (dh->tree.len & 0xFFFFFFFF));
        }
        break;
    case AOECMD_REMOVENODE:
        dh->tree.err = clydefscore_node_remove(dh->tree.tid, dh->tree.nid);
        skb_trim(skb, sizeof(*ah) + sizeof(*dh));
        break;
    default:
        pr_alert("UNKNOWN TREE CMD SENT, CODE: (%u)\n", ah->cmd);
        return NULL; /*FIXME: always dropping now*/
    }
    /*__dbg_print_treecmd(OUTGOING, ah, dh);*/
    
    return skb;
}

static struct sk_buff* make_response(struct sk_buff *skb, int major, int minor)
{
	struct aoe_hdr *aoe;
	struct sk_buff *rskb;

	rskb = skb_new(skb->dev, skb->dev->mtu);
	if (rskb == NULL)
		return NULL;
	aoe = (struct aoe_hdr *) skb_mac_header(rskb);
	memcpy(skb_mac_header(rskb), skb_mac_header(skb), skb->len);
	memcpy(aoe->dst, aoe->src, ETH_ALEN);
	memcpy(aoe->src, skb->dev->dev_addr, ETH_ALEN);
	aoe->type = __constant_htons(ETH_P_AOE);
	aoe->verfl = AOE_HVER | AOEFL_RSP;
	aoe->major = cpu_to_be16(major);
	aoe->minor = minor;
	aoe->err = 0;
	return rskb;
}

static int rcv(struct sk_buff *skb, struct net_device *ndev, struct packet_type *pt, struct net_device *orig_dev)
{
	struct aoe_hdr *aoe;

	skb = skb_share_check(skb, GFP_ATOMIC);
	if (skb == NULL)
		return -ENOMEM;

	if (skb_linearize(skb) < 0) {
		dev_kfree_skb(skb);
		return -ENOMEM;
	}
	skb_push(skb, ETH_HLEN);

	aoe = (struct aoe_hdr *) skb_mac_header(skb);
	if (~aoe->verfl & AOEFL_RSP) {
		skb_queue_tail(&skb_inq, skb);
		wake_up(&ktwaitq);
	} else {
		dev_kfree_skb(skb);
	}

	return 0;
}

static __always_inline int is_tree_cmd(unsigned char cmd)
{
    /*check that cmd is inside the range of values designating tree commands*/
    return (cmd >= AOECMD_CREATETREE) && (cmd <= AOECMD_REMOVENODE);
}


static void ktrcv(struct sk_buff *skb)
{
	struct sk_buff *rskb;
	struct aoedev *d;
	struct aoe_hdr *aoe;
	int major, minor;
    struct tree_work *tw;

	aoe = (struct aoe_hdr *) skb_mac_header(skb);
	major = be16_to_cpu(aoe->major);
	minor = aoe->minor;

	spin_lock(&lock);

	for (d=devlist; d; d=d->next) {
		if ((major != d->major && major != 0xffff) ||
			(minor != d->minor && minor != 0xff) ||
			(skb->dev != d->netdev))
			continue;

        count_inc(&mycounter);
		rskb = make_response(skb, d->major, d->minor);
		if (rskb == NULL)
			continue;

		switch (aoe->cmd) {
		case AOECMD_ATA:
			rskb = ata(d, rskb);
			break;
		case AOECMD_CFG:
			rskb = cfg(d, rskb);
			break;
        /*TODO branch on vendor-specifc codes in a meaningful way*/
        case AOECMD_CREATETREE:
        case AOECMD_REMOVETREE:
        case AOECMD_READNODE:
        case AOECMD_INSERTNODE:
        case AOECMD_UPDATENODE:
        case AOECMD_REMOVENODE:
            pdbg(KERN_INFO "Received vendor-specific cmd: %u\n", aoe->cmd);
            tw = kmem_cache_alloc(tw_pool, GFP_ATOMIC);
            if (!tw) {
                printk("failed to allocate tree_work\n");
                dev_kfree_skb(rskb);
                break;            
            } else {
                tw->rskb = rskb;
                tw->d = d;
                PREPARE_WORK(&tw->work, do_tree_work);
                rskb = NULL; /*nothing to return presently, async OP*/
                atomic_inc(&tw->d->busy);
                queue_work(tree_wq, &tw->work);
            }
            
            
            break;
		default:
			dev_kfree_skb(rskb);
			continue;
		}

		if (rskb)
			skb_queue_tail(&skb_outq, rskb);
	}

    
	spin_unlock(&lock);

	dev_kfree_skb(skb);
}

static int kthread(void *errorparameternameomitted)
{
	struct sk_buff *iskb, *oskb;
	DECLARE_WAITQUEUE(wait, current);
	sigset_t blocked;

#ifdef PF_NOFREEZE
	current->flags |= PF_NOFREEZE;
#endif
	set_user_nice(current, -5);
	sigfillset(&blocked);
	sigprocmask(SIG_BLOCK, &blocked, NULL);
	flush_signals(current);
	complete(&ktrendez);
	do {
		__set_current_state(TASK_RUNNING);
		do {
			if ((iskb = skb_dequeue(&skb_inq)))
				ktrcv(iskb);
			if ((oskb = skb_dequeue(&skb_outq)))
				dev_queue_xmit(oskb);
		} while (iskb || oskb);
		set_current_state(TASK_INTERRUPTIBLE);
		add_wait_queue(&ktwaitq, &wait);
		schedule();
		remove_wait_queue(&ktwaitq, &wait);
	} while (!kthread_should_stop());
	__set_current_state(TASK_RUNNING);
	complete(&ktrendez);
	return 0;
}

static struct packet_type pt = {
	.type = __constant_htons(ETH_P_AOE),
	.func = rcv,
};

static int __init kvblade_module_init(void)
{
	skb_queue_head_init(&skb_outq);
	skb_queue_head_init(&skb_inq);
    
	
	spin_lock_init(&lock);
	
	init_completion(&ktrendez);
	init_waitqueue_head(&ktwaitq);
    setup_timer( &tmr, tmr_cb, 0 );
    if ( mod_timer(&tmr, jiffies + msecs_to_jiffies(10000)) ) {
        printk("error initialising timer (mod_timer)\n");
    }

    tree_wq = alloc_workqueue("kvblade_treewq", 
                  WQ_HIGHPRI | WQ_CPU_INTENSIVE, 256);
    if (!tree_wq) {
        return -ENOMEM;
    }

    tw_pool = kmem_cache_create(
        "kvblade_tw_pool",
		sizeof(struct tree_work),
        0,
        /*objects are reclaimable*/
		SLAB_RECLAIM_ACCOUNT 
        /*spread allocation across memory rather than favouring memory local to current cpu*/
         | SLAB_MEM_SPREAD,  
        /*called whenever new pages are added to the cache*/
		tree_work_init_once
    );
	if (!tw_pool) {
        pr_debug("Failed to allocate a memcache for tree work items\n");
        destroy_workqueue(tree_wq);
		return -ENOMEM;
    }
	
	task = kthread_run(kthread, NULL, "kvblade");
	if (task == NULL || IS_ERR(task))
		return -EAGAIN;

	kobject_init_and_add(&kvblade_kobj, &kvblade_ktype_ops, NULL, "kvblade");

	wait_for_completion(&ktrendez);
	init_completion(&ktrendez);	// for exit

	dev_add_pack(&pt);
	return 0;
}

static __exit void kvblade_module_exit(void)
{
	struct aoedev *d, *nd;

	printk("Testing exiting\n");
    while ( del_timer(&tmr) ) {
        printk("waiting for timer...\n");
        msleep(3000);
    }

    /*Finish outstanding work -- TODO - how does ata_io_complete fare in this regard*/
    flush_workqueue(tree_wq);

	dev_remove_pack(&pt);
	spin_lock(&lock);
	d = devlist;
	devlist = NULL;
	spin_unlock(&lock);
	for (; d; d=nd) {
		nd = d->next;
		while (atomic_read(&d->busy))
			msleep(100);
		blkdev_put(d->blkdev, FMODE_READ|FMODE_WRITE);
		
		kobject_del(&d->kobj);
		kobject_put(&d->kobj);
	}
	kthread_stop(task);
	wait_for_completion(&ktrendez);
	skb_queue_purge(&skb_outq);
	skb_queue_purge(&skb_inq);
	
	kobject_del(&kvblade_kobj);
	kobject_put(&kvblade_kobj);
    
    destroy_workqueue(tree_wq);
    kmem_cache_destroy(tw_pool);
    
}

module_init(kvblade_module_init);
module_exit(kvblade_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sam Hopkins <sah@coraid.com>");
MODULE_DESCRIPTION("Virtual EtherDrive(R) Blade");

