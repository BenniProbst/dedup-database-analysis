# Session 9: Triple Pipeline + 100ms Monitoring-Architektur
**Datum:** 2026-02-17, ~22:00–23:30 UTC
**Commits:** `5ac1732`, `5d23bbb`, `70c113f`
**Vorherige Commits:** `b0bb5b6` (Longhorn metrics, MariaDB/ClickHouse stubs)
**Branch:** `development`
**GitLab Project ID:** 280
**Remotes:** GitLab (HTTPS, sslVerify=false) + GitHub (HTTPS, PAT)

---

## Inhaltsverzeichnis

1. [Zusammenfassung](#zusammenfassung)
2. [CI Triple Pipeline](#ci-triple-pipeline)
3. [Cleanup-Only Modus](#cleanup-only-modus)
4. [Cluster-Reconnaissance](#cluster-reconnaissance)
5. [Monitoring-Architektur (100ms)](#monitoring-architektur-100ms)
6. [MetricsTrace Implementation](#metricstrace-implementation)
7. [Experiment-Workflow](#experiment-workflow)
8. [Schema-Isolation](#schema-isolation)
9. [Umgebungsregeln](#umgebungsregeln)
10. [Vollstaendige Quelldatei-Uebersicht](#vollstaendige-quelldatei-uebersicht)
11. [Offene Aufgaben (Priorisiert)](#offene-aufgaben-priorisiert)
12. [Git History](#git-history)

---

## 1. Zusammenfassung

Session 9 umfasst drei grosse Arbeitspakete:

1. **CI Triple Pipeline** komplett implementiert und validiert (.gitlab-ci.yml)
2. **Cluster-Zustand** per READ-ONLY Reconnaissance erfasst (alle DBs, PVCs, Namespaces)
3. **100ms Monitoring-Architektur** entworfen und teilweise implementiert (Header fertig, .cpp fehlt)

### Commits dieser Session

| Commit | Beschreibung | Dateien |
|--------|-------------|---------|
| `5ac1732` | Triple pipeline: fix tag matching, add cleanup-only mode, safety docs | .gitlab-ci.yml, main.cpp |
| `5d23bbb` | Session 9 docs + fix PVC names from cluster scan + metrics trace config | config.hpp, config.example.json, session docs |
| `70c113f` | WIP: MetricsTrace architecture + config fixes for 100ms Kafka sampling | experiment/metrics_trace.hpp, config.hpp |

---

## 2. CI Triple Pipeline

### Problem
Pipeline #1304 hing im `created` Status — Jobs wurden nicht vom K8s Runner abgeholt.

### Root Causes und Fixes

| # | Root Cause | Fix |
|---|-----------|-----|
| 1 | K8s Runner (ID 6) hatte KEINEN `kubernetes` Tag, aber CI nutzte `tags: [kubernetes]` | `PUT /api/v4/runners/6 {"tag_list":["kubernetes"]}` |
| 2 | `experiment:run` mit `allow_failure: false` blockierte gesamte Pipeline | Alle Experiment-Jobs auf `allow_failure: true` |
| 3 | Doppelte YAML `<<:` Merge-Keys (invalid YAML, zweiter ueberschreibt ersten) | Explizite `rules:` pro Job statt doppelter Anchor-Merge |

### Pipeline-Architektur (7 Jobs in 6 Stages)

```
Pipeline 1 (LaTeX, AUTOMATISCH bei docs/** Aenderungen):
  ┌─────────────┐
  │ latex:compile│ → docs/doku.pdf + doku.log Artifact (30 Tage)
  └─────────────┘    Image: texlive:latest, 3x pdflatex + bibtex

Pipeline 2 (C++, AUTOMATISCH bei src/cpp/** Aenderungen):
  ┌─────────┐     ┌────────────────┐     ┌──────────────────┐
  │ cpp:build│ → │ cpp:smoke-test │ → │ cpp:full-dry-test│
  └─────────┘     └────────────────┘     └──────────────────┘
  Image: gcc:14-bookworm
  Build: dedup-test + dedup-smoke-test (DEDUP_DRY_RUN=1)
  Smoke: 100 Files/Grade @ 16KB, SHA-256 Duplicate Ratio Verification
  Dry-Test: Alle Systeme x Alle Grades (Simulation, kein DB-Zugriff)

Pipeline 3 (Experiment, NUR MANUELL — Produktions-DBs!):
  ┌──────────────────┐     ┌─────────────────┐     ┌──────────────────────┐
  │ experiment:build │ → │ experiment:run  │ → │ experiment:cleanup │
  └──────────────────┘     └─────────────────┘     └──────────────────────┘
  experiment:build  = Eigener Binary-Build (unabhaengig von Pipeline 2)
  experiment:run    = ECHTES Experiment: 500 Files x 3 Grades, 4h Timeout,
                      Results Artifact 90 Tage, NUR nach manuellem Trigger!
  experiment:cleanup = --cleanup-only: Droppt alle Lab-Schemas
                       optional: true (laeuft auch wenn experiment:run fehlschlaegt)
```

### Sicherheitskonzept pro Job

- `experiment:build` — Kompiliert nur, kein DB-Zugriff
- `experiment:run` — `when: manual` + `allow_failure: true` = blockiert Pipeline NICHT
- `experiment:cleanup` — `needs: [{job: experiment:run, optional: true}]` = laeuft IMMER
- Alle 3 Jobs dokumentieren im YAML-Kommentar das Sicherheitsmodell

### CI Validierung
- GitLab Lint API: `POST /api/v4/projects/280/ci/lint` = `valid: true`
- Beide Remotes (GitLab + GitHub) synchron auf `5ac1732`

### Pipeline Runner Problem (OFFEN!)
- K8s Runner (ID 6): `contacted_at: 2026-02-17T21:36:24Z` — ueber 35 Minuten alt
- Status = `online` laut API, aber Jobs werden NICHT abgeholt
- Pipeline #1305 steht auf `created`
- **INFRA-Aufgabe:** `kubectl rollout restart deployment gitlab-runner -n gitlab-runner`

---

## 3. Cleanup-Only Modus

### Implementierung in `main.cpp`

Neuer CLI-Flag `--cleanup-only` (Zeile 59, 92, 125-126, 244-260):

```
Ablauf:
1. Verbinde zu allen konfigurierten Datenbanken
2. Droppe Lab-Schemas (dedup_lab) auf ALLEN Systemen
3. Disconnecte alle
4. Exit mit Code 0
```

**Sicherheit:**
- NUR `dedup_lab` Schemas/Databases/Keys/Topics/Buckets werden gedroppt
- Produktionsdaten werden NIEMALS beruehrt
- Wird von CI-Job `experiment:cleanup` automatisch nach Experiment verwendet
- LOG_WRN gibt explizite Warnung aus: "Customer data is NOT affected"

---

## 4. Cluster-Reconnaissance

### Datenbanken im Cluster (LIVE, READ-ONLY)

| System | Namespace | Pods | PVC Groesse | StorageClass | K8s Service | Port |
|--------|-----------|------|-------------|-------------|-------------|------|
| PostgreSQL HA | databases | 4/4 | 50Gi x4 = 200Gi | longhorn-database | postgres-lb | 5432 |
| CockroachDB | cockroach-operator-system | 4/4 | 125Gi x4 = 500Gi | longhorn-database | cockroachdb-public | 26257 |
| Redis Cluster | redis | 4/4 | 25Gi x4 = 100Gi | longhorn-database | redis-cluster | 6379 |
| Kafka (Strimzi) | kafka | 4B+4C=8 | 50Gi x4 + 10Gi x4 = 240Gi | longhorn-database | kafka-bootstrap | 9092 |
| MinIO | minio | 4/4 | Direct Disk | - (kein Longhorn!) | minio-lb | 9000 |
| Samba AD | samba-ad | 4/4 | - | - | samba-ad-lb | 389/636 |
| Redis (GitLab intern) | gitlab | 4/4 | 5Gi x4 = 20Gi | longhorn-database | (intern) | 6379 |

**TOTAL Longhorn Database PVCs:** 24 PVCs, ~1.060 Gi

### NICHT im Cluster (muss noch deployed werden)

| System | Prioritaet | C++ Connector | Cluster-Deploy | Geschaetzter Storage |
|--------|-----------|---------------|---------------|---------------------|
| MariaDB | HOCH (doku.tex 5.2) | FERTIG (mariadb_connector.cpp/hpp) | TODO: StatefulSet, 4 Replicas | 50Gi x4 = 200Gi |
| ClickHouse | HOCH (doku.tex 5.2) | FERTIG (clickhouse_connector.cpp/hpp) | TODO: StatefulSet, 4 Replicas | 50Gi x4 = 200Gi |
| Prometheus | KRITISCH | MetricsCollector nutzt es | TODO: kube-prometheus-stack | ~10Gi |
| Grafana | KRITISCH | MetricsCollector pusht Daten | TODO: mit kube-prometheus-stack | ~5Gi |

### PVC-Namen Mapping (KORRIGIERT aus Cluster-Scan)

Die `config.example.json` und `default_k8s_config()` hatten falsche PVC-Namen.
In dieser Session korrigiert:

| System | VORHER (FALSCH) | NACHHER (KORREKT) | Namespace |
|--------|----------------|-------------------|-----------|
| PostgreSQL | data-postgres-postgresql-0 | **data-postgres-ha-0** | databases |
| Redis Host | redis-standalone | **redis-cluster.redis.svc.cluster.local** | redis |
| Redis PVC | redis-data-redis-node-0 | **data-redis-cluster-0** | redis |
| Kafka PVC | data-kafka-cluster-kafka-0 | **data-kafka-cluster-broker-0** | kafka |
| MinIO PVC | (hatte Wert) | **""** (Direct Disk, KEIN PVC!) | minio |

### StorageClasses im Cluster

| Name | Provisioner | ReclaimPolicy | Verwendung |
|------|-------------|---------------|------------|
| longhorn (default) | driver.longhorn.io | Delete | Standard-Workloads |
| longhorn-database | driver.longhorn.io | **Retain** | ALLE Datenbanken (PG, CRDB, Redis, Kafka) |
| longhorn-data | driver.longhorn.io | Retain | User Data |
| longhorn-backup | driver.longhorn.io | Retain | Backups |
| longhorn-opnsense | driver.longhorn.io | Retain | OPNsense VMs |

---

## 5. Monitoring-Architektur (100ms Sampling)

### Anforderungen (User-Anweisung)

1. **JEDE Datenbank-Metrik JEDER Datenbank** alle 100ms (10 Hz) samplen
2. **Persistent Trace auf Kafka** — Topic `dedup-lab-metrics`
3. **Experiment-Events auf Kafka** — Topic `dedup-lab-events`
4. **Live-Dashboard auf Grafana** (Echtzeit-Visualisierung)
5. **Export VOR Cleanup** — Messwerte als CSV committen+pushen, DANN erst Daten loeschen
6. **Kafka = Doppelrolle** — DB unter Test UND Messdaten-Log

### Metriken pro Datenbank-System

#### PostgreSQL (libpq, SQL)
| Metrik | Quelle | Einheit |
|--------|--------|---------|
| pg_database_size | `pg_database_size('dedup_lab')` | bytes |
| xact_commit | pg_stat_database | count |
| xact_rollback | pg_stat_database | count |
| tup_inserted | pg_stat_database | count |
| tup_updated | pg_stat_database | count |
| tup_deleted | pg_stat_database | count |
| blks_hit | pg_stat_database | count |
| blks_read | pg_stat_database | count |
| buffers_checkpoint | pg_stat_bgwriter | count |
| wal_bytes | pg_stat_wal | bytes |

#### CockroachDB (libpq + libcurl)
| Metrik | Quelle | Einheit |
|--------|--------|---------|
| ranges | crdb_internal.kv_store_status | count |
| replicas | crdb_internal.kv_store_status | count |
| leaseholders | crdb_internal.kv_store_status | count |
| livebytes | crdb_internal.kv_store_status | bytes |
| keybytes | crdb_internal.kv_store_status | bytes |
| queries_per_sec | /_status/vars | rate |
| latency_p99 | /_status/vars | ms |

#### Redis (hiredis, INFO ALL)
| Metrik | Quelle | Einheit |
|--------|--------|---------|
| used_memory | INFO memory | bytes |
| used_memory_rss | INFO memory | bytes |
| keyspace_hits | INFO stats | count |
| keyspace_misses | INFO stats | count |
| instantaneous_ops_per_sec | INFO stats | rate |
| total_connections_received | INFO stats | count |
| connected_clients | INFO clients | count |

#### Kafka (librdkafka + libcurl)
| Metrik | Quelle | Einheit |
|--------|--------|---------|
| UnderReplicatedPartitions | JMX / Admin API | count |
| BytesInPerSec | JMX / Admin API | bytes/s |
| BytesOutPerSec | JMX / Admin API | bytes/s |
| LogEndOffset | Admin API | offset |
| LogSize | Metrics endpoint | bytes |

#### MinIO (libcurl, Prometheus endpoint)
| Metrik | Quelle | Einheit |
|--------|--------|---------|
| bucket_usage_total_bytes | :9000/minio/v2/metrics/cluster | bytes |
| s3_requests_total | :9000/minio/v2/metrics/cluster | count |
| s3_rx_bytes_total | :9000/minio/v2/metrics/cluster | bytes |
| s3_tx_bytes_total | :9000/minio/v2/metrics/cluster | bytes |

#### MariaDB (libmysqlclient, SHOW GLOBAL STATUS)
| Metrik | Quelle | Einheit |
|--------|--------|---------|
| Innodb_buffer_pool_reads | SHOW GLOBAL STATUS | count |
| Innodb_data_written | SHOW GLOBAL STATUS | bytes |
| Threads_connected | SHOW GLOBAL STATUS | count |
| Bytes_received | SHOW GLOBAL STATUS | bytes |
| Bytes_sent | SHOW GLOBAL STATUS | bytes |
| Com_insert | SHOW GLOBAL STATUS | count |
| Com_delete | SHOW GLOBAL STATUS | count |

#### ClickHouse (libcurl, HTTP API SQL)
| Metrik | Quelle | Einheit |
|--------|--------|---------|
| Query | system.metrics | count |
| Merge | system.metrics | count |
| ReplicatedFetch | system.metrics | count |
| InsertedRows | system.events | count |
| InsertedBytes | system.events | bytes |
| CompressedReadBufferBytes | system.asynchronous_metrics | bytes |

### Datenfluss-Diagramm

```
┌─────────────────────────────────────────────────┐
│ C++ dedup-test Binary (K8s Runner Pod)          │
│                                                  │
│  ┌──────────────────────┐                       │
│  │ Main Experiment Loop │                       │
│  │ (bulk_insert,        │                       │
│  │  perfile_insert,     │ publish_event()       │
│  │  perfile_delete,     │──────────┐            │
│  │  maintenance)        │          │            │
│  └──────────────────────┘          │            │
│                                     │            │
│  ┌──────────────────────┐          │            │
│  │ MetricsTrace Thread  │          │            │
│  │ (100ms sampling)     │          │            │
│  │                      │          │            │
│  │ for each DB system:  │          │            │
│  │   collect_*() →      │          │            │
│  │   MetricPoint JSON   │          │            │
│  └──────────┬───────────┘          │            │
│             │                      │            │
│             ▼                      ▼            │
│  ┌──────────────────────────────────────┐       │
│  │ Kafka Producer (librdkafka)          │       │
│  │                                       │       │
│  │ Topic: dedup-lab-metrics (100ms)     │       │
│  │ Topic: dedup-lab-events  (stages)    │       │
│  └──────────────────┬───────────────────┘       │
│                      │                           │
│  ┌──────────────────┐│  ┌────────────────────┐  │
│  │ Prometheus Push   ││  │ ResultsExporter    │  │
│  │ (Pushgateway)     ││  │ Kafka→CSV→git push │  │
│  └──────────┬────────┘│  └────────┬───────────┘  │
└─────────────┼─────────┼──────────┼───────────────┘
              │         │          │
              ▼         ▼          ▼
    ┌─────────────┐  ┌──────┐  ┌──────────────────┐
    │  Grafana     │  │Kafka │  │ GitLab Repo      │
    │  Dashboard   │  │Broker│  │ results/*.csv    │
    │  (Live)      │  │      │  │ auto-commit+push │
    └─────────────┘  └──────┘  └──────────────────┘
```

### Kafka Topic-Schema

**`dedup-lab-metrics`** (100ms, partitioned by system):
```json
{
  "ts": 1708210800123,
  "system": "postgresql",
  "metric": "pg_database_size",
  "value": 123456789,
  "unit": "bytes"
}
```

**`dedup-lab-events`** (experiment lifecycle):
```json
{
  "ts": 1708210800000,
  "event_type": "stage_start",
  "system": "postgresql",
  "dup_grade": "U50",
  "stage": "bulk_insert",
  "detail": "{\"num_files\": 500, \"data_dir\": \"/tmp/datasets/U50\"}"
}
```

---

## 6. MetricsTrace Implementation

### Fertig (Header)

**`experiment/metrics_trace.hpp`** — Interface komplett:

| Klasse/Struct | Beschreibung | Status |
|---------------|-------------|--------|
| `MetricPoint` | Einzelner Metrik-Datenpunkt (JSON-serialisierbar) | FERTIG |
| `ExperimentEvent` | Experiment-Lifecycle-Event (JSON-serialisierbar) | FERTIG |
| `MetricsTrace` | Background-Thread-Klasse mit Kafka-Producer | FERTIG (Interface) |
| `MetricCollectorFn` | `std::function<vector<MetricPoint>(const DbConnection&)>` | FERTIG |
| `collectors::collect_*()` | 7 Built-in Collectors (PG, CRDB, Redis, Kafka, MinIO, MariaDB, CH) | Deklariert |
| `collectors::for_system()` | Factory-Funktion: DbSystem → Collector | Deklariert |

**`config.hpp`** — Erweitert um:
- `MetricsTraceConfig` Struct (kafka_bootstrap, topics, interval, enabled)
- `GitExportConfig` Struct (remote_name, branch, auto_push, ssl_verify)
- JSON-Parsing fuer beide Config-Sektionen
- Korrigierte PVC-Namen in `default_k8s_config()`

### NICHT fertig (naechste Session)

| Datei | Beschreibung | Geschaetzter Umfang |
|-------|-------------|---------------------|
| `experiment/metrics_trace.cpp` | Kafka-Producer Init, 100ms Sampling Loop, 7 DB-Collectors | ~400-500 Zeilen |
| `experiment/results_exporter.hpp` | Interface fuer Kafka→CSV→git Export | ~50 Zeilen |
| `experiment/results_exporter.cpp` | Kafka Consumer, CSV-Writer, git commit+push | ~250-300 Zeilen |
| `main.cpp` Integration | MetricsTrace start/stop, Export, Cleanup-Reihenfolge | ~30-40 Zeilen Aenderung |
| `CMakeLists.txt` | metrics_trace.cpp + results_exporter.cpp hinzufuegen | ~2 Zeilen |

---

## 7. Experiment-Workflow (ZIEL)

### Aktueller Workflow (main.cpp, Stand Session 9)

```
1. SHA-256 Self-Test
2. Optional: generate_all() Datasets
3. Load ExperimentConfig (JSON oder K8s defaults)
4. Connect all DBs
5. IF --cleanup-only: drop schemas → exit(0)
6. create_all_lab_schemas()
7. run_full_experiment() per Connector
8. Save results/combined_results.json
9. drop_all_lab_schemas()
10. Disconnect, print summary table
```

### Ziel-Workflow (nach MetricsTrace Integration)

```
 1. SHA-256 Self-Test
 2. Optional: generate_all() Datasets
 3. Load ExperimentConfig (JSON oder K8s defaults)
 4. Connect all DBs
 5. IF --cleanup-only: drop schemas + delete metrics topics → exit(0)
 6. create_all_lab_schemas()
 7. Create Kafka metrics/events Topics
 8. MetricsTrace::register_system() fuer jede DB
 9. MetricsTrace::start() — 100ms Background Thread startet
10. MetricsTrace::publish_event({experiment_start})
11. FOR EACH connector, dup_grade, stage:
      a. MetricsTrace::publish_event({stage_start})
      b. run_stage(connector, stage, grade)
      c. MetricsTrace::publish_event({stage_end})
12. MetricsTrace::publish_event({experiment_end})
13. MetricsTrace::stop() — Background Thread stoppt
14. ResultsExporter::export_metrics(Kafka → CSV/JSON)
15. ResultsExporter::export_events(Kafka → CSV/JSON)
16. Save results/combined_results.json
17. Git: add results/ + commit + push to GitLab
18. ERST DANN: drop_all_lab_schemas() + delete metrics topics
19. Disconnect, print summary table
```

**KRITISCH:** Schritt 17 (git push) MUSS vor Schritt 18 (Cleanup) erfolgen!
Messdaten muessen persistent in GitLab sein, bevor sie aus Kafka geloescht werden.

---

## 8. Schema-Isolation

### Lab-Schema pro Datenbank

| Datenbank | Isolation-Methode | Lab-Schema | Lab-User | Reset-Methode | Connector Status |
|-----------|-------------------|------------|----------|--------------|-----------------|
| PostgreSQL | `CREATE SCHEMA` | `dedup_lab` | dedup-lab (Samba AD) | DROP SCHEMA CASCADE + CREATE | DONE |
| CockroachDB | `CREATE DATABASE` | `dedup_lab` | dedup_lab (TLS, Passwort) | DROP DATABASE + CREATE | DONE |
| Redis | Key-Prefix | `dedup:*` | (kein Auth) | SCAN + DEL (cluster-safe) | DONE |
| Kafka | Topic-Prefix | `dedup-lab-*` | (kein Auth) | kafka-topics --delete | DONE |
| MinIO | Bucket-Prefix | `dedup-lab-*` | dedup-lab (S3 Auth) | mc rb --force + mc mb | DONE |
| MariaDB | `CREATE DATABASE` | `dedup_lab` | TODO: Samba AD | DROP DATABASE + CREATE | **NOT DEPLOYED** |
| ClickHouse | `CREATE DATABASE` | `dedup_lab` | TODO | DROP DATABASE + CREATE | **NOT DEPLOYED** |

### Kafka Doppelrolle — Topic-Aufteilung

| Topic | Zweck | Lebenszeit | Wird zurueckgesetzt |
|-------|-------|-----------|---------------------|
| `dedup-lab-u0` | Testdaten U0 Grade | Pro Experiment | JA (mit Lab-Schema) |
| `dedup-lab-u50` | Testdaten U50 Grade | Pro Experiment | JA (mit Lab-Schema) |
| `dedup-lab-u90` | Testdaten U90 Grade | Pro Experiment | JA (mit Lab-Schema) |
| `dedup-lab-metrics` | 100ms DB-Metrik-Snapshots | Pro Experiment | JA (NACH Export!) |
| `dedup-lab-events` | Experiment-Stage-Events | Pro Experiment | JA (NACH Export!) |

**Alle 5 Topics laufen unter dem Lab-User und werden zusammen zurueckgesetzt.**

### Sicherheitsgarantien

1. Lab-User `dedup-lab` hat NUR Zugriff auf Lab-Schemas
2. Produktionsdaten werden NIEMALS gelesen, geaendert oder geloescht
3. Lab-Schemas werden VOR und NACH jedem Experiment zurueckgesetzt
4. `--cleanup-only` Modus fuer CI-Job (Schritt 5 im Workflow)
5. Samba AD Integration = **anderer Agent** (NICHT in diesem Scope!)

---

## 9. Umgebungsregeln

### Fragile Cluster-Regeln (aus Session 1-8 Lektuere)

| # | Regel | Auswirkung |
|---|-------|-----------|
| 1 | **MinIO = Direct Disk** — KEIN Longhorn PVC | Longhorn-Metriken nicht verfuegbar, S3 API fuer Size |
| 2 | **Redis = Cluster Mode** — kein SELECT | Key-Prefix `dedup:*` statt DB-Nummer fuer Isolation |
| 3 | **CockroachDB = TLS** — sslmode=verify-full | Zertifikate aus K8s Secrets, seit Session 5 |
| 4 | **Longhorn thin-provisioned** | Physischer Speicher waechst, schrumpft NICHT nach Deletes |
| 5 | **Cluster_NFS Daten NIE LOESCHEN** | Nur in tmp/ Ordner entpacken |
| 6 | **Samba AD = ANDERER AGENT** | Nicht in meinem Scope! |
| 7 | **Cluster-Ordner = NUR LESEN** | Zweiter Agent aktiv |
| 8 | **Pipeline 3 = NUR MANUELL** | Arbeitet mit Produktions-DBs |

### Git-Regeln

- **Pull = MERGE** (kein Rebase!)
- **Branch = development** (mind. so fortgeschritten wie master)
- **Push zu GitLab UND GitHub** (HTTPS, `http.sslVerify=false` fuer GitLab)

### Hardware-Kontext

- **K8s-Nodes:** 4x Intel N97 (Alder Lake-N, SSE4.2 + AVX2)
- **C++ statt Python** — N97 zu schwach fuer Python-Overhead (Session 4 Entscheidung)
- **C++20** mit CMake 3.20+, gcc:14-bookworm Docker Image

---

## 10. Vollstaendige Quelldatei-Uebersicht

### Verzeichnisstruktur

```
dedup-database-analysis/
├── .gitlab-ci.yml              ← 7 Jobs, 6 Stages, Triple Pipeline
├── .gitignore
├── README.md
├── credentials.env             ← Lab-Credentials (NICHT committen in Prod!)
│
├── docs/
│   ├── doku.tex                ← Hauptdokument (LaTeX)
│   ├── doku.bib                ← Bibliographie
│   ├── Makefile                ← pdflatex + bibtex Build
│   └── ...                     ← Hilfsklassen, Logos, aeltere Versionen
│
├── sessions/
│   ├── 20260216-kartografie-doku-tex.md
│   ├── 20260216-session-dedup-projekt-setup.md    ← Sessions 1-8
│   └── 20260217-session-9-triple-pipeline-monitoring.md  ← DIESE Session
│
├── src/cpp/
│   ├── CMakeLists.txt          ← Build-System (C++20, optionale Deps)
│   ├── config.hpp              ← ExperimentConfig, alle Structs + JSON-Parsing
│   ├── config.example.json     ← Beispiel-Config mit korrekten PVC-Namen
│   ├── main.cpp                ← Hauptprogramm (324 Zeilen)
│   │
│   ├── connectors/
│   │   ├── db_connector.hpp    ← Abstrakte Schnittstelle (DbConnector)
│   │   ├── postgres_connector.cpp/hpp   ← PostgreSQL + CockroachDB (PG Wire)
│   │   ├── redis_connector.cpp/hpp      ← Redis Cluster (hiredis)
│   │   ├── kafka_connector.cpp/hpp      ← Kafka (librdkafka)
│   │   ├── minio_connector.cpp/hpp      ← MinIO (libcurl, S3 API)
│   │   ├── mariadb_connector.cpp/hpp    ← MariaDB (libmysqlclient) STUB
│   │   └── clickhouse_connector.cpp/hpp ← ClickHouse (libcurl HTTP) STUB
│   │
│   ├── experiment/
│   │   ├── schema_manager.cpp/hpp       ← Lab-Schema Create/Drop/Reset
│   │   ├── data_loader.cpp/hpp          ← Experiment-Runner + EDR-Berechnung
│   │   ├── dataset_generator.cpp/hpp    ← Synthetische Testdaten (U0/U50/U90)
│   │   ├── metrics_collector.cpp/hpp    ← Longhorn Prometheus + Grafana Push
│   │   ├── metrics_trace.hpp            ← 100ms Background Thread (Interface) WIP
│   │   ├── metrics_trace.cpp            ← (FEHLT! Implementation TODO)
│   │   ├── results_exporter.hpp         ← (FEHLT! Interface TODO)
│   │   └── results_exporter.cpp         ← (FEHLT! Kafka→CSV→git TODO)
│   │
│   └── utils/
│       ├── logger.hpp           ← LOG_INF/LOG_ERR/LOG_WRN/LOG_DBG Makros
│       ├── sha256.hpp           ← FIPS 180-4 SHA-256 (Header-only)
│       └── timer.hpp            ← High-precision Timer (chrono)
│
├── scripts/
│   ├── generate_datasets.py     ← Python Legacy (wird durch C++ ersetzt)
│   └── measure_longhorn.sh      ← Longhorn Metrics Shell-Script
│
├── src/cleanup/
│   └── postgresql_maintenance.py ← Legacy Python Cleanup
│
├── src/loaders/
│   └── postgresql_loader.py      ← Legacy Python Loader
│
├── src/reporters/
│   ├── aggregate_results.py      ← Legacy Python Reporter
│   ├── final_report.py
│   └── generate_charts.py
│
└── k8s/
    ├── base/                     ← (leer, TODO: K8s Manifeste)
    └── jobs/                     ← (leer, TODO: CronJobs)
```

### Datei-Status Tabelle

| Datei | Zeilen | Status | Letzte Aenderung |
|-------|--------|--------|------------------|
| `.gitlab-ci.yml` | ~329 | KOMPLETT | Session 9 (5ac1732) |
| `main.cpp` | 324 | FUNKTIONAL, MetricsTrace-Integration fehlt | Session 9 (5ac1732) |
| `config.hpp` | 257 | KOMPLETT (inkl. MetricsTrace + GitExport) | Session 9 (70c113f) |
| `config.example.json` | 102 | KOMPLETT (PVC-Namen korrigiert) | Session 9 (5d23bbb) |
| `db_connector.hpp` | ~55 | KOMPLETT (abstrakte Schnittstelle) | Session 4 |
| `postgres_connector.cpp/hpp` | ~200+60 | KOMPLETT (PG + CRDB) | Session 5 |
| `redis_connector.cpp/hpp` | ~150+40 | KOMPLETT (Cluster Mode) | Session 5 |
| `kafka_connector.cpp/hpp` | ~180+50 | KOMPLETT (librdkafka) | Session 6 |
| `minio_connector.cpp/hpp` | ~160+45 | KOMPLETT (S3 Auth) | Session 6 |
| `mariadb_connector.cpp/hpp` | ~100+40 | **STUB** (DB nicht deployed) | Session 8 |
| `clickhouse_connector.cpp/hpp` | ~100+40 | **STUB** (DB nicht deployed) | Session 8 |
| `schema_manager.cpp/hpp` | ~60+30 | KOMPLETT | Session 5 |
| `data_loader.cpp/hpp` | ~230+68 | KOMPLETT (EDR + Throughput) | Session 8 |
| `dataset_generator.cpp/hpp` | ~280+75 | KOMPLETT (U0/U50/U90) | Session 7 |
| `metrics_collector.cpp/hpp` | ~120+42 | KOMPLETT (Prometheus + Grafana) | Session 8 |
| `metrics_trace.hpp` | 117 | KOMPLETT (Interface) | Session 9 (70c113f) |
| `metrics_trace.cpp` | 0 | **FEHLT KOMPLETT** | - |
| `results_exporter.hpp` | 0 | **FEHLT KOMPLETT** | - |
| `results_exporter.cpp` | 0 | **FEHLT KOMPLETT** | - |
| `CMakeLists.txt` | ~120 | Funktional, 2 neue Sources fehlen | Session 7 |

---

## 11. Offene Aufgaben (Priorisiert)

### P0: BLOCKIERT (INFRA-Scope, muss zuerst)

| # | Typ | Aufgabe | Beschreibung | Abhaengigkeit |
|---|-----|---------|-------------|---------------|
| 1 | INFRA | K8s Runner Neustart | Pipeline #1305 Jobs werden nicht abgeholt, Runner contacted_at veraltet | `kubectl rollout restart deployment gitlab-runner -n gitlab-runner` |
| 2 | INFRA | Prometheus + Grafana | KEIN Monitoring-Stack im Cluster! Braucht: kube-prometheus-stack (Helm), Longhorn ServiceMonitor, Pushgateway | MariaDB+CH Deploy, Grafana Dashboard |
| 3 | INFRA | MariaDB deployen | StatefulSet, 4 Replicas, longhorn-database SC, 50Gi/Pod, Samba AD Auth | Connector FERTIG, wartet auf Cluster |
| 4 | INFRA | ClickHouse deployen | StatefulSet, 4 Replicas, longhorn-database SC, 50Gi/Pod | Connector FERTIG, wartet auf Cluster |

### P1: HOCH (CODE-Scope, mein Arbeitsbereich)

| # | Typ | Aufgabe | Beschreibung | Geschaetzter Umfang |
|---|-----|---------|-------------|---------------------|
| 5 | CODE | `metrics_trace.cpp` | Kafka Producer Init (librdkafka), 100ms Sampling Loop (std::thread + sleep_for), 7 DB Metric Collectors (PG, CRDB, Redis, Kafka, MinIO, MariaDB, ClickHouse) | ~400-500 Zeilen |
| 6 | CODE | `results_exporter.hpp/.cpp` | Interface + Implementation: Kafka Consumer → CSV/JSON Table Export, git add+commit+push to GitLab, Signal Cleanup OK | ~300-350 Zeilen |
| 7 | CODE | `main.cpp` Integration | MetricsTrace register+start/stop, ResultsExporter export+push, Cleanup NACH Export | ~30-40 Zeilen |
| 8 | CODE | `CMakeLists.txt` Update | metrics_trace.cpp + results_exporter.cpp zu DEDUP_SOURCES | 2 Zeilen |

### P2: MITTEL (nach erstem erfolgreichen Dry-Run)

| # | Typ | Aufgabe | Beschreibung |
|---|-----|---------|-------------|
| 9 | CODE | Grafana Dashboard JSON | Vorkonfiguriertes Dashboard mit Panels fuer alle 7 DB-Systeme |
| 10 | CODE | MinIO Size ohne Longhorn | Alternative Messmethode via `mc admin info` oder S3 ListBuckets API |
| 11 | DOC | doku.tex Phase 2 | DuckDB, Cassandra, MongoDB Sektionen ergaenzen |
| 12 | DOC | ~~Naming~~ | ~~Redcomponent-DB → comdare-DB in doku.tex~~ **DONE (Session 9c)** |

### P3: SPAETER (erweiterte Datenbank-Systeme)

| # | Typ | Aufgabe |
|---|-----|---------|
| 13 | INFRA+CODE | QuestDB deployen + Connector |
| 14 | INFRA+CODE | InfluxDB v3 deployen + Connector |
| 15 | INFRA+CODE | TimescaleDB deployen + Connector |
| 16 | INFRA+CODE | Cassandra/ScyllaDB deployen + Connector |

### Abhaengigkeits-Graph

```
P0.1 (Runner)     P0.2 (Prometheus)    P0.3 (MariaDB)    P0.4 (ClickHouse)
     │                  │                    │                   │
     │                  │                    └─────┬─────────────┘
     │                  │                          │
     │                  ▼                          ▼
     │             P1.5 (metrics_trace.cpp)  Connector-Tests
     │                  │                       gegen Live-DBs
     │                  ▼
     │             P1.6 (results_exporter)
     │                  │
     │                  ▼
     │             P1.7 (main.cpp Integration)
     │                  │
     │                  ▼
     ├────────→  P1.8 (CMakeLists.txt)
     │                  │
     ▼                  ▼
  CI Pipeline ←── Build + Test (automatisch)
                        │
                        ▼
                  Experiment (MANUELL)
                        │
                        ▼
                  P2.9 (Grafana Dashboard)
```

---

## Session 9c: Continuation (~23:30+ UTC)

### Implementiert in Session 9c

#### 1. MetricsTrace.cpp KOMPLETT (~400 Zeilen)
- **7 DB-Collectors implementiert:**
  - PostgreSQL: pg_database_size, pg_stat_database (7 Metriken), pg_stat_bgwriter (3), pg_stat_wal
  - CockroachDB: crdb_internal.kv_store_status (6 Metriken) + HTTP /_status/vars
  - Redis: INFO ALL Parser (7 Metriken: used_memory, rss, hits/misses, ops/sec, clients)
  - Kafka: JMX Exporter Port 9404 (4 Metriken: bytes_in/out, under_replicated, log_size)
  - MinIO: /minio/v2/metrics/cluster Prometheus endpoint (4 Metriken: bucket_usage, s3_requests, rx/tx)
  - MariaDB: SHOW GLOBAL STATUS (7 Metriken: innodb_pool, data_written, threads, bytes, com_*)
  - ClickHouse: system.metrics + system.events via HTTP API (5 Metriken)
- **Kafka Producer:** librdkafka mit 10ms Buffer, async produce, non-blocking poll
- **Background Thread:** 100ms sleep mit praeziser Kompensation (elapsed-aware)
- **Dry-Run:** Alle Metriken werden geloggt aber nicht gesendet
- **Commit:** `a041c82`

#### 2. ResultsExporter KOMPLETT (~300 Zeilen)
- **Kafka Consumer:** Liest ALLE Nachrichten von metrics/events Topics (auto.offset.reset=earliest)
- **CSV-Writer:** Separate CSVs fuer Metriken und Events mit korrektem Escaping
- **Git Pipeline:** `git add results/ → git commit → git push gitlab development`
- **export_all():** Vollstaendiger Pipeline-Aufruf (consume → CSV → git push)
- **Commit:** `a041c82`

#### 3. main.cpp Integration
- MetricsTrace register_system() + start() vor Experiment-Loop
- ExperimentEvents: experiment_start/end, system_start/end
- ResultsExporter::export_all() NACH Experiment, VOR Cleanup
- **Export-before-Cleanup Garantie:** Daten persistent in GitLab bevor Lab-Schemas gedroppt werden
- **Commit:** `a041c82`

#### 4. doku.tex Naming Fix
- 2x "Redcomponent-DB" → "comdare-DB" (Zeile 201, 204)
- Aeltere Dokumentversionen (20260108, 20260127, 20260216) = historische Snapshots, NICHT geaendert
- **Commit:** `0c5edf3`

#### 5. MinIO Physical Size Measurement
- **Problem:** MinIO = Direct Disk, kein Longhorn PVC → Longhorn-Metriken funktionieren NICHT
- **Loesung:** `MetricsCollector::get_minio_physical_size()` via MinIO Prometheus Endpoint
- Parsed `minio_bucket_usage_total_bytes{bucket="dedup-lab-*"}` Prometheus-Format
- `data_loader.cpp` BEFORE/AFTER: Erkennt MinIO-System und nutzt Prometheus statt Longhorn
- **Commit:** `0c5edf3`

#### 6. CMakeLists.txt + Config
- 2 neue Source-Dateien: metrics_trace.cpp, results_exporter.cpp
- config.example.json: events_topic + git_export Sektion
- **Commit:** `a041c82`

### Session 9c Commits

```
0c5edf3 Fix naming + MinIO physical size: comdare-DB rename, S3 metrics endpoint
a041c82 Complete MetricsTrace + ResultsExporter: 100ms sampling, Kafka dual output, CSV export
```

### Dateien geaendert/erstellt in Session 9c

| Datei | Aktion | Zeilen |
|-------|--------|--------|
| `experiment/metrics_trace.cpp` | NEU | ~400 |
| `experiment/results_exporter.hpp` | NEU | ~55 |
| `experiment/results_exporter.cpp` | NEU | ~250 |
| `experiment/metrics_trace.hpp` | Erweitert | +6 (now_ms() inline) |
| `experiment/metrics_collector.hpp` | Erweitert | +4 (get_minio_physical_size) |
| `experiment/metrics_collector.cpp` | Erweitert | +50 (MinIO Prometheus parser) |
| `experiment/data_loader.cpp` | Erweitert | +10 (MinIO BEFORE/AFTER) |
| `main.cpp` | Erweitert | +47 (MetricsTrace + Exporter Integration) |
| `CMakeLists.txt` | Erweitert | +2 |
| `config.example.json` | Erweitert | +10 |
| `docs/doku.tex` | Fix | 2 Ersetzungen |
| Session-Doku | Erweitert | +200 |

### Verbleibende Aufgaben nach Session 9c

#### DONE (Session 9 + 9c)
- [x] CI Triple Pipeline (7 Jobs, 6 Stages)
- [x] --cleanup-only Modus
- [x] PVC-Namen korrigiert (4 Fehler)
- [x] MetricsTrace Header (Interface)
- [x] MetricsTrace Implementation (7 Collectors + Kafka Producer + Background Thread)
- [x] ResultsExporter (Kafka→CSV→git push)
- [x] main.cpp Integration (register/start/stop + export-before-cleanup)
- [x] CMakeLists.txt Update
- [x] config.example.json (events_topic + git_export)
- [x] doku.tex Naming (Redcomponent-DB → comdare-DB)
- [x] MinIO Physical Size via Prometheus Endpoint

#### OFFEN (naechste Session)

| Prioritaet | Typ | Aufgabe | Abhaengigkeit |
|-----------|-----|---------|---------------|
| P0 | INFRA | K8s Runner neustart | kubectl rollout restart |
| P0 | INFRA | Prometheus + Grafana deployen | kube-prometheus-stack |
| P0 | INFRA | MariaDB in Cluster deployen | StatefulSet, 4 Replicas |
| P0 | INFRA | ClickHouse in Cluster deployen | StatefulSet, 4 Replicas |
| P2 | CODE | Grafana Dashboard JSON Template | Prometheus muss deployed sein |
| P2 | DOC | doku.tex Phase 2 (DuckDB, Cassandra, MongoDB) | Kein Blocker |

---

## Session 9d: comdare-DB Connector + Redis doku.tex + Missing-DB-Analyse

**Datum:** 2026-02-17, Kontext-Fortsetzung
**Branch:** `development`

### Zusammenfassung Session 9d

1. **Redis zu doku.tex hinzugefuegt** (Section 5.2 "Systems under test")
   - AOF/RDB Persistenz-Modi erklärend eingeordnet
   - Cluster-Modus und Key-Prefix-Isolation dokumentiert
   - War als einziges der 5 bereits deployed-en Systeme NICHT in doku.tex gelistet

2. **comdare-DB Connector erstellt** (8. Connector, vollstaendiges Skeleton)
   - `comdare_connector.hpp` (45 Zeilen): REST API Interface
   - `comdare_connector.cpp` (230 Zeilen): HTTP GET/POST via libcurl, DRY_RUN support
   - `DbSystem::COMDARE_DB` Enum + Parser in config.hpp
   - MetricsTrace: `collect_comdare_db()` Collector + Factory-Case
   - main.cpp: include + switch-case + --help Aktualisierung
   - CMakeLists.txt: comdare_connector.cpp in DEDUP_SOURCES

3. **Missing-DB-Analyse** (aus vorheriger Session, dokumentiert):
   - **Im Cluster vorhanden, in C++ integriert:** PostgreSQL, CockroachDB, Redis, Kafka, MinIO
   - **Connector vorhanden, K8s Install FEHLT:** MariaDB, ClickHouse, comdare-DB
   - **In doku.tex erwähnt, KEIN Connector:** Prometheus (nur Monitoring), Grafana (nur Dashboard)
   - **doku.tex Phase 2 (P2):** DuckDB, Cassandra, MongoDB (noch ohne Connector)

### comdare-DB API Annahmen (Black Box)

Das REST-API-Design ist vorläufig und wird angepasst sobald comdare-DB deployed ist:

```
POST   /api/v1/databases/{name}           -- Create database
DELETE /api/v1/databases/{name}           -- Drop database
POST   /api/v1/databases/{name}/ingest    -- Binary payload ingest
DELETE /api/v1/databases/{name}/objects   -- Delete all objects
POST   /api/v1/databases/{name}/maintain  -- Compaction/GC trigger
GET    /api/v1/databases/{name}/stats     -- {"logical_size_bytes": N, "object_count": N}
GET    /api/v1/health                     -- Health check
```

### MetricsTrace Collector fuer comdare-DB

Der 100ms-Collector pollt `/api/v1/databases/{db}/stats` und extrahiert:
- `logical_size_bytes` (Bytes)
- `object_count` (Anzahl)
- `compaction_pending` (Anzahl ausstehende Compaction-Jobs)

### Dateien geaendert/erstellt in Session 9d

| Datei | Aktion | Zeilen |
|-------|--------|--------|
| `connectors/comdare_connector.hpp` | NEU | ~45 |
| `connectors/comdare_connector.cpp` | NEU | ~230 |
| `config.hpp` | Erweitert | +4 (COMDARE_DB enum+str+parser) |
| `experiment/metrics_trace.hpp` | Erweitert | +1 (collect_comdare_db decl) |
| `experiment/metrics_trace.cpp` | Erweitert | +25 (collector + factory case) |
| `main.cpp` | Erweitert | +5 (include, switch, help, comment) |
| `CMakeLists.txt` | Erweitert | +1 |
| `docs/doku.tex` | Erweitert | +1 (Redis in Section 5.2) |
| Session-Doku | Erweitert | +120 |

### Connector-Inventar nach Session 9d (8 von 8 komplett)

| # | System | Header | .cpp | MetricsCollector | DRY_RUN | K8s deployed |
|---|--------|--------|------|------------------|---------|--------------|
| 1 | PostgreSQL | postgres_connector.hpp | ✅ | ✅ collect_postgresql | ✅ | ✅ 4/4 |
| 2 | CockroachDB | postgres_connector.hpp | ✅ | ✅ collect_cockroachdb | ✅ | ✅ 4/4 |
| 3 | Redis | redis_connector.hpp | ✅ | ✅ collect_redis | ✅ | ✅ 4+4/4 |
| 4 | Kafka | kafka_connector.hpp | ✅ | ✅ collect_kafka | ✅ | ✅ 7/7 |
| 5 | MinIO | minio_connector.hpp | ✅ | ✅ collect_minio | ✅ | ✅ 4/4 |
| 6 | MariaDB | mariadb_connector.hpp | ✅ | ✅ collect_mariadb | ✅ | ❌ TODO |
| 7 | ClickHouse | clickhouse_connector.hpp | ✅ | ✅ collect_clickhouse | ✅ | ❌ TODO |
| 8 | **comdare-DB** | **comdare_connector.hpp** | **✅** | **✅ collect_comdare_db** | **✅** | **❌ TODO** |

### Verbleibende Aufgaben nach Session 9d

#### DONE (Session 9 + 9c + 9d)
- [x] CI Triple Pipeline (7 Jobs, 6 Stages)
- [x] --cleanup-only Modus
- [x] PVC-Namen korrigiert (4 Fehler)
- [x] MetricsTrace Header + Implementation (8 Collectors + Kafka Producer)
- [x] ResultsExporter (Kafka→CSV→git push)
- [x] main.cpp Integration (register/start/stop + export-before-cleanup)
- [x] MinIO Physical Size via Prometheus Endpoint
- [x] doku.tex Naming (Redcomponent-DB → comdare-DB)
- [x] Redis in doku.tex Section 5.2 aufgenommen
- [x] comdare-DB Connector Skeleton (vollstaendig)

#### OFFEN (naechste Session)

| Prioritaet | Typ | Aufgabe | Abhaengigkeit |
|-----------|-----|---------|---------------|
| P0 | INFRA | K8s Runner neustart | kubectl rollout restart |
| P0 | INFRA | Prometheus + Grafana deployen | kube-prometheus-stack |
| P0 | INFRA | MariaDB in Cluster deployen | StatefulSet, 4 Replicas |
| P0 | INFRA | ClickHouse in Cluster deployen | StatefulSet, 4 Replicas |
| P0 | INFRA | comdare-DB in Cluster deployen | StatefulSet, Longhorn PVC |
| P1 | CODE | comdare-DB API Endpunkte alignen | comdare-DB muss deployed sein |
| P2 | CODE | Grafana Dashboard JSON Template | Prometheus muss deployed sein |
| P2 | DOC | doku.tex Phase 2 (DuckDB, Cassandra, MongoDB) | Kein Blocker |

---

## 12. Git History

```
34ddc2d Session 9c docs: elaborate with all implementation details + status
0c5edf3 Fix naming + MinIO physical size: comdare-DB rename, S3 metrics endpoint
a041c82 Complete MetricsTrace + ResultsExporter: 100ms sampling, Kafka dual output, CSV export
70c113f WIP: MetricsTrace architecture + config fixes for 100ms Kafka sampling
5d23bbb Session 9 docs + fix PVC names from cluster scan + metrics trace config
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

---

## Session-Historie (Kurzreferenz)

| Session | Datum | Schwerpunkt | Commits |
|---------|-------|-------------|---------|
| 1-2 | 2026-02-16 | Initiales Setup, LaTeX Import, Kartografie | fa9d890, 2cac967 |
| 3 | 2026-02-16 | C++ Framework-Grundstruktur, CI Pipeline v1 | 39cb309 |
| 4 | 2026-02-16 | SHA-256, Dataset Generator, S3 Auth, C++ statt Python Entscheidung | 61a71c3 |
| 5 | 2026-02-16 | Redis Cluster Fix, CockroachDB TLS, Lab-Schema Isolation | e29c9d4, c2fc814 |
| 6-7 | 2026-02-16 | Kafka + MinIO Connector, Session-Docs | 38b6824 |
| 8 | 2026-02-17 | Longhorn Metrics, MariaDB/ClickHouse Stubs, EDR + Throughput | b0bb5b6 |
| **9** | **2026-02-17** | **Triple Pipeline, Cleanup-Only, MetricsTrace Arch** | **5ac1732, 5d23bbb, 70c113f** |
| **9c** | **2026-02-17** | **MetricsTrace+Exporter KOMPLETT, MinIO Phys, Naming** | **a041c82, 0c5edf3** |
| **9d** | **2026-02-17** | **comdare-DB Connector, Redis doku.tex, Missing-DB-Analyse** | **(pending commit)** |
