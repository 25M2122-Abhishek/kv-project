#!/usr/bin/env bash
# monitor.sh - monitor CPU, disk, memory for externally-run client/server
# Usage:
#   /app/monitor.sh <CPU_CLIENT> <CPU_SERVER> <CPU_STATOOL> [INTERVAL_MP] [INTERVAL_IO] [INTERVAL_VM]

set -euo pipefail

# -----------------------
# Parse args / defaults
# -----------------------
CPU_CLIENT="${1:-4-7}"
CPU_SERVER="${2:-0}"
CPU_STATOOL="${3:-7}"
INTERVAL_MP="${4:-1}"
INTERVAL_IO="${5:-1}"
INTERVAL_VM="${6:-1}"

OUTPUT_DIR="results/monitor_logs"
TIMESTAMP=$(date +"%Y-%m-%d_%H-%M-%S")
TMPDIR="$OUTPUT_DIR/temp_${TIMESTAMP}"
OUTFILE="$TMPDIR/stats_${TIMESTAMP}.txt"

mkdir -p "$OUTPUT_DIR" "$TMPDIR"

echo "===== KV MONITOR START ====="
echo "Time: $(date)"
echo "Client CPU(s): $CPU_CLIENT"
echo "Server CPU(s): $CPU_SERVER"
echo "Monitor CPU (tool runner): $CPU_STATOOL"
echo "Intervals (mpstat,iostat,vmstat): $INTERVAL_MP,$INTERVAL_IO,$INTERVAL_VM"
echo "Output dir: $OUTPUT_DIR"
echo "Temp logs: $TMPDIR"
echo

# -----------------------
# Expand CPU List
# -----------------------
expand_cpu_list() {
    local spec="$1"
    local out=""
    IFS=',' read -ra parts <<< "$spec"
    for p in "${parts[@]}"; do
        if [[ "$p" == *-* ]]; then
            s=${p%-*}
            e=${p#*-}
            for ((i=s;i<=e;i++)); do out+="$i "; done
        else
            out+="$p "
        fi
    done
    echo "$out" | xargs
}

# -----------------------
# Detect DB Device
# -----------------------
detect_postgres_device() {
    candidates=(
        "/var/lib/postgresql"
        "/var/lib/postgresql/data"
        "/var/lib/postgresql/12/main"
        "/var/lib/pgsql/data"
    )

    for p in "${candidates[@]}"; do
        if [ -e "$p" ]; then
            dev=$(df -P "$p" 2>/dev/null | awk 'NR==2 {print $1}')
            if [ -n "$dev" ]; then
                basename_dev=$(basename "$dev")
                if [[ "$basename_dev" =~ ^(.*)p[0-9]+$ ]]; then
                    echo "${BASH_REMATCH[1]}"
                else
                    echo "$basename_dev"
                fi
                return 0
            fi
        fi
    done

    if command -v lsblk >/dev/null 2>&1; then
        dev=$(lsblk -ndo NAME,TYPE | awk '$2=="disk"{print $1; exit}')
        if [ -n "$dev" ]; then
            echo "$dev"
            return 0
        fi
    fi
    echo ""
}

# DEVICE="$(detect_postgres_device || true)"
# [ -z "$DEVICE" ] && DEVICE="UNKNOWN"

# echo "Detected DB device: $DEVICE"
DEVICE="sde"
echo "Overriding DB device to actual disk performing I/O: $DEVICE"
echo

# -----------------------
# --- DISK: capture initial kernel stat (if possible) ---
# -----------------------
SYS_BLOCK_STAT_PATH=""
DISK_STAT_START=0
DISK_TS_START=0

if [ "$DEVICE" != "UNKNOWN" ]; then
    if [ -r "/sys/block/$DEVICE/stat" ]; then
        SYS_BLOCK_STAT_PATH="/sys/block/$DEVICE/stat"
    else
        devbase=$(basename "$DEVICE")
        if [ -r "/sys/block/$devbase/stat" ]; then
            SYS_BLOCK_STAT_PATH="/sys/block/$devbase/stat"
            DEVICE="$devbase"
        fi
    fi
fi

if [ -n "$SYS_BLOCK_STAT_PATH" ] && [ -r "$SYS_BLOCK_STAT_PATH" ]; then
    DISK_STAT_START=$(awk '{print $10+0}' "$SYS_BLOCK_STAT_PATH" 2>/dev/null || echo 0)
    DISK_TS_START=$(date +%s%3N 2>/dev/null || date +%s000)
    echo "Disk stat sampling enabled: $SYS_BLOCK_STAT_PATH (start=$DISK_STAT_START at $DISK_TS_START ms)"
else
    echo "Disk stat sampling not available for device: $DEVICE (will fallback to iostat parsing)"
fi

# -----------------------
# Start Collectors
# -----------------------
SERVER_CORES_EXPANDED=$(expand_cpu_list "$CPU_SERVER")
SERVER_CORES_CSV=$(echo "$SERVER_CORES_EXPANDED" | tr ' ' ',')
taskset -c "$CPU_STATOOL" mpstat -P "$SERVER_CORES_CSV" "$INTERVAL_MP" > "$TMPDIR/cpu_server.log" 2>&1 &
MPSTAT_SERVER_PID=$!

CLIENT_CORES_EXPANDED=$(expand_cpu_list "$CPU_CLIENT")
CLIENT_CORES_CSV=$(echo "$CLIENT_CORES_EXPANDED" | tr ' ' ',')
taskset -c "$CPU_STATOOL" mpstat -P "$CLIENT_CORES_CSV" "$INTERVAL_MP" > "$TMPDIR/cpu_client.log" 2>&1 &
MPSTAT_CLIENT_PID=$!

taskset -c "$CPU_STATOOL" iostat -dx "$INTERVAL_IO" > "$TMPDIR/disk.log" 2>&1 &
IOSTAT_PID=$!

taskset -c "$CPU_STATOOL" vmstat "$INTERVAL_VM" > "$TMPDIR/mem.log" 2>&1 &
VMSTAT_PID=$!

#-----------------------------
# TOTAL MEMORY (for RAM %)  
#-----------------------------
# Get total RAM from /proc/meminfo (in kB)
MEM_TOTAL_KB=$(awk '/MemTotal/ {print $2}' /proc/meminfo)


echo "Collectors started."
echo "Stop container to generate report."
echo

# -----------------------
# Shutdown + Summary
# -----------------------
generate_report_and_exit() {
    echo "Stopping collectors..."
    kill "$MPSTAT_SERVER_PID" "$MPSTAT_CLIENT_PID" "$IOSTAT_PID" "$VMSTAT_PID" 2>/dev/null || true
    sleep 1

    echo "Generating report to $OUTFILE ..."

    # -----------------------------
    # FIXED CPU SUMMARY (SERVER)
    # -----------------------------
    CPU_USAGE_AVG_SERVER=$(
        awk '
        /^[0-9]{2}:[0-9]{2}:[0-9]{2}/ && $2 ~ /^[0-9]+$/ {
            idle = $NF
            sum_idle += idle
            count++
        }
        END {
            if (count > 0) {
                avg_idle = sum_idle / count
                printf "%.2f", (100 - avg_idle)
            } else print "0.00"
        }' "$TMPDIR/cpu_server.log"
    )

    # -----------------------------
    # FIXED CPU SUMMARY (CLIENT)
    # -----------------------------
    CPU_USAGE_AVG_CLIENT=$(
        awk '
        /^[0-9]{2}:[0-9]{2}:[0-9]{2}/ && $2 ~ /^[0-9]+$/ {
            idle = $NF
            sum_idle += idle
            count++
        }
        END {
            if (count > 0) {
                avg_idle = sum_idle / count
                printf "%.2f", (100 - avg_idle)
            } else print "0.00"
        }' "$TMPDIR/cpu_client.log"
    )

    # -----------------------------
    # Disk Util: prefer /sys/block stat delta; fallback to iostat
    # -----------------------------
    DISK_UTIL_AVG="N/A"
    if [ -s "$TMPDIR/disk.log" ]; then
        # Use prefix match in case iostat reports sda1, sda2, etc.
        DISK_UTIL_AVG=$(
            awk -v dev="$DEVICE" '
                NR>6 && $1 ~ dev && $(NF) ~ /^[0-9.]+$/ {
                    sum += $(NF)
                    count++
                }
                END {
                    if(count>0) printf "%.2f", sum/count;
                    else print "0.00"
                }
            ' "$TMPDIR/disk.log"
        )
    fi

    # ---------- MEMORY UTIL (RAM) ----------
    # vmstat fields (after header) are:
    # r b swpd free buff cache si so bi bo in cs us sy id wa st
    # We average free, buff, cache over all samples and derive used from total.

    MEM_STATS=$(
    awk '
        NR > 2 && $1 ~ /^[0-9]+$/ {
        free_sum  += $4
        buff_sum  += $5
        cache_sum += $6
        count++
        }
        END {
        if (count > 0) {
            avg_free  = free_sum / count
            avg_buff  = buff_sum / count
            avg_cache = cache_sum / count
            printf "%.0f %.0f %.0f %.0f", avg_free, avg_buff, avg_cache, count
        } else {
            print "0 0 0 0"
        }
        }
    ' "${TMPDIR}/mem.log"
    )

    read MEM_FREE_AVG_KB MEM_BUFF_AVG_KB MEM_CACHE_AVG_KB MEM_SAMPLES <<< "${MEM_STATS}"

    # Effective "available"-like memory (free+buffers+cache)
    MEM_AVAIL_AVG_KB=$((MEM_FREE_AVG_KB + MEM_BUFF_AVG_KB + MEM_CACHE_AVG_KB))

    # Used memory = total - (free+buff+cache)
    MEM_USED_AVG_KB=$((MEM_TOTAL_KB - MEM_AVAIL_AVG_KB))

    # Convert to MB (integer)
    MEM_USED_AVG_MB=$((MEM_USED_AVG_KB / 1024))
    MEM_TOTAL_MB=$((MEM_TOTAL_KB / 1024))

    # Used memory percentage (integer)
    MEM_USED_PCT=$((100 * MEM_USED_AVG_KB / MEM_TOTAL_KB))

    # -----------------------------
    # Write Report
    # -----------------------------
    {
      echo "--------------------------------------------------------------"
      echo " SYSTEM RESOURCE MONITOR REPORT "
      echo "--------------------------------------------------------------"
      echo
      echo "Timestamp           : $(date)"
      echo "Client CPU(s)       : $CPU_CLIENT"
      echo "Server CPU(s)       : $CPU_SERVER"
      echo "Monitor CPU(s)      : $CPU_STATOOL"
      echo "Disk Device         : $DEVICE"
      echo "Total RAM (MB)      : ${MEM_TOTAL_MB}"
      echo
      echo ">>>>>>>>>>>>>>>>>>>>>> SUMMARY <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<"
      echo "Average Server CPU Usage (%) : $CPU_USAGE_AVG_SERVER"
      echo "Average Client CPU Usage (%) : $CPU_USAGE_AVG_CLIENT"
      echo "Average Disk Util (%)        : $DISK_UTIL_AVG"
      echo "Average RAM Used (MB)        : ${MEM_USED_AVG_MB}"
      echo "Average RAM Used (%)         : ${MEM_USED_PCT}"
      echo
      echo ">>> CPU Stats (server):"
      cat "$TMPDIR/cpu_server.log" || echo "(missing)"
      echo
      echo ">>> CPU Stats (client):"
      cat "$TMPDIR/cpu_client.log" || echo "(missing)"
      echo
      echo ">>> Disk Stats:"
      cat "$TMPDIR/disk.log" || echo "(missing)"
      echo
      echo ">>> Memory Stats:"
      cat "$TMPDIR/mem.log" || echo "(missing)"
      echo
      echo "--------------------------------------------------------------"
    } > "$OUTFILE"

    echo "Report saved: $OUTFILE"
    exit 0
}

trap 'generate_report_and_exit' SIGINT SIGTERM

while true; do sleep 1; done
