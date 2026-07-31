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

#ifdef CONFIG_ZSTD
#include <zstd.h>
#endif

#define A64_CPT_SNAPSHOT_MAGIC UINT64_C(0x0050414e53343641)
#define A64_CPT_SNAPSHOT_VERSION UINT64_C(1)
#define A64_CPT_SNAPSHOT_HEADER_SIZE UINT64_C(256)
#define A64_CPT_RESTORER_RESERVED_SIZE UINT64_C(0x100000)
#define A64_CPT_DEFAULT_HEADER_OFFSET A64_CPT_RESTORER_RESERVED_SIZE
#define A64_CPT_STREAM_ALIGN UINT64_C(16)
#define A64_CPT_ZSTD_CHUNK_SIZE (1 * MiB)
#define A64_CPT_ZSTD_LEVEL 1

#define A64_CPT_FLAG_HAS_FPSIMD (UINT64_C(1) << 0)
#define A64_CPT_FLAG_HAS_SVE    (UINT64_C(1) << 1)
#define A64_CPT_FLAG_HAS_PAUTH  (UINT64_C(1) << 4)

#define A64_CPT_SVE_ZREG_COUNT 32U
#define A64_CPT_SVE_PREG_COUNT 16U
#define A64_CPT_MAX_SVE_VL_BYTES 256U
#define A64_CPT_INT_REGS_SIZE (31U * sizeof(uint64_t))
#define A64_CPT_SP_REGS_SIZE (4U * sizeof(uint64_t))
#define A64_CPT_ELR_SPSR_SIZE (6U * sizeof(uint64_t))
#define A64_CPT_SYSREG_STATE_SIZE (49U * sizeof(uint64_t))
#define A64_CPT_FPSIMD_SIZE (32U * 16U + 2U * sizeof(uint64_t))
#define A64_CPT_PAUTH_SIZE (10U * sizeof(uint64_t))

typedef struct A64CheckpointPoint {
    uint64_t measure_insns;
    uint64_t checkpoint_insns;
    uint64_t warmup_insns;
} A64CheckpointPoint;

typedef struct A64CheckpointOverlay {
    uint64_t offset;
    const uint8_t *data;
    size_t len;
} A64CheckpointOverlay;

typedef struct A64SnapshotLayout {
    uint64_t feature_flags;
    uint64_t int_regs_offset;
    uint64_t sp_regs_offset;
    uint64_t elr_spsr_offset;
    uint64_t sysregs_offset;
    uint64_t fpsimd_offset;
    uint64_t sve_zregs_offset;
    uint64_t sve_pregs_offset;
    uint64_t sve_ffr_offset;
    uint64_t sve_vl_bytes;
    uint64_t sve_max_vl_bytes;
    uint64_t pauth_offset;
    uint64_t total_size;
} A64SnapshotLayout;

typedef struct A64CheckpointState {
    bool enabled;
    bool window_started;
    bool exit_after_last;
    bool writing;
    uint64_t window_base;
    uint64_t warmup_interval;
    uint64_t ram_base;
    uint64_t ram_size;
    MemoryRegion *ram;
    char *output_dir;
    GArray *points;
    size_t next_point;
    uint64_t dropped_points;
} A64CheckpointState;

static A64CheckpointState a64_cpt;

static void put_u64(void *base, uint64_t off, uint64_t value)
{
    stq_le_p((uint8_t *)base + off, value);
}

static void put_le_words(void *base, uint64_t off, const uint64_t *words,
                         size_t len)
{
    uint8_t *dst = (uint8_t *)base + off;

    for (size_t i = 0; i < len; i++) {
        dst[i] = words[i / sizeof(uint64_t)] >>
                 ((i % sizeof(uint64_t)) * 8);
    }
}

static int compare_points(gconstpointer a, gconstpointer b)
{
    const A64CheckpointPoint *pa = a;
    const A64CheckpointPoint *pb = b;

    if (pa->checkpoint_insns != pb->checkpoint_insns) {
        return pa->checkpoint_insns < pb->checkpoint_insns ? -1 : 1;
    }
    if (pa->measure_insns != pb->measure_insns) {
        return pa->measure_insns < pb->measure_insns ? -1 : 1;
    }
    if (pa->warmup_insns == pb->warmup_insns) {
        return 0;
    }
    return pa->warmup_insns < pb->warmup_insns ? -1 : 1;
}

static void clear_points(void)
{
    if (a64_cpt.points) {
        g_array_unref(a64_cpt.points);
        a64_cpt.points = NULL;
    }
    a64_cpt.next_point = 0;
}

static void add_point(uint64_t measure_insns)
{
    A64CheckpointPoint point = {
        .measure_insns = measure_insns,
    };

    if (measure_insns >= a64_cpt.warmup_interval) {
        point.checkpoint_insns = measure_insns - a64_cpt.warmup_interval;
        point.warmup_insns = a64_cpt.warmup_interval;
    } else {
        a64_cpt.dropped_points++;
        info_report("a64 checkpoint: dropping measurement point %" PRIu64
                    " before warmup %" PRIu64,
                    measure_insns, a64_cpt.warmup_interval);
        return;
    }

    if (!a64_cpt.points) {
        a64_cpt.points = g_array_new(false, false, sizeof(point));
    }
    g_array_append_val(a64_cpt.points, point);
}

static void sort_and_dedupe_points(void)
{
    GArray *dedup;
    A64CheckpointPoint last = { 0 };
    bool have_last = false;

    if (!a64_cpt.points || a64_cpt.points->len == 0) {
        return;
    }

    g_array_sort(a64_cpt.points, compare_points);

    dedup = g_array_new(false, false, sizeof(A64CheckpointPoint));
    for (guint i = 0; i < a64_cpt.points->len; i++) {
        A64CheckpointPoint point =
            g_array_index(a64_cpt.points, A64CheckpointPoint, i);

        if (have_last &&
            point.measure_insns == last.measure_insns &&
            point.checkpoint_insns == last.checkpoint_insns &&
            point.warmup_insns == last.warmup_insns) {
            continue;
        }
        g_array_append_val(dedup, point);
        last = point;
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

#ifdef CONFIG_ZSTD
static int write_full_fd(int fd, const void *buf, size_t len)
{
    ssize_t ret;

    ret = qemu_write_full(fd, buf, len);
    if (ret == len) {
        return 0;
    }
    return errno ? -errno : -EIO;
}

static bool range_overlaps(uint64_t a_start, uint64_t a_len,
                           uint64_t b_start, uint64_t b_len)
{
    uint64_t a_end = a_start + a_len;
    uint64_t b_end = b_start + b_len;

    return a_start < b_end && b_start < a_end;
}

static bool apply_overlays(uint8_t *chunk, uint64_t chunk_offset,
                           size_t chunk_len,
                           const A64CheckpointOverlay *overlays,
                           size_t nr_overlays)
{
    bool applied = false;

    for (size_t i = 0; i < nr_overlays; i++) {
        const A64CheckpointOverlay *overlay = &overlays[i];
        uint64_t start;
        uint64_t end;

        if (!range_overlaps(chunk_offset, chunk_len,
                            overlay->offset, overlay->len)) {
            continue;
        }

        start = MAX(chunk_offset, overlay->offset);
        end = MIN(chunk_offset + chunk_len, overlay->offset + overlay->len);
        memcpy(chunk + start - chunk_offset,
               overlay->data + start - overlay->offset,
               end - start);
        applied = true;
    }

    return applied;
}

static int zstd_write_input(ZSTD_CCtx *cctx, int fd, const void *buf,
                            size_t len, ZSTD_EndDirective directive)
{
    g_autofree uint8_t *outbuf = g_malloc(ZSTD_CStreamOutSize());
    ZSTD_inBuffer input = {
        .src = buf,
        .size = len,
        .pos = 0,
    };
    size_t outbuf_size = ZSTD_CStreamOutSize();

    do {
        ZSTD_outBuffer output = {
            .dst = outbuf,
            .size = outbuf_size,
            .pos = 0,
        };
        size_t ret = ZSTD_compressStream2(cctx, &output, &input, directive);

        if (ZSTD_isError(ret)) {
            error_report("a64 checkpoint: zstd compression failed: %s",
                         ZSTD_getErrorName(ret));
            return -EIO;
        }
        if (output.pos > 0) {
            int write_ret = write_full_fd(fd, outbuf, output.pos);

            if (write_ret < 0) {
                return write_ret;
            }
        }
        if (directive == ZSTD_e_end && ret == 0) {
            break;
        }
    } while (input.pos < input.size || directive == ZSTD_e_end);

    return 0;
}
#endif

static uint64_t checkpoint_feature_flags(CPUARMState *env)
{
    ARMCPU *cpu = env_archcpu(env);
    uint64_t features = 0;

    if (cpu_isar_feature(aa64_fp_simd, cpu)) {
        features |= A64_CPT_FLAG_HAS_FPSIMD;
    }
    if (cpu_isar_feature(aa64_sve, cpu)) {
        features |= A64_CPT_FLAG_HAS_SVE;
    }
    if (cpu_isar_feature(aa64_pauth, cpu)) {
        features |= A64_CPT_FLAG_HAS_PAUTH;
    }
    return features;
}

static uint64_t append_snapshot_block(uint64_t *stream_end, uint64_t size)
{
    uint64_t offset = ROUND_UP(*stream_end, A64_CPT_STREAM_ALIGN);

    g_assert(size <= UINT64_MAX - offset);
    *stream_end = offset + size;
    return offset;
}

static void build_snapshot_layout(CPUARMState *env, A64SnapshotLayout *layout)
{
    ARMCPU *cpu = env_archcpu(env);
    uint64_t stream_end = A64_CPT_SNAPSHOT_HEADER_SIZE;

    memset(layout, 0, sizeof(*layout));
    layout->feature_flags = checkpoint_feature_flags(env);
    layout->int_regs_offset = append_snapshot_block(&stream_end,
                                                     A64_CPT_INT_REGS_SIZE);
    layout->sp_regs_offset = append_snapshot_block(&stream_end,
                                                   A64_CPT_SP_REGS_SIZE);
    layout->elr_spsr_offset = append_snapshot_block(&stream_end,
                                                    A64_CPT_ELR_SPSR_SIZE);
    layout->sysregs_offset = append_snapshot_block(&stream_end,
                                                   A64_CPT_SYSREG_STATE_SIZE);

    if (layout->feature_flags & A64_CPT_FLAG_HAS_FPSIMD) {
        layout->fpsimd_offset = append_snapshot_block(&stream_end,
                                                      A64_CPT_FPSIMD_SIZE);
    }
    if (layout->feature_flags & A64_CPT_FLAG_HAS_SVE) {
        uint64_t pred_bytes;

        layout->sve_vl_bytes =
            (sve_vqm1_for_el_sm(env, arm_current_el(env), false) + 1) * 16;
        layout->sve_max_vl_bytes = cpu->sve_max_vq * 16;
        pred_bytes = layout->sve_vl_bytes / 8;

        g_assert(layout->sve_vl_bytes >= 16);
        g_assert(layout->sve_vl_bytes <= A64_CPT_MAX_SVE_VL_BYTES);
        g_assert((layout->sve_vl_bytes & 15) == 0);
        g_assert(layout->sve_max_vl_bytes >= layout->sve_vl_bytes);
        g_assert(layout->sve_max_vl_bytes <= A64_CPT_MAX_SVE_VL_BYTES);

        layout->sve_zregs_offset = append_snapshot_block(
            &stream_end, A64_CPT_SVE_ZREG_COUNT * layout->sve_vl_bytes);
        layout->sve_pregs_offset = append_snapshot_block(
            &stream_end, A64_CPT_SVE_PREG_COUNT * pred_bytes);
        layout->sve_ffr_offset = append_snapshot_block(&stream_end,
                                                       pred_bytes);
    }
    if (layout->feature_flags & A64_CPT_FLAG_HAS_PAUTH) {
        layout->pauth_offset = append_snapshot_block(&stream_end,
                                                     A64_CPT_PAUTH_SIZE);
    }

    layout->total_size = ROUND_UP(stream_end, A64_CPT_STREAM_ALIGN);
    g_assert(layout->total_size <= A64_CPT_RESTORER_RESERVED_SIZE);
}

static bool tag_memory_has_data(MemoryRegion *mr)
{
    const uint8_t *tags;
    uint64_t size;

    if (!mr) {
        return false;
    }
    if (!memory_region_is_ram(mr)) {
        return true;
    }

    tags = memory_region_get_ram_ptr(mr);
    size = memory_region_size(mr);
    for (uint64_t i = 0; i < size; i++) {
        if (tags[i] != 0) {
            return true;
        }
    }
    return false;
}

static bool checkpoint_state_is_supported(CPUARMState *env)
{
    ARMCPU *cpu = env_archcpu(env);

    if (cpu_isar_feature(aa64_sme, cpu)) {
        if (env->svcr != 0 || env->cp15.tpidr2_el0 != 0 ||
            env->vfp.smcr_el[1] != 0 || env->vfp.smcr_el[2] != 0 ||
            env->vfp.smcr_el[3] != 0) {
            error_report("a64 checkpoint: nonzero SME state is not supported "
                         "by snapshot version 1");
            return false;
        }
    }

    if (cpu_isar_feature(aa64_mte, cpu)) {
        if (env->cp15.gcr_el1 != 0 || env->cp15.rgsr_el1 != 0 ||
            env->cp15.tfsr_el[0] != 0 || env->cp15.tfsr_el[1] != 0 ||
            env->cp15.tfsr_el[2] != 0 || env->cp15.tfsr_el[3] != 0 ||
            tag_memory_has_data(cpu->tag_memory) ||
            tag_memory_has_data(cpu->secure_tag_memory)) {
            error_report("a64 checkpoint: nonzero MTE state is not supported "
                         "by snapshot version 1");
            return false;
        }
    }

    if (cpu_isar_feature(aa64_fpmr, cpu) && env->vfp.fpmr != 0) {
        error_report("a64 checkpoint: nonzero FPMR is not supported by "
                     "snapshot version 1");
        return false;
    }
    if (cpu_isar_feature(aa64_scxtnum, cpu) &&
        (env->scxtnum_el[0] != 0 || env->scxtnum_el[1] != 0 ||
         env->scxtnum_el[2] != 0 || env->scxtnum_el[3] != 0)) {
        error_report("a64 checkpoint: nonzero SCXTNUM state is not supported "
                     "by snapshot version 1");
        return false;
    }
    return true;
}

static void write_snapshot_header(uint8_t *snapshot, CPUARMState *env,
                                  uint64_t pc,
                                  const A64SnapshotLayout *layout)
{
    uint64_t pstate = pstate_read(env);

    put_u64(snapshot, 0, A64_CPT_SNAPSHOT_MAGIC);
    put_u64(snapshot, 8, A64_CPT_SNAPSHOT_VERSION);
    put_u64(snapshot, 16, A64_CPT_SNAPSHOT_HEADER_SIZE);
    put_u64(snapshot, 24, layout->total_size);
    put_u64(snapshot, 32, 1);
    put_u64(snapshot, 40, 0);
    put_u64(snapshot, 48, layout->feature_flags);
    put_u64(snapshot, 56, arm_current_el(env));
    put_u64(snapshot, 64, pc);
    put_u64(snapshot, 72, pstate);
    put_u64(snapshot, 80, layout->int_regs_offset);
    put_u64(snapshot, 88, layout->sp_regs_offset);
    put_u64(snapshot, 96, layout->elr_spsr_offset);
    put_u64(snapshot, 104, layout->sysregs_offset);
    put_u64(snapshot, 112, layout->fpsimd_offset);
    put_u64(snapshot, 120, layout->sve_zregs_offset);
    put_u64(snapshot, 128, layout->sve_pregs_offset);
    put_u64(snapshot, 136, layout->sve_ffr_offset);
    put_u64(snapshot, 144, layout->sve_vl_bytes);
    put_u64(snapshot, 152, layout->sve_max_vl_bytes);
    for (int i = 0; i < 4; i++) {
        put_u64(snapshot, 160 + i * 8, env->vfp.zcr_el[i]);
    }
    put_u64(snapshot, 192, layout->pauth_offset);
}

static void put_next_u64(uint8_t *snapshot, uint64_t *offset, uint64_t value)
{
    put_u64(snapshot, *offset, value);
    *offset += sizeof(value);
}

static void write_sysreg_state(uint8_t *snapshot, CPUARMState *env,
                               uint64_t offset)
{
    uint64_t end = offset + A64_CPT_SYSREG_STATE_SIZE;

    put_next_u64(snapshot, &offset, env->cp15.sctlr_el[1]);
    put_next_u64(snapshot, &offset, env->cp15.tcr_el[1]);
    put_next_u64(snapshot, &offset, env->cp15.ttbr0_el[1]);
    put_next_u64(snapshot, &offset, env->cp15.ttbr1_el[1]);
    put_next_u64(snapshot, &offset, env->cp15.mair_el[1]);
    put_next_u64(snapshot, &offset, 0); /* AMAIR_EL1 is RAZ/WI in QEMU. */
    put_next_u64(snapshot, &offset, env->cp15.vbar_el[1]);
    put_next_u64(snapshot, &offset, env->cp15.contextidr_el[1]);
    put_next_u64(snapshot, &offset, env->cp15.tpidr_el[0]);
    put_next_u64(snapshot, &offset, env->cp15.tpidrro_el[0]);
    put_next_u64(snapshot, &offset, env->cp15.tpidr_el[1]);
    put_next_u64(snapshot, &offset, env->cp15.cpacr_el1);
    put_next_u64(snapshot, &offset, env->cp15.esr_el[1]);
    put_next_u64(snapshot, &offset, env->cp15.far_el[1]);
    put_next_u64(snapshot, &offset, 0); /* AFSR0_EL1 is RAZ/WI. */
    put_next_u64(snapshot, &offset, 0); /* AFSR1_EL1 is RAZ/WI. */
    put_next_u64(snapshot, &offset, env->cp15.c14_timer[GTIMER_PHYS].ctl);
    put_next_u64(snapshot, &offset, env->cp15.c14_timer[GTIMER_PHYS].cval);
    put_next_u64(snapshot, &offset, env->cp15.c14_timer[GTIMER_VIRT].ctl);
    put_next_u64(snapshot, &offset, env->cp15.c14_timer[GTIMER_VIRT].cval);
    put_next_u64(snapshot, &offset, env->cp15.c14_cntfrq);
    put_next_u64(snapshot, &offset, env->cp15.c14_cntkctl);

    put_next_u64(snapshot, &offset, env->cp15.sctlr_el[2]);
    put_next_u64(snapshot, &offset, env->cp15.tcr_el[2]);
    put_next_u64(snapshot, &offset, env->cp15.ttbr0_el[2]);
    put_next_u64(snapshot, &offset, env->cp15.ttbr1_el[2]);
    put_next_u64(snapshot, &offset, env->cp15.mair_el[2]);
    put_next_u64(snapshot, &offset, 0); /* AMAIR_EL2 is RAZ/WI. */
    put_next_u64(snapshot, &offset, env->cp15.vbar_el[2]);
    put_next_u64(snapshot, &offset, env->cp15.tpidr_el[2]);
    put_next_u64(snapshot, &offset, env->cp15.hcr_el2);
    put_next_u64(snapshot, &offset, env->cp15.cptr_el[2]);
    put_next_u64(snapshot, &offset, env->cp15.cnthctl_el2);
    put_next_u64(snapshot, &offset, env->cp15.cntvoff_el2);
    put_next_u64(snapshot, &offset, env->cp15.vtcr_el2);
    put_next_u64(snapshot, &offset, env->cp15.vttbr_el2);
    put_next_u64(snapshot, &offset, env->cp15.esr_el[2]);
    put_next_u64(snapshot, &offset, env->cp15.far_el[2]);
    put_next_u64(snapshot, &offset, 0); /* AFSR0_EL2 is RAZ/WI. */
    put_next_u64(snapshot, &offset, 0); /* AFSR1_EL2 is RAZ/WI. */

    put_next_u64(snapshot, &offset, env->cp15.sctlr_el[3]);
    put_next_u64(snapshot, &offset, env->cp15.scr_el3);
    put_next_u64(snapshot, &offset, env->cp15.cptr_el[3]);
    put_next_u64(snapshot, &offset, env->cp15.vbar_el[3]);
    put_next_u64(snapshot, &offset, env->cp15.tpidr_el[3]);
    put_next_u64(snapshot, &offset, env->cp15.esr_el[3]);
    put_next_u64(snapshot, &offset, env->cp15.far_el[3]);
    put_next_u64(snapshot, &offset, 0); /* AFSR0_EL3 is RAZ/WI. */
    put_next_u64(snapshot, &offset, 0); /* AFSR1_EL3 is RAZ/WI. */

    g_assert(offset == end);
}

static void write_sve_state(uint8_t *snapshot, CPUARMState *env,
                            const A64SnapshotLayout *layout)
{
    uint64_t pred_bytes;

    if (!(layout->feature_flags & A64_CPT_FLAG_HAS_SVE)) {
        return;
    }

    pred_bytes = layout->sve_vl_bytes / 8;
    for (unsigned int i = 0; i < A64_CPT_SVE_ZREG_COUNT; i++) {
        put_le_words(snapshot,
                     layout->sve_zregs_offset + i * layout->sve_vl_bytes,
                     env->vfp.zregs[i].d, layout->sve_vl_bytes);
    }
    for (unsigned int i = 0; i < A64_CPT_SVE_PREG_COUNT; i++) {
        put_le_words(snapshot,
                     layout->sve_pregs_offset + i * pred_bytes,
                     env->vfp.pregs[i].p, pred_bytes);
    }
    put_le_words(snapshot, layout->sve_ffr_offset,
                 env->vfp.pregs[FFR_PRED_NUM].p, pred_bytes);
}

static void write_pauth_state(uint8_t *snapshot, CPUARMState *env,
                              const A64SnapshotLayout *layout)
{
    const ARMPACKey *keys[] = {
        &env->keys.apia,
        &env->keys.apib,
        &env->keys.apda,
        &env->keys.apdb,
        &env->keys.apga,
    };

    if (!(layout->feature_flags & A64_CPT_FLAG_HAS_PAUTH)) {
        return;
    }

    for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
        put_u64(snapshot, layout->pauth_offset + i * 16, keys[i]->lo);
        put_u64(snapshot, layout->pauth_offset + i * 16 + 8, keys[i]->hi);
    }
}

static void write_snapshot_state(uint8_t *snapshot, CPUARMState *env,
                                 uint64_t pc,
                                 const A64SnapshotLayout *layout)
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

    write_snapshot_header(snapshot, env, pc, layout);

    for (int i = 0; i < 31; i++) {
        put_u64(snapshot, layout->int_regs_offset + i * 8, env->xregs[i]);
    }

    for (int i = 0; i < 4; i++) {
        put_u64(snapshot, layout->sp_regs_offset + i * 8, sp_el[i]);
    }

    put_u64(snapshot, layout->elr_spsr_offset, env->elr_el[1]);
    put_u64(snapshot, layout->elr_spsr_offset + 8,
            env->banked_spsr[aarch64_banked_spsr_index(1)]);
    put_u64(snapshot, layout->elr_spsr_offset + 16, env->elr_el[2]);
    put_u64(snapshot, layout->elr_spsr_offset + 24,
            env->banked_spsr[aarch64_banked_spsr_index(2)]);
    put_u64(snapshot, layout->elr_spsr_offset + 32, env->elr_el[3]);
    put_u64(snapshot, layout->elr_spsr_offset + 40,
            env->banked_spsr[aarch64_banked_spsr_index(3)]);

    write_sysreg_state(snapshot, env, layout->sysregs_offset);

    if (layout->feature_flags & A64_CPT_FLAG_HAS_FPSIMD) {
        for (int i = 0; i < 32; i++) {
            uint64_t *q = aa64_vfp_qreg(env, i);

            put_u64(snapshot, layout->fpsimd_offset + i * 16, q[0]);
            put_u64(snapshot, layout->fpsimd_offset + i * 16 + 8, q[1]);
        }
        put_u64(snapshot, layout->fpsimd_offset + 32 * 16,
                vfp_get_fpsr(env));
        put_u64(snapshot, layout->fpsimd_offset + 32 * 16 + 8,
                vfp_get_fpcr(env));
    }

    write_sve_state(snapshot, env, layout);
    write_pauth_state(snapshot, env, layout);
}

static void build_snapshot(CPUARMState *env, uint64_t pc,
                           uint8_t **snapshot, size_t *snapshot_len)
{
    A64SnapshotLayout layout;

    build_snapshot_layout(env, &layout);
    g_assert(layout.total_size <= SIZE_MAX);
    *snapshot_len = layout.total_size;
    *snapshot = g_malloc0(*snapshot_len);

    write_snapshot_state(*snapshot, env, pc, &layout);
}

static char *checkpoint_output_path(const A64CheckpointPoint *point)
{
    if (a64_cpt.warmup_interval == 0) {
        return g_strdup_printf("%s/%" PRIu64 "/_%" PRIu64 "_.bin.zst",
                               a64_cpt.output_dir, point->measure_insns,
                               point->measure_insns);
    }

    return g_strdup_printf("%s/%" PRIu64 "/_%" PRIu64
                           "_warmup_%" PRIu64 "_cpt_%" PRIu64 "_.bin.zst",
                           a64_cpt.output_dir, point->measure_insns,
                           point->measure_insns, point->warmup_insns,
                           point->checkpoint_insns);
}

#ifdef CONFIG_ZSTD
static int write_zstd_checkpoint(int fd, CPUARMState *env, uint64_t pc,
                                 const uint8_t *ram, uint64_t ram_size)
{
    g_autofree uint8_t *snapshot = NULL;
    g_autofree uint8_t *scratch = g_malloc(A64_CPT_ZSTD_CHUNK_SIZE);
    ZSTD_CCtx *cctx = NULL;
    A64CheckpointOverlay overlays[1];
    size_t snapshot_len;
    uint64_t overlay_end;
    size_t zret;
    int ret = 0;

    build_snapshot(env, pc, &snapshot, &snapshot_len);
    overlays[0] = (A64CheckpointOverlay) {
        .offset = A64_CPT_DEFAULT_HEADER_OFFSET,
        .data = snapshot,
        .len = snapshot_len,
    };

    overlay_end = overlays[0].offset + overlays[0].len;
    if (overlay_end > ram_size) {
        error_report("a64 checkpoint: RAM size 0x%" PRIx64
                     " is smaller than snapshot stream end 0x%" PRIx64,
                     ram_size, overlay_end);
        return -EFBIG;
    }

    cctx = ZSTD_createCCtx();
    if (!cctx) {
        return -ENOMEM;
    }

    zret = ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel,
                                  A64_CPT_ZSTD_LEVEL);
    if (ZSTD_isError(zret)) {
        error_report("a64 checkpoint: failed to set zstd level: %s",
                     ZSTD_getErrorName(zret));
        ret = -EIO;
        goto out;
    }

    for (uint64_t off = 0; off < ram_size; off += A64_CPT_ZSTD_CHUNK_SIZE) {
        size_t chunk_len = MIN((uint64_t)A64_CPT_ZSTD_CHUNK_SIZE,
                               ram_size - off);
        const uint8_t *input = ram + off;
        bool overlay_hit = false;

        for (size_t i = 0; i < ARRAY_SIZE(overlays); i++) {
            if (range_overlaps(off, chunk_len,
                               overlays[i].offset, overlays[i].len)) {
                overlay_hit = true;
                break;
            }
        }

        if (overlay_hit) {
            memcpy(scratch, ram + off, chunk_len);
            apply_overlays(scratch, off, chunk_len,
                           overlays, ARRAY_SIZE(overlays));
            input = scratch;
        }

        ret = zstd_write_input(cctx, fd, input, chunk_len, ZSTD_e_continue);
        if (ret < 0) {
            goto out;
        }
    }

    ret = zstd_write_input(cctx, fd, "", 0, ZSTD_e_end);

out:
    ZSTD_freeCCtx(cctx);
    return ret;
}
#endif

static bool dump_checkpoint(CPUARMState *env, uint64_t pc,
                            uint64_t actual_insns,
                            const A64CheckpointPoint *point)
{
    g_autofree char *path = checkpoint_output_path(point);
    g_autofree char *dir = g_path_get_dirname(path);
    uint8_t *ram_ptr = memory_region_get_ram_ptr(a64_cpt.ram);
    uint64_t overshoot = actual_insns - point->checkpoint_insns;
    int fd;
    int ret;

    if (!checkpoint_state_is_supported(env)) {
        a64_cpt.enabled = false;
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_QMP_QUIT);
        return false;
    }

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

#ifndef CONFIG_ZSTD
    error_report("a64 checkpoint: zstd support is not available; "
                 "reconfigure QEMU with --enable-zstd");
    close(fd);
    unlink(path);
    return false;
#else
    ret = write_zstd_checkpoint(fd, env, pc, ram_ptr, a64_cpt.ram_size);
    if (close(fd) < 0 && ret == 0) {
        ret = -errno;
    }

    if (ret < 0) {
        error_report("a64 checkpoint: failed to write '%s': %s",
                     path, g_strerror(-ret));
        return false;
    }

    info_report("a64 checkpoint: wrote zstd checkpoint %s at requested "
                "relative instruction %" PRIu64 " actual=%" PRIu64
                " overshoot=%" PRIu64 " pc=0x%" PRIx64
                " measurement=%" PRIu64 " warmup=%" PRIu64,
                path, point->checkpoint_insns, actual_insns, overshoot, pc,
                point->measure_insns, point->warmup_insns);
    return true;
#endif
}

void a64_checkpoint_configure(MemoryRegion *ram, uint64_t ram_base,
                              uint64_t ram_size, const char *mode,
                              const char *output_dir,
                              const char *cutpoints,
                              const char *cutpoints_file,
                              const char *simpoint_path,
                              const char *simpoint_file,
                              uint64_t cpt_interval,
                              uint64_t warmup_interval,
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
    a64_cpt.warmup_interval = warmup_interval;
    a64_cpt.exit_after_last = exit_after_last;
    a64_cpt.window_started = false;
    a64_cpt.window_base = 0;
    a64_cpt.writing = false;
    a64_cpt.dropped_points = 0;

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
    info_report("a64 checkpoint: enabled with %u cutpoint(s), output '%s', "
                "warmup %" PRIu64 ", dropped %" PRIu64,
                a64_cpt.points->len, a64_cpt.output_dir,
                a64_cpt.warmup_interval, a64_cpt.dropped_points);
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

static void a64_checkpoint_try_take_at(CPUARMState *env, uint64_t pc,
                                       uint64_t absolute_insns)
{
    uint64_t rel_insns;

    if (!a64_checkpoint_has_pending() || !a64_cpt.window_started ||
        a64_cpt.writing || absolute_insns < a64_cpt.window_base) {
        return;
    }

    rel_insns = absolute_insns - a64_cpt.window_base;
    a64_cpt.writing = true;
    while (a64_checkpoint_has_pending()) {
        A64CheckpointPoint *point =
            &g_array_index(a64_cpt.points, A64CheckpointPoint,
                           a64_cpt.next_point);

        if (rel_insns < point->checkpoint_insns) {
            break;
        }
        if (rel_insns > point->measure_insns) {
            uint64_t overshoot = rel_insns - point->checkpoint_insns;
            uint64_t late = rel_insns - point->measure_insns;

            a64_cpt.dropped_points++;
            info_report("a64 checkpoint: skipping measurement point %" PRIu64
                        " because checkpoint boundary actual=%" PRIu64
                        " is after measurement point; requested=%" PRIu64
                        " overshoot=%" PRIu64 " late=%" PRIu64
                        " pc=0x%" PRIx64 " warmup=%" PRIu64,
                        point->measure_insns, rel_insns,
                        point->checkpoint_insns, overshoot, late, pc,
                        point->warmup_insns);
            a64_cpt.next_point++;
            continue;
        }
        if (dump_checkpoint(env, pc, rel_insns, point)) {
            a64_cpt.next_point++;
        } else {
            break;
        }
    }
    a64_cpt.writing = false;

    if (!a64_checkpoint_has_pending() && a64_cpt.exit_after_last) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_QMP_QUIT);
    }
}

void a64_checkpoint_notify_profiler(CPUARMState *env, bool start, uint64_t pc)
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
        if (env->profiling_insns > 0) {
            a64_checkpoint_try_take_at(env, pc, env->profiling_insns - 1);
        }
        a64_cpt.window_started = false;
        info_report("a64 checkpoint: profiling window stopped at absolute "
                    "instruction %" PRIu64, env->profiling_insns);
    }
}

void a64_checkpoint_try_take(CPUARMState *env, uint64_t pc)
{
    a64_checkpoint_try_take_at(env, pc, env->profiling_insns);
}
