/*
 * Generate gzip-compressed SimPoint 3.2 basic block vectors.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include <qemu-plugin.h>

typedef struct BbKey {
    uint64_t vaddr;
    uint64_t n_insns;
} BbKey;

typedef struct Bb {
    BbKey key;
    struct qemu_plugin_scoreboard *count;
    unsigned int index;
} Bb;

typedef struct Vcpu {
    uint64_t total_count;
    uint64_t interval_count;
    uint64_t profile_count;
    bool started;
} Vcpu;

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define A64_HLT_INSN_MASK 0xffe0001f
#define A64_HLT_INSN      0xd4400000
#define SIMTRAP_DISABLE_TIME_INTR     0x100
#define SIMTRAP_NOTIFY_PROFILER       0x101
#define SIMTRAP_NOTIFY_WORKLOAD_EXIT  0x102
#define A64_SIMTRAP_PROFILE_START SIMTRAP_NOTIFY_PROFILER
#define A64_SIMTRAP_PROFILE_STOP  SIMTRAP_NOTIFY_WORKLOAD_EXIT

static GHashTable *bbs;
static GPtrArray *bb_list;
static GRWLock bbs_lock;
static GMutex file_lock;
static struct qemu_plugin_scoreboard *vcpus;
static gzFile bbv_file;

static char *outfile;
static uint64_t interval = 100000000;
static uint64_t skip = 0;
static unsigned int profile_cpu = 0;
static bool dump_final;
static bool profiling_mode;

static guint bb_key_hash(gconstpointer data)
{
    const BbKey *key = data;
    uint64_t hash = key->vaddr;

    hash ^= key->n_insns + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    return hash ^ (hash >> 32);
}

static gboolean bb_key_equal(gconstpointer a, gconstpointer b)
{
    const BbKey *ka = a;
    const BbKey *kb = b;

    return ka->vaddr == kb->vaddr && ka->n_insns == kb->n_insns;
}

static void free_bb(void *data)
{
    Bb *bb = data;

    qemu_plugin_scoreboard_free(bb->count);
    g_free(bb);
}

static qemu_plugin_u64 total_count_u64(void)
{
    return qemu_plugin_scoreboard_u64_in_struct(vcpus, Vcpu, total_count);
}

static qemu_plugin_u64 interval_count_u64(void)
{
    return qemu_plugin_scoreboard_u64_in_struct(vcpus, Vcpu, interval_count);
}

static qemu_plugin_u64 profile_count_u64(void)
{
    return qemu_plugin_scoreboard_u64_in_struct(vcpus, Vcpu, profile_count);
}

static qemu_plugin_u64 bb_count_u64(Bb *bb)
{
    return qemu_plugin_scoreboard_u64(bb->count);
}

static bool parse_uint64(const char *name, const char *value, uint64_t *out)
{
    char *endptr;
    uint64_t parsed;

    if (!value || !*value) {
        fprintf(stderr, "simpoint: missing value for %s\n", name);
        return false;
    }

    parsed = g_ascii_strtoull(value, &endptr, 0);
    if (*endptr != '\0') {
        fprintf(stderr, "simpoint: invalid integer for %s: %s\n", name, value);
        return false;
    }

    *out = parsed;
    return true;
}

static bool parse_bool(const char *name, const char *value, bool *out)
{
    if (g_strcmp0(value, "true") == 0 || g_strcmp0(value, "on") == 0 ||
        g_strcmp0(value, "yes") == 0 || g_strcmp0(value, "1") == 0) {
        *out = true;
        return true;
    }
    if (g_strcmp0(value, "false") == 0 || g_strcmp0(value, "off") == 0 ||
        g_strcmp0(value, "no") == 0 || g_strcmp0(value, "0") == 0) {
        *out = false;
        return true;
    }

    fprintf(stderr, "simpoint: invalid boolean for %s: %s\n", name, value);
    return false;
}

static bool parse_trigger(const char *value)
{
    if (g_strcmp0(value, "immediate") == 0) {
        profiling_mode = false;
        return true;
    }
    if (g_strcmp0(value, "simtrap") == 0) {
        profiling_mode = true;
        return true;
    }

    fprintf(stderr, "simpoint: invalid trigger: %s\n", value);
    return false;
}

static bool set_outfile_from_target(const char *target)
{
    if (!target || !*target) {
        fputs("simpoint: target path is empty\n", stderr);
        return false;
    }
    if (g_mkdir_with_parents(target, 0775) < 0) {
        fprintf(stderr, "simpoint: cannot create target directory %s: %s\n",
                target, g_strerror(errno));
        return false;
    }

    g_free(outfile);
    outfile = g_build_filename(target, "simpoint_bbv.gz", NULL);
    return true;
}

static bool set_outfile(const char *path)
{
    g_autofree char *dirname = NULL;

    if (!path || !*path) {
        fputs("simpoint: outfile path is empty\n", stderr);
        return false;
    }

    dirname = g_path_get_dirname(path);
    if (g_strcmp0(dirname, ".") != 0 &&
        g_mkdir_with_parents(dirname, 0775) < 0) {
        fprintf(stderr, "simpoint: cannot create output directory %s: %s\n",
                dirname, g_strerror(errno));
        return false;
    }

    g_free(outfile);
    outfile = g_strdup(path);
    return true;
}

static void clear_bb_counts(unsigned int vcpu_index)
{
    g_rw_lock_reader_lock(&bbs_lock);
    for (guint i = 0; i < bb_list->len; i++) {
        Bb *bb = g_ptr_array_index(bb_list, i);
        qemu_plugin_u64_set(bb_count_u64(bb), vcpu_index, 0);
    }
    g_rw_lock_reader_unlock(&bbs_lock);
}

static void write_bbv_interval(unsigned int vcpu_index)
{
    Vcpu *vcpu = qemu_plugin_scoreboard_find(vcpus, vcpu_index);

    if (!vcpu->started || !bbv_file) {
        return;
    }
    vcpu->interval_count = 0;

    g_mutex_lock(&file_lock);
    gzputc(bbv_file, 'T');

    g_rw_lock_reader_lock(&bbs_lock);
    for (guint i = 0; i < bb_list->len; i++) {
        Bb *bb = g_ptr_array_index(bb_list, i);
        uint64_t bb_count = qemu_plugin_u64_get(bb_count_u64(bb), vcpu_index);

        if (bb_count) {
            gzprintf(bbv_file, ":%u:%" PRIu64 " ", bb->index, bb_count);
            qemu_plugin_u64_set(bb_count_u64(bb), vcpu_index, 0);
        }
    }
    g_rw_lock_reader_unlock(&bbs_lock);

    gzputc(bbv_file, '\n');
    gzflush(bbv_file, Z_SYNC_FLUSH);
    g_mutex_unlock(&file_lock);
}

static bool a64_hlt_imm(uint32_t opcode, uint32_t *imm)
{
    if ((opcode & A64_HLT_INSN_MASK) != A64_HLT_INSN) {
        return false;
    }

    *imm = (opcode >> 5) & 0xffff;
    return true;
}

static bool a64_simtrap_imm(uint32_t imm)
{
    return imm == SIMTRAP_DISABLE_TIME_INTR ||
           imm == A64_SIMTRAP_PROFILE_START ||
           imm == A64_SIMTRAP_PROFILE_STOP;
}

static bool insn_is_a64_simtrap(struct qemu_plugin_insn *insn, uint32_t *imm)
{
    uint32_t opcode = 0;

    if (qemu_plugin_insn_size(insn) != sizeof(opcode) ||
        qemu_plugin_insn_data(insn, &opcode, sizeof(opcode)) !=
        sizeof(opcode)) {
        return false;
    }

    return a64_hlt_imm(opcode, imm) && a64_simtrap_imm(*imm);
}

static void vcpu_stop(unsigned int vcpu_index)
{
    Vcpu *vcpu = qemu_plugin_scoreboard_find(vcpus, vcpu_index);

    if (!vcpu->started) {
        return;
    }

    if (dump_final && vcpu->interval_count > 0) {
        write_bbv_interval(vcpu_index);
    } else {
        clear_bb_counts(vcpu_index);
        qemu_plugin_u64_set(interval_count_u64(), vcpu_index, 0);
        vcpu->interval_count = 0;
    }
    vcpu->started = false;
    fprintf(stderr,
            "simpoint: vcpu %u profiling stopped after %" PRIu64
            " instructions\n",
            vcpu_index, vcpu->profile_count);
}

static void vcpu_start(unsigned int vcpu_index)
{
    Vcpu *vcpu = qemu_plugin_scoreboard_find(vcpus, vcpu_index);

    if (vcpu->started) {
        return;
    }

    clear_bb_counts(vcpu_index);
    qemu_plugin_u64_set(interval_count_u64(), vcpu_index, 0);
    qemu_plugin_u64_set(profile_count_u64(), vcpu_index, 0);
    vcpu->interval_count = 0;
    vcpu->profile_count = 0;
    vcpu->started = true;
    fprintf(stderr, "simpoint: vcpu %u profiling started\n", vcpu_index);
}

static void vcpu_init(unsigned int vcpu_index, void *userdata)
{
    if (!profiling_mode && vcpu_index == profile_cpu && skip == 0) {
        vcpu_start(vcpu_index);
    }
}

static void vcpu_skip_exec(unsigned int vcpu_index, void *userdata)
{
    Vcpu *vcpu;

    if (vcpu_index != profile_cpu) {
        return;
    }

    vcpu = qemu_plugin_scoreboard_find(vcpus, vcpu_index);
    if (!vcpu->started && vcpu->total_count >= skip) {
        vcpu_start(vcpu_index);
    }
}

static void vcpu_interval_exec(unsigned int vcpu_index, void *userdata)
{
    Vcpu *vcpu;

    if (vcpu_index != profile_cpu) {
        return;
    }

    vcpu = qemu_plugin_scoreboard_find(vcpus, vcpu_index);
    if (!vcpu->started) {
        qemu_plugin_u64_set(interval_count_u64(), vcpu_index, 0);
        vcpu->interval_count = 0;
    } else if (vcpu->interval_count >= interval) {
        write_bbv_interval(vcpu_index);
    }
}

static void vcpu_simtrap_exec(unsigned int vcpu_index, void *userdata)
{
    uint32_t imm = GPOINTER_TO_UINT(userdata);

    if (vcpu_index != profile_cpu) {
        return;
    }

    switch (imm) {
    case A64_SIMTRAP_PROFILE_START:
        vcpu_start(vcpu_index);
        break;
    case A64_SIMTRAP_PROFILE_STOP:
        vcpu_stop(vcpu_index);
        break;
    default:
        break;
    }
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *userdata)
{
    uint64_t n_insns = qemu_plugin_tb_n_insns(tb);
    uint64_t profile_n_insns = n_insns;
    uint64_t vaddr = qemu_plugin_tb_vaddr(tb);
    uint32_t imm;
    Bb *bb = NULL;

    if (profiling_mode && n_insns > 0) {
        struct qemu_plugin_insn *last =
            qemu_plugin_tb_get_insn(tb, n_insns - 1);

        if (insn_is_a64_simtrap(last, &imm)) {
            profile_n_insns--;
        }
    }

    if (profile_n_insns > 0) {
        BbKey key = { vaddr, profile_n_insns };

        g_rw_lock_writer_lock(&bbs_lock);
        bb = g_hash_table_lookup(bbs, &key);
        if (!bb) {
            bb = g_new0(Bb, 1);
            bb->key = key;
            bb->count = qemu_plugin_scoreboard_new(sizeof(uint64_t));
            bb->index = bb_list->len + 1;
            g_hash_table_replace(bbs, &bb->key, bb);
            g_ptr_array_add(bb_list, bb);
        }
        g_rw_lock_writer_unlock(&bbs_lock);

        qemu_plugin_u64 total_count = total_count_u64();
        qemu_plugin_u64 interval_count = interval_count_u64();
        qemu_plugin_u64 profile_count = profile_count_u64();
        qemu_plugin_u64 bb_count = bb_count_u64(bb);

        for (uint64_t i = 0; i < profile_n_insns; i++) {
            struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);

            qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(
                insn, QEMU_PLUGIN_INLINE_ADD_U64, total_count, 1);
            qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(
                insn, QEMU_PLUGIN_INLINE_ADD_U64, interval_count, 1);
            qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(
                insn, QEMU_PLUGIN_INLINE_ADD_U64, profile_count, 1);
            qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(
                insn, QEMU_PLUGIN_INLINE_ADD_U64, bb_count, 1);

            if (!profiling_mode && skip > 0) {
                qemu_plugin_register_vcpu_insn_exec_cond_cb(
                    insn, vcpu_skip_exec, QEMU_PLUGIN_CB_NO_REGS,
                    QEMU_PLUGIN_COND_GE, total_count, skip, NULL);
            }
            qemu_plugin_register_vcpu_insn_exec_cond_cb(
                insn, vcpu_interval_exec, QEMU_PLUGIN_CB_NO_REGS,
                QEMU_PLUGIN_COND_GE, interval_count, interval, NULL);
        }
    }

    if (profiling_mode) {
        for (uint64_t i = 0; i < n_insns; i++) {
            struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);

            if (insn_is_a64_simtrap(insn, &imm) &&
                (imm == A64_SIMTRAP_PROFILE_START ||
                 imm == A64_SIMTRAP_PROFILE_STOP)) {
                qemu_plugin_register_vcpu_insn_exec_cb(
                    insn, vcpu_simtrap_exec, QEMU_PLUGIN_CB_NO_REGS,
                    GUINT_TO_POINTER(imm));
            }
        }
    }
}

static void plugin_exit(void *p)
{
    if (profiling_mode) {
        qemu_plugin_a64_simtrap_set_profiling_mode(false);
    }

    if (dump_final && profile_cpu < qemu_plugin_num_vcpus()) {
        Vcpu *vcpu = qemu_plugin_scoreboard_find(vcpus, profile_cpu);

        if (vcpu->started && vcpu->interval_count > 0) {
            write_bbv_interval(profile_cpu);
        }
    }

    if (bbv_file) {
        gzclose(bbv_file);
    }
    g_hash_table_unref(bbs);
    g_ptr_array_free(bb_list, true);
    qemu_plugin_scoreboard_free(vcpus);
    g_free(outfile);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    if (!info->system_emulation) {
        fputs("simpoint: system emulation is required\n", stderr);
        return -1;
    }

    for (int i = 0; i < argc; i++) {
        char *opt = argv[i];
        g_auto(GStrv) tokens = g_strsplit(opt, "=", 2);

        if (!tokens[0] || !tokens[1]) {
            fprintf(stderr, "simpoint: option parsing failed: %s\n", opt);
            return -1;
        }

        if (g_strcmp0(tokens[0], "interval") == 0 ||
            g_strcmp0(tokens[0], "intervals") == 0) {
            if (!parse_uint64(tokens[0], tokens[1], &interval)) {
                return -1;
            }
        } else if (g_strcmp0(tokens[0], "skip") == 0 ||
                   g_strcmp0(tokens[0], "warmup") == 0) {
            if (!parse_uint64(tokens[0], tokens[1], &skip)) {
                return -1;
            }
        } else if (g_strcmp0(tokens[0], "cpu") == 0) {
            uint64_t cpu;

            if (!parse_uint64(tokens[0], tokens[1], &cpu)) {
                return -1;
            }
            if (cpu > G_MAXUINT) {
                fprintf(stderr, "simpoint: cpu index too large: %" PRIu64 "\n",
                        cpu);
                return -1;
            }
            profile_cpu = cpu;
        } else if (g_strcmp0(tokens[0], "dump-final") == 0) {
            if (!parse_bool(tokens[0], tokens[1], &dump_final)) {
                return -1;
            }
        } else if (g_strcmp0(tokens[0], "trigger") == 0) {
            if (!parse_trigger(tokens[1])) {
                return -1;
            }
        } else if (g_strcmp0(tokens[0], "simtrap") == 0) {
            if (!parse_bool(tokens[0], tokens[1], &profiling_mode)) {
                return -1;
            }
        } else if (g_strcmp0(tokens[0], "target") == 0) {
            if (!set_outfile_from_target(tokens[1])) {
                return -1;
            }
        } else if (g_strcmp0(tokens[0], "outfile") == 0) {
            if (!set_outfile(tokens[1])) {
                return -1;
            }
        } else if (g_strcmp0(tokens[0], "workload") == 0) {
            /* Accepted for compatibility with XiangShan's profiling plugin. */
        } else {
            fprintf(stderr, "simpoint: unknown argument %s\n", tokens[0]);
            return -1;
        }
    }

    if (interval == 0) {
        fputs("simpoint: interval must be greater than zero\n", stderr);
        return -1;
    }

    if (!outfile) {
        if (!set_outfile("simpoint_bbv.gz")) {
            return -1;
        }
    }

    bbv_file = gzopen(outfile, "wb");
    if (!bbv_file) {
        fprintf(stderr, "simpoint: cannot open %s: %s\n", outfile,
                g_strerror(errno));
        return -1;
    }

    bbs = g_hash_table_new_full(bb_key_hash, bb_key_equal, NULL, free_bb);
    bb_list = g_ptr_array_new();
    vcpus = qemu_plugin_scoreboard_new(sizeof(Vcpu));

    fprintf(stderr,
            "simpoint: output=%s interval=%" PRIu64 " skip=%" PRIu64
            " cpu=%u trigger=%s\n",
            outfile, interval, skip, profile_cpu,
            profiling_mode ? "simtrap" : "immediate");

    if (profiling_mode) {
        qemu_plugin_a64_simtrap_set_profiling_mode(true);
    }

    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    qemu_plugin_register_vcpu_init_cb(id, vcpu_init, NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);

    return 0;
}
