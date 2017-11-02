#include <net/pkt_sched.h>

#include "qos_marktbl_main.h"
#include "qos_marktbl_tc.h"


char QMARK_LAN_DEV[16] = "eth0";
char QMARK_WAN_DEV[16] = "agl0";

char *qmark_lan_dev = QMARK_LAN_DEV;
char *qmark_wan_dev = QMARK_WAN_DEV;

static struct socket *qmark_sock = NULL;
static struct sockaddr_nl qmark_nladdr;

static int qmark_tc_rtnl_talk(struct nlmsghdr *n) {
    int ret;
    struct sockaddr_nl nladdr;
    struct msghdr msg;
    struct iovec iov;
    mm_segment_t oldfs;

    memset(&msg, 0, sizeof(msg));
    memset(&iov, 0, sizeof(iov));

    iov.iov_base = (void*)n;
    iov.iov_len = n->nlmsg_len;

    msg.msg_name = &nladdr;
    msg.msg_namelen = sizeof(nladdr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    memset(&nladdr, 0, sizeof(nladdr));
    nladdr.nl_family = AF_NETLINK;
    nladdr.nl_pid = 0;
    nladdr.nl_groups = 0;

    n->nlmsg_flags |= NLM_F_ACK;

    if (rtnl_is_locked()) {
        printk(KERN_WARNING "qmark_tc_msg: rtnetlink is locked, skip tc sock_sendmsg.\n");
        return ERROR;
    }
    oldfs = get_fs(); set_fs(KERNEL_DS);
    ret = sock_sendmsg(qmark_sock, &msg, n->nlmsg_len);
    set_fs(oldfs);

    if (ret < 0) {
        printk(KERN_WARNING "qmark_tc_err: cannot talk to rtnetlink\n");
        return ERROR;
    } else if (n->nlmsg_len != ret) {
        printk(KERN_INFO "qmark_tc_msg: kernel_sendmsg return %d != %d(nlmsg_len)\n", ret, n->nlmsg_len);
    }

    return OK;
}

static int qmark_tc_addattr(struct nlmsghdr *n, int maxlen, int type, const void *data,
          int alen)
{
    int len = RTA_LENGTH(alen);
    struct rtattr *rta;

    if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len) > maxlen) {
        printk(KERN_WARNING "qmark_tc_err: message exceeded bound of %d\n", maxlen);
        return ERROR;
    }
    rta = NLMSG_TAIL(n);
    rta->rta_type = type;
    rta->rta_len = len;
    memcpy(RTA_DATA(rta), data, alen);
    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len);
    return OK;
}

static int qmark_tc_calc_rtable(struct tc_ratespec *r, uint32_t *rtab,
           int cell_log, unsigned int mtu)
{
    int i;
    unsigned int sz;
    unsigned int bps = r->rate;
    unsigned int mpu = r->mpu;

    if (mtu == 0)
        mtu = 2047;

    if (cell_log < 0) {
        cell_log = 0;
        while ((mtu >> cell_log) > 255)
            cell_log++;
    }

    for (i=0; i<256; i++) {
        sz = (i + 1) << cell_log;
        if (sz < mpu) sz = mpu;
        rtab[i] = (100 * sz * PSCHED_NS2TICKS(NSEC_PER_MSEC)) / (bps / 10);
    }

    r->cell_align = -1; // Due to the sz calc
    r->cell_log = cell_log;
    r->linklayer = (1 & TC_LINKLAYER_MASK);
    return cell_log;
}

/* tc operation */
static void tc_htb_opt_parse(struct nlmsghdr *n, uint32_t rate, uint32_t ceil, unsigned int mtu) {
    struct tc_htb_opt opt;
    struct rtattr *tail;
    uint32_t rtab[256], ctab[256];
    int cell_log, ccell_log;
    uint32_t buffer, cbuffer;

    memset(&opt, 0, sizeof(opt));
    cell_log = -1;
    ccell_log = -1;
    rate = (rate * 1000) / 8; //byte per second
    ceil = (ceil * 1000) / 8;

    // buffer = (rate/100)*2 here, rate is Bps, buffer is Byte
    buffer = rate / 50;
    cbuffer = ceil / 50;

    if (0 == rate) {
        rate = 125000000;
    }
    if (0 == ceil) {
        ceil = 125000000;
    }
    if (buffer < mtu) buffer = mtu;
    if (cbuffer < mtu) cbuffer = mtu;

    opt.rate.rate = rate;
    opt.ceil.rate = ceil;

    opt.ceil.overhead = 0;
    opt.rate.overhead = 0;

    opt.ceil.mpu = 0;
    opt.rate.mpu = 0;

    qmark_tc_calc_rtable(&opt.rate, rtab, cell_log, mtu);
    // (buffer/rate) * TIME_UNITS_PER_SEC * tick_in_usec
    // (buffer/rate) == 1/50 here, TIME_UNITS_PER_SEC == 1000000 here
    opt.buffer = 20 * PSCHED_NS2TICKS(NSEC_PER_MSEC);

    qmark_tc_calc_rtable(&opt.ceil, ctab, ccell_log, mtu);
    opt.cbuffer = 20 * PSCHED_NS2TICKS(NSEC_PER_MSEC);

    tail = NLMSG_TAIL(n);
    qmark_tc_addattr(n, 1024, TCA_OPTIONS, NULL, 0);
    qmark_tc_addattr(n, 2024, TCA_HTB_PARMS, &opt, sizeof(opt));
    qmark_tc_addattr(n, 3024, TCA_HTB_RTAB, rtab, 1024);
    qmark_tc_addattr(n, 4024, TCA_HTB_CTAB, ctab, 1024);
    tail->rta_len = (void *) NLMSG_TAIL(n) - (void *) tail;
}

static int tc_htb_class_modify(int cmd, unsigned int flags, ruletbl_node_t* rn, uint32_t mark) {
    struct rtnl_req req;
    struct net_device *dev;
    unsigned int dev_mtu;
    uint32_t mark_offset = mark >> MARK_BASE;
    int i;

    for (i = 0; i < 2; i++) {
        memset(&req, 0, sizeof(req));

        req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg));
        req.n.nlmsg_flags = NLM_F_REQUEST | flags;
        req.n.nlmsg_type = cmd;
        req.t.tcm_family = AF_UNSPEC;

        if (0 == rn->devid) {
            req.t.tcm_handle = (1<<16) | (mark_offset + 1);
            req.t.tcm_parent = (1<<16) | 1;
        } else {
            req.t.tcm_handle = (1<<16) | ((rn->devid+1)*TC_MAX_IP_CNT+mark_offset);
            req.t.tcm_parent = (1<<16) | (rn->devid+1);
        }

        if (0 == i) {
            dev = dev_get_by_name(&init_net, qmark_lan_dev);
        } else {
            dev = dev_get_by_name(&init_net, qmark_wan_dev);
        }
        if (dev != NULL) {
            req.t.tcm_ifindex = dev->ifindex;
            dev_mtu = dev->mtu;
            dev_put(dev);
        } else {
            return ERROR;
        }

        qmark_tc_addattr(&req.n, sizeof(req), TCA_KIND, "htb", 4);

        if (cmd == RTM_NEWTCLASS) {
            if (0 == i) {
                tc_htb_opt_parse(&req.n, rn->rate_min_mate, rn->rate_mate, dev_mtu);
            } else {
                tc_htb_opt_parse(&req.n, rn->rate_min, rn->rate, dev_mtu);
            }
        }
        qmark_tc_rtnl_talk(&req.n);
    }

    return OK;
}

static int tc_sfq_qdisc_modify(int cmd, unsigned int flags, ruletbl_node_t* rn, uint32_t mark) {
    struct rtnl_req req;
    // char d[16]; // dev name
    // char k[16]; // qdisc name
    // struct tc_sfq_qopt_v1 opt;
    struct net_device *lan_dev, *wan_dev;

    uint32_t mark_offset = mark >> MARK_BASE;

    memset(&req, 0, sizeof(req));
    // memset(&opt, 0, sizeof(opt));

    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg));
    req.n.nlmsg_flags = NLM_F_REQUEST | flags;
    req.n.nlmsg_type = cmd;
    req.t.tcm_family = AF_UNSPEC;
    if (0 == rn->devid) {
        req.t.tcm_handle = (mark_offset + 1) << 16;
        req.t.tcm_parent = (1<<16) | (mark_offset + 1);
    } else {
        req.t.tcm_handle = ((rn->devid+1)*TC_MAX_IP_CNT+mark_offset) << 16;
        req.t.tcm_parent = (1<<16) | ((rn->devid+1)*TC_MAX_IP_CNT+mark_offset);
    }

    lan_dev = dev_get_by_name(&init_net, qmark_lan_dev);
    if (lan_dev != NULL) {
        req.t.tcm_ifindex = lan_dev->ifindex;
        dev_put(lan_dev);
    } else {
        return ERROR;
    }

    qmark_tc_addattr(&req.n, sizeof(req), TCA_KIND, "sfq", 4);
    // qmark_tc_addattr(&req.n, 1024, TCA_OPTIONS, &opt, sizeof(opt));

    qmark_tc_rtnl_talk(&req.n);

    wan_dev = dev_get_by_name(&init_net, qmark_wan_dev);

    if (wan_dev != NULL) {
        req.t.tcm_ifindex = wan_dev->ifindex;
        dev_put(wan_dev);
    } else {
        return ERROR;
    }

    qmark_tc_rtnl_talk(&req.n);

    return OK;
}

static int tc_fw_filter_modify(int cmd, unsigned int flags, ruletbl_node_t* rn, uint32_t mark) {
    struct rtnl_req req;
    struct net_device *dev;
    struct rtattr *tail;
    int i;

    uint32_t prio = PRIO_FILTER;
    uint32_t protocol = 0;
    uint32_t mask = 0xfff800;
    uint32_t flowid;
    uint32_t mark_offset = mark >> MARK_BASE;


    for (i = 0; i < 2; i++) {
        memset(&req, 0, sizeof(req));

        req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg));
        req.n.nlmsg_flags = NLM_F_REQUEST | flags;
        req.n.nlmsg_type = cmd;
        req.t.tcm_family = AF_UNSPEC;

        if (cmd == RTM_NEWTFILTER && flags & NLM_F_CREATE)
            protocol = htons(ETH_P_ALL);

        req.t.tcm_parent = 1<<16;
        req.t.tcm_info = TC_H_MAKE(prio<<16, protocol);
        req.t.tcm_handle = mark;

        if (0 == rn->devid) {
            flowid = (1 << 16) | (mark_offset + 1);
        } else {
            flowid = (1<<16) | ((rn->devid+1)*TC_MAX_IP_CNT+mark_offset);
        }

        qmark_tc_addattr(&req.n, sizeof(req), TCA_KIND, "fw", 3);

        tail = NLMSG_TAIL(&req.n);
        qmark_tc_addattr(&req.n, 4096, TCA_OPTIONS, NULL, 0);
        qmark_tc_addattr(&req.n, 16384, TCA_FW_MASK, &mask, 4);
        qmark_tc_addattr(&req.n, 4096, TCA_FW_CLASSID, &flowid, 4);
        tail->rta_len = (void *) NLMSG_TAIL(&req.n) - (void *) tail;

        if (0 == i) {
            dev = dev_get_by_name(&init_net, qmark_lan_dev);
        } else {
            dev = dev_get_by_name(&init_net, qmark_wan_dev);
        }
        if (dev != NULL) {
            req.t.tcm_ifindex = dev->ifindex;
            dev_put(dev);
        } else {
            return ERROR;
        }

        qmark_tc_rtnl_talk(&req.n);
    }
    return OK;
}

void qmark_tc_add_rule(ruletbl_node_t* rn, uint32_t mark) {
    // tc class add dev $dev parent 1:$pid classid 1:$cid htb
    //     rate $rate_min ceil $rate burst $burst cburst $cburst
    tc_htb_class_modify(RTM_NEWTCLASS, NLM_F_EXCL|NLM_F_CREATE, rn, mark);

    // tc qdisc add dev $dev parent 1:$cid handle $hid sfq
    tc_sfq_qdisc_modify(RTM_NEWQDISC, NLM_F_EXCL|NLM_F_CREATE, rn, mark);

    // tc filter add dev $dev parent 1: prio $PRIO_FILTER
    //     handle $nfmark/$QOS_MARK_MASK fw flowid 1:$cid
    tc_fw_filter_modify(RTM_NEWTFILTER, NLM_F_EXCL|NLM_F_CREATE, rn, mark);
}

void qmark_tc_del_rule(ruletbl_node_t* rn, uint32_t mark) {
    // tc filter del dev $dev parent 1:0 prio $PRIO_FILTER
    //     handle $nfmark/$QOS_MARK_MASK fw flowid 1:$cid
    tc_fw_filter_modify(RTM_DELTFILTER, 0, rn, mark);

    // tc qdisc del dev $dev parent 1:$cid handle $hid
    // delelte relative qdisc is unnecessary, since it will be deleted by its parent
    // tc_sfq_qdisc_modify(RTM_DELQDISC, 0, rn, mark);

    // tc class del dev $dev classid 1:$cid
    tc_htb_class_modify(RTM_DELTCLASS, 0, rn, mark);
}

int qmark_tc_sock_init(void) {
    unsigned int addr_len;

    int ret;
    int sndbuf = 32768;
    int rcvbuf = 1024 * 1024;

    memset(&qmark_nladdr, 0, sizeof(qmark_nladdr));

    qmark_nladdr.nl_family = AF_NETLINK;
    qmark_nladdr.nl_groups = 0;

    ret = sock_create_kern(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE, &qmark_sock);
    if (ret < 0) {
        printk(KERN_WARNING "qmark_tc_err: sock_create_kern error\n");
        goto sock_exit;
    }
    ret = kernel_setsockopt(qmark_sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    if (ret < 0) {
        printk(KERN_WARNING "qmark_tc_err: kernel_setsockopt sndbuf error\n");
        goto sock_exit;
    }
    ret = kernel_setsockopt(qmark_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    if (ret < 0) {
        printk(KERN_WARNING "qmark_tc_err: kernel_setsockopt rcvbuf error\n");
        goto sock_exit;
    }
    ret = kernel_bind(qmark_sock, (struct sockaddr*)&qmark_nladdr, sizeof(qmark_nladdr));
    if (ret < 0) {
        printk(KERN_WARNING "qmark_tc_err: kernel_bind error\n");
        goto sock_exit;
    }
    addr_len = sizeof(qmark_nladdr);
    ret = kernel_getsockname(qmark_sock, (struct sockaddr*)&qmark_nladdr, &addr_len);
    if (ret < 0) {
        printk(KERN_WARNING "qmark_tc_err: kernel_getsockname error\n");
        goto sock_exit;
    }
    if (addr_len != sizeof(qmark_nladdr)) {
        printk(KERN_WARNING "qmark_tc_err: wrong address length %d\n", addr_len);
        goto sock_exit;
    }
    if (qmark_nladdr.nl_family != AF_NETLINK) {
        printk(KERN_WARNING "qmark_tc_err: wrong address family %d\n", qmark_nladdr.nl_family);
        goto sock_exit;
    }
    return OK;

sock_exit:
    if (qmark_sock) {
        sock_release(qmark_sock);
        qmark_sock = NULL;
    }
    return ERROR;
}

void qmark_tc_sock_exit(void) {
    if (qmark_sock) {
        sock_release(qmark_sock);
        qmark_sock = NULL;
    }
}

/* Export symbol */
EXPORT_SYMBOL(qmark_lan_dev);
EXPORT_SYMBOL(qmark_wan_dev);

