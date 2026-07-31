#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run an end-to-end AArch64 gcpt SimPoint flow:
#   1. profile a raw payload into a SimPoint BBV
#   2. cluster the BBV with SimPoint 3.2
#   3. generate zstd checkpoints from the selected SimPoint locations
#
# The gcpt payload controls the profiling window with AArch64 simtrap
# pseudo-instructions. This script intentionally does not use an instruction
# count stop trigger; PROFILE_STOP exits QEMU.
#
# Defaults are chosen for the gcpt gemm workload used during AArch64
# mini-virt checkpoint development. Override paths and knobs with env vars.

set -euo pipefail

usage()
{
    cat <<'EOF'
Usage:
  scripts/a64-gcpt-simpoint-flow.sh

Environment overrides:
  PAYLOAD=/path/to/gcpt.bin
  OUT_DIR=/tmp/a64-gcpt-simpoint-flow
  QEMU_BIN=/path/to/qemu-system-aarch64
  SIMPOINT_BIN=/path/to/SimPoint.3.2-fix/bin/simpoint
  SIMPOINT_PLUGIN=/path/to/libsimpoint.so
  ZSTD_BIN=/path/to/zstd
  CPU_MODEL=cortex-a57
  INTERVAL=10000
  WARMUP=5000
  MEMORY=1G
  MAXK=30
  NUM_INIT_SEEDS=2
  ITERS=1000
  SEED_KM=12345
  SEED_PROJ=67890
  RESTORE_VALIDATE=1
  RESTORE_TIMEOUT=30s
  FORCE=1

Paths containing whitespace are rejected.

Outputs:
  $OUT_DIR/profile/simpoint_bbv.gz
  $OUT_DIR/cluster/simpoints0.raw
  $OUT_DIR/cluster/weights0.raw
  $OUT_DIR/cluster/simpoints0
  $OUT_DIR/cluster/weights0
  $OUT_DIR/cluster/dropped-warmup.tsv
  $OUT_DIR/checkpoints/<measurement-point>/*.bin.zst
  $OUT_DIR/slices.tsv
  $OUT_DIR/restore-validation.tsv
  $OUT_DIR/summary.txt
EOF
}

die()
{
    printf 'error: %s\n' "$*" >&2
    exit 1
}

info()
{
    printf '[a64-gcpt-flow] %s\n' "$*"
}

require_file()
{
    local path=$1
    [[ -f "$path" ]] || die "missing file: $path"
}

require_exec()
{
    local path=$1
    [[ -x "$path" ]] || die "missing executable: $path"
}

require_path_without_whitespace()
{
    local name=$1
    local path=$2

    [[ ! "$path" =~ [[:space:]] ]] ||
        die "$name must not contain whitespace: $path"
}

require_nonempty_gzip_text()
{
    local path=$1

    gzip -dc "$path" | awk '
        NF { found = 1 }
        END { exit(found ? 0 : 1) }
    ' || die "gzip file has no text records: $path"
}

validate_bbv_intervals()
{
    local path=$1
    local profile_count_re
    local stats

    stats=$(gzip -dc "$path" | awk -v target="$interval" '
        {
            sum = 0
            entries = 0
            for (i = 1; i <= NF; i++) {
                token = $i
                if (i == 1 && substr(token, 1, 1) == "T") {
                    token = substr(token, 2)
                }
                if (split(token, fields, ":") != 3 || fields[1] != "" ||
                    fields[2] !~ /^[0-9]+$/ ||
                    fields[3] !~ /^[0-9]+$/) {
                    bad = 1
                    next
                }
                sum += fields[3]
                entries++
            }
            if (entries == 0) {
                bad = 1
            }
            total += sum
            boundary_drift = total - NR * target
            if (boundary_drift < 0 || boundary_drift >= target) {
                bad = 1
            } else if (boundary_drift > max_boundary_drift) {
                max_boundary_drift = boundary_drift
            }
        }
        END {
            if (NR == 0 || bad) {
                exit 1
            }
            printf "%d %d %d %d\n", NR, total,
                max_boundary_drift, total - NR * target
        }
    ') || die "BBV interval validation failed: $path"

    read -r bbv_vectors bbv_total bbv_max_boundary_drift \
        bbv_final_boundary_drift <<< "$stats"
    profile_count_re='s/^simpoint: vcpu [0-9][0-9]* profiling stopped after '
    profile_count_re+='\([0-9][0-9]*\) instructions$/\1/p'
    profile_total=$(sed -n "$profile_count_re" \
        "$logs_dir/profile.stderr" | tail -n 1)
    [[ "$profile_total" =~ ^[0-9]+$ ]] ||
        die "profiling log did not contain the final instruction count"
    (( bbv_total <= profile_total )) ||
        die "BBV instruction sum $bbv_total exceeds profiler count" \
            "$profile_total"
    profile_tail=$((profile_total - bbv_total))
    (( bbv_final_boundary_drift + profile_tail < interval )) ||
        die "profiler omitted a complete BBV: final drift" \
            "$bbv_final_boundary_drift plus tail $profile_tail reaches" \
            "interval $interval"

    info "validated $bbv_vectors complete BBV vector(s)," \
        "emitted $bbv_total instructions, tail $profile_tail," \
        "max boundary drift $bbv_max_boundary_drift"
}

find_simpoint_bin()
{
    local candidate

    for candidate in \
        "${SIMPOINT_BIN:-}" \
        "$repo_root/../SimPoint.3.2-fix/bin/simpoint" \
        "$repo_root/../simpoint.3.2-fix/bin/simpoint"
    do
        if [[ -n "$candidate" && -x "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    return 1
}

filter_and_normalize_simpoints()
{
    local raw_simpoints=$cluster_dir/simpoints0.raw
    local raw_weights=$cluster_dir/weights0.raw
    local simpoints=$cluster_dir/simpoints0
    local weights=$cluster_dir/weights0
    local dropped=$cluster_dir/dropped-warmup.tsv
    local stats

    rm -f "$simpoints" "$weights" "$dropped"
    stats=$(awk -v interval="$interval" -v warmup="$warmup" \
        -v simout="$simpoints" -v weightout="$weights" \
        -v dropout="$dropped" '
        BEGIN {
            count = 0
        }
        FNR == NR {
            if (NF < 2 || $1 + 0 < 0 || seen_weight[$2]++) {
                bad = 1
                next
            }
            weight[$2] = $1 + 0
            weight_rows++
            next
        }
        {
            if (NF < 2 || $1 !~ /^[0-9]+$/ || seen_label[$2]++ ||
                seen_location[$1]++ || !($2 in weight)) {
                bad = 1
                next
            }
            location[count] = $1
            label[count] = $2
            measurement = $1 * interval
            original_total += weight[$2]
            if (measurement < warmup) {
                drop[count] = 1
                dropped_weight += weight[$2]
            } else {
                retained_weight += weight[$2]
            }
            count++
        }
        END {
            header = "label\tlocation\toriginal_weight"
            header = header "\tmeasurement\twarmup"
            print header > dropout
            if (bad || count == 0 || weight_rows != count ||
                retained_weight <= 0) {
                exit 1
            }
            for (i = 0; i < count; i++) {
                measurement = location[i] * interval
                if (drop[i]) {
                    printf "%s\t%s\t%.17g\t%.0f\t%s\n", label[i],
                        location[i], weight[label[i]], measurement, warmup \
                        > dropout
                    dropped_count++
                    continue
                }
                printf "%s %s\n", location[i], label[i] > simout
                printf "%.17g %s\n", weight[label[i]] / retained_weight,
                    label[i] > weightout
                retained_count++
            }
            printf "%d %d %.17g %.17g %.17g\n", retained_count,
                dropped_count, original_total, retained_weight, dropped_weight
        }
    ' "$raw_weights" "$raw_simpoints") ||
        die "failed to pair, filter, and normalize SimPoint outputs"

    read -r retained_simpoint_count dropped_warmup_count \
        original_weight_total retained_original_weight \
        dropped_warmup_weight <<< "$stats"
    [[ -s "$simpoints" && -s "$weights" ]] ||
        die "warmup filtering removed every SimPoint"

    normalized_total=$(awk '{ sum += $1 } END { printf "%.17g\n", sum }' \
        "$weights")
    awk -v total="$normalized_total" 'BEGIN {
        delta = total - 1
        if (delta < 0) {
            delta = -delta
        }
        exit(delta <= 1e-12 ? 0 : 1)
    }' || die "normalized SimPoint weights sum to $normalized_total" \
        "instead of 1"

    info "retained $retained_simpoint_count SimPoint(s)," \
        "dropped $dropped_warmup_count before warmup=$warmup," \
        "normalized retained weight $retained_original_weight to 1"
}

count_expected_checkpoints()
{
    awk -v interval="$interval" '
        NF >= 1 {
            measurement = $1 * interval
            if (!seen[measurement]++) {
                count++
            }
        }
        END { print count + 0 }
    ' "$cluster_dir/simpoints0"
}

count_runtime_skips()
{
    grep -c 'skipping measurement point .* after measurement point' \
        "$logs_dir/checkpoint.stderr" || true
}

validate_cutpoints()
{
    local report=$out_dir/slices.tsv
    local fail_count=0
    local expected_from_simpoints=0
    local checkpoint_logs=0
    local runtime_skip_count
    local skipped_re
    local status
    local written_re

    declare -A weights
    declare -A skipped_after_measure
    declare -A skipped_actual_by_measure
    declare -A skipped_requested_by_measure
    declare -A skipped_overshoot_by_measure
    declare -A skipped_pc_by_measure
    declare -A skipped_warmup_by_measure
    declare -A actual_by_measure
    declare -A requested_by_measure
    declare -A overshoot_by_measure
    declare -A pc_by_measure
    declare -A path_by_measure
    declare -A warmup_by_measure
    declare -A seen_measure

    while read -r weight label _; do
        [[ -n "${weight:-}" && -n "${label:-}" ]] || continue
        weights[$label]=$weight
    done < "$cluster_dir/weights0"

    skipped_re='skipping measurement point ([0-9]+) because checkpoint '
    skipped_re+='boundary actual=([0-9]+) is after measurement point; '
    skipped_re+='requested=([0-9]+) overshoot=([0-9]+) late=([0-9]+) '
    skipped_re+='pc=([^[:space:]]+) warmup=([0-9]+)'
    written_re='wrote zstd checkpoint ([^[:space:]]+) at requested relative '
    written_re+='instruction ([0-9]+) actual=([0-9]+) '
    written_re+='overshoot=([0-9]+) pc=([^[:space:]]+) '
    written_re+='measurement=([0-9]+) warmup=([0-9]+)'

    runtime_skip_count=$(count_runtime_skips)
    while IFS= read -r line; do
        if [[ $line =~ $skipped_re ]]; then
            skipped_after_measure[${BASH_REMATCH[1]}]=${BASH_REMATCH[5]}
            skipped_actual_by_measure[${BASH_REMATCH[1]}]=${BASH_REMATCH[2]}
            skipped_requested_by_measure[${BASH_REMATCH[1]}]=${BASH_REMATCH[3]}
            skipped_overshoot_by_measure[${BASH_REMATCH[1]}]=${BASH_REMATCH[4]}
            skipped_pc_by_measure[${BASH_REMATCH[1]}]=${BASH_REMATCH[6]}
            skipped_warmup_by_measure[${BASH_REMATCH[1]}]=${BASH_REMATCH[7]}
            continue
        fi
        if [[ $line =~ $written_re ]]; then
            path_by_measure[${BASH_REMATCH[6]}]=${BASH_REMATCH[1]}
            requested_by_measure[${BASH_REMATCH[6]}]=${BASH_REMATCH[2]}
            actual_by_measure[${BASH_REMATCH[6]}]=${BASH_REMATCH[3]}
            overshoot_by_measure[${BASH_REMATCH[6]}]=${BASH_REMATCH[4]}
            pc_by_measure[${BASH_REMATCH[6]}]=${BASH_REMATCH[5]}
            warmup_by_measure[${BASH_REMATCH[6]}]=${BASH_REMATCH[7]}
            checkpoint_logs=$((checkpoint_logs + 1))
        fi
    done < "$logs_dir/checkpoint.stderr"

    local report_header='label\tlocation\tweight\tmeasurement'
    report_header+='\trequested_checkpoint\tactual_checkpoint\tovershoot'
    report_header+='\teffective_warmup\tpc\tstatus\tcheckpoint'
    printf '%b\n' "$report_header" > "$report"

    while read -r location label _; do
        [[ -n "${location:-}" && -n "${label:-}" ]] || continue

        local measurement=$((location * interval))
        local weight=${weights[$label]:-NA}
        local requested=
        local actual=
        local overshoot=
        local effective_warmup=
        local pc=
        local checkpoint_path=

        if [[ -n "${seen_measure[$measurement]+x}" ]]; then
            info "duplicate measurement point $measurement in simpoints0;" \
                "one checkpoint is expected"
            continue
        fi
        seen_measure[$measurement]=1

        expected_from_simpoints=$((expected_from_simpoints + 1))
        if (( measurement < warmup )); then
            status=retained_before_warmup
            fail_count=$((fail_count + 1))
            printf '%s\t%s\t%s\t%s\t\t\t\t\t\t%s\t\n' \
                "$label" "$location" "$weight" "$measurement" "$status" \
                >> "$report"
            continue
        fi

        requested=$((measurement - warmup))
        actual=${actual_by_measure[$measurement]:-}
        overshoot=${overshoot_by_measure[$measurement]:-}
        pc=${pc_by_measure[$measurement]:-}
        checkpoint_path=${path_by_measure[$measurement]:-}
        status=ok

        if [[ -n "${skipped_after_measure[$measurement]+x}" ]]; then
            actual=${skipped_actual_by_measure[$measurement]}
            overshoot=${skipped_overshoot_by_measure[$measurement]}
            pc=${skipped_pc_by_measure[$measurement]}
            effective_warmup=$((measurement - actual))
            status=skipped_after_measurement
            fail_count=$((fail_count + 1))

            if [[ "${skipped_requested_by_measure[$measurement]}" \
                  != "$requested" ]]; then
                status=wrong_skipped_requested_checkpoint
                fail_count=$((fail_count + 1))
            elif [[ "${skipped_warmup_by_measure[$measurement]}" \
                    != "$warmup" ]]; then
                status=wrong_skipped_warmup
                fail_count=$((fail_count + 1))
            elif (( actual <= measurement )); then
                status=skip_not_after_measurement
                fail_count=$((fail_count + 1))
            elif (( actual - requested != overshoot )); then
                status=wrong_skipped_overshoot
                fail_count=$((fail_count + 1))
            elif [[ -d "$checkpoint_dir/$measurement" ]] &&
                 find "$checkpoint_dir/$measurement" -type f \
                 -name '*.bin.zst' |
                 grep -q .; then
                status=skipped_but_checkpoint_file_exists
                fail_count=$((fail_count + 1))
            fi

            printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                "$label" "$location" "$weight" "$measurement" "$requested" \
                "$actual" "$overshoot" "$effective_warmup" "$pc" "$status" \
                "$checkpoint_path" >> "$report"
            continue
        fi

        if [[ -z "$actual" || -z "$overshoot" || -z "$checkpoint_path" ]]; then
            status=missing_checkpoint_log
            fail_count=$((fail_count + 1))
        elif [[ ! -f "$checkpoint_path" ]]; then
            status=missing_checkpoint_file
            fail_count=$((fail_count + 1))
        elif [[ "${requested_by_measure[$measurement]}" \
                != "$requested" ]]; then
            status=wrong_requested_checkpoint
            fail_count=$((fail_count + 1))
        elif [[ "${warmup_by_measure[$measurement]}" != "$warmup" ]]; then
            status=wrong_warmup
            fail_count=$((fail_count + 1))
        elif (( actual < requested )); then
            status=actual_before_requested
            fail_count=$((fail_count + 1))
        elif (( actual - requested != overshoot )); then
            status=wrong_overshoot
            fail_count=$((fail_count + 1))
        elif (( actual > measurement )); then
            status=actual_after_measurement
            fail_count=$((fail_count + 1))
        fi

        if [[ -n "$actual" ]]; then
            effective_warmup=$((measurement - actual))
        fi

        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$label" "$location" "$weight" "$measurement" "$requested" \
            "$actual" "$overshoot" "$effective_warmup" "$pc" "$status" \
            "$checkpoint_path" >> "$report"
    done < "$cluster_dir/simpoints0"

    if (( runtime_skip_count != 0 )); then
        fail_count=$((fail_count + 1))
        info "$runtime_skip_count retained SimPoint checkpoint(s) were" \
            "skipped after their measurement point"
    fi
    if (( checkpoint_logs != expected_from_simpoints )); then
        fail_count=$((fail_count + 1))
        info "checkpoint log count mismatch: expected" \
            "$expected_from_simpoints, got $checkpoint_logs written"
    fi

    (( fail_count == 0 )) || die "cutpoint validation failed; see $report"
    info "validated all $expected_from_simpoints checkpoint cutpoint(s);" \
        "report: $report"
}

normalize_pc()
{
    printf '%s\n' "$1" | tr 'A-F' 'a-f' |
        sed -E 's/^0x0*([0-9a-f])$/0x\1/; s/^0x0*([0-9a-f][0-9a-f]+)$/0x\1/'
}

read_snapshot_pc()
{
    local checkpoint_path=$1
    local bytes_text
    local magic=
    local version=
    local pc=
    local -a bytes
    local i

    bytes_text=$(
        dd if=<("$zstd_bin" -dc -- "$checkpoint_path" 2> /dev/null) \
            bs=1 skip=$((0x100000)) count=72 status=none |
            od -An -v -tx1
    )
    read -r -a bytes <<< "$(tr '\n' ' ' <<< "$bytes_text")"
    (( ${#bytes[@]} == 72 )) || return 1

    for ((i = 7; i >= 0; i--)); do
        magic+="${bytes[i]}"
    done
    for ((i = 15; i >= 8; i--)); do
        version+="${bytes[i]}"
    done
    [[ "$magic" == 0050414e53343641 ]] || return 1
    [[ "$version" == 0000000000000001 ]] || return 1

    for ((i = 71; i >= 64; i--)); do
        pc+="${bytes[i]}"
    done
    pc=$(sed -E 's/^0+//; s/^$/0/' <<< "$pc")
    printf '0x%s\n' "$pc"
}

run_restore_qemu()
{
    local checkpoint_path=$1
    local restore_plugin
    local slice_dir=$2

    restore_plugin="$simpoint_plugin,trigger=simtrap,interval=$interval"
    restore_plugin+=",target=$slice_dir/profile,dump-final=true"

    if [[ "$restore_timeout" == 0 || "$restore_timeout" == "0s" ]]; then
        "$qemu_bin" \
            -icount shift=0,sleep=off \
            -machine mini-virt \
            -cpu "$cpu_model" \
            -smp 1 \
            -m "$memory" \
            -nographic \
            -kernel "$checkpoint_path" \
            -plugin "$restore_plugin"
        return
    fi

    timeout "$restore_timeout" "$qemu_bin" \
        -icount shift=0,sleep=off \
        -machine mini-virt \
        -cpu "$cpu_model" \
        -smp 1 \
        -m "$memory" \
        -nographic \
        -kernel "$checkpoint_path" \
        -plugin "$restore_plugin"
}

validate_restores()
{
    local restore_dir=$out_dir/restore-validation
    local report=$out_dir/restore-validation.tsv
    local report_header='measurement\tcheckpoint\texpected_pc\tsnapshot_pc'
    local fail_count=0
    local restore_count=0

    report_header+='\tprofile_start_seen\texit_code\tstatus'
    rm -rf "$restore_dir"
    mkdir -p "$restore_dir"
    printf '%b\n' "$report_header" > "$report"

    while IFS=$'\t' read -r label location weight measurement \
        requested actual \
        overshoot effective_warmup pc status checkpoint_path; do
        [[ "$status" == ok ]] || continue

        local slice_dir=$restore_dir/measure-$measurement
        local snapshot_pc=
        local expected_norm=
        local snapshot_norm=
        local profile_start_seen=0
        local row_status=ok
        local exit_code

        mkdir -p "$slice_dir"
        set +e
        run_restore_qemu "$checkpoint_path" "$slice_dir" \
            > "$slice_dir/stdout" 2> "$slice_dir/stderr" < /dev/null
        exit_code=$?
        set -e

        snapshot_pc=$(read_snapshot_pc "$checkpoint_path" || true)
        expected_norm=$(normalize_pc "$pc")
        snapshot_norm=$(normalize_pc "$snapshot_pc")

        if [[ -z "$snapshot_pc" ]]; then
            row_status=missing_snapshot_pc
        elif [[ "$snapshot_norm" != "$expected_norm" ]]; then
            row_status=pc_mismatch
        fi
        if grep -Eq '^simpoint: vcpu [0-9]+ profiling started$' \
            "$slice_dir/stderr"; then
            profile_start_seen=1
            if [[ "$row_status" == ok ]]; then
                row_status=unexpected_profile_start
            else
                row_status=${row_status}_unexpected_profile_start
            fi
        fi
        if [[ "$exit_code" -ne 0 ]]; then
            row_status=${row_status}_exit_$exit_code
        fi
        if [[ "$row_status" != ok ]]; then
            fail_count=$((fail_count + 1))
        fi

        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$measurement" "$checkpoint_path" "$expected_norm" \
            "$snapshot_norm" "$profile_start_seen" "$exit_code" \
            "$row_status" >> "$report"
        restore_count=$((restore_count + 1))
    done < <(tail -n +2 "$out_dir/slices.tsv")

    (( fail_count == 0 )) || die "restore validation failed; see $report"
    info "validated $restore_count checkpoint header PC(s) and restore run(s)"
    info "restore report: $report"
}

write_summary()
{
    local expected_count=$1
    local actual_count=$2

    {
        printf 'payload\t%s\n' "$payload"
        printf 'qemu\t%s\n' "$qemu_bin"
        printf 'cpu_model\t%s\n' "$cpu_model"
        printf 'simpoint\t%s\n' "$simpoint_bin"
        printf 'interval\t%s\n' "$interval"
        printf 'warmup\t%s\n' "$warmup"
        printf 'memory\t%s\n' "$memory"
        printf 'profile_bbv\t%s\n' "$profile_dir/simpoint_bbv.gz"
        printf 'profile_instructions\t%s\n' "$profile_total"
        printf 'bbv_instructions\t%s\n' "$bbv_total"
        printf 'profile_tail_instructions\t%s\n' "$profile_tail"
        printf 'bbv_vectors\t%s\n' "$bbv_vectors"
        printf 'bbv_max_boundary_drift\t%s\n' "$bbv_max_boundary_drift"
        printf 'bbv_final_boundary_drift\t%s\n' "$bbv_final_boundary_drift"
        printf 'raw_simpoints\t%s\n' "$cluster_dir/simpoints0.raw"
        printf 'raw_weights\t%s\n' "$cluster_dir/weights0.raw"
        printf 'simpoints\t%s\n' "$cluster_dir/simpoints0"
        printf 'weights\t%s\n' "$cluster_dir/weights0"
        printf 'dropped_warmup_report\t%s\n' "$cluster_dir/dropped-warmup.tsv"
        printf 'dropped_warmup_simpoints\t%s\n' "$dropped_warmup_count"
        printf 'original_weight_total\t%s\n' "$original_weight_total"
        printf 'dropped_warmup_weight\t%s\n' "$dropped_warmup_weight"
        printf 'retained_original_weight\t%s\n' "$retained_original_weight"
        printf 'normalized_weight_total\t%s\n' "$normalized_total"
        printf 'checkpoint_dir\t%s\n' "$checkpoint_dir"
        printf 'slice_report\t%s\n' "$out_dir/slices.tsv"
        if [[ "$restore_validate" == 1 ]]; then
            printf 'restore_validation\t%s\n' "$out_dir/restore-validation.tsv"
        fi
        printf 'expected_checkpoints\t%s\n' "$expected_count"
        printf 'actual_checkpoints\t%s\n' "$actual_count"
        printf 'runtime_skipped_checkpoints\t%s\n' "$runtime_skip_count"
    } > "$out_dir/summary.txt"
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
repo_root=$(cd -- "$script_dir/.." && pwd -P)
workspace_root=$(cd -- "$repo_root/../.." && pwd -P)

default_payload=$workspace_root/unified-workload/build/plat
default_payload+=/qemu-minivirt-aarch64-gcpt/gemm/gcpt/gcpt.bin
payload=${PAYLOAD:-$default_payload}
build_dir=${BUILD_DIR:-$repo_root/build}
qemu_bin=${QEMU_BIN:-$build_dir/qemu-system-aarch64}
simpoint_plugin=${SIMPOINT_PLUGIN:-$build_dir/contrib/plugins/libsimpoint.so}
zstd_bin=${ZSTD_BIN:-$(command -v zstd || true)}
simpoint_bin=$(find_simpoint_bin) ||
    die "set SIMPOINT_BIN or place SimPoint at" \
        "../SimPoint.3.2-fix/bin/simpoint"

interval=${INTERVAL:-10000}
warmup=${WARMUP:-5000}
memory=${MEMORY:-1G}
cpu_model=${CPU_MODEL:-cortex-a57}
maxk=${MAXK:-30}
num_init_seeds=${NUM_INIT_SEEDS:-2}
iters=${ITERS:-1000}
seed_km=${SEED_KM:-12345}
seed_proj=${SEED_PROJ:-67890}
restore_validate=${RESTORE_VALIDATE:-1}
restore_timeout=${RESTORE_TIMEOUT:-30s}
out_dir=${OUT_DIR:-/tmp/a64-gcpt-simpoint-flow-$(date +%Y%m%d-%H%M%S)}
force=${FORCE:-0}

if [[ -z "$out_dir" || "$out_dir" == "/" ]]; then
    die "refusing unsafe OUT_DIR: $out_dir"
fi
if [[ ! "$interval" =~ ^[0-9]+$ || "$interval" -le 0 ]]; then
    die "INTERVAL must be a positive integer"
fi
if [[ ! "$warmup" =~ ^[0-9]+$ ]]; then
    die "WARMUP must be a non-negative integer"
fi
if [[ ! "$maxk" =~ ^[0-9]+$ || "$maxk" -le 0 ]]; then
    die "MAXK must be a positive integer"
fi
if [[ ! "$num_init_seeds" =~ ^[0-9]+$ || "$num_init_seeds" -le 0 ]]; then
    die "NUM_INIT_SEEDS must be a positive integer"
fi
if [[ "$restore_validate" != 0 && "$restore_validate" != 1 ]]; then
    die "RESTORE_VALIDATE must be 0 or 1"
fi

require_path_without_whitespace PAYLOAD "$payload"
require_path_without_whitespace BUILD_DIR "$build_dir"
require_path_without_whitespace QEMU_BIN "$qemu_bin"
require_path_without_whitespace SIMPOINT_PLUGIN "$simpoint_plugin"
require_path_without_whitespace SIMPOINT_BIN "$simpoint_bin"
require_path_without_whitespace ZSTD_BIN "$zstd_bin"
require_path_without_whitespace OUT_DIR "$out_dir"

require_file "$payload"
require_exec "$qemu_bin"
require_exec "$simpoint_bin"
require_exec "$simpoint_plugin"
if [[ "$restore_validate" == 1 ]]; then
    require_exec "$zstd_bin"
fi

profile_dir=$out_dir/profile
cluster_dir=$out_dir/cluster
checkpoint_dir=$out_dir/checkpoints
logs_dir=$out_dir/logs

if [[ -e "$out_dir" && "$force" != 1 ]]; then
    die "OUT_DIR already exists: $out_dir; set FORCE=1 to reuse it"
fi
if [[ -e "$out_dir" && "$force" == 1 ]]; then
    rm -rf "$profile_dir" "$cluster_dir" "$checkpoint_dir" \
        "$logs_dir" "$out_dir/restore-validation" "$out_dir/slices.tsv" \
        "$out_dir/restore-validation.tsv" "$out_dir/summary.txt"
fi
mkdir -p "$profile_dir" "$cluster_dir" "$checkpoint_dir" "$logs_dir"

info "output: $out_dir"
info "profiling payload"
profile_plugin="$simpoint_plugin,trigger=simtrap,interval=$interval"
profile_plugin+=",target=$profile_dir,dump-final=false"
profile_cmd=(
    "$qemu_bin"
    -icount shift=0,sleep=off
    -machine mini-virt
    -cpu "$cpu_model"
    -smp 1
    -m "$memory"
    -nographic
    -kernel "$payload"
    -plugin "$profile_plugin"
)
"${profile_cmd[@]}" > "$logs_dir/profile.stdout" 2> "$logs_dir/profile.stderr"
if [[ ! -s "$profile_dir/simpoint_bbv.gz" ]]; then
    die "profiling did not create a non-empty simpoint_bbv.gz"
fi
require_nonempty_gzip_text "$profile_dir/simpoint_bbv.gz"
validate_bbv_intervals "$profile_dir/simpoint_bbv.gz"

info "clustering BBV"
simpoint_cmd=(
    "$simpoint_bin"
    -loadFVFile "$profile_dir/simpoint_bbv.gz"
    -saveSimpoints "$cluster_dir/simpoints0.raw"
    -saveSimpointWeights "$cluster_dir/weights0.raw"
    -saveLabels "$cluster_dir/labels"
    -inputVectorsGzipped
    -maxK "$maxk"
    -numInitSeeds "$num_init_seeds"
    -iters "$iters"
    -seedkm "$seed_km"
    -seedproj "$seed_proj"
)
"${simpoint_cmd[@]}" \
    > "$logs_dir/simpoint.stdout" 2> "$logs_dir/simpoint.stderr"
if [[ ! -s "$cluster_dir/simpoints0.raw" ]]; then
    die "SimPoint did not create non-empty simpoints0.raw"
fi
if [[ ! -s "$cluster_dir/weights0.raw" ]]; then
    die "SimPoint did not create non-empty weights0.raw"
fi
filter_and_normalize_simpoints

expected_count=$(count_expected_checkpoints)
if [[ "$expected_count" -le 0 ]]; then
    die "warmup filtering left no checkpoint locations"
fi

info "generating checkpoints"
checkpoint_machine='mini-virt,checkpoint-mode=SimpointCheckpoint'
checkpoint_machine+=",simpoint-path=$cluster_dir,cpt-interval=$interval"
checkpoint_machine+=",warmup-interval=$warmup"
checkpoint_machine+=",checkpoint-dir=$checkpoint_dir"
checkpoint_cmd=(
    "$qemu_bin"
    -icount shift=0,sleep=off
    -machine "$checkpoint_machine"
    -cpu "$cpu_model"
    -smp 1
    -m "$memory"
    -nographic
    -kernel "$payload"
)
"${checkpoint_cmd[@]}" \
    > "$logs_dir/checkpoint.stdout" 2> "$logs_dir/checkpoint.stderr"

actual_count=$(find "$checkpoint_dir" -type f -name '*.bin.zst' | wc -l)
runtime_skip_count=$(count_runtime_skips)
if (( actual_count > 0 )); then
    if ! grep -q 'overshoot=' "$logs_dir/checkpoint.stderr"; then
        die "checkpoint log did not include overshoot information"
    fi
fi
validate_cutpoints
(( runtime_skip_count == 0 )) ||
    die "$runtime_skip_count retained SimPoint checkpoint(s) were skipped"
(( actual_count == expected_count )) ||
    die "checkpoint count mismatch: expected $expected_count," \
        "got $actual_count"
if [[ "$restore_validate" == 1 ]]; then
    validate_restores
fi

write_summary "$expected_count" "$actual_count"

info "done"
info "summary: $out_dir/summary.txt"
