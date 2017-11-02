#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/ioctl.h>
#include <linux/device.h>
#include <linux/uaccess.h>

#include "qos_marktbl_ioctl.h"
#include "qos_marktbl_main.h"
#include "qos_marktbl_tc.h"

#define QMARK_CDEV_NAME     "qmarkcdev"
#define QMARK_CLASS_NAME    "qmark"
#define QMARK_DEV_NAME      "qmarkctl"
#define QMARK_CDEV_NUM      1

static int qmark_cdev_major = 0;
static struct cdev *qmark_cdev = NULL;
static struct class *qmark_class = NULL;

extern spinlock_t qmark_op_lock;
extern ruletbl_node_t *g_ruletbl;
extern char *qmark_lan_dev;
extern char *qmark_wan_dev;

typedef struct qmark_rule {
    uint32_t id;
    uint32_t mode;
    uint32_t devid;
    uint32_t rate;
    uint32_t rate_mate;
    uint32_t rate_min;
    uint32_t rate_min_mate;
} qmark_rule_t;

typedef struct qmark_hash {
    __be32 ip;
    uint32_t id;
    uint32_t mark;
} qmark_hash_t;

typedef struct qmark_mark {
    uint32_t mark;
} qmark_mark_t;

typedef struct qmark_netdev {
    char lan_dev[16];
    char wan_dev[16];
} qmark_netdev_t;

typedef struct qmark_spec {
    uint32_t max_ip_cnt;
    uint32_t max_rule_cnt;
} qmark_spec_t;

#define QMARK_IOC_MAGIC 'q'
enum qmark_ioc_t {
    QMARK_IOC_RULE_ADD = _IOWR(QMARK_IOC_MAGIC, 0x1, qmark_rule_t),
    QMARK_IOC_RULE_DEL = _IOWR(QMARK_IOC_MAGIC, 0x2, qmark_rule_t),
    QMARK_IOC_RULE_LST = _IO(QMARK_IOC_MAGIC, 0x3),
    QMARK_IOC_RULE_FLUSH = _IO(QMARK_IOC_MAGIC, 0x4),
    QMARK_IOC_HASH_ADD = _IOWR(QMARK_IOC_MAGIC, 0x5, qmark_hash_t),
    QMARK_IOC_HASH_DEL = _IOWR(QMARK_IOC_MAGIC, 0x6, qmark_hash_t),
    QMARK_IOC_HASH_CHK = _IOWR(QMARK_IOC_MAGIC, 0x7, qmark_hash_t),
    QMARK_IOC_HASH_LST = _IO(QMARK_IOC_MAGIC, 0x8),
    QMARK_IOC_HASH_FLUSH = _IO(QMARK_IOC_MAGIC, 0x9),
    QMARK_IOC_MARK_ALO = _IOWR(QMARK_IOC_MAGIC, 0xa, qmark_mark_t),
    QMARK_IOC_MARK_FREE = _IOWR(QMARK_IOC_MAGIC, 0xb, qmark_mark_t),
    QMARK_IOC_MARK_FLUSH = _IO(QMARK_IOC_MAGIC, 0xc),
    QMARK_IOC_DUMP = _IO(QMARK_IOC_MAGIC, 0xd),
    QMARK_IOC_RESET = _IOWR(QMARK_IOC_MAGIC, 0xe, qmark_mark_t),
    QMARK_IOC_NETDEV_SET = _IOWR(QMARK_IOC_MAGIC, 0xf, qmark_netdev_t),
    QMARK_IOC_SPEC_SET = _IOWR(QMARK_IOC_MAGIC, 0x10, qmark_spec_t),
    QMARK_IOC_TC_ADD = _IO(QMARK_IOC_MAGIC, 0x11),
    QMARK_IOC_TC_DEL = _IO(QMARK_IOC_MAGIC, 0x12),

    QMARK_IOC_MAX
};

long qmark_cdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    uint32_t id = 0;
    uint32_t mark = 0;
    int ret = 0;
    qmark_rule_t qr = { 0 };
    qmark_hash_t qh = { 0 };
    qmark_mark_t qm = { 0 };
    qmark_netdev_t qn;
    qmark_spec_t qs;
    memset(&qn, 0, sizeof(qmark_netdev_t));

    if (_IOC_TYPE(cmd) != QMARK_IOC_MAGIC) {
        printk(KERN_WARNING "qmark_ioctl_err: Invalid ioctl magic 0x%x\n", cmd);
        return -EINVAL;
    }
    if (_IOC_NR(cmd) > QMARK_IOC_MAX) {
        printk(KERN_WARNING "qmark_ioctl_err: Invalid ioctl command 0x%x\n", cmd);
        return -EINVAL;
    }

    switch (cmd) {
    case QMARK_IOC_RULE_ADD:
        ret = copy_from_user(&qr, (qmark_rule_t *)arg, sizeof(qmark_rule_t));
        if (ret < 0) {
            printk(KERN_WARNING "qmark_ioctl_err: copy_from_user failed, qmark rule add error\n");
            return -EINVAL;
        }
        qmark_add_rule(qr.id, qr.mode, qr.devid, qr.rate, qr.rate_mate, qr.rate_min, qr.rate_min_mate);

        break;

    case QMARK_IOC_RULE_DEL:
        ret = copy_from_user(&qr, (qmark_rule_t *)arg, sizeof(qmark_rule_t));
        if (ret < 0) {
            printk(KERN_WARNING "qmark_ioctl_err: copy_from_user failed, qmark rule del error\n");
            return -EINVAL;
        }
        qmark_del_rule(qr.id);
        break;

    case QMARK_IOC_RULE_LST:
        qmark_list_rule();
        break;

    case QMARK_IOC_RULE_FLUSH:
        qmark_flush_rule();
        break;

    case QMARK_IOC_HASH_ADD:
        ret = copy_from_user(&qh, (qmark_hash_t *)arg, sizeof(qmark_hash_t));
        if (ret < 0) {
            printk(KERN_WARNING "qmark_ioctl_err: copy_from_user failed, qmark hash add error\n");
            return -EINVAL;
        }
        if (0 == qh.ip) {
            printk(KERN_WARNING "qmark_ioctl_err: ip could not be zero, qmark hash add error\n");
            return -EINVAL;
        }
        id = qh.id;
        mark = qh.mark;
        ret = qmark_add_hash(qh.ip, id, &mark);
        if (ret < 0) {
            printk(KERN_WARNING "qmark_ioctl_err: add hash failed, qmark hash add error\n");
            return -EINVAL;
        }
        break;

    case QMARK_IOC_HASH_DEL:
        ret = copy_from_user(&qh, (qmark_hash_t *)arg, sizeof(qmark_hash_t));
        if (ret < 0) {
            printk(KERN_WARNING "qmark_ioctl_err: copy_from_user failed, qmark hash del error\n");
            return -EINVAL;
        }
        if (0 == qh.ip) {
            printk(KERN_WARNING "qmark_ioctl_err: ip could not be zero, qmark hash del error\n");
            return -EINVAL;
        }
        ret = qmark_del_hash(qh.ip, &mark);
        if (ret < 0) {
            printk(KERN_WARNING "qmark_ioctl_err: del hash failed, qmark hash del error\n");
            return -EINVAL;
        }
        printk(KERN_INFO "qmark_ioctl_msg: del hash return mark = 0x%x\n", mark);
        break;

    case QMARK_IOC_HASH_CHK:
        ret = copy_from_user(&qh, (qmark_hash_t *)arg, sizeof(qmark_hash_t));
        if (ret < 0) {
            printk(KERN_WARNING "qmark_ioctl_err: copy_from_user failed, qmark hash check error\n");
            return -EINVAL;
        }
        if (0 == qh.ip) {
            printk(KERN_WARNING "qmark_ioctl_err: ip could not be zero, qmark hash check error\n");
            return -EINVAL;
        }
        ret = qmark_find_hash(qh.ip, &mark);
        qh.mark = mark;
        ret = copy_to_user((qmark_hash_t *)arg, &qh, sizeof(qmark_hash_t));
        if (ret < 0) {
            printk(KERN_WARNING "qmark_ioctl_err: copy_to_user failed, qmark hash check error\n");
            return -EINVAL;
        }
        break;

    case QMARK_IOC_HASH_LST:
        qmark_list_hash();
        break;

    case QMARK_IOC_HASH_FLUSH:
        qmark_flush_hash();
        break;

    case QMARK_IOC_MARK_ALO:
        ret = qmark_alloc_mark(&mark);
        qm.mark = mark;
        ret = copy_to_user((qmark_mark_t *)arg, &qm, sizeof(qmark_mark_t));
        if (ret < 0) {
            printk(KERN_WARNING "qmark_ioctl_err: copy_to_user failed, qmark mark allocate error\n");
            return -EINVAL;
        }
        break;

    case QMARK_IOC_MARK_FREE:
        ret = copy_from_user(&qm, (qmark_mark_t *)arg, sizeof(qmark_mark_t));
        if (ret < 0) {
            printk(KERN_WARNING "qmark_ioctl_err: copy_from_user failed, qmark mark free error\n");
            return -EINVAL;
        }
        mark = qm.mark;
        qmark_free_mark(&mark);
        break;

    case QMARK_IOC_MARK_FLUSH:
        qmark_flush_mark();
        break;

    case QMARK_IOC_DUMP:
        qmark_dump();
        break;

    case QMARK_IOC_RESET:
        ret = copy_from_user(&qm, (qmark_mark_t *)arg, sizeof(qmark_mark_t));
        if (ret < 0) {
            printk(KERN_WARNING "qmark_ioctl_err: copy_from_user failed, qmark reset error\n");
            return -EINVAL;
        }
        qmark_reset(qm.mark);
        break;

    case QMARK_IOC_NETDEV_SET:
        ret = copy_from_user(&qn, (qmark_netdev_t *)arg, sizeof(qmark_netdev_t));
        if (ret < 0) {
            printk(KERN_WARNING "qmark_ioctl_err: copy_from_user failed, qmark netdev set error\n");
            return -EINVAL;
        }
        spin_lock_bh(&qmark_op_lock);
        memcpy(qmark_lan_dev, qn.lan_dev, 16);
        memcpy(qmark_wan_dev, qn.wan_dev, 16);
        spin_unlock_bh(&qmark_op_lock);

        break;

    case QMARK_IOC_SPEC_SET:
        ret = copy_from_user(&qs, (qmark_spec_t *)arg, sizeof(qmark_spec_t));
        if (ret < 0) {
            printk(KERN_WARNING "qmark_ioctl_err: copy_from_user failed, qmark spec set error\n");
            return -EINVAL;
        }

        qmark_set_spec(qs.max_ip_cnt, qs.max_rule_cnt);

        break;

    case QMARK_IOC_TC_ADD:
        qmark_tc_add_rule(&g_ruletbl[0], 0x800);
        break;

    case QMARK_IOC_TC_DEL:
        qmark_tc_del_rule(&g_ruletbl[0], 0x800);
        break;

    default:
        printk(KERN_WARNING "qmark_ioctl_err: Invalid ioctl command 0x%x\n", cmd);
        return -EINVAL;
    }
    return 0;
}

const struct file_operations qmark_cdev_fops = {
    .owner = THIS_MODULE,
    .open = NULL,
    .release = NULL,
    .read = NULL,
    .write = NULL,
    .unlocked_ioctl = qmark_cdev_ioctl,
};

int qmark_dev_init(void) {
    int ret = 0;
    dev_t devid = MKDEV(qmark_cdev_major, 0);
    if (qmark_cdev_major) {
        ret = register_chrdev_region(devid, QMARK_CDEV_NUM, QMARK_CDEV_NAME);
    } else {
        ret = alloc_chrdev_region(&devid, 0, QMARK_CDEV_NUM, QMARK_CDEV_NAME);
        qmark_cdev_major = MAJOR(devid);
    }
    if (ret < 0) {
        goto alloc_chrdev_err;
    }

    qmark_cdev = cdev_alloc();
    if (NULL == qmark_cdev) {
        goto alloc_cdev_err;
    }
    qmark_cdev->owner = THIS_MODULE;
    qmark_cdev->ops = &qmark_cdev_fops;
    ret = cdev_add(qmark_cdev, devid, QMARK_CDEV_NUM);
    if (ret < 0) {
        goto add_cdev_err;
    }
    qmark_class = class_create(THIS_MODULE, QMARK_CLASS_NAME);
    if (IS_ERR(qmark_class)) {
        goto add_cdev_err;
    } else {
        if (NULL == device_create(qmark_class, NULL, devid, NULL, QMARK_DEV_NAME)) {
            goto create_device_err;
        }
    }

    return OK;

create_device_err:
    class_destroy(qmark_class);
add_cdev_err:
    cdev_del(qmark_cdev);
alloc_cdev_err:
    unregister_chrdev_region(devid, QMARK_CDEV_NUM);
alloc_chrdev_err:
    printk(KERN_WARNING "qmark dev init failed\n");

    return ERROR;
}

void qmark_dev_exit(void) {
    if (NULL != qmark_class) {
        device_destroy(qmark_class, MKDEV(qmark_cdev_major, 0));
        class_destroy(qmark_class);
        qmark_class = NULL;
    }
    if (NULL != qmark_cdev) {
        cdev_del(qmark_cdev);
        unregister_chrdev_region(MKDEV(qmark_cdev_major, 0), QMARK_CDEV_NUM);
        qmark_cdev = NULL;
        qmark_cdev_major = 0;
    }
}
