# Session 97b: Experiment PostgreSQL Run + Bug-Analyse
**Datum:** 2026-02-22
**Agent:** Infrastruktur (system32 Kontext)
**Projekt:** dedup-database-analysis (GitLab Project 280)

## Kontext
Fortsetzung von Session 97 (Kontext-Continuation). Pipeline #5345 experiment:postgresql gestartet.

## Chronologie

### 1. Pipeline #5345 Setup
- experiment:build: SUCCESS (439s inkl. Sidekiq-Delay)
- experiment:preflight: SUCCESS (344s inkl. Sidekiq-Delay)
- experiment:postgresql: PENDING → Runner-4 Stall

### 2. Runner-4 Fixes
- `request_concurrency` von 1 auf 4 gepatcht (ConfigMap)
- `kubectl rollout restart deployment gitlab-runner-4`
- experiment:postgresql: RUNNING

### 3. Push-Pipeline #5597 Interference
- Commit `8f6f7a3` (PG bleibt AN aus S97) hat Push-Pipeline erzeugt
- Pipeline #5597 gecancelt
- Pipeline #5445 (alt, canceling) ebenfalls gecancelt

### 4. PostgreSQL Permission Bug (KRITISCH)
**Problem:** `ERROR: permission denied for database postgres` bei `CREATE SCHEMA IF NOT EXISTS dedup_lab`

**Root Cause:** PostgreSQL HA = ZWEI UNABHÄNGIGE PRIMARIES (postgres-ha-0 + postgres-ha-1)
- KEINE Streaming Replication! (`pg_stat_replication` = 0 rows)
- `postgres-lb` Service = Round-Robin Load Balancer ueber BEIDE Pods
- Grants auf ha-0 sind NICHT auf ha-1 sichtbar
- 50% der Connections landen auf dem Pod OHNE Grants

**Fix:** Gleiche Grants auf BEIDEN Primaries ausgefuehrt:
```sql
GRANT CREATE ON DATABASE postgres TO "dedup-lab";
GRANT pg_checkpoint TO "dedup-lab";
ALTER USER "dedup-lab" WITH CREATEDB;
```

**Ergebnis:** Schema-Erstellung funktioniert ab Run 2/3.
Run 1 komplett ungueltig (0 rows ueberall).

### 5. Experiment-Ergebnisse (Run 2+3)
**VALIDE DATEN ab Run 2:**
- Per-file insert: 1000-1500 rows pro Grade
- Bulk insert: Daten werden geschrieben, Row-Count Bug (siehe Bugs)
- EDR-Werte: U50=2081, U90=840, U0=2518 (plausible Werte!)
- Latenz: Insert p50=86ms, Delete p50=8ms
- Longhorn phys_delta gemessen

**Payload-Typen:**
- 5 synthetische FUNKTIONIEREN: random_binary, structured_json, text_document, uuid_keys, jsonb_documents
- 3 Real-World LEER (0 bytes): bank_transactions, text_corpus, numeric_dataset (NFS-Mount fehlgeschlagen)

## Bugs fuer Fix-Durchgang

### Bug 1: Bulk Insert Row Count = 0 (Code)
**Datei:** `src/cpp/connectors/postgres_connector.cpp`, Funktion `bulk_insert()`
**Problem:** COPY-Kommando Row-Count wird nicht aus PGresult extrahiert
**Impact:** rows_affected=0 in allen bulk_insert Results, Daten werden aber geschrieben
**Fix:** `PQcmdTuples(res)` auslesen nach COPY-Ausfuehrung

### Bug 2: NFS-Mount fehlgeschlagen (CI/Runner)
**Problem:** CI-Script versucht `mount -t nfs 10.0.110.184:/ /datasets/real-world` im Pod
**Root Cause:** K8s Pods koennen kein NFS mounten ohne privileged SecurityContext
**Impact:** Checkpoints nur lokal (verloren bei OOM), Real-World Datasets fehlen
**Fix:** NFS Volume Mount in Runner-4 ConfigMap:
```toml
[[runners.kubernetes.volumes.nfs]]
  name = "experiment-nfs"
  mount_path = "/datasets/real-world"
  server = "10.0.110.184"
  path = "/"
  read_only = false
```

### Bug 3: Kafka Metrics Trace Flood (Code/Config)
**Problem:** metrics_trace schickt alle 100ms Samples an Kafka
**Root Cause:** Kafka Topic `dedup-lab-metrics` hat keine ACLs fuer den Client
**Impact:** Tausende "Broker: Topic authorization failed" Errors/Sekunde, Log-Buffer ueberflutet
**Fix:** Entweder:
  a) Kafka Topic ACLs fuer dedup-lab User erstellen, ODER
  b) `metrics_trace.enabled = false` in config.example.json wenn Kafka nicht das Testziel ist, ODER
  c) Backoff-Logik in metrics_trace.cpp bei wiederholten Fehlern

### Bug 4: PostgreSQL Dual-Primary ohne Replikation (Infra)
**Problem:** postgres-ha-0 und postgres-ha-1 sind zwei unabhaengige Primaries
**Root Cause:** Patroni/HA nicht korrekt konfiguriert oder nie aktiviert
**Impact:** Writes gehen zufaellig auf verschiedene Pods, keine Konsistenz
**Fix:** Active/Active mit NGINX + MetalLB (User-Direktive)

### Bug 5: Ergebnis-Upload fehlt (CI)
**Problem:** Experiment-Ergebnisse bleiben im Pod und gehen nach Job-Ende verloren
**Fix:** CI-Script muss Ergebnisse in `measurement_results/` committen und pushen

### Bug 6: Samba AD LDAP Auth (Infra)
**Problem:** Alle DBs nutzen lokale User statt Samba AD LDAP
**User-Direktive:** JEDE DB muss ueber Samba AD authentifiziert werden
**Fix:** pg_hba.conf LDAP, MariaDB PAM, ClickHouse ldap_servers, Redis ACL, Kafka SASL

## Commits (diese Session)
- Runner-4 ConfigMap: request_concurrency 1→4
- PostgreSQL Grants auf BEIDEN Primaries (runtime, kein Commit)

## Pipeline #5345 Status (bei Session-Ende)
- experiment:build: SUCCESS
- experiment:preflight: SUCCESS
- experiment:postgresql: RUNNING (~90min, Run 3/3 laeuft)
- experiment:cockroachdb → minio: CREATED (warten auf PG-Abschluss)

## Naechste Session (AUTONOM)

### Phase 1: Bugs fixen (Code + CI)
1. Bug 1: bulk_insert Row Count Fix in postgres_connector.cpp
2. Bug 2: NFS Volume Mount in Runner-4 ConfigMap
3. Bug 3: Kafka metrics_trace Backoff oder Disable
4. Bug 5: Ergebnis-Upload nach `measurement_results/` mit git commit+push
5. Alle Fixes committen und pushen

### Phase 2: Pipeline neu triggern
1. Pipeline #5345 Ergebnis (experiment:postgresql) auswerten
2. Neue API-Pipeline triggern
3. Kaskade ueberwachen: PG → CRDB → MariaDB → CH → Redis → Kafka → MinIO

### Phase 3: Infrastruktur (spaeter)
1. PostgreSQL Active/Active NGINX+MetalLB
2. Samba AD LDAP Auth fuer alle 7 DBs
3. Streaming Replication einrichten

## DB Scaling Matrix (FINAL, korrigiert S97b)

| DB | Scaling | Typ | Hinweis |
|----|---------|-----|---------|
| PostgreSQL | **BLEIBT AN** | **Produktion** | Dual-Primary, kein Replication! |
| CockroachDB | **BLEIBT AN** | Produktion | Shared mit Produktion |
| MinIO | **BLEIBT AN** | Produktion | LDAP Access Key |
| MariaDB | 0 ↔ 1 | Experiment | |
| ClickHouse | 0 ↔ 1 | Experiment | |
| Redis | 0 ↔ 3 | Experiment | |
| Kafka | broker 0↔1, controller 0↔1 | Experiment | |
