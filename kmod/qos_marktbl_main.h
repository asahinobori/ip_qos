#ifndef _QOS_MARKTBL_MAIN_H
#define _QOS_MARKTBL_MAIN_H

#include <linux/list.h>

#define OK               0
#define ERROR           -1
#define COUNT_NOZERO    -2
#define NO_EXIST        -3
#define OVER_MAX        -4
#define ALLOC_FAIL      -5
#define EXIST           -6

// rule mode
#define PRIV_MODE 0
#define SHARE_MODE 1

// default spec
#define TC_MAX_IP_CNT 4096
#define MAX_RULE_CNT 128
#define MARK_BASE 11

typedef struct marktbl_head {
    unsigned int elem_num;  /* The number of elements currently in the hash table */
    unsigned int size;      /* The size of the hash table */
    struct hlist_head *ht;  /* The hash table */
    spinlock_t lock;
} marktbl_head_t;


typedef struct marktbl_node {
    __be32 src_ip;
    uint32_t id;
    uint32_t mark;
    atomic_t count; // reference count
    struct hlist_node hln;
} marktbl_node_t;

typedef struct marktbl_node_key {
    __be32 src_ip;
} marktbl_node_key_t;

typedef struct ruletbl_node {
    uint32_t mode;
    uint32_t mark; // for share mode, priv mode is 0
    atomic_t count;
    bool on;
    uint32_t devid;
    uint32_t rate;
    uint32_t rate_mate;
    uint32_t rate_min;
    uint32_t rate_min_mate;
} ruletbl_node_t;

void qmark_add_rule(uint32_t id, uint32_t mode, uint32_t devid, uint32_t rate, uint32_t rate_mate,
                    uint32_t rate_min, uint32_t rate_min_mate);
void qmark_del_rule(uint32_t id);
void qmark_list_rule(void);
void qmark_flush_rule(void);
int qmark_find_hash(__be32 src_ip, uint32_t *mark);
int qmark_add_hash(__be32 src_ip, uint32_t id, uint32_t *mark);
int qmark_del_hash(__be32 src_ip, uint32_t *mark);
void qmark_list_hash(void);
void qmark_flush_hash(void);
int qmark_alloc_mark(unsigned int *mark);
void qmark_free_mark(unsigned int *mark);
void qmark_flush_mark(void);
void qmark_set_spec(uint32_t ip_spec, uint32_t rule_cnt);
void qmark_dump(void);
void qmark_reset(uint32_t flag);

#endif
