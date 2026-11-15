#include "argparse.h"

#include "elog.h"
#include "mem.h"
#include "syscall_shim.h"

static aether_option_t *find_opt(aether_option_t *opts, size_t nopts, char c)
{
    size_t i;
    for (i = 0; i < nopts; i++) {
        if (opts[i].short_name == c)
            return &opts[i];
    }
    return 0;
}

int aether_opt_count(const aether_option_t *opts, size_t nopts, char short_name)
{
    size_t i;
    for (i = 0; i < nopts; i++) {
        if (opts[i].short_name == short_name)
            return opts[i].count;
    }
    return -1;
}

const char *aether_opt_value(const aether_option_t *opts, size_t nopts, char short_name)
{
    size_t i;
    for (i = 0; i < nopts; i++) {
        if (opts[i].short_name == short_name)
            return opts[i].takes_arg ? opts[i].value : 0;
    }
    return 0;
}

int aether_parse_args(const char *prog, aether_option_t *opts, size_t nopts,
                      int argc, char **argv, mem_arena_t *arena, aether_args_t *out)
{
    int i;
    size_t npos = 0;
    size_t k;
    const char **pos = mem_arena_alloc(arena, (size_t)(argc > 0 ? argc : 1) * sizeof(char *));

    if (!pos) {
        elog_err("%s: out of memory", prog);
        return -1;
    }

    for (k = 0; k < nopts; k++) {
        opts[k].value = 0;
        opts[k].count = 0;
    }

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *p;

        if (arg[0] == '-' && arg[1] == '-' && arg[2] == '\0') {
            /* `--`：其后全部是位置参数 */
            i++;
            break;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            p = arg + 1;
            while (*p != '\0') {
                char c = *p++;
                aether_option_t *o = find_opt(opts, nopts, c);
                if (!o) {
                    elog_err("%s: unknown option '-%c'", prog, c);
                    return -1;
                }
                if (o->takes_arg) {
                    if (*p != '\0') {
                        o->value = p; /* -bVALUE 形式 */
                    } else {
                        if (i + 1 >= argc) {
                            elog_err("%s: option '-%c' requires an argument", prog, c);
                            return -1;
                        }
                        o->value = argv[++i]; /* -b VALUE 形式 */
                    }
                    o->count++;
                    p = ""; /* 本参数已被完全消费 */
                    break;
                }
                o->count++; /* 无参短选项，可聚合：-vv */
            }
        } else {
            pos[npos++] = arg;
        }
    }
    /* `--` 之后：全部位置参数 */
    for (; i < argc; i++)
        pos[npos++] = argv[i];

    out->positionals = pos;
    out->positional_count = npos;
    return 0;
}

void aether_usage(const char *prog, const char *synopsis,
                  const aether_option_t *opts, size_t nopts)
{
    size_t i;
    elog_info("Usage: %s %s", prog, synopsis);
    for (i = 0; i < nopts; i++) {
        if (opts[i].takes_arg)
            elog_info("  -%c ARG\t%s", opts[i].short_name, opts[i].help ? opts[i].help : "");
        else
            elog_info("  -%c\t%s", opts[i].short_name, opts[i].help ? opts[i].help : "");
    }
}
