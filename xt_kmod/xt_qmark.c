#include <linux/module.h>
#include <linux/skbuff.h>

#include <linux/ip.h>
#include <linux/netfilter/x_tables.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("chenxusheng@tp-link.com.cn");
MODULE_DESCRIPTION("Xtables: packet qos mark operations");
MODULE_ALIAS("ipt_qmark");
MODULE_ALIAS("ip6t_qmark");
MODULE_ALIAS("ipt_QMARK");
MODULE_ALIAS("ip6t_QMARK");

struct xt_qmark_tginfo {
    uint32_t mask;
    uint32_t id;
    uint32_t dir;
};

extern void qmark_find_hash(__be32 src_ip, uint32_t *mark);
extern int qmark_alloc_mark_auto(__be32 src_ip, uint32_t id, uint32_t *mark);


static void qmark_handle(struct sk_buff *skb, const struct xt_qmark_tginfo *info) {
    uint32_t mask = info->mask;
    uint32_t id = info->id;
    uint32_t mark = 0;
    struct iphdr *iph = ip_hdr(skb);
    __be32 ip_addr;
    if (info->dir == 0) {
        ip_addr = iph->saddr;
    } else {
        ip_addr = iph->daddr;
    }

    qmark_find_hash(ip_addr, &mark);

    if (mark == 0) {
        if (0 != qmark_alloc_mark_auto(ip_addr, id, &mark)) {
            return;
        }
    }

    skb->mark = (skb->mark & ~mask) ^ mark;

    return;
}

static unsigned int
qmark_tg(struct sk_buff *skb, const struct xt_action_param *par) {
    struct iphdr *iph = NULL;

    const struct xt_qmark_tginfo *info = par->targinfo;

    if (NULL == skb) {
        printk(KERN_ERR "qmark_target: skb is null\n");
    }

    iph = ip_hdr(skb);
    if (!iph) {
        return XT_CONTINUE;
    }

    qmark_handle(skb, info);
    return XT_CONTINUE;
}

static struct xt_target qmark_tg_reg __read_mostly = {
    .name           = "qmark",
    .revision       = 0,
    .family         = NFPROTO_UNSPEC,
    .target         = qmark_tg,
    .targetsize     = sizeof(struct xt_qmark_tginfo),
    .me             = THIS_MODULE,
};

static int __init qmark_tg_init(void) {
    int ret;

    ret = xt_register_target(&qmark_tg_reg);
    if (ret < 0)
        return ret;

    return 0;
}

static void __exit qmark_tg_exit(void) {
    xt_unregister_target(&qmark_tg_reg);
}

module_init(qmark_tg_init);
module_exit(qmark_tg_exit);
