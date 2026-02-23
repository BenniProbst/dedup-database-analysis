# Session 97: Experiment OOM-Fix + Per-DB Scaling
**Datum:** 2026-02-22
**Agent:** Infrastruktur (system32 Kontext)
**Projekt:** dedup-database-analysis (GitLab Project 280)

## Kontext
Fortsetzung von Session 74d. Pipeline #5163 experiment:postgresql 2x OOMKilled (exit 137, 12Gi Limit).

## Root Cause Analyse

### OOM-Ursache (2 Bugs)
1. **config.example.json** listet ALLE 12 Payload-Typen inkl. `blender_video` (Sintel 1.18GB in RAM)
2. **main.cpp Bug:** `--payload-types` CLI-Filter wurde in Zeile 338 angewendet, NACH `--generate-data` Return in Zeile 224 → Filter griff NIE bei Datengenerierung
3. **CI Bug:** `PAYLOAD_TYPES` Variable war definiert aber wurde NIE als `--payload-types` an Binary uebergeben

### Experiment-Flow Bug
- `--generate-data` Flag generiert Daten und macht `return 0` → DB-Operationen wurden NIE ausgefuehrt
- Fix: 2-Phasen-Aufruf (generate-data + run als separate Aufrufe)

## Implementierte Fixes

### Fix 1: C++ payload_types Filter (main.cpp)
- payload_types_filter Block von Zeile 338 VOR generate_data Block (Zeile 194) verschoben
- real_world_dir Override ebenfalls verschoben
- Commit: Teil von `2ed478f`

### Fix 2-5: CI YAML (.gitlab-ci.yml)
- `--payload-types "${PAYLOAD_TYPES}"` zum dedup-test Aufruf hinzugefuegt
- 2-Phasen-Aufruf: Step 1 generate-data, Step 2 run experiment
- Checkpoint-Dir auf NFS: `/datasets/real-world/checkpoints/${DB_SYSTEM}`
- Retry: max 1 → max 2 (GitLab Maximum ist 2, nicht 3!)
- NFS-persistente Checkpoint-Pruefung vor jedem Run (Skip wenn schon complete)
- Per-DB Scaling via K8s API (curl + ServiceAccount Token)

### Fix 6: K8s RBAC
- ServiceAccount `experiment-scaler` in `gitlab-runner` Namespace
- ClusterRole `experiment-db-scaler`: StatefulSet scale + Pod list + KafkaNodePool patch
- ClusterRoleBinding `experiment-scaler-binding`
- Verifiziert: `kubectl auth can-i` alle 3 Checks passed

### Fix 7: Runner-4 ConfigMap
- `service_account = "experiment-scaler"` hinzugefuegt
- Runner-4 Deployment neu gestartet

### Fix 8: PostgreSQL bleibt AN (User-Direktive)
- PostgreSQL (postgres-ha) aus `scale_down_experiment_dbs()` entfernt
- experiment:postgresql Job: `DB_TYPE: "none"` (kein Scaling)
- Commit: `8f6f7a3` (lokal, push pending wegen GitLab 502)

## DB Scaling Matrix (FINAL)

| DB | Scaling | Typ |
|----|---------|-----|
| PostgreSQL | **BLEIBT AN** | Produktion |
| CockroachDB | **BLEIBT AN** | Produktion |
| MinIO | **BLEIBT AN** | Produktion |
| MariaDB | 0 ↔ 1 | Experiment |
| ClickHouse | 0 ↔ 1 | Experiment |
| Redis | 0 ↔ 3 | Experiment |
| Kafka | broker 0↔1, controller 0↔1 | Experiment |

## Commits
- `2ed478f` fix: OOM-Root-Cause + per-DB scaling + NFS checkpoints (PUSHED)
- `59aaf08` fix(ci): retry max 3 -> 2 (PUSHED)
- `8f6f7a3` fix(ci): PostgreSQL bleibt AN (LOKAL, push pending)

## Offene Punkte
- **PUSH PENDING:** Commit `8f6f7a3` muss noch zu GitLab gepusht werden (502 wegen OPNsense-Wartung)
- **Pipeline #5176:** Wurde getriggert aber auf altem Code (vor PostgreSQL-Fix). Muss nach Push neu getriggert werden.
- **OPNsense-Wartung:** Laeuft parallel durch anderen Agent, verursacht GitLab 502

## Naechste Session
1. `git -c http.sslVerify=false push gitlab development` (Commit 8f6f7a3)
2. Push-Pipeline canceln, API-Pipeline triggern
3. experiment:build + preflight beobachten
4. experiment:postgresql beobachten (sollte jetzt <12Gi RAM bleiben)
5. Kaskade ueberwachen: PG → CRDB → MariaDB → CH → Redis → Kafka → MinIO
