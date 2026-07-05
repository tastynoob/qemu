/*
 * AArch64 SimPoint checkpoint support.
 *
 * This writes the raw-memory checkpoint format consumed by
 * libcheckpoint-for-aarch64. The output is a sparse mini-virt RAM image with
 * checkpoint metadata overlaid at gcpt_base + 0x100000.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "system/system.h"
#include "target/arm/a64-checkpoint.h"
#include "internals.h"

#define A64_CPT_MAGIC_NUMBER UINT64_C(0xdeadbeef)
#define A64_CPT_CORE_MAGIC_NUMBER UINT64_C(0xbeef)
#define A64_CPT_VERSION UINT64_C(0x20260705)
#define A64_CPT_SINGLE_CORE_SIZE UINT64_C(0x100000)
#define A64_CPT_RESTORER_RESERVED_SIZE UINT64_C(0x100000)
#define A64_CPT_DEFAULT_HEADER_OFFSET A64_CPT_RESTORER_RESERVED_SIZE
#define A64_CPT_METADATA_ALIGN UINT64_C(0x1000)

#define A64_CPT_FLAG_HAS_FPSIMD (UINT64_C(1) << 0)

#define A64_CPT_MAX_SYSREG_RECORDS 512U
#define A64_CPT_SYSREG_RECORD_F_RESTORE (UINT16_C(1) << 0)

#define A64_CPT_RESTORER_CNTP_CTL_EL0  UINT16_C(0xdec1)
#define A64_CPT_RESTORER_CNTP_CVAL_EL0 UINT16_C(0xdec2)
#define A64_CPT_RESTORER_CNTV_CTL_EL0  UINT16_C(0xded9)
#define A64_CPT_RESTORER_CNTV_CVAL_EL0 UINT16_C(0xdeda)

#define A64_CPT_SYSREG_ENCODING(op0, op1, crn, crm, op2) \
    ((((uint16_t)(op0) & 0x3U) << 14) |                  \
     (((uint16_t)(op1) & 0x7U) << 11) |                  \
     (((uint16_t)(crn) & 0xfU) << 7) |                   \
     (((uint16_t)(crm) & 0xfU) << 3) |                   \
     ((uint16_t)(op2) & 0x7U))

#define A64_CPT_OFF_MAGIC       UINT64_C(0x0000)
#define A64_CPT_OFF_PC          UINT64_C(0x0008)
#define A64_CPT_OFF_PSTATE      UINT64_C(0x0010)
#define A64_CPT_OFF_CURRENT_EL  UINT64_C(0x0018)
#define A64_CPT_OFF_FEATURES    UINT64_C(0x0020)
#define A64_CPT_OFF_MISC_DONE   UINT64_C(0x0028)
#define A64_CPT_OFF_INT_REG     UINT64_C(0x1000)
#define A64_CPT_OFF_INT_DONE    UINT64_C(0x10f8)
#define A64_CPT_OFF_SP_REG      UINT64_C(0x1100)
#define A64_CPT_OFF_SP_DONE     UINT64_C(0x1120)
#define A64_CPT_OFF_ELR_SPSR    UINT64_C(0x1120)
#define A64_CPT_OFF_ELR_DONE    UINT64_C(0x1150)
#define A64_CPT_OFF_SYSREG      UINT64_C(0x2000)
#define A64_CPT_OFF_SYSREG_DONE UINT64_C(0x4010)
#define A64_CPT_OFF_IDREG       UINT64_C(0x5000)
#define A64_CPT_OFF_IDREG_DONE  UINT64_C(0x5410)
#define A64_CPT_OFF_FLOAT_REG   UINT64_C(0x6000)
#define A64_CPT_OFF_FLOAT_DONE  UINT64_C(0x6210)
#define A64_CPT_OFF_SVE_REG     UINT64_C(0x10000)
#define A64_CPT_OFF_SVE_DONE    UINT64_C(0x30000)
#define A64_CPT_OFF_SME_REG     UINT64_C(0x30000)
#define A64_CPT_OFF_SME_DONE    UINT64_C(0x60000)
#define A64_CPT_OFF_MTE_REG     UINT64_C(0x60000)
#define A64_CPT_OFF_MTE_DONE    UINT64_C(0x70000)
#define A64_CPT_OFF_PAUTH_REG   UINT64_C(0x70000)
#define A64_CPT_OFF_PAUTH_DONE  UINT64_C(0x70050)

typedef struct A64CheckpointPoint {
    uint64_t insns;
} A64CheckpointPoint;

typedef struct A64CheckpointState {
    bool enabled;
    bool window_started;
    bool exit_after_last;
    bool writing;
    uint64_t window_base;
    uint64_t ram_base;
    uint64_t ram_size;
    MemoryRegion *ram;
    char *output_dir;
    GArray *points;
    size_t next_point;
} A64CheckpointState;

static A64CheckpointState a64_cpt;

static void put_u16(void *base, uint64_t off, uint16_t value)
{
    stw_le_p((uint8_t *)base + off, value);
}

static void put_u32(void *base, uint64_t off, uint32_t value)
{
    stl_le_p((uint8_t *)base + off, value);
}

static void put_u64(void *base, uint64_t off, uint64_t value)
{
    stq_le_p((uint8_t *)base + off, value);
}

static int compare_points(gconstpointer a, gconstpointer b)
{
    const A64CheckpointPoint *pa = a;
    const A64CheckpointPoint *pb = b;

    if (pa->insns == pb->insns) {
        return 0;
    }
    return pa->insns < pb->insns ? -1 : 1;
}

static void clear_points(void)
{
    if (a64_cpt.points) {
        g_array_unref(a64_cpt.points);
        a64_cpt.points = NULL;
    }
    a64_cpt.next_point = 0;
}

static void add_point(uint64_t insns)
{
    A64CheckpointPoint point = { .insns = insns };

    if (!a64_cpt.points) {
        a64_cpt.points = g_array_new(false, false, sizeof(point));
    }
    g_array_append_val(a64_cpt.points, point);
}

static void sort_and_dedupe_points(void)
{
    GArray *dedup;
    uint64_t last = 0;
    bool have_last = false;

    if (!a64_cpt.points || a64_cpt.points->len == 0) {
        return;
    }

    g_array_sort(a64_cpt.points, compare_points);

    dedup = g_array_new(false, false, sizeof(A64CheckpointPoint));
    for (guint i = 0; i < a64_cpt.points->len; i++) {
        A64CheckpointPoint point =
            g_array_index(a64_cpt.points, A64CheckpointPoint, i);

        if (have_last && point.insns == last) {
            continue;
        }
        g_array_append_val(dedup, point);
        last = point.insns;
        have_last = true;
    }

    g_array_unref(a64_cpt.points);
    a64_cpt.points = dedup;
}

static bool parse_u64_token(const char *token, uint64_t *value, Error **errp)
{
    const char *endp = NULL;

    if (!token || token[0] == '\0') {
        return false;
    }
    if (qemu_strtou64(token, &endp, 0, value) < 0 || (endp && *endp != '\0')) {
        error_setg(errp, "invalid checkpoint cutpoint '%s'", token);
        return false;
    }
    return true;
}

static bool parse_cutpoints_string(const char *cutpoints, Error **errp)
{
    g_auto(GStrv) tokens = NULL;

    if (!cutpoints || cutpoints[0] == '\0') {
        return true;
    }

    tokens = g_strsplit_set(cutpoints, ",;: \t\r\n", -1);
    for (char **p = tokens; *p; p++) {
        uint64_t value;

        if ((*p)[0] == '\0') {
            continue;
        }
        if (!parse_u64_token(*p, &value, errp)) {
            return false;
        }
        add_point(value);
    }
    return true;
}

static bool parse_cutpoints_file(const char *path, bool simpoint_locations,
                                 uint64_t cpt_interval, Error **errp)
{
    g_autoptr(GError) gerr = NULL;
    g_autofree char *contents = NULL;
    g_auto(GStrv) lines = NULL;
    size_t len;

    if (!path || path[0] == '\0') {
        return true;
    }
    if (!g_file_get_contents(path, &contents, &len, &gerr)) {
        error_setg(errp, "failed to read checkpoint cutpoint file '%s': %s",
                   path, gerr->message);
        return false;
    }

    lines = g_strsplit(contents, "\n", -1);
    for (char **linep = lines; *linep; linep++) {
        g_auto(GStrv) fields = NULL;
        char *line = g_strstrip(*linep);
        uint64_t value;

        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        fields = g_strsplit_set(line, " \t\r", 0);
        if (!fields[0] || fields[0][0] == '\0') {
            continue;
        }
        if (!parse_u64_token(fields[0], &value, errp)) {
            return false;
        }
        if (simpoint_locations) {
            if (cpt_interval == 0) {
                error_setg(errp,
                           "cpt-interval must be non-zero for simpoint files");
                return false;
            }
            if (value > UINT64_MAX / cpt_interval) {
                error_setg(errp, "simpoint cutpoint overflow: %" PRIu64
                           " * %" PRIu64, value, cpt_interval);
                return false;
            }
            value *= cpt_interval;
        }
        add_point(value);
    }
    return true;
}

static bool parse_simpoint_path(const char *simpoint_path,
                                const char *simpoint_file,
                                uint64_t cpt_interval,
                                Error **errp)
{
    g_autofree char *path = NULL;

    if (simpoint_file && simpoint_file[0] != '\0') {
        return parse_cutpoints_file(simpoint_file, true, cpt_interval, errp);
    }

    if (!simpoint_path || simpoint_path[0] == '\0') {
        return true;
    }

    if (g_file_test(simpoint_path, G_FILE_TEST_IS_DIR)) {
        path = g_build_filename(simpoint_path, "simpoints0", NULL);
    } else {
        path = g_strdup(simpoint_path);
    }
    return parse_cutpoints_file(path, true, cpt_interval, errp);
}

static int pwrite_full(int fd, const void *buf, size_t len, off_t offset)
{
    const uint8_t *p = buf;

    while (len > 0) {
        ssize_t ret = pwrite(fd, p, len, offset);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        if (ret == 0) {
            return -EIO;
        }
        p += ret;
        offset += ret;
        len -= ret;
    }
    return 0;
}

static int write_sparse_ram(int fd, const uint8_t *ram, uint64_t size)
{
    const size_t chunk_size = 1 * MiB;

    for (uint64_t off = 0; off < size; off += chunk_size) {
        size_t chunk = MIN((uint64_t)chunk_size, size - off);
        int ret;

        if (buffer_is_zero(ram + off, chunk)) {
            continue;
        }

        ret = pwrite_full(fd, ram + off, chunk, off);
        if (ret < 0) {
            return ret;
        }
    }

    if (ftruncate(fd, size) < 0) {
        return -errno;
    }
    return 0;
}

static void write_checkpoint_header(uint8_t *metadata)
{
    uint64_t metadata_size = 40 + 208;
    uint64_t cpt_offset = ROUND_UP(metadata_size, A64_CPT_METADATA_ALIGN);
    uint64_t off = 0;

    put_u64(metadata, off, A64_CPT_MAGIC_NUMBER);
    off += 8;
    put_u64(metadata, off, cpt_offset);
    off += 8;
    put_u64(metadata, off, 1);
    off += 8;
    put_u64(metadata, off, A64_CPT_SINGLE_CORE_SIZE);
    off += 8;
    put_u64(metadata, off, A64_CPT_VERSION);
}

static void write_checkpoint_layout(uint8_t *metadata)
{
    static const uint64_t layout[] = {
        A64_CPT_OFF_MAGIC,
        A64_CPT_OFF_PC,
        A64_CPT_OFF_PSTATE,
        A64_CPT_OFF_CURRENT_EL,
        A64_CPT_OFF_FEATURES,
        A64_CPT_OFF_MISC_DONE,
        A64_CPT_OFF_INT_REG,
        A64_CPT_OFF_INT_DONE,
        A64_CPT_OFF_SP_REG,
        A64_CPT_OFF_SP_DONE,
        A64_CPT_OFF_ELR_SPSR,
        A64_CPT_OFF_ELR_DONE,
        A64_CPT_OFF_SYSREG,
        A64_CPT_OFF_SYSREG_DONE,
        A64_CPT_OFF_IDREG,
        A64_CPT_OFF_IDREG_DONE,
        A64_CPT_OFF_FLOAT_REG,
        A64_CPT_OFF_FLOAT_DONE,
        A64_CPT_OFF_SVE_REG,
        A64_CPT_OFF_SVE_DONE,
        A64_CPT_OFF_SME_REG,
        A64_CPT_OFF_SME_DONE,
        A64_CPT_OFF_MTE_REG,
        A64_CPT_OFF_MTE_DONE,
        A64_CPT_OFF_PAUTH_REG,
        A64_CPT_OFF_PAUTH_DONE,
    };

    for (uint64_t i = 0; i < ARRAY_SIZE(layout); i++) {
        put_u64(metadata, 40 + i * 8, layout[i]);
    }
}

static void add_sysreg(uint8_t *core, uint64_t *count, uint16_t encoding,
                       uint64_t value)
{
    uint64_t off = A64_CPT_OFF_SYSREG + 16 + *count * 16;

    g_assert(*count < A64_CPT_MAX_SYSREG_RECORDS);
    put_u16(core, off, encoding);
    put_u16(core, off + 2, A64_CPT_SYSREG_RECORD_F_RESTORE);
    put_u32(core, off + 4, 0);
    put_u64(core, off + 8, value);
    (*count)++;
}

static uint16_t sysreg_encoding(int op0, int op1, int crn, int crm, int op2)
{
    return A64_CPT_SYSREG_ENCODING(op0, op1, crn, crm, op2);
}

static void write_sysregs(uint8_t *core, CPUARMState *env)
{
    uint64_t count = 0;

    add_sysreg(core, &count, sysreg_encoding(3, 0, 1, 0, 0),
               env->cp15.sctlr_el[1]);
    add_sysreg(core, &count, sysreg_encoding(3, 0, 2, 0, 2),
               env->cp15.tcr_el[1]);
    add_sysreg(core, &count, sysreg_encoding(3, 0, 2, 0, 0),
               env->cp15.ttbr0_el[1]);
    add_sysreg(core, &count, sysreg_encoding(3, 0, 2, 0, 1),
               env->cp15.ttbr1_el[1]);
    add_sysreg(core, &count, sysreg_encoding(3, 0, 10, 2, 0),
               env->cp15.mair_el[1]);
    add_sysreg(core, &count, sysreg_encoding(3, 0, 12, 0, 0),
               env->cp15.vbar_el[1]);
    add_sysreg(core, &count, sysreg_encoding(3, 0, 13, 0, 1),
               env->cp15.contextidr_el[1]);
    add_sysreg(core, &count, sysreg_encoding(3, 3, 13, 0, 2),
               env->cp15.tpidr_el[0]);
    add_sysreg(core, &count, sysreg_encoding(3, 3, 13, 0, 3),
               env->cp15.tpidrro_el[0]);
    add_sysreg(core, &count, sysreg_encoding(3, 0, 13, 0, 4),
               env->cp15.tpidr_el[1]);
    add_sysreg(core, &count, sysreg_encoding(3, 0, 1, 0, 2),
               env->cp15.cpacr_el1);
    add_sysreg(core, &count, sysreg_encoding(3, 0, 5, 2, 0),
               env->cp15.esr_el[1]);
    add_sysreg(core, &count, sysreg_encoding(3, 0, 6, 0, 0),
               env->cp15.far_el[1]);
    add_sysreg(core, &count, A64_CPT_RESTORER_CNTP_CTL_EL0,
               env->cp15.c14_timer[GTIMER_PHYS].ctl);
    add_sysreg(core, &count, A64_CPT_RESTORER_CNTP_CVAL_EL0,
               env->cp15.c14_timer[GTIMER_PHYS].cval);
    add_sysreg(core, &count, A64_CPT_RESTORER_CNTV_CTL_EL0,
               env->cp15.c14_timer[GTIMER_VIRT].ctl);
    add_sysreg(core, &count, A64_CPT_RESTORER_CNTV_CVAL_EL0,
               env->cp15.c14_timer[GTIMER_VIRT].cval);
    add_sysreg(core, &count, sysreg_encoding(3, 0, 14, 1, 0),
               env->cp15.c14_cntkctl);

    add_sysreg(core, &count, sysreg_encoding(3, 4, 1, 0, 0),
               env->cp15.sctlr_el[2]);
    add_sysreg(core, &count, sysreg_encoding(3, 4, 2, 0, 2),
               env->cp15.tcr_el[2]);
    add_sysreg(core, &count, sysreg_encoding(3, 4, 2, 0, 0),
               env->cp15.ttbr0_el[2]);
    add_sysreg(core, &count, sysreg_encoding(3, 4, 10, 2, 0),
               env->cp15.mair_el[2]);
    add_sysreg(core, &count, sysreg_encoding(3, 4, 12, 0, 0),
               env->cp15.vbar_el[2]);
    add_sysreg(core, &count, sysreg_encoding(3, 4, 13, 0, 2),
               env->cp15.tpidr_el[2]);
    add_sysreg(core, &count, sysreg_encoding(3, 4, 1, 1, 0),
               env->cp15.hcr_el2);
    add_sysreg(core, &count, sysreg_encoding(3, 4, 1, 1, 2),
               env->cp15.cptr_el[2]);
    add_sysreg(core, &count, sysreg_encoding(3, 4, 14, 1, 0),
               env->cp15.cnthctl_el2);
    add_sysreg(core, &count, sysreg_encoding(3, 4, 14, 0, 3),
               env->cp15.cntvoff_el2);
    add_sysreg(core, &count, sysreg_encoding(3, 4, 2, 1, 2),
               env->cp15.vtcr_el2);
    add_sysreg(core, &count, sysreg_encoding(3, 4, 2, 1, 0),
               env->cp15.vttbr_el2);
    add_sysreg(core, &count, sysreg_encoding(3, 4, 5, 2, 0),
               env->cp15.esr_el[2]);
    add_sysreg(core, &count, sysreg_encoding(3, 4, 6, 0, 0),
               env->cp15.far_el[2]);

    add_sysreg(core, &count, sysreg_encoding(3, 6, 1, 0, 0),
               env->cp15.sctlr_el[3]);
    add_sysreg(core, &count, sysreg_encoding(3, 6, 1, 1, 0),
               env->cp15.scr_el3);
    add_sysreg(core, &count, sysreg_encoding(3, 6, 1, 1, 2),
               env->cp15.cptr_el[3]);
    add_sysreg(core, &count, sysreg_encoding(3, 6, 12, 0, 0),
               env->cp15.vbar_el[3]);
    add_sysreg(core, &count, sysreg_encoding(3, 6, 13, 0, 2),
               env->cp15.tpidr_el[3]);
    add_sysreg(core, &count, sysreg_encoding(3, 6, 5, 2, 0),
               env->cp15.esr_el[3]);
    add_sysreg(core, &count, sysreg_encoding(3, 6, 6, 0, 0),
               env->cp15.far_el[3]);

    put_u64(core, A64_CPT_OFF_SYSREG, count);
    put_u64(core, A64_CPT_OFF_SYSREG + 8, 0);
    put_u64(core, A64_CPT_OFF_SYSREG_DONE, 1);
}

static void write_core_state(uint8_t *core, CPUARMState *env, uint64_t pc)
{
    uint64_t pstate = pstate_read(env);
    uint64_t sp_el[4];
    int current_el = arm_current_el(env);

    memcpy(sp_el, env->sp_el, sizeof(sp_el));
    if (pstate & PSTATE_SP) {
        sp_el[current_el] = env->xregs[31];
    } else {
        sp_el[0] = env->xregs[31];
    }

    put_u64(core, A64_CPT_OFF_MAGIC, A64_CPT_CORE_MAGIC_NUMBER);
    put_u64(core, A64_CPT_OFF_PC, pc);
    put_u64(core, A64_CPT_OFF_PSTATE, pstate);
    put_u64(core, A64_CPT_OFF_CURRENT_EL, current_el);
    put_u64(core, A64_CPT_OFF_FEATURES, A64_CPT_FLAG_HAS_FPSIMD);
    put_u64(core, A64_CPT_OFF_MISC_DONE, 1);

    for (int i = 0; i < 31; i++) {
        put_u64(core, A64_CPT_OFF_INT_REG + i * 8, env->xregs[i]);
    }
    put_u64(core, A64_CPT_OFF_INT_DONE, 1);

    for (int i = 0; i < 4; i++) {
        put_u64(core, A64_CPT_OFF_SP_REG + i * 8, sp_el[i]);
    }
    put_u64(core, A64_CPT_OFF_SP_DONE, 1);

    put_u64(core, A64_CPT_OFF_ELR_SPSR, env->elr_el[1]);
    put_u64(core, A64_CPT_OFF_ELR_SPSR + 8,
            env->banked_spsr[aarch64_banked_spsr_index(1)]);
    put_u64(core, A64_CPT_OFF_ELR_SPSR + 16, env->elr_el[2]);
    put_u64(core, A64_CPT_OFF_ELR_SPSR + 24,
            env->banked_spsr[aarch64_banked_spsr_index(2)]);
    put_u64(core, A64_CPT_OFF_ELR_SPSR + 32, env->elr_el[3]);
    put_u64(core, A64_CPT_OFF_ELR_SPSR + 40,
            env->banked_spsr[aarch64_banked_spsr_index(3)]);
    put_u64(core, A64_CPT_OFF_ELR_DONE, 1);

    write_sysregs(core, env);

    put_u64(core, A64_CPT_OFF_IDREG, 0);
    put_u64(core, A64_CPT_OFF_IDREG + 8, 0);
    put_u64(core, A64_CPT_OFF_IDREG_DONE, 1);

    for (int i = 0; i < 32; i++) {
        uint64_t *q = aa64_vfp_qreg(env, i);

        put_u64(core, A64_CPT_OFF_FLOAT_REG + i * 16, q[0]);
        put_u64(core, A64_CPT_OFF_FLOAT_REG + i * 16 + 8, q[1]);
    }
    put_u64(core, A64_CPT_OFF_FLOAT_REG + 32 * 16, vfp_get_fpsr(env));
    put_u64(core, A64_CPT_OFF_FLOAT_REG + 32 * 16 + 8, vfp_get_fpcr(env));
    put_u64(core, A64_CPT_OFF_FLOAT_DONE, 1);
}

static int write_metadata(int fd, CPUARMState *env, uint64_t pc)
{
    const uint64_t metadata_size = 40 + 208;
    const uint64_t cpt_offset = ROUND_UP(metadata_size, A64_CPT_METADATA_ALIGN);
    g_autofree uint8_t *metadata = g_malloc0(cpt_offset);
    g_autofree uint8_t *core = g_malloc0(A64_CPT_SINGLE_CORE_SIZE);
    int ret;

    write_checkpoint_header(metadata);
    write_checkpoint_layout(metadata);
    write_core_state(core, env, pc);

    ret = pwrite_full(fd, metadata, cpt_offset, A64_CPT_DEFAULT_HEADER_OFFSET);
    if (ret < 0) {
        return ret;
    }
    return pwrite_full(fd, core, A64_CPT_SINGLE_CORE_SIZE,
                       A64_CPT_DEFAULT_HEADER_OFFSET + cpt_offset);
}

static char *checkpoint_output_path(uint64_t insns)
{
    return g_strdup_printf("%s/%" PRIu64 "/_%" PRIu64 "_.bin",
                           a64_cpt.output_dir, insns, insns);
}

static bool dump_checkpoint(CPUARMState *env, uint64_t pc, uint64_t rel_insns)
{
    g_autofree char *path = checkpoint_output_path(rel_insns);
    g_autofree char *dir = g_path_get_dirname(path);
    uint8_t *ram_ptr = memory_region_get_ram_ptr(a64_cpt.ram);
    int fd;
    int ret;

    if (g_mkdir_with_parents(dir, 0775) < 0) {
        error_report("a64 checkpoint: failed to create directory '%s': %s",
                     dir, g_strerror(errno));
        return false;
    }

    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_BINARY, 0664);
    if (fd < 0) {
        error_report("a64 checkpoint: failed to open '%s': %s",
                     path, g_strerror(errno));
        return false;
    }

    ret = write_sparse_ram(fd, ram_ptr, a64_cpt.ram_size);
    if (ret == 0) {
        ret = write_metadata(fd, env, pc);
    }
    if (close(fd) < 0 && ret == 0) {
        ret = -errno;
    }

    if (ret < 0) {
        error_report("a64 checkpoint: failed to write '%s': %s",
                     path, g_strerror(-ret));
        return false;
    }

    info_report("a64 checkpoint: wrote %s at relative instruction %" PRIu64
                " pc=0x%" PRIx64,
                path, rel_insns, pc);
    return true;
}

void a64_checkpoint_configure(MemoryRegion *ram, uint64_t ram_base,
                              uint64_t ram_size, const char *mode,
                              const char *output_dir,
                              const char *cutpoints,
                              const char *cutpoints_file,
                              const char *simpoint_path,
                              const char *simpoint_file,
                              uint64_t cpt_interval,
                              bool exit_after_last,
                              Error **errp)
{
    bool enabled;

    enabled = mode && (g_strcmp0(mode, "SimpointCheckpoint") == 0 ||
                       g_strcmp0(mode, "simpoint") == 0 ||
                       g_strcmp0(mode, "checkpoint") == 0);
    if (!enabled) {
        a64_cpt.enabled = false;
        clear_points();
        return;
    }

    if (!ram) {
        error_setg(errp, "checkpoint mode requires a RAM MemoryRegion");
        return;
    }

    clear_points();
    g_free(a64_cpt.output_dir);
    a64_cpt.output_dir = g_strdup(output_dir && output_dir[0] != '\0' ?
                                  output_dir : "a64-checkpoints");
    a64_cpt.ram = ram;
    a64_cpt.ram_base = ram_base;
    a64_cpt.ram_size = ram_size;
    a64_cpt.exit_after_last = exit_after_last;
    a64_cpt.window_started = false;
    a64_cpt.window_base = 0;
    a64_cpt.writing = false;

    if (!parse_cutpoints_string(cutpoints, errp) ||
        !parse_cutpoints_file(cutpoints_file, false, cpt_interval, errp) ||
        !parse_simpoint_path(simpoint_path, simpoint_file, cpt_interval, errp)) {
        a64_cpt.enabled = false;
        clear_points();
        return;
    }

    sort_and_dedupe_points();
    if (!a64_cpt.points || a64_cpt.points->len == 0) {
        error_setg(errp, "checkpoint mode requires at least one cutpoint");
        a64_cpt.enabled = false;
        return;
    }

    a64_cpt.enabled = true;
    info_report("a64 checkpoint: enabled with %u cutpoint(s), output '%s'",
                a64_cpt.points->len, a64_cpt.output_dir);
}

bool a64_checkpoint_is_enabled(void)
{
    return a64_cpt.enabled;
}

bool a64_checkpoint_has_pending(void)
{
    return a64_cpt.enabled && a64_cpt.points &&
           a64_cpt.next_point < a64_cpt.points->len;
}

void a64_checkpoint_notify_profiler(CPUARMState *env, bool start)
{
    if (!a64_cpt.enabled) {
        return;
    }

    if (start) {
        a64_cpt.window_base = env->profiling_insns;
        a64_cpt.window_started = true;
        info_report("a64 checkpoint: profiling window started at absolute "
                    "instruction %" PRIu64, a64_cpt.window_base);
    } else {
        a64_cpt.window_started = false;
        info_report("a64 checkpoint: profiling window stopped at absolute "
                    "instruction %" PRIu64, env->profiling_insns);
    }
}

void a64_checkpoint_try_take(CPUARMState *env, uint64_t pc)
{
    uint64_t rel_insns;
    A64CheckpointPoint *point;

    if (!a64_checkpoint_has_pending() || !a64_cpt.window_started ||
        a64_cpt.writing || env->profiling_insns < a64_cpt.window_base) {
        return;
    }

    rel_insns = env->profiling_insns - a64_cpt.window_base;
    point = &g_array_index(a64_cpt.points, A64CheckpointPoint,
                           a64_cpt.next_point);
    if (rel_insns < point->insns) {
        return;
    }

    a64_cpt.writing = true;
    if (dump_checkpoint(env, pc, point->insns)) {
        a64_cpt.next_point++;
    }
    a64_cpt.writing = false;

    if (!a64_checkpoint_has_pending() && a64_cpt.exit_after_last) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_QMP_QUIT);
    }
}
