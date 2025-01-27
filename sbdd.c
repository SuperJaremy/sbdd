#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/bio.h>
#include <linux/bvec.h>
#include <linux/init.h>
#include <linux/wait.h>
#include <linux/stat.h>
#include <linux/slab.h>
#include <linux/numa.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/jiffies.h>
#include <linux/moduleparam.h>
#include <linux/spinlock_types.h>
#ifdef BLK_MQ_MODE
#include <linux/blk-mq.h>
#endif

static unsigned int     __pre_mode = 0;
enum mode{AUTO = 0, USER};
static enum mode        __mode = AUTO;

static int check_mode(void)
{
    if(__pre_mode == 0 || __pre_mode == 1){
        __mode = __pre_mode;
        switch(__mode){
        case AUTO:
            pr_info("working in auto mode\n");
            break;
        case USER:
            pr_info("working in user mode\n");
        }

        return 0;
    }
    pr_warn("incorrect mode. auto mode will be used instead\n");
    return 1;
}


#define SBDD_BUS_NAME "sbdd_bus"
#define MAX_DEV_NAME_SIZE 8
/*
 * Creating our own bus to register driver on it
 */

static int sbdd_match(struct device *dev, struct device_driver *drv);

static int sbdd_uevent(struct device *dev, struct kobj_uevent_env *env);

static struct bus_type sbdd_bus_type = {
    .name = SBDD_BUS_NAME,
    .match = sbdd_match,
    .uevent = sbdd_uevent
};

static void sbdd_bus_release(struct device *dev)
{
    pr_debug("sbdd_bus_dev released\n");
}

static struct device sbdd_bus = {
    .init_name = "sbdd_bus_dev",
    .release = sbdd_bus_release
};

struct sbd_driver {
    struct device_driver driver;
    struct driver_attribute command_attr;
};

static struct sbd_driver sbddrv = {
    .driver = {
        .name = "sbdd"
    }
};

static int sbdd_match(struct device *dev, struct device_driver *drv)
{
    struct sbd_driver *driver = dev_get_drvdata(dev);
    if(!driver)
        return 0;
    return !strcmp(driver->driver.name, drv->name);
}

static int sbdd_uevent(struct device *dev, struct kobj_uevent_env *env)
{
    return 0;
}

static int sbdd_bus_register(void)
{
    int ret = 0;
    pr_info("registering sbdd_bus\n");
    ret = bus_register(&sbdd_bus_type);
    if(ret)
        pr_err("error registering sbdd_bus_type with code %d\n", ret);
    else
        pr_info("sbdd_bus registered successfully\n");
    ret = device_register(&sbdd_bus);
    if(ret){
        pr_err("error registering bus device with code %d\n", ret);
        bus_unregister(&sbdd_bus_type);
    }
    else
        pr_info("bus device registered successfully\n");
    return ret;
}

static void sbdd_bus_unregister(void)
{
    device_unregister(&sbdd_bus);
    pr_info("unregistered sbdd_bus_dev\n");
    bus_unregister(&sbdd_bus_type);
    pr_info("unregistering sbdd_bus\n");
}

static ssize_t execute_command(struct device_driver *driver, const char *buf,
                               size_t count);

/*
 * Structure that representing our driver in sysfs
 */

int register_sbd_driver(struct sbd_driver *driver)
{
    int ret = 0;
    pr_info("registering sbd_driver...\n");
    driver->driver.bus = &sbdd_bus_type;
    ret = driver_register(&driver->driver);
    if(ret){
        pr_err("registering sbd_driver failed with code %d\n", ret);
        return ret;
    }
    driver->command_attr.attr.name = "command";
    driver->command_attr.attr.mode = S_IWUSR;
    driver->command_attr.store = execute_command;
    driver->command_attr.show = NULL;
    ret = driver_create_file(&driver->driver, &driver->command_attr);
    if(ret){
        pr_err("creating attribute failed with code %d\n", ret);
        driver_unregister(&driver->driver);
        return ret;
    }
    pr_info("sbd_driver registered\n");
    return ret;
}

void unregister_sbd_driver(struct sbd_driver *driver)
{
    driver_remove_file(&driver->driver, &driver->command_attr);
    driver_unregister(&driver->driver);
    pr_info("unregistered sbd_driver\n");
}



#define SBDD_SECTOR_SHIFT      9
#define SBDD_SECTOR_SIZE       (1 << SBDD_SECTOR_SHIFT)
#define SBDD_MIB_SECTORS       (1 << (20 - SBDD_SECTOR_SHIFT))
#define SBDD_NAME              "sbdd"
#define SBDEV_NAME             "sbd"
#define MAX_DEVICES            16

struct sbdd {
    char                    *name;
	wait_queue_head_t       exitwait;
	spinlock_t              datalock;
    spinlock_t              transferring;
	atomic_t                deleting;
	atomic_t                refs_cnt;
	sector_t                capacity;
	u8                      *data;
	struct gendisk          *gd;
	struct request_queue    *q;
    struct device           *dev;
#ifdef BLK_MQ_MODE
	struct blk_mq_tag_set   *tag_set;
#endif
};

static struct sbdd      *__devices;
static struct sbdd      __zero_sbdd = {0};
static int              __sbdd_major = 0;
static unsigned long    __sbdd_capacity_mib = 100;
static spinlock_t       __creating_new_disk;

/*
 * Making a unified interface for user command execution
 */

#define COMMAND_NUMBER 2

enum commands {CREATE_COMMAND = 0, CHANGE_MODE_COMMAND};

static const char *command_names[] = {[CREATE_COMMAND] = "create", [CHANGE_MODE_COMMAND] = "change_mode"};

typedef int (*executor)(const char*, size_t);

static int create_com(const char* buf, size_t count);

static int change_mode_com(const char* buf, size_t count);

static int add_new_sbdd(unsigned long capacity_mib, char* name, size_t name_len);

/*
 * executors should parse the command's args, check them
 * and then execute the command itself
 */

static const executor command_execs[] = {[CREATE_COMMAND] = create_com, [CHANGE_MODE_COMMAND] = change_mode_com};

static ssize_t execute_command(struct device_driver *driver, const char *buf,
                               size_t count)
{
    int i = CREATE_COMMAND;
    pr_info("parsing command...\n");
    for(; i < COMMAND_NUMBER; i++){
        int ret;
        const char *name = command_names[i];
        const size_t name_len = strlen(name);
        const char *begin = strstr(buf, name);
        if(begin && (begin + name_len <= buf + count) &&
                (*(begin + name_len)==' ' ||
                 *(begin + name_len) == '\n' ||
                 *(begin + name_len) == '\0')){
            pr_info("command %s parsed\n", name);
            ret = command_execs[i](buf, count);
            if(ret)
                return ret;
            else
                return count;
        }
    }
    pr_info("unknown command\n");
    return count;
}

static int create_com(const char* buf, size_t count)
{
    const char *comm = command_names[CREATE_COMMAND];
    const int args_num = 2;
    const char *args = strstr(buf, comm) + strlen(comm) + 1;
    const char *space = strstr(args, " ");
    size_t name_len = 0;
    char* name;
    unsigned long capacity_mib = 0;
    int ret = 0;
    if(__mode == AUTO){
        pr_warn("create command is unavailable in auto mode\n");
        return 0;
    }
    if(!space){
        pr_err("wrong command format\n");
        return -EINVAL;
    }
    name_len = space - args + 1;
    if(name_len == 0){
        pr_err("wrong command format\n");
        return -EINVAL;
    }
    if(name_len - 1 > MAX_DEV_NAME_SIZE){
        pr_err("maximal device name length is %d\n", MAX_DEV_NAME_SIZE);
        return -EINVAL;
    }
    name = kzalloc(name_len, GFP_KERNEL);
    ret = sscanf(args, "%s %lu", name, &capacity_mib);
    if(ret < args_num){
        kvfree(name);
        pr_err("wrong command format\n");
        return -EINVAL;
    }
    pr_debug("create command args: %s %lu\n", name, capacity_mib);
    ret = add_new_sbdd(capacity_mib, name, name_len);
    if(!ret)
        pr_info("device %s created\n", name);
    kvfree(name);
    return ret;
}

static struct sbdd *find_device_by_name(char *name);

static int change_mode_com(const char* buf, size_t count)
{
    const char *comm = command_names[CHANGE_MODE_COMMAND];
    const int args_num = 2;
    const char *args = strstr(buf, comm) + strlen(comm) + 1;
    const char *space = strstr(args, " ");
    size_t name_len = 0;
    char* name;
    int mode = 0;
    int ret = 0;
    struct sbdd *dev;
    if(!space){
        pr_err("wrong command format\n");
        return -EINVAL;
    }
    name_len = space - args + 1;
    if(name_len == 0){
        pr_err("wrong command format\n");
        return -EINVAL;
    }
    name = kzalloc(name_len, GFP_KERNEL);
    ret = sscanf(args, "%s %d", name, &mode);
    if(ret < args_num){
        kvfree(name);
        pr_err("wrong command format\n");
        return -EINVAL;
    }
    pr_debug("change command args: %s, %d\n", name, mode);
    if(mode != 0 && mode != 1){
        pr_err("device mode can be 0 or 1\n");
        kvfree(name);
        return ret;
    }
    dev = find_device_by_name(name);
    if(!dev){
        pr_warn("device with name %s not found\n", name);
        return 0;
    }
    if(atomic_read(&dev->deleting)){
        pr_warn("device %s is being deleted\n", name);
        kvfree(name);
        return 1;
    }
    spin_lock(&dev->transferring);
    set_disk_ro(dev->gd, mode);
    spin_unlock(&dev->transferring);
    pr_info("device %s is now in mode %d\n", name, mode);
    kvfree(name);
    return 0;
}

static sector_t sbdd_xfer(struct bio_vec* bvec, sector_t pos, int dir, struct sbdd *dev)
{
	void *buff = page_address(bvec->bv_page) + bvec->bv_offset;
	sector_t len = bvec->bv_len >> SBDD_SECTOR_SHIFT;
	size_t offset;
	size_t nbytes;

    if (pos + len > dev->capacity){
        len = dev->capacity - pos;
    }

	offset = pos << SBDD_SECTOR_SHIFT;
	nbytes = len << SBDD_SECTOR_SHIFT;

    spin_lock(&dev->datalock);

    if (dir)
        memcpy(dev->data + offset, buff, nbytes);
	else
        memcpy(buff, dev->data + offset, nbytes);

    spin_unlock(&dev->datalock);

	pr_debug("pos=%6llu len=%4llu %s\n", pos, len, dir ? "written" : "read");

	return len;
}

#ifdef BLK_MQ_MODE

static void sbdd_xfer_rq(struct request *rq, struct sbdd *dev)
{
	struct req_iterator iter;
	struct bio_vec bvec;
	int dir = rq_data_dir(rq);
	sector_t pos = blk_rq_pos(rq);

	rq_for_each_segment(bvec, rq, iter)
        pos += sbdd_xfer(&bvec, pos, dir, dev);
}

static blk_status_t sbdd_queue_rq(struct blk_mq_hw_ctx *hctx,
                                  struct blk_mq_queue_data const *bd)
{
    struct sbdd *dev = bd->rq->rq_disk->private_data;
    if (atomic_read(&dev->deleting))
		return BLK_STS_IOERR;

    spin_lock(&dev->transferring);

    atomic_inc(&dev->refs_cnt);
    blk_mq_start_request(bd->rq);
    sbdd_xfer_rq(bd->rq, dev);
    blk_mq_end_request(bd->rq, BLK_STS_OK);

    if (atomic_dec_and_test(&dev->refs_cnt))
        wake_up(&dev->exitwait);

    spin_unlock(&dev->transferring);

    return BLK_STS_OK;
}

static struct blk_mq_ops const __sbdd_blk_mq_ops = {
	/*
	The function receives requests for the device as arguments
	and can use various functions to process them. The functions
	used to process requests in the handler are described below:

	blk_mq_start_request()   - must be called before processing a request
	blk_mq_requeue_request() - to re-send the request in the queue
	blk_mq_end_request()     - to end request processing and notify upper layers
	*/
	.queue_rq = sbdd_queue_rq,
};

#else

static void sbdd_xfer_bio(struct bio *bio, struct sbdd *dev)
{
	struct bvec_iter iter;
	struct bio_vec bvec;
	int dir = bio_data_dir(bio);
	sector_t pos = bio->bi_iter.bi_sector;

	bio_for_each_segment(bvec, bio, iter)
        pos += sbdd_xfer(&bvec, pos, dir, dev);
}

static blk_qc_t sbdd_make_request(struct request_queue *q, struct bio *bio)
{
    struct sbdd *dev = bio->bi_disk->private_data;
    spin_lock(&dev->transferring);
    if (atomic_read(&dev->deleting)){
        spin_unlock(&dev->transferring);
		return BLK_STS_IOERR;
    }

    atomic_inc(&dev->refs_cnt);
    sbdd_xfer_bio(bio, dev);
	bio_endio(bio);

    if (atomic_dec_and_test(&dev->refs_cnt))
        wake_up(&dev->exitwait);

    spin_unlock(&dev->transferring);
    return BLK_STS_OK;
}

#endif /* BLK_MQ_MODE */

/*
There are no read or write operations. These operations are performed by
the request() function associated with the request queue of the disk.
*/
static struct block_device_operations const __sbdd_bdev_ops = {
	.owner = THIS_MODULE,
};

/*
 * In order to manage several devices with one module let's
 * split getting major number and adding disk operations.
 */

static void sbdd_device_release(struct device *dev)
{}

static int sbdd_device_register(struct sbdd *dev, char* name)
{
    int ret = 0;
    dev->dev = kzalloc(sizeof (struct device), GFP_KERNEL);
    if(!dev->dev){
        pr_err("cannot allocate memory for sysfs device enetry\n");
        return -ENOMEM;
    }
    pr_info("registering %s on sysfs\n", name);
    dev_set_name(dev->dev, "%s", name);
    dev_set_drvdata(dev->dev, &sbddrv);
    dev->dev->bus = &sbdd_bus_type;
    dev->dev->parent = &sbdd_bus;
    dev->dev->release = sbdd_device_release;
    ret = device_register(dev->dev);
    if(ret)
        pr_err("registering %s failed with code %d\n", name, ret);
    return ret;
}

static void sbdd_device_unregister(struct sbdd *dev)
{
    device_unregister(dev->dev);
    kvfree(dev->dev);
    kvfree(dev->name);
}

static int sbdd_setup(struct sbdd *dev, size_t idx, unsigned long capacity_mib, char* name, size_t name_len)
{
    int ret = 0;
    memset(dev, 0, sizeof(struct sbdd));
    dev->capacity = (sector_t)capacity_mib * SBDD_MIB_SECTORS;

    pr_info("allocating data\n");
    dev->data = vmalloc(dev->capacity << SBDD_SECTOR_SHIFT);
    if (!dev->data) {
        pr_err("unable to alloc data\n");
        return -ENOMEM;
    }

    spin_lock_init(&dev->datalock);
    spin_lock_init(&dev->transferring);
    init_waitqueue_head(&dev->exitwait);

#ifdef BLK_MQ_MODE
    pr_info("allocating tag_set\n");
    dev->tag_set = kzalloc(sizeof(struct blk_mq_tag_set), GFP_KERNEL);
    if (!dev->tag_set) {
        pr_err("unable to alloc tag_set\n");
        return -ENOMEM;
    }

    /* Number of hardware dispatch queues */
    dev->tag_set->nr_hw_queues = 1;
    /* Depth of hardware dispatch queues */
    dev->tag_set->queue_depth = 128;
    dev->tag_set->numa_node = NUMA_NO_NODE;
    dev->tag_set->ops = &__sbdd_blk_mq_ops;

    ret = blk_mq_alloc_tag_set(_dev->tag_set);
    if (ret) {
        pr_err("call blk_mq_alloc_tag_set() failed with %d\n", ret);
        return ret;
    }

    /* Creates both the hardware and the software queues and initializes structs */
    pr_info("initing queue\n");
    dev->q = blk_mq_init_queue(dev->tag_set);
    if (IS_ERR(dev->q)) {
        ret = (int)PTR_ERR(dev->q);
        pr_err("call blk_mq_init_queue() failed witn %d\n", ret);
        dev->q = NULL;
        return ret;
    }
#else
    pr_info("allocating queue\n");
    dev->q = blk_alloc_queue(GFP_KERNEL);
    if (!dev->q) {
        pr_err("call blk_alloc_queue() failed\n");
        return -EINVAL;
    }
    blk_queue_make_request(dev->q, sbdd_make_request);
#endif /* BLK_MQ_MODE */

    /* Configure queue */
    blk_queue_logical_block_size(dev->q, SBDD_SECTOR_SIZE);

    /* A disk must have at least one minor */
    pr_info("allocating disk\n");
    dev->gd = alloc_disk(1);

    /* Configure gendisk */
    dev->gd->queue = dev->q;
    dev->gd->major = __sbdd_major;
    dev->gd->first_minor = idx;
    dev->gd->private_data = dev;
    dev->gd->fops = &__sbdd_bdev_ops;
    /* Represents name in /proc/partitions and /sys/block */
    scnprintf(dev->gd->disk_name, name_len, "%s", name);
    set_capacity(dev->gd, dev->capacity);

    /*
    Allocating gd does not make it available, add_disk() required.
    After this call, gd methods can be called at any time. Should not be
    called before the driver is fully initialized and ready to process reqs.
    */
    pr_info("adding disk\n");
    add_disk(dev->gd);
    if(!ret){
        ret = sbdd_device_register(dev, name);
    }
    dev->name = kzalloc(name_len, GFP_KERNEL);
    if(!name){
        pr_err("cannot allocate memory for device name\n");
        return -ENOMEM;
    }
    scnprintf(dev->name, name_len, "%s", name);
    return ret;
}

static struct sbdd *find_device_by_name(char *name){
    int i;
    for(i = 0; i < MAX_DEVICES; i++){
        if(!memcmp(&__devices[i], &__zero_sbdd, sizeof(struct sbdd)))
            break;
        if(!strcmp(name, __devices[i].name))
            return &__devices[i];
    }
    return NULL;
}

static int add_new_sbdd(unsigned long capacity_mib, char* name, size_t name_len)
{
    int i = 0;
    spin_lock(&__creating_new_disk);
    if(find_device_by_name(name)){
        pr_err("Device with name %s already exists\n", name);
        spin_unlock(&__creating_new_disk);
        return -EINVAL;
    }
    pr_info("adding new sbdd..\n");

    for(;i < MAX_DEVICES; i++){
        int res = memcmp(&__devices[i], &__zero_sbdd, sizeof (struct sbdd));
        if(!res){
            spin_unlock(&__creating_new_disk);
            return sbdd_setup(&__devices[i], i, capacity_mib, name, name_len);
        }
    }
    pr_info("too many devices\n");
    spin_unlock(&__creating_new_disk);
    return 1;
}

static int sbdd_create(void)
{
	int ret = 0;

	/*
	This call is somewhat redundant, but used anyways by tradition.
	The number is to be displayed in /proc/devices (0 for auto).
	*/
	pr_info("registering blkdev\n");
	__sbdd_major = register_blkdev(0, SBDD_NAME);
	if (__sbdd_major < 0) {
		pr_err("call register_blkdev() failed with %d\n", __sbdd_major);
		return -EBUSY;
	}
    __devices = kcalloc(MAX_DEVICES,sizeof(struct sbdd), GFP_KERNEL);
    if(!__devices){
        pr_err("cannot allocate memory for the devices\n");
        return -ENOMEM;
    }
    if(__mode == AUTO){
        int i;
        for(i = 0; i < MAX_DEVICES; i++){
            char name[5] = {0};
            sprintf(name, "%s%x", SBDEV_NAME, i);
            add_new_sbdd(__sbdd_capacity_mib, name, 5);
        }
    }
	return ret;
}

static void sbdd_destroy(struct sbdd *dev){
    atomic_set(&dev->deleting, 1);

    wait_event(dev->exitwait, !atomic_read(&dev->refs_cnt));

    sbdd_device_unregister(dev);

    /* gd will be removed only after the last reference put */
    if (dev->gd) {
        pr_info("deleting disk\n");
        del_gendisk(dev->gd);
    }

    if (dev->q) {
        pr_info("cleaning up queue\n");
        blk_cleanup_queue(dev->q);
    }

    if (dev->gd)
        put_disk(dev->gd);

#ifdef BLK_MQ_MODE
    if (dev->tag_set && dev->tag_set->tags) {
        pr_info("freeing tag_set\n");
        blk_mq_free_tag_set(dev->tag_set);
    }

    if (dev->tag_set)
        kfree(dev->tag_set);
#endif

    if (dev->data) {
        pr_info("freeing data\n");
        vfree(dev->data);
    }
    memset(dev, 0, sizeof(struct sbdd));
}

static void sbdd_delete(void)
{
    u8 i;
    for(i = 0; i < MAX_DEVICES; i++){
        if(!memcmp(&__devices[i], &__zero_sbdd, sizeof(struct sbdd)))
            break;
        sbdd_destroy(&__devices[i]);
    }
	if (__sbdd_major > 0) {
		pr_info("unregistering blkdev\n");
		unregister_blkdev(__sbdd_major, SBDD_NAME);
		__sbdd_major = 0;
	}
    kvfree(__devices);
}

/*
Note __init is for the kernel to drop this function after
initialization complete making its memory available for other uses.
There is also __initdata note, same but used for variables.
*/
static int __init sbdd_init(void)
{

	int ret = 0;
    memset(&__zero_sbdd, 0, sizeof (struct sbdd));
    spin_lock_init(&__creating_new_disk);
	pr_info("starting initialization...\n");
    check_mode();
    ret = sbdd_bus_register();
    if(ret){
        pr_warn("initialization failed\n");
        goto unregister_bus;
    }
    ret = register_sbd_driver(&sbddrv);
    if(ret){
        pr_warn("initialization failed\n");
        goto unregister_driver;
    }
	ret = sbdd_create();

	if (ret) {
		pr_warn("initialization failed\n");
        goto sbdd_delete;
	} else {
		pr_info("initialization complete\n");
        return ret;
	}
    sbdd_delete: sbdd_delete();
    unregister_driver: unregister_sbd_driver(&sbddrv);
    unregister_bus: sbdd_bus_unregister();
	return ret;
}

/*
Note __exit is for the compiler to place this code in a special ELF section.
Sometimes such functions are simply discarded (e.g. when module is built
directly into the kernel). There is also __exitdata note.
*/
static void __exit sbdd_exit(void)
{
	pr_info("exiting...\n");
	sbdd_delete();
    unregister_sbd_driver(&sbddrv);
    sbdd_bus_unregister();
	pr_info("exiting complete\n");
}

/* Called on module loading. Is mandatory. */
module_init(sbdd_init);

/* Called on module unloading. Unloading module is not allowed without it. */
module_exit(sbdd_exit);

/* Set desired capacity with insmod */
module_param_named(capacity_mib, __sbdd_capacity_mib, ulong, S_IRUGO);

/* Set driver mode: 0 - disks are created automatically, 1 - disks are created by user */
module_param_named(mode, __pre_mode, uint, S_IRUGO);

/* Note for the kernel: a free license module. A warning will be outputted without it. */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Simple Block Device Driver");
