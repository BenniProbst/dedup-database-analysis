#!/bin/bash
# General Reset: Runs ALL per-DB resets before experiment start
# Usage: ./general_reset.sh [--dry-run] [DB_NAME]
#   No args    = reset ALL 7 databases
#   DB_NAME    = reset only that DB (mariadb, clickhouse, redis, kafka, minio, postgresql, cockroachdb)
#   --dry-run  = show what would be done
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DRY_RUN=""
TARGET_DB=""

for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN="--dry-run" ;;
    *) TARGET_DB="$arg" ;;
  esac
done

ALL_DBS="mariadb clickhouse redis kafka minio postgresql cockroachdb"

if [ -n "$TARGET_DB" ]; then
  DBS="$TARGET_DB"
else
  DBS="$ALL_DBS"
fi

echo "============================================="
echo "  EXPERIMENT GENERAL RESET"
echo "  Databases: $DBS"
echo "  Mode: ${DRY_RUN:-LIVE}"
echo "  Time: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "============================================="
echo ""

PASS=0
FAIL=0
for db in $DBS; do
  script="$SCRIPT_DIR/reset_${db}.sh"
  if [ ! -f "$script" ]; then
    echo "[ERROR] Script not found: $script"
    FAIL=$((FAIL+1))
    continue
  fi

  echo "--- Resetting $db ---"
  if bash "$script" $DRY_RUN; then
    PASS=$((PASS+1))
  else
    echo "[WARN] $db reset had errors (continuing)"
    FAIL=$((FAIL+1))
  fi
  echo ""
done

echo "============================================="
echo "  RESET COMPLETE: $PASS passed, $FAIL failed"
echo "============================================="

if [ "$FAIL" -gt 0 ]; then
  echo "[WARN] Some resets failed. Check output above."
  exit 1
fi
