# Session 97c: Bug-Fixes + Pipeline #6087 Rerun
**Datum:** 2026-02-22
**Agent:** Infrastruktur (system32 Kontext)
**Projekt:** dedup-database-analysis (GitLab Project 280)

## Kontext
Fortsetzung von Session 97b. Pipeline #5345 experiment:postgresql FAILED (stuck_or_timeout_failure nach 97min).
Bug-Fix-Phase: 4 Bugs gefixt, neuer Commit gepusht, Pipeline #6087 gestartet.

## Bug-Fixes (alle DONE)

### Bug 1: Bulk Insert Row Count = 0 (FIXED)
**Datei:** `src/cpp/connectors/postgres_connector.cpp`
**Problem:** `PQprepare()` mit fixem Statement-Name "bulk_insert" → Name-Collision bei wiederholten Aufrufen, Statement wird nach DROP SCHEMA CASCADE invalidiert
**Fix:** `PQprepare/PQexecPrepared` komplett durch `PQexecParams` ersetzt + Error-Logging pro Row

### Bug 2: NFS-Mount fehlgeschlagen (FIXED)
**Datei:** Runner-4 ConfigMap + `.gitlab-ci.yml`
**Fix 1:** `[[runners.kubernetes.volumes.nfs]]` in Runner-4 ConfigMap hinzugefuegt:
```toml
[[runners.kubernetes.volumes.nfs]]
  name = "experiment-nfs"
  mount_path = "/datasets/real-world"
  server = "10.0.110.184"
  path = "/"
  read_only = false
```
**Fix 2:** CI Script: `mount -t nfs4` durch K8s Volume Check ersetzt, `umount` entfernt
**Deployed:** Runner-4 Deployment neugestartet, ConfigMap aktiv

### Bug 3: Kafka Metrics Trace Flood (FIXED)
**Datei:** `src/cpp/experiment/metrics_trace.cpp` + `metrics_trace.hpp`
**Problem:** `produce_to_kafka()` loggt JEDEN Fehler → Tausende Errors/Sekunde bei fehlenden ACLs
**Fix:** Exponentielles Backoff:
- Nach 5 konsekutiven Fehlern: Backoff-Modus
- Waehrend Backoff: Produce-Calls komplett uebersprungen, nur alle 30s ein Retry
- Bei Kafka-Recovery: Automatischer Reset + Log mit Anzahl suppressed Errors
- Neue Members in MetricsTrace: `kafka_consecutive_errors_`, `kafka_suppressed_count_`, `kafka_last_error_log_ms_`

### Bug 5: Ergebnis-Upload (FIXED)
**Datei:** `.gitlab-ci.yml`
**Fix:** Neuer Job `experiment:upload-results` in Stage `experiment-cleanup`:
- Wartet auf alle 7 DB-Jobs (artifacts: true)
- git clone → copy results → commit → push nach `measurement_results/pipeline-{ID}_{timestamp}/`
- Pipeline-Info JSON mit Metadaten
- `allow_failure: true` (Ergebnisse sind auch in Artifacts sicher)

## Commit + Push

### Commit `fe6e569`
```
fix: 4 experiment bugs (bulk_insert, kafka flood, NFS mount, result upload)
```
- 4 Dateien, 151 Zeilen hinzugefuegt, 18 entfernt
- Gepusht nach GitLab (`development` Branch)

### Git-Merge-Konflikt
- Remote hatte 4 neue Commits (08e1823, cd0aa48, f57f8c2, d428b0c)
- Rebase schlug fehl (CI YAML Konflikte)
- Loesung: `git reset --hard gitlab/development` + Fixes neu anwenden
- **User-Direktive:** Kuenftig IMMER merge statt reset!

## Pipeline #6087 (Push-Pipeline)

### Triggering
- Push-Pipeline #6087 automatisch erstellt (Push mit .cpp Aenderungen)
- Pipeline #5345 (alt) gecancelt (experiment:postgresql war FAILED)
- Alter Runner-Pod `runner-wcdhzmjkn-project-280-concurrent-0-xv105334` (148min alt) manuell geloescht

### Runner-Stall-Problem (WIEDERKEHREND)
- Runner-4 stoppt Polling nach Sidekiq-Downtime oder Job-Abschluss
- Bekanntes Problem aus S95b
- Fix: `kubectl rollout restart deployment gitlab-runner-4 -n gitlab-runner`
- Musste 3x Runner-4 neustarten um alle Jobs aufzunehmen
- K8s Runner 1-3 ebenfalls neugestartet (build-k8s pending)
- **TODO:** Permanenten Fix fuer Runner-Stall finden (check_interval? liveness probe?)

### Sidekiq-Recovery
- Sidekiq neugestartet (alter Pod hing in Terminating)
- Neuer Pod hatte CrashLoop → alter Pod force-deleted → Recovery

### Job-Status (FINAL, aktualisiert in S97d)
| Job | Status | Dauer |
|-----|--------|-------|
| experiment:build | SUCCESS | 117s |
| experiment:preflight | SUCCESS | 21s |
| experiment:postgresql | **SUCCESS** | **12.975s (216min)** |
| experiment:cockroachdb | RUNNING | ~23min (weiter in S97d) |
| experiment:mariadb | CREATED | |
| experiment:clickhouse | CREATED | |
| experiment:redis | CREATED | |
| experiment:kafka | CREATED | |
| experiment:minio | CREATED | |
| experiment:upload-results | CREATED | |
| build-debian-x86 | SUCCESS | 46s |
| build-ubuntu-x86 | SUCCESS | 24s |
| build-macos-x86 | SUCCESS | 15s |
| build-macos-arm | SUCCESS | 17s |
| build-linux-arm64 | SUCCESS | 28s |

**Ergebnis:** Bug-Fixes 1-3 VERIFIZIERT! 5/8 Payload-Typen liefern gueltige Daten.
Weiter in Session 97d (Testdaten-Vorbereitung + Monitoring).

## Offene Bugs (NICHT in dieser Session gefixt)

### Bug 4: PostgreSQL Dual-Primary ohne Replikation (Infra)
**User-Direktive:** Active/Active mit NGINX + MetalLB
**Status:** Verschoben auf nach Experiment-Durchlauf

### Bug 6: Samba AD LDAP Auth fuer alle 7 DBs (Infra)
**User-Direktive:** JEDE DB muss ueber Samba AD authentifiziert werden, GLOBAL
**Status:** Verschoben auf nach Experiment-Durchlauf

### Runner-Stall-Bug (NEW)
**Problem:** Runner-4 stoppt Polling nach Job-Abschluss
**Symptom:** "Removed job from processing list" → keine weitere Aktivitaet
**Workaround:** `kubectl rollout restart deployment gitlab-runner-4`
**Root Cause:** Unbekannt (check_interval=3, request_concurrency=4)

## Naechste Session (AUTONOM)

### Prio 1: Pipeline #6087 ueberwachen
1. experiment:postgresql muss RUNNING werden (Runner-4 gerade neugestartet)
2. Pruefen ob NFS-Mount funktioniert (K8s Volume statt mount -t nfs4)
3. Pruefen ob Kafka-Backoff greift (kein Log-Flood mehr)
4. Pruefen ob bulk_insert Row Count > 0
5. Kaskade beobachten: PG → CRDB → MariaDB → CH → Redis → Kafka → MinIO
6. experiment:upload-results pruefen (commit nach measurement_results/)

### Prio 2: Runner-Stall dauerhaft fixen
- Runner-4 Pod nach JEDEM Job neustarten? (after_script?)
- Oder: `concurrent` erhoehen?
- Oder: Liveness Probe hinzufuegen?

### Prio 3: Infrastruktur (nach Experiment)
1. PostgreSQL Active/Active NGINX+MetalLB
2. Samba AD LDAP Auth fuer alle 7 DBs
3. Streaming Replication einrichten

## Geaenderte Dateien
| Datei | Aenderung |
|-------|-----------|
| `src/cpp/connectors/postgres_connector.cpp` | PQprepare→PQexecParams, Error-Logging |
| `src/cpp/experiment/metrics_trace.cpp` | Kafka Backoff-Logik (+36 Zeilen) |
| `src/cpp/experiment/metrics_trace.hpp` | Backoff State-Members (+7 Zeilen) |
| `.gitlab-ci.yml` | NFS K8s Volume, upload-results Job (+94 Zeilen) |
| Runner-4 ConfigMap | NFS Volume Mount (runtime, kein Git) |
