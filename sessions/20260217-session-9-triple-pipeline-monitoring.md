# Session 9: Triple Pipeline + 100ms Monitoring-Architektur
**Datum:** 2026-02-17, ~22:00 UTC
**Commit:** `5ac1732` (Triple pipeline fix)
**Vorherige Commits:** `b0bb5b6` (Longhorn metrics, MariaDB/ClickHouse stubs)

---

## Was wurde gemacht

### 1. CI Triple Pipeline GEFIXT
- **Problem:** Pipeline #1304 hing in `created` Status
- **Root Cause 1:** Runner Tag Mismatch — K8s Runner (ID 6) hatte keinen `kubernetes` Tag
  - **Fix:** Tag via API hinzugefuegt: `PUT /api/v4/runners/6 {"tag_list":["kubernetes"]}`
- **Root Cause 2:** `experiment:run` mit `allow_failure: false` blockierte Pipeline
  - **Fix:** Alle Experiment-Jobs auf `allow_failure: true`
- **Root Cause 3:** Doppelte YAML `<<:` Merge-Keys (invalid YAML)
  - **Fix:** Explizite `rules:` pro Job statt doppelter Anchor-Merge
- **Pipeline-Struktur (7 Jobs in 6 Stages):**

```
Pipeline 1 (LaTeX, auto):
  latex:compile → docs/doku.pdf Artifact

Pipeline 2 (C++, auto):
  cpp:build → build/dedup-test + build/dedup-smoke-test
  cpp:smoke-test → SHA-256 + Dataset Verification
  cpp:full-dry-test → All Systems x All Grades (dry-run)

Pipeline 3 (Experiment, MANUELL):
  experiment:build → Binary kompilieren
  experiment:run → ECHTES Experiment gegen Produktions-DBs
  experiment:cleanup → Lab-Schemas droppen (Kundendaten UNBERUEHRT)
```

### 2. --cleanup-only Modus in main.cpp
- Neuer CLI-Flag `--cleanup-only`
- Verbindet zu allen DBs, droppt Lab-Schemas, disconnectet, exit
- Wird von `experiment:cleanup` CI-Job verwendet
- **Sicherheit:** Nur `dedup_lab` Schemas werden gedroppt, NIEMALS Produktionsdaten

### 3. CI YAML Validierung
- GitLab `/api/v4/projects/280/ci/lint` = `valid: true`
- Beide Remotes (GitLab + GitHub) auf `5ac1732` synchron

### 4. Pipeline Runner Problem (OFFEN!)
- K8s Runner (ID 6) zeigt `contacted_at: 2026-02-17T21:36:24Z` — 35+ Minuten alt
- Runner Status = `online` laut API, aber Jobs werden NICHT abgeholt
- Pipeline #1305 steht auf `created` — Jobs warten auf Runner
- **Vermutung:** K8s Runner Pod muss neugestartet werden
- **TODO:** `kubectl rollout restart deployment gitlab-runner -n gitlab-runner`

---

## Cluster-Zustand (READ-ONLY Reconnaissance)

### Datenbanken im Cluster

| System | Namespace | Pods | PVC | StorageClass | Service |
|--------|-----------|------|-----|-------------|---------|
| PostgreSQL HA | databases | 4/4 | 50Gi x4 = 200Gi | longhorn-database | postgres-lb:5432 |
| CockroachDB | cockroach-operator-system | 4/4 | 125Gi x4 = 500Gi | longhorn-database | cockroachdb-public:26257 |
| Redis Cluster | redis | 4/4 | 25Gi x4 = 100Gi | longhorn-database | redis-cluster:6379 |
| Kafka (Strimzi) | kafka | 4B+4C=8 | 50Gi x4 + 10Gi x4 = 240Gi | longhorn-database | kafka-bootstrap:9092 |
| MinIO | minio | 4/4 | Direct Disk (KEIN Longhorn!) | - | minio-lb:9000 (LB 10.0.90.55) |
| Samba AD | samba-ad | 4/4 | - | - | samba-ad-lb:389/636 (LB 10.0.30.5) |
| Redis GitLab | gitlab | 4/4 | 5Gi x4 = 20Gi | longhorn-database | (intern) |

**TOTAL Longhorn Database PVCs:** 24 PVCs, ~1.060 Gi

### NICHT im Cluster (MUSS noch deployed werden)

| System | Prioritaet | Connector | Cluster-Deploy |
|--------|-----------|-----------|---------------|
| MariaDB | HOCH (doku.tex 5.2) | FERTIG (mariadb_connector.*) | TODO: StatefulSet, 4 Replicas |
| ClickHouse | HOCH (doku.tex 5.2) | FERTIG (clickhouse_connector.*) | TODO: StatefulSet, 4 Replicas |
| Prometheus | KRITISCH (Metriken!) | MetricsCollector nutzt es | TODO: kube-prometheus-stack |
| Grafana | KRITISCH (Dashboard!) | MetricsCollector pusht | TODO: mit kube-prometheus-stack |

### StorageClasses

| Name | Provisioner | ReclaimPolicy | Notiz |
|------|-------------|---------------|-------|
| longhorn (default) | driver.longhorn.io | Delete | Standard-Workloads |
| longhorn-database | driver.longhorn.io | Retain | ALLE Datenbanken |
| longhorn-data | driver.longhorn.io | Retain | User Data |
| longhorn-backup | driver.longhorn.io | Retain | Backups |
| longhorn-opnsense | driver.longhorn.io | Retain | OPNsense VMs |

---

## Monitoring-Architektur (100ms Sampling)

### Anforderung (User)
- **JEDE Datenbank-Metrik JEDER Datenbank** alle 100ms (10 Hz)
- **Persistent Trace auf Kafka** (Topic: `dedup-lab-metrics`)
- **Live-Dashboard auf Grafana** (Echtzeit-Visualisierung)
- Datenbanken manuell vorbereiten, automatisch mit Testdaten bespielen

### Metriken pro System

| DB | Metriken | Quelle | Abfrage-Methode |
|----|----------|--------|----------------|
| PostgreSQL | pg_stat_user_tables, pg_database_size, pg_stat_bgwriter, pg_stat_wal, xact_commit/rollback, tup_inserted/updated/deleted, blks_hit/read | SQL `pg_stat_*` | libpq query |
| CockroachDB | ranges, replicas, leaseholders, capacity, queries/sec, latency_p99, livebytes | SQL `crdb_internal.kv_store_status` + HTTP /_status/vars | libpq + libcurl |
| Redis | used_memory, used_memory_rss, keyspace_hits/misses, instantaneous_ops_per_sec, total_connections_received | `INFO ALL` command | hiredis |
| Kafka | UnderReplicatedPartitions, BytesInPerSec, BytesOutPerSec, LogEndOffset, LogSize | JMX via Admin API / metrics endpoint | librdkafka + libcurl |
| MinIO | bucket_usage_total_bytes, s3_requests_total, s3_rx/tx_bytes_total | Prometheus endpoint :9000/minio/v2/metrics/cluster | libcurl |
| MariaDB | Innodb_buffer_pool_reads, Innodb_data_written, Threads_connected, Bytes_received/sent, Com_insert/delete | `SHOW GLOBAL STATUS` | libmysqlclient |
| ClickHouse | system.metrics (Query, Merge, ReplicatedFetch), system.events, system.asynchronous_metrics | SQL via HTTP API | libcurl |

### Datenfluss

```
C++ dedup-test (10 Hz Sampling Thread)
  |
  ├──→ Kafka Topic: dedup-lab-metrics (JSON, persistent trace)
  │      Format: {"ts": 1708210800123, "system": "postgresql", "metric": "pg_database_size", "value": 123456789}
  │
  └──→ Prometheus Pushgateway (oder direkt Prometheus Remote Write)
         └──→ Grafana Dashboard (Live-Visualisierung)
```

### C++ Implementation TODO
- **Background Metrics Thread** — std::thread mit 100ms sleep loop
- **Metric Snapshot** — Atomic struct pro DB-System
- **Kafka Producer** — librdkafka async produce to `dedup-lab-metrics`
- **Prometheus Push** — libcurl POST to Pushgateway

---

## Schema-Isolation Status (Session 5 Stand)

| Datenbank | Lab-Schema | Lab-User | Reset-Methode | Status |
|-----------|-----------|----------|--------------|--------|
| PostgreSQL | SCHEMA dedup_lab | dedup-lab (Samba AD) | DROP SCHEMA CASCADE + CREATE | DONE |
| CockroachDB | DATABASE dedup_lab | dedup_lab (TLS, Passwort) | DROP DATABASE + CREATE | DONE |
| Redis | Key-Prefix `dedup:*` | (kein Auth) | SCAN + DEL (cluster-safe) | DONE |
| Kafka | Topic-Prefix `dedup-lab-*` | (kein Auth) | kafka-topics --delete | DONE |
| MinIO | Bucket-Prefix `dedup-lab-*` | dedup-lab (S3 Auth) | mc rb --force + mc mb | DONE |
| MariaDB | DATABASE dedup_lab | (TODO: Samba AD) | DROP DATABASE + CREATE | NOT DEPLOYED |
| ClickHouse | DATABASE dedup_lab | (TODO) | DROP DATABASE + CREATE | NOT DEPLOYED |

**SICHERHEIT:**
- Lab-User `dedup-lab` hat NUR Zugriff auf Lab-Schemas
- Produktionsdaten werden NIEMALS gelesen, geaendert oder geloescht
- Lab-Schemas werden VOR und NACH jedem Experiment zurueckgesetzt
- Samba AD Integration = anderer Agent (NICHT in diesem Scope!)

---

## Umgebungsregeln (aus Session-Lektuere, VERBINDLICH)

### Fragile Cluster-Regeln
1. **MinIO = Direct Disk** — KEIN Longhorn PVC → Longhorn-Metriken funktionieren NICHT
2. **Redis = Cluster Mode** — kein SELECT, Key-Prefix `dedup:*` Isolation
3. **CockroachDB = TLS** — sslmode=verify-full, Zertifikate in K8s Secrets
4. **Longhorn thin-provisioned** — physischer Speicher waechst, schrumpft NICHT automatisch
5. **Cluster_NFS Daten NIE LOESCHEN** — nur in tmp/ Ordner entpacken
6. **Samba AD = ANDERER AGENT** — nicht in meinem Scope!
7. **Cluster-Ordner = NUR LESEN** — zweiter Agent aktiv
8. **Pipeline 3 (Experiment) = NUR MANUELL** — Produktions-DBs!

### Git-Regeln
- **Pull = MERGE** (kein Rebase!)
- **Branch = development** (mind. so fortgeschritten wie master)
- **Push zu GitLab UND GitHub** (HTTPS, `http.sslVerify=false` fuer GitLab)

---

## OFFENE AUFGABEN (Priorisiert)

### P0 (BLOCKIERT — muss zuerst)
1. **[INFRA] K8s Runner neustart** — Pipeline Jobs werden nicht abgeholt
   - `kubectl rollout restart deployment gitlab-runner -n gitlab-runner`
2. **[INFRA] Prometheus + Grafana deployen** — KEIN Monitoring-Stack im Cluster!
   - kube-prometheus-stack (Helm) oder standalone
   - Longhorn ServiceMonitor fuer `longhorn_volume_actual_size_bytes`
   - Pushgateway fuer C++ Metric Push

### P1 (HOCH — Experiment-Voraussetzungen)
3. **[INFRA] MariaDB deployen** — StatefulSet, 4 Replicas, longhorn-database SC, 50Gi/Pod
4. **[INFRA] ClickHouse deployen** — StatefulSet, 4 Replicas, longhorn-database SC, 50Gi/Pod
5. **[CODE] 100ms Metrics Thread** — Background-Thread in C++ fuer Echtzeit-Sampling
6. **[CODE] Kafka Metrics Producer** — JSON Trace an `dedup-lab-metrics` Topic
7. **[CODE] Prometheus Push Integration** — MetricsCollector → Pushgateway
8. **[CODE] config.example.json updaten** — Korrigierte PVC-Namen (data-postgres-ha-0, datadir-cockroachdb-0, etc.)

### P2 (MITTEL — nach erstem erfolgreichen Dry-Run)
9. **[CODE] Grafana Dashboard JSON** — Vorkonfiguriertes Dashboard fuer Experiment
10. **[CODE] MinIO Size ohne Longhorn** — Alternative Messmethode (mc admin info / S3 API)
11. **[DOC] doku.tex Phase 2** — DuckDB, Cassandra, MongoDB Sektionen ergaenzen
12. **[DOC] Redcomponent-DB → comdare-DB** in doku.tex

### P3 (SPAETER — erweiterte Systeme)
13. **[INFRA+CODE] QuestDB deployen + Connector**
14. **[INFRA+CODE] InfluxDB v3 deployen + Connector**
15. **[INFRA+CODE] TimescaleDB deployen + Connector**
16. **[INFRA+CODE] Cassandra/ScyllaDB deployen + Connector**

---

## PVC-Namen Mapping (KORRIGIERT aus Cluster-Scan)

Die config.example.json hatte teilweise falsche PVC-Namen. Hier die korrekten:

| System | PVC Name | Namespace | Groesse |
|--------|----------|-----------|---------|
| PostgreSQL | data-postgres-ha-0 | databases | 50Gi |
| CockroachDB | datadir-cockroachdb-0 | cockroach-operator-system | 125Gi |
| Redis | data-redis-cluster-0 | redis | 25Gi |
| Kafka | data-kafka-cluster-broker-0 | kafka | 50Gi |
| MinIO | (Direct Disk — KEIN PVC!) | minio | - |

**HINWEIS:** Fuer EDR-Berechnung wird pro System NUR ein PVC gemessen.
Die Longhorn-Replikation (N=4) ist im PVC-Volume bereits enthalten.

---

## Git History

```
5ac1732 Triple pipeline: fix tag matching, add cleanup-only mode, safety docs
b0bb5b6 Longhorn metrics integration, MariaDB/ClickHouse stubs, EDR + throughput fixes
61a71c3 Complete C++ test framework: SHA-256, dataset generator, S3 auth, K8s CI
e29c9d4 Fix Redis connector for cluster mode + CockroachDB TLS + MinIO/Kafka lab setup
c2fc814 Lab isolation: Samba AD user, PG schema, CockroachDB database
38b6824 Update session docs: C++ framework complete, triple pipeline, next steps
39cb309 Add C++ integration test framework + triple CI pipeline
2cac967 Dual CI pipeline + session update: 3-pipeline architecture
fa9d890 Initial commit: LaTeX paper + experiment framework + kartografie
```
