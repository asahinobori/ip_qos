#include <stdbool.h>
#include <stdio.h>
#include <xtables.h>

#    ifndef ARRAY_SIZE
#        define ARRAY_SIZE(x) (sizeof(x) / sizeof(*(x)))
#    endif

struct xt_qmark_tginfo {
    uint32_t mask;
    uint32_t id;
    uint32_t dir;
};

static void qmark_help(void)
{
    printf(
"MARK target options:\n"
"  --set-mask value                   Set nfmark mask\n"
"  --set-id value                     Set rule index\n"
"  --set-dir value(0:src, 1:dst)      Set direction\n");
}

static const struct xt_option_entry qmark_opts[] = {
    {.name = "set-mask", .id = 0, .type = XTTYPE_UINT32},
    {.name = "set-id", .id = 1, .type = XTTYPE_UINT32},
    {.name = "set-dir", .id = 2, .type = XTTYPE_UINT32},
    XTOPT_TABLEEND,
};


static void qmark_parse(struct xt_option_call *cb)
{
    struct xt_qmark_tginfo *markinfo = cb->data;

    xtables_option_parse(cb);
    switch (cb->entry->id) {
    case 0:
        markinfo->mask = cb->val.u32;
        break;
    case 1:
        markinfo->id = cb->val.u32;
        break;
    case 2:
        markinfo->dir = cb->val.u32;
        break;
    default:
        xtables_error(PARAMETER_PROBLEM,
               "qmark target param error");
    }
}

static void qmark_check(struct xt_fcheck_call *cb)
{
    if ((cb->xflags & 1) == 0)
        printf("qmark target: Parameter --set-mask is required\n");
    if ((cb->xflags & 2) == 0)
        printf("qmark target: Parameter --set-id is required\n");
    if ((cb->xflags & 4) == 0)
        printf("qmark target: Parameter --set-dir is required\n");
    if (cb->xflags != 7) {
        xtables_error(PARAMETER_PROBLEM, "qmark target param error");
    }
}

static void qmark_print(const void *ip,
                          const struct xt_entry_target *target, int numeric)
{
    const struct xt_qmark_tginfo *markinfo =
        (const struct xt_qmark_tginfo *)target->data;
    printf(" qmark mask 0x%x id %d dir %d", markinfo->mask, markinfo->id, markinfo->dir);
}

static void qmark_save(const void *ip, const struct xt_entry_target *target)
{
    const struct xt_qmark_tginfo *markinfo =
        (const struct xt_qmark_tginfo *)target->data;

    printf(" --set-mask 0x%x --set-id %d --set-dir %d", markinfo->mask, markinfo->id, markinfo->dir);
}

static struct xtables_target qmark_reg[] = {
    {
        .family        = NFPROTO_UNSPEC,
        .name          = "qmark",
        .version       = XTABLES_VERSION,
        .revision      = 0,
        .size          = XT_ALIGN(sizeof(struct xt_qmark_tginfo)),
        .userspacesize = XT_ALIGN(sizeof(struct xt_qmark_tginfo)),
        .help          = qmark_help,
        .print         = qmark_print,
        .save          = qmark_save,
        .x6_parse      = qmark_parse,
        .x6_fcheck     = qmark_check,
        .x6_options    = qmark_opts,
    },
};

void _init(void)
{
    xtables_register_targets(qmark_reg, ARRAY_SIZE(qmark_reg));
}

