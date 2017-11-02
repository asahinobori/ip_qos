#include <linux/module.h>
#include <linux/slab.h>
#include <net/netfilter/nf_conntrack_ecache.h>

#include "qos_marktbl_main.h"
#include "qos_marktbl_ioctl.h"
#include "qos_marktbl_tc.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("chenxusheng@tp-link.com.cn");
MODULE_DESCRIPTION("qos mark table module");
MODULE_VERSION("0.1");


marktbl_head_t *g_marktbl_head = NULL;
ruletbl_node_t *g_ruletbl = NULL;
unsigned long *mark_bitmap = NULL;
bool reset_flag = false;
spinlock_t qmark_op_lock;

uint32_t max_ip_cnt = TC_MAX_IP_CNT;
uint32_t max_rule_cnt = MAX_RULE_CNT;

static bool elem_over_limit(void) {
    unsigned int elem_num = g_marktbl_head->elem_num;

    return ((elem_num + 1) > max_ip_cnt);
}

/* Mark bitmap operation */
static int mark_bitmap_init(uint32_t cnt) {
    mark_bitmap = kmalloc(BITS_TO_LONGS(cnt) * sizeof(unsigned long), GFP_ATOMIC);
    if(NULL == mark_bitmap) {
        return ALLOC_FAIL;
    }
    memset(mark_bitmap, 0, BITS_TO_LONGS(cnt) * sizeof(unsigned long));
    return OK;
}

static void mark_bitmap_exit() {
    if (mark_bitmap) {
        kfree(mark_bitmap);
        mark_bitmap = NULL;
    }
}

static uint32_t alloc_mark(void) {
    unsigned long first_zero_bit = 0;
    uint32_t mark_offset = 0;
    uint32_t mark = 0;

    first_zero_bit = find_first_zero_bit(mark_bitmap, max_ip_cnt);
    if (first_zero_bit >= max_ip_cnt) {
        return 0;
    }
    bitmap_set(mark_bitmap, first_zero_bit, 1);
    mark_offset = (uint32_t)first_zero_bit + 1;
    mark = mark_offset << MARK_BASE;
    return mark;
}

static void free_mark(uint32_t mark) {
    uint32_t mark_offset = mark >> MARK_BASE;
    bitmap_clear(mark_bitmap, mark_offset - 1, 1);
}

static int test_mark(uint32_t mark) {
    uint32_t mark_offset = mark >> MARK_BASE;
    return test_bit(mark_offset - 1, mark_bitmap);
}

static void flush_mark(void) {
    bitmap_zero(mark_bitmap, max_ip_cnt);
}

/* Hash related function */
static unsigned int hash_fn(const void *key_param, int key_len) {
    unsigned int a = 0;
    unsigned int b = 0;
    unsigned len;
    uint8_t *key = (uint8_t *) key_param;

    /* Set up the internal state */
    len = (unsigned int) key_len;
    a = len;

    if (len >= 4) {
        b = key[0] + (key[1] << 8) + (key[2] << 16) + (key[3] << 24);
    }

    while (len >= 8) {
        a += b;
        key += 4;
        a += ~(a << 15);
        len -= 4;
        a ^=  (a >> 10);
        b = key[0];
        a +=  (a << 3);
        b += key[1] << 8;
        a ^=  (a >> 6);
        b += key[2] << 16;
        a += ~(a << 11);
        b += key[3] << 24;
        a ^=  (a >> 16);
    }

    if (len >= 4) {
        a += b;
        a += ~(a << 15);
        len -= 4;
        a ^=  (a >> 10);
        a +=  (a << 3);
        a ^=  (a >> 6);
        a += ~(a << 11);
        a ^=  (a >> 16);
    }

    /* All the case statements fall through */
    switch (len) {
        case 3 :
            a += key[2] << 16;
        case 2 :
            a ^= key[1] << 8;
        case 1 :
            a += key[0];
        default:
            break;
    }

    return a;
}

static bool marktbl_hash_cmp(marktbl_node_t *node, marktbl_node_key_t *key) {
    return (node->src_ip == key->src_ip);
}

static marktbl_node_t *marktbl_hash_get(marktbl_node_key_t *key) {
    unsigned int hash = 0;
    unsigned int hash_index = 0;
    marktbl_node_t *entry = NULL;
    marktbl_head_t *head = g_marktbl_head;

    hash = hash_fn((void *)key, sizeof(marktbl_node_key_t));
    hash_index = hash % head->size;

    spin_lock_bh(&head->lock);
    hlist_for_each_entry(entry, &head->ht[hash_index], hln) {
        if (marktbl_hash_cmp(entry, key)) {
            spin_unlock_bh(&head->lock);
            return entry;
        }
    }
    spin_unlock_bh(&head->lock);

    return NULL;
}

static int marktbl_hash_add(marktbl_node_key_t *key, uint32_t id, uint32_t *mark) {
    unsigned int hash = 0;
    unsigned int hash_index = 0;
    marktbl_node_t *entry, *mark_node = NULL;
    marktbl_head_t *head = g_marktbl_head;

    if(elem_over_limit())
        return OVER_MAX;

    hash = hash_fn((void *)key, sizeof(marktbl_node_key_t));
    hash_index = hash % head->size;

    spin_lock_bh(&head->lock);
    hlist_for_each_entry(entry, &head->ht[hash_index], hln) {
        if (marktbl_hash_cmp(entry, key)) {
            atomic_inc(&mark_node->count);
            spin_unlock_bh(&head->lock);
            return OK;
        }
    }

    mark_node = kmalloc(sizeof(marktbl_node_t), GFP_ATOMIC);
    if(NULL == mark_node) {
        spin_unlock_bh(&head->lock);
        return ALLOC_FAIL;
    }

    mark_node->src_ip = key->src_ip;
    mark_node->id = id;
    mark_node->mark = *mark;
    atomic_set(&mark_node->count, 0);
    atomic_inc(&(g_ruletbl[id-1].count));

    hlist_add_head(&mark_node->hln, &head->ht[hash_index]);
    head->elem_num++;

    spin_unlock_bh(&head->lock);

    return OK;
}

static int marktbl_hash_remove(marktbl_node_key_t *key, marktbl_node_t **mark_node) {
    unsigned int hash = 0;
    unsigned int hash_index = 0;
    marktbl_node_t *entry = NULL;
    marktbl_head_t *head = g_marktbl_head;

    hash = hash_fn((void *)key, sizeof(marktbl_node_key_t));
    hash_index = hash % head->size;

    spin_lock_bh(&head->lock);
    hlist_for_each_entry(entry, &head->ht[hash_index], hln) {
        if (marktbl_hash_cmp(entry, key)) {
            if(atomic_read(&entry->count) > 0) {
                spin_unlock_bh(&head->lock);
                return COUNT_NOZERO;
            } else {
                __hlist_del(&entry->hln);
                head->elem_num--;

                *mark_node = entry;

                atomic_dec(&(g_ruletbl[(entry->id)-1].count));
                spin_unlock_bh(&head->lock);
                return OK;
            }
        }
    }
    spin_unlock_bh(&head->lock);

    return NO_EXIST;
}

static void marktbl_hash_clean(void) {
    unsigned int i = 0;
    marktbl_node_t *entry = NULL;
    marktbl_head_t *head = g_marktbl_head;
    struct hlist_node *node;

    spin_lock_bh(&head->lock);
    for(i = 0; i < head->size; i++) {
        hlist_for_each_entry_safe(entry, node, &head->ht[i], hln) {
            __hlist_del(&entry->hln);
            head->elem_num--;
            kfree(entry);
            entry = NULL;
        }
    }
    spin_unlock_bh(&head->lock);
}

static void marktbl_hash_list(void) {
    unsigned int i = 0;
    marktbl_node_t *entry = NULL;
    marktbl_head_t *head = g_marktbl_head;
    struct hlist_node *node;

    spin_lock_bh(&head->lock);
    printk(KERN_INFO "ip              id  mark     count\n");
    for(i = 0; i < head->size; i++) {
        hlist_for_each_entry_safe(entry, node, &head->ht[i], hln) {
            printk(KERN_INFO "%-15pI4 %-3d 0x%-6x %d\n",
                   &(entry->src_ip), entry->id, entry->mark, atomic_read(&entry->count));
        }
    }
    spin_unlock_bh(&head->lock);
}

/* qos mark table operation */
static int qos_marktbl_hashtbl_add(__be32 src_ip, uint32_t id, uint32_t *mark) {
    int ret = 0;
    marktbl_node_key_t key;
    key.src_ip = src_ip;

    if (!test_mark(*mark)) {
        printk(KERN_WARNING "mark not alloc.\n");
        return ERROR;
    }

    ret = marktbl_hash_add(&key, id, mark);
    if (ret != OK) {
        printk(KERN_WARNING "qos mark table fail to add.\n");
    }
    return ret;
}

static int qos_marktbl_hashtbl_del(__be32 src_ip, uint32_t *mark) {
    int ret = 0;
    marktbl_node_key_t key;
    marktbl_node_t *mark_node = NULL;
    key.src_ip = src_ip;

    ret = marktbl_hash_remove(&key, &mark_node);
    if (ret == OK && NULL != mark_node) {
        *mark = mark_node->mark;
        kfree(mark_node);
    } else {
        *mark = 0;
    }
    return ret;
}

static int qos_marktbl_hashtbl_get(__be32 src_ip, uint32_t *mark) {
    marktbl_node_t *mark_node = NULL;
    marktbl_node_key_t key;
    key.src_ip = src_ip;

    mark_node = marktbl_hash_get(&key);
    if (NULL != mark_node) {
        *mark = mark_node->mark;
        return OK;
    } else {
        *mark = 0;
        return NO_EXIST;
    }
}

static void qos_marktbl_hashtbl_list(void) {
    marktbl_hash_list();
}

static int qos_marktbl_hashtbl_init(void) {
    struct hlist_head *ht = NULL;
    unsigned int htsize = max_ip_cnt;
    int i = 0;

    g_marktbl_head = kmalloc(sizeof(marktbl_head_t), GFP_ATOMIC);
    if (NULL == g_marktbl_head)
        return ERROR;

    ht = kmalloc(htsize * sizeof(struct hlist_head), GFP_ATOMIC);
    if (NULL == ht) {
        kfree(g_marktbl_head);
        return ERROR;
    }

    memset(g_marktbl_head, 0, sizeof(*g_marktbl_head));
    g_marktbl_head->elem_num = 0;
    g_marktbl_head->size = htsize;
    g_marktbl_head->ht = ht;
    for (i = 0; i < htsize; i++) {
        INIT_HLIST_HEAD(&g_marktbl_head->ht[i]);
    }
    spin_lock_init(&g_marktbl_head->lock);

    return 0;
}

static void qos_marktbl_hashtbl_exit(void) {
    if (g_marktbl_head) {
        marktbl_hash_clean();

        kfree(g_marktbl_head->ht);
        g_marktbl_head->ht = NULL;

        kfree(g_marktbl_head);
        g_marktbl_head = NULL;
    }
}

static int qos_ruletbl_init(void) {
    g_ruletbl = kmalloc(sizeof(ruletbl_node_t) * max_rule_cnt, GFP_ATOMIC);
    if (NULL == g_ruletbl) {
        return ERROR;
    }

    memset(g_ruletbl, 0, sizeof(ruletbl_node_t) * max_rule_cnt);
    return OK;
}

static void qos_ruletbl_exit(void) {
    if (g_ruletbl) {
        kfree(g_ruletbl);
        g_ruletbl = NULL;
    }
}

/* conntrack event handle */
#ifdef CONFIG_NF_CONNTRACK_EVENTS
/*
 * qmark_conntrack_event()
 * Callback event invoked when a conntrack connection's state changes.
 */
#ifdef CONFIG_NF_CONNTRACK_CHAIN_EVENTS
static int qmark_conntrack_event(struct notifier_block *this,
        unsigned long events, void *ptr)
#else
static int qmark_conntrack_event(unsigned int events, struct nf_ct_event *item)
#endif
{
#ifdef CONFIG_NF_CONNTRACK_CHAIN_EVENTS
    struct nf_ct_event *item = ptr;
#endif
    struct nf_conn *ct = item->ct;
    __be32 ip_addr = 0;
    struct nf_conntrack_tuple *to = NULL;
    struct nf_conntrack_tuple *tr = NULL;

    marktbl_node_key_t key;
    marktbl_node_t *mark_node = NULL;
    uint32_t mark = 0;
    uint32_t id = 0;

    // If we don't have a conntrack entry then we're done.
    if (unlikely(!ct)) {
        return NOTIFY_DONE;
    }

    // Ignore anything other than IPv4 connections.
    if (unlikely(nf_ct_l3num(ct) != AF_INET)) {
        return NOTIFY_DONE;
    }

    if (nf_ct_is_untracked(ct))
        return NOTIFY_DONE;

    if (0 == (ct->mark & 0xfff800)){
        return NOTIFY_DONE;
    }

    to = &ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple;
    tr = &ct->tuplehash[IP_CT_DIR_REPLY].tuple;
    if (to->dst.u3.ip == tr->src.u3.ip) {
        ip_addr = to->src.u3.ip;
    } else {
        ip_addr = tr->src.u3.ip;
    }
    key.src_ip = ip_addr;

    if (events & (1 << IPCT_DESTROY)) {
        spin_lock_bh(&qmark_op_lock);
        mark_node = marktbl_hash_get(&key);
        if (mark_node) {
            atomic_dec(&mark_node->count);
            if (atomic_read(&mark_node->count) <= 0) {
                id = mark_node->id;
                qos_marktbl_hashtbl_del(ip_addr, &mark);
                if ((PRIV_MODE == g_ruletbl[id-1].mode) || (0 == atomic_read(&(g_ruletbl[id-1].count)))) {
                    g_ruletbl[id-1].mark = 0;

                    // del tc rule here
                    qmark_tc_del_rule(&g_ruletbl[id-1], mark);

                    free_mark(mark);
                }
            }
        }
        spin_unlock_bh(&qmark_op_lock);
    } else if (events & (1 << IPCT_MARK)){
        spin_lock_bh(&qmark_op_lock);
        mark_node = marktbl_hash_get(&key);
        if (mark_node) {
            atomic_inc(&mark_node->count);
        }
        spin_unlock_bh(&qmark_op_lock);
    }

    return NOTIFY_DONE;
}

/*
 * Netfilter conntrack event system to monitor connection tracking changes
 */
#ifdef CONFIG_NF_CONNTRACK_CHAIN_EVENTS
static struct notifier_block qmark_conntrack_notifier =
{
    .notifier_call = qmark_conntrack_event,
};
#else
static struct nf_ct_event_notifier qmark_conntrack_notifier =
{
    .fcn = qmark_conntrack_event,
};
#endif
#endif

static int __init qos_marktbl_init(void) {
    int ret = OK;
    spin_lock_init(&qmark_op_lock);

    ret = mark_bitmap_init(max_ip_cnt);
    if (ret < 0) goto bitmap_exit;

    ret = qmark_tc_sock_init();
    if (ret < 0) goto sock_exit;

    ret = qmark_dev_init();
    if (ret < 0) goto dev_exit;

    ret = qos_ruletbl_init();
    if (ret < 0) goto ruletbl_exit;

    ret = qos_marktbl_hashtbl_init();
    if (ret < 0) goto marktbl_exit;

#ifdef CONFIG_NF_CONNTRACK_EVENTS
    ret = nf_conntrack_register_notifier(&init_net, &qmark_conntrack_notifier);
    if (ret < 0) {
        printk(KERN_WARNING "Can't register nf notifier hook for qos marktable, ret=[%d].\n", ret);
        goto ctnotifier_exit;
    }
#endif

    return ret;

#ifdef CONFIG_NF_CONNTRACK_EVENTS
ctnotifier_exit:
    qos_marktbl_hashtbl_exit();
#endif
marktbl_exit:
    qos_ruletbl_exit();
ruletbl_exit:
    qmark_dev_exit();
dev_exit:
    qmark_tc_sock_exit();
sock_exit:
    mark_bitmap_exit();
bitmap_exit:
    return ret;
}

static void __exit qos_marktbl_exit(void) {
#ifdef CONFIG_NF_CONNTRACK_EVENTS
    nf_conntrack_unregister_notifier(&init_net, &qmark_conntrack_notifier);
#endif
    qos_marktbl_hashtbl_exit();
    qos_ruletbl_exit();
    qmark_dev_exit();
    qmark_tc_sock_exit();
    mark_bitmap_exit();
}


/* API */
void qmark_add_rule(uint32_t id, uint32_t mode, uint32_t devid, uint32_t rate, uint32_t rate_mate,
                    uint32_t rate_min, uint32_t rate_min_mate) {
    spin_lock_bh(&qmark_op_lock);
    g_ruletbl[id-1].on = true;
    g_ruletbl[id-1].mode = mode;
    g_ruletbl[id-1].devid = devid;
    g_ruletbl[id-1].rate = rate;
    g_ruletbl[id-1].rate_mate = rate_mate;
    g_ruletbl[id-1].rate_min = rate_min;
    g_ruletbl[id-1].rate_min_mate = rate_min_mate;
    spin_unlock_bh(&qmark_op_lock);
}

void qmark_del_rule(uint32_t id) {
    spin_lock_bh(&qmark_op_lock);
    memset(&g_ruletbl[id-1], 0, sizeof(ruletbl_node_t));
    g_ruletbl[id-1].on = false;
    spin_unlock_bh(&qmark_op_lock);
}

void qmark_list_rule(void) {
    int i;
    spin_lock_bh(&qmark_op_lock);
    printk(KERN_INFO "  id  mode mark     count devid rate    rate_mate rate_min rate_min_mate\n");
    for (i = 0; i < max_rule_cnt; i++) {
        printk(KERN_INFO "%c %-3d %c    0x%-6x %-4d  %-1d     %-7d %-7d   %-7d  %-7d\n",
               g_ruletbl[i].on ? '*' : '-',
               i+1, g_ruletbl[i].mode ? 'S' : 'P',
               g_ruletbl[i].mark, atomic_read(&(g_ruletbl[i].count)),
               g_ruletbl[i].devid, g_ruletbl[i].rate, g_ruletbl[i].rate_mate,
               g_ruletbl[i].rate_min, g_ruletbl[i].rate_min_mate);
    }
    spin_unlock_bh(&qmark_op_lock);
}

void qmark_flush_rule(void) {
    spin_lock_bh(&qmark_op_lock);
    qos_ruletbl_exit();
    qos_ruletbl_init();
    spin_unlock_bh(&qmark_op_lock);
}

int qmark_find_hash(__be32 src_ip, uint32_t *mark) {
    return qos_marktbl_hashtbl_get(src_ip, mark);
}

int qmark_add_hash(__be32 src_ip, uint32_t id, uint32_t *mark) {
    int ret = OK;
    spin_lock_bh(&qmark_op_lock);
    ret = qos_marktbl_hashtbl_add(src_ip, id, mark);
    spin_unlock_bh(&qmark_op_lock);
    return ret;
}

int qmark_del_hash(__be32 src_ip, uint32_t *mark) {
    marktbl_node_t *mark_node = NULL;
    marktbl_node_key_t key;
    key.src_ip = src_ip;
    uint32_t id;
    spin_lock_bh(&qmark_op_lock);
    mark_node = marktbl_hash_get(&key);
    if (mark_node) {
        atomic_dec(&mark_node->count);
        if (atomic_read(&mark_node->count) <= 0) {
            id = mark_node->id;
            qos_marktbl_hashtbl_del(src_ip, mark);
            if ((PRIV_MODE == g_ruletbl[id-1].mode) || (0 == atomic_read(&(g_ruletbl[id-1].count)))) {
                g_ruletbl[id-1].mark = 0;
                free_mark(*mark);
            }
            spin_unlock_bh(&qmark_op_lock);
            return OK;
        }
    }
    spin_unlock_bh(&qmark_op_lock);
    return ERROR;
}

void qmark_list_hash(void) {
    spin_lock_bh(&qmark_op_lock);
    qos_marktbl_hashtbl_list();
    spin_unlock_bh(&qmark_op_lock);
}

void qmark_flush_hash(void) {
    spin_lock_bh(&qmark_op_lock);
    qos_marktbl_hashtbl_exit();
    qos_marktbl_hashtbl_init();
    spin_unlock_bh(&qmark_op_lock);
}

int qmark_alloc_mark(uint32_t *mark) {
    spin_lock_bh(&qmark_op_lock);
    *mark = alloc_mark();
    if (0 == *mark) {
        spin_unlock_bh(&qmark_op_lock);
        return ERROR;
    }
    spin_unlock_bh(&qmark_op_lock);
    return OK;
}

void qmark_free_mark(uint32_t *mark) {
    spin_lock_bh(&qmark_op_lock);
    free_mark(*mark);
    spin_unlock_bh(&qmark_op_lock);
}

void qmark_flush_mark(void) {
    spin_lock_bh(&qmark_op_lock);
    flush_mark();
    spin_unlock_bh(&qmark_op_lock);
}

int qmark_alloc_mark_auto(__be32 src_ip, uint32_t id, uint32_t *mark) {
    if (true == reset_flag) {
        return ERROR;
    }

    if (PRIV_MODE == g_ruletbl[id-1].mode) {
        // private mode, need to allocate a new mark
        if (0 != qmark_alloc_mark(mark)) return ERROR;
    } else {
        // share mode
        if (0 == atomic_read(&(g_ruletbl[id-1].count))) {
            if (0 != qmark_alloc_mark(mark)) return ERROR;
            g_ruletbl[id-1].mark = *mark;
        } else {
            *mark = g_ruletbl[id-1].mark;
        }
    }

    if (0 != qmark_add_hash(src_ip, id, mark)) {
        if ((PRIV_MODE == g_ruletbl[id-1].mode) || (0 == atomic_read(&(g_ruletbl[id-1].count)))) {
            g_ruletbl[id-1].mark = 0;
            qmark_free_mark(mark);
        }
        return ERROR;
    }

    // add tc rule here
    spin_lock_bh(&qmark_op_lock);
    qmark_tc_add_rule(&g_ruletbl[id-1], *mark);
    spin_unlock_bh(&qmark_op_lock);
    return OK;
}

void qmark_set_spec(uint32_t ip_spec, uint32_t rule_spec) {
    spin_lock_bh(&qmark_op_lock);

    qos_marktbl_hashtbl_exit();
    qos_ruletbl_exit();
    mark_bitmap_exit();
    max_rule_cnt = rule_spec;
    max_ip_cnt = ip_spec;
    mark_bitmap_init(ip_spec);
    qos_ruletbl_init();
    qos_marktbl_hashtbl_init();

    spin_unlock_bh(&qmark_op_lock);
}

void qmark_reset(uint32_t flag) {
    int i;
    if (0 == flag) {
        reset_flag = false;
        return;
    }
    reset_flag = true;
    spin_lock_bh(&qmark_op_lock);

    flush_mark();
    qmark_flush_hash();
    for (i = 0; i < max_rule_cnt; i++) {
        atomic_set(&(g_ruletbl[i].count), 0);
    }

    spin_unlock_bh(&qmark_op_lock);
}

/* Debug */
void qmark_dump(void) {
    int i;
    spin_lock_bh(&qmark_op_lock);
    printk(KERN_INFO "==========\n"
                     "rule table\n"
                     "==========\n"
                     "id  mode mark     count devid rate    rate_mate rate_min rate_min_mate\n");
    for (i = 0; i < max_rule_cnt; i++) {
        if (g_ruletbl[i].on) {
            printk(KERN_INFO "%-3d %c    0x%-6x %-4d  %-1d     %-7d %-7d   %-7d  %-7d\n",
                   i+1, g_ruletbl[i].mode ? 'S' : 'P',
                   g_ruletbl[i].mark, atomic_read(&(g_ruletbl[i].count)),
                   g_ruletbl[i].devid, g_ruletbl[i].rate, g_ruletbl[i].rate_mate,
                   g_ruletbl[i].rate_min, g_ruletbl[i].rate_min_mate);
        }
    }
    printk(KERN_INFO "\n==========\n"
                     "hash table\n"
                     "==========\n");
    qos_marktbl_hashtbl_list();
    printk(KERN_INFO "\n==========\n"
                     "mark table\n"
                     "==========\n"
                     "id  mark\n");
    for (i = 0; i < max_ip_cnt; i++) {
        if (test_bit(i, mark_bitmap)) {
            printk(KERN_INFO "%-3d 0x%x\n", (i+1), ((i+1)<<MARK_BASE));
        }
    }
    printk(KERN_INFO "\n");
    spin_unlock_bh(&qmark_op_lock);
}

/* Module init and exit */
module_init(qos_marktbl_init);
module_exit(qos_marktbl_exit);

/* Export symbol */
EXPORT_SYMBOL(qmark_op_lock);
EXPORT_SYMBOL(g_ruletbl);
EXPORT_SYMBOL(qmark_find_hash);
EXPORT_SYMBOL(qmark_alloc_mark_auto);

