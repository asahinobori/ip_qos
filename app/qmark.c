#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>
#include <linux/types.h>

typedef unsigned int uint32_t;

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

static void usage(char const *argv0) {
    fprintf(stderr, "Usage: %s [rule|hash|mark|other]_cmd\n\n", argv0);
    fprintf(stderr, "rule_cmd:\n");
    fprintf(stderr, "   -a or --add_rule \"id mode devid rate rate_mate rate_min rate_min_mate\"\n");
    fprintf(stderr, "   -d or --del_rule id\n");
    fprintf(stderr, "   -l or --list_rule\n");
    fprintf(stderr, "   -x or --flush_rule\n");
    fprintf(stderr, "hash_cmd:\n");
    fprintf(stderr, "   -A or --add_hash \"ip mark rule_id\"\n");
    fprintf(stderr, "   -D or --del_hash ip \n");
    fprintf(stderr, "   -C or --check_hash ip\n");
    fprintf(stderr, "   -L or --list_hash\n");
    fprintf(stderr, "   -X or --flush_hash\n");
    fprintf(stderr, "mark_cmd:\n");
    fprintf(stderr, "   -g or --alloc_mark\n");
    fprintf(stderr, "   -f or --free_mark mark\n");
    fprintf(stderr, "   -F or --flush_mark\n");
    fprintf(stderr, "other_cmd:\n");
    fprintf(stderr, "   -z or --dump\n");
    fprintf(stderr, "   -r or --reset 0 or 1\n");
    fprintf(stderr, "   -n or --netdev_set \"lan_dev wan_dev\"\n");
    fprintf(stderr, "   -s or --spec_set \"max_ip_cnt max_rule_cnt\"\n");
    fprintf(stderr, "   -h or --help\n");
    fprintf(stderr, "   -t\n");
    fprintf(stderr, "   -u\n");
}

int main(int argc, char *argv[]) {
    int fd;
    qmark_rule_t qr = { 0 };
    qmark_hash_t qh = { 0 };
    qmark_mark_t qm = { 0 };
    qmark_netdev_t qn;
    qmark_spec_t qs;
    memset(&qn, 0, sizeof(qmark_netdev_t));

    int ip[4] = { 0 };
    int i = 0;
    int opt;
    int option_index = 0;
    char *optstring = "a:d:lxA:D:C:LXgf:Fzr:n:s:htu";
    static struct option long_options[] = {
        {"add_rule", required_argument, NULL, 'a'},
        {"del_rule", required_argument, NULL, 'd'},
        {"list_rule", no_argument, NULL, 'l'},
        {"flush_rule", no_argument, NULL, 'x'},
        {"add_hash", required_argument, NULL, 'A'},
        {"del_hash", required_argument, NULL, 'D'},
        {"check_hash", required_argument, NULL, 'C'},
        {"list_hash", no_argument, NULL, 'L'},
        {"flush_hash", no_argument, NULL, 'X'},
        {"alloc_mark", no_argument, NULL, 'g'},
        {"free_mark", required_argument, NULL, 'f'},
        {"flush_mark", no_argument, NULL, 'F'},
        {"dump", no_argument, NULL, 'z'},
        {"reset", required_argument, NULL, 'r'},
        {"netdev_set", required_argument, NULL, 'n'},
        {"spec_set", required_argument, NULL, 's'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0}
    };

    fd = open("/dev/qmarkctl", O_RDWR);
    if (fd < 0) {
        perror("open dev");
        return -1;
    }

    while ((opt = getopt_long(argc, argv, optstring, long_options, &option_index)) != -1) {
        switch (opt) {
        case 'a':
            sscanf(optarg, "%d %d %d %d %d %d %d", &qr.id, &qr.mode, &qr.devid,
                   &qr.rate, &qr.rate_mate, &qr.rate_min, &qr.rate_min_mate);
            if (ioctl(fd, QMARK_IOC_RULE_ADD, &qr) < 0) {
                printf("ioctl add rule failed\n");
            }
            break;
        case 'd':
            sscanf(optarg, "%d", &qr.id);
            if (ioctl(fd, QMARK_IOC_RULE_DEL, &qr) < 0) {
                printf("ioctl del rule failed\n");
            }
            break;
        case 'l':
            if (ioctl(fd, QMARK_IOC_RULE_LST, NULL) < 0) {
                printf("ioctl list rule failed\n");
            }
            break;
        case 'x':
            if (ioctl(fd, QMARK_IOC_RULE_FLUSH, NULL) < 0) {
                printf("ioctl flush rule failed\n");
            }
            break;
        case 'A':
            sscanf(optarg, "%d.%d.%d.%d 0x%x %d", &ip[0], &ip[1], &ip[2], &ip[3], &qh.mark, &qh.id);
            for (i = 0; i < 4; i++) {
                qh.ip += (ip[i] << ((3-i) << 3) );
            }
            if (ioctl(fd, QMARK_IOC_HASH_ADD, &qh) < 0) {
                printf("ioctl add hash failed\n");
            }
            break;
        case 'D':
            sscanf(optarg, "%d.%d.%d.%d", &ip[0], &ip[1], &ip[2], &ip[3]);
            for (i = 0; i < 4; i++) {
                qh.ip += (ip[i] << ((3-i) << 3) );
            }
            if (ioctl(fd, QMARK_IOC_HASH_DEL, &qh) < 0) {
                printf("ioctl delete hash failed\n");
            }
            break;
        case 'C':
            sscanf(optarg, "%d.%d.%d.%d", &ip[0], &ip[1], &ip[2], &ip[3]);
            for (i = 0; i < 4; i++) {
                qh.ip += (ip[i] << ((3-i) << 3) );
            }
            if (ioctl(fd, QMARK_IOC_HASH_CHK, &qh) < 0) {
                printf("ioctl check hash failed\n");
            }
            printf("0x%x\n", qh.mark);
            break;
        case 'L':
            if (ioctl(fd, QMARK_IOC_HASH_LST, NULL) < 0) {
                printf("ioctl list hash failed\n");
            }
            break;
        case 'X':
            if (ioctl(fd, QMARK_IOC_HASH_FLUSH) < 0) {
                printf("ioctl flush hash failed\n");
            }
            break;
        case 'g':
            if (ioctl(fd, QMARK_IOC_MARK_ALO, &qm) < 0) {
                printf("ioctl alloc mark failed\n");
            }
            printf("0x%x\n", qm.mark);
            break;
        case 'f':
            sscanf(optarg, "0x%x", &qm.mark);
            if (ioctl(fd, QMARK_IOC_MARK_FREE, &qm) < 0) {
                printf("ioctl free mark failed\n");
            }
            break;
        case 'F':
            if (ioctl(fd, QMARK_IOC_MARK_FLUSH) < 0) {
                printf("ioctl flush mark failed\n");
            }
            break;
        case 'z':
            if (ioctl(fd, QMARK_IOC_DUMP, NULL) < 0) {
                printf("ioctl dump failed\n");
            }
            break;
        case 'r':
            sscanf(optarg, "%d", &qm.mark);
            if (ioctl(fd, QMARK_IOC_RESET, &qm) < 0) {
                printf("ioctl reset failed\n");
            }
            break;
        case 'n':
            sscanf(optarg, "%s %s", qn.lan_dev, qn.wan_dev);
            if (ioctl(fd, QMARK_IOC_NETDEV_SET, &qn) < 0) {
                printf("ioctl net dev set failed\n");
            }
            break;
        case 's':
            sscanf(optarg, "%d %d", &qs.max_ip_cnt, &qs.max_rule_cnt);
            if (ioctl(fd, QMARK_IOC_SPEC_SET, &qs) < 0) {
                printf("ioctl spec set failed\n");
            }
            break;
        case 't':
            if (ioctl(fd, QMARK_IOC_TC_ADD, NULL) < 0) {
                printf("ioctl tc add test failed\n");
            }
            break;
        case 'u':
            if (ioctl(fd, QMARK_IOC_TC_DEL, NULL) < 0) {
                printf("ioctl tc del test failed\n");
            }
            break;
        case 'h':
            usage(argv[0]);
            exit(0);
        default:
            usage(argv[0]);
            break;
        }
    }
    close(fd);
    return 0;
}
