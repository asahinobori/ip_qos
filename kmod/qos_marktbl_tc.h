#ifndef _QOS_MARKTBL_TC_H
#define _QOS_MARKTBL_TC_H

#include <linux/rtnetlink.h>

#define PRIO_FILTER 3

#define TCA_BUF_MAX    (4096)
#define NLMSG_TAIL(nmsg) \
    ((struct rtattr *) (((void *) (nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

struct rtnl_req {
    struct nlmsghdr n;
    struct tcmsg t;
    char buf[TCA_BUF_MAX];
};

void qmark_tc_add_rule(ruletbl_node_t* rn, uint32_t mark);
void qmark_tc_del_rule(ruletbl_node_t* rn, uint32_t mark);
int qmark_tc_sock_init(void);
void qmark_tc_sock_exit(void);

#endif
