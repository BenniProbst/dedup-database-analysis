# Session 12: Security Audit + Experiment-Redesign nach doku.tex Ueberarbeitung
**Datum:** 2026-02-18
**Agent:** Code-Agent (Implementierung + Security)
**Branch:** development
**Vorheriger Commit:** `46f958d` (Session 10-11 K8s credentials)
**Pipeline:** 1309 (push, SUCCESS), 1310 (API, running)

---

## Zusammenfassung

Dreifach-Session: (1) Pipeline endlich funktional — erste erfolgreiche Pipeline fuer
das Projekt. (2) Vollstaendiger Security-Audit aller Projektdateien fuer bevorstehende
Veroeffentlichung. (3) Umfassende Gap-Analyse zwischen aktueller C++ Implementierung
und dem ueberarbeiteten doku.tex Dokument, das ein signifikant erweitertes Experiment
beschreibt.

---

## 1. Pipeline-Erfolg (Pipelines 1309 + 1310)

### Pipeline 1309 (Push-triggered, Commit 46f958d)
- **latex:compile:** SUCCESS (39s, 26 Seiten, 216KB PDF)
- **Artifacts:** doku.pdf + doku.log hochgeladen
- **Pipeline Status:** SUCCESS (erste funktionierende Pipeline!)
- **Erkenntnis:** Push-Trigger funktioniert jetzt nach Auto DevOps deaktiviert

### Pipeline 1310 (API-triggered, alle Jobs)
- **latex:compile:** SUCCESS (39s, Image gecached)
- **cpp:build:** SUCCESS (271s, gcc:14-bookworm, 509MB Image in 44s gepullt)
- **cpp:smoke-test:** RUNNING (MetricsTrace DRY-Mode laeuft)
- **cpp:full-dry-test:** RUNNING (parallel zum Smoke-Test)
- **experiment:*:** MANUAL (korrekt, nicht auto-ausgeloest)

### Pipeline-Debugging Erkenntnisse (final)
| Problem | Root Cause | Fix |
|---------|-----------|-----|
| Push erstellt keine Pipeline | Auto DevOps aktiv | `auto_devops_enabled=false` |
| Jobs bleiben in `created` | Runner-Tag mismatch | `tag_list=["kubernetes"]` |
| API-Trigger: `changes:` versagt | `before_sha=0000` | Separate rules web/api vs push |
| Stage-Transition haengt | Sidekiq ProcessPipelineWorker | Rails Console `ProcessPipelineService.execute` |
| texlive Image Timeout | 2.6GB Pull > 180s poll_timeout | Image jetzt gecached |

### Sidekiq-Bug (offen)
API-getriggerte Pipelines benoetigen manuelles `ProcessPipelineService.execute` via
Rails Console nach JEDER Stage-Transition. Push-getriggerte Pipelines funktionieren
automatisch. Ursache vermutlich: Sidekiq Queue-Konfiguration oder Pipeline-Worker
nicht fuer alle Projekte registriert. **Workaround:** Push statt API verwenden.

---

## 2. Security-Audit fuer Veroeffentlichung

### 2.1 Audit-Ergebnis (KRITISCH)

**Gesamtfund:** 51+ Instanzen des Cluster-Root-Passworts in 7 Dateien.

| Datei | Instanzen | Schwere | Aktion |
|-------|-----------|---------|--------|
| `k8s/base/secrets.yaml` | 8 | KRITISCH | Alle Passwoerter durch Platzhalter ersetzen |
| `infrastructure/sessions/...session-10...md` | 51 | KRITISCH | Alle Passwoerter redaktieren |
| `infrastructure/security/setup-lab-user.sh` | 1 | KRITISCH | Passwort durch Variable ersetzen |
| `infrastructure/security/db-hardening-checklist.md` | 1 | HOCH | Passwort redaktieren |
| `sessions/20260216-session-dedup-projekt-setup.md` | 3+1 Token | KRITISCH | Passwort + GitLab Token |
| `credentials.env` | 4 | KRITISCH | Datei loeschen (gitignored, aber existiert) |

### 2.2 Gefundene Credential-Typen

1. **Cluster-Root-Passwort** `[CLUSTER-PW-REDACTED]` — 51+ Instanzen (MUSS WEG)
2. **GitLab Personal Access Token** `glpat-3z61s--...` — 1 Instanz in Session-Setup-Doku
3. **MinIO LDAP Access Key** `dedup-lab-s3` / `dedup-lab-s3-secret` — Mehrfach (ERLAUBT, Lab-User)
4. **Samba AD Admin-Passwort** — In Session-10-Doku (gleich wie Root, MUSS WEG)

### 2.3 Erlaubte Credentials (duerfen veroeffentlicht werden)

- Lab-Benutzername: `dedup-lab`, `dedup_lab`
- Lab-LDAP-UPN: `dedup-lab@comdare.de`
- Service-Hostnamen/Ports (K8s-intern, nicht erreichbar)
- Schema-Namen (`dedup_lab`, `dedup-lab`)
- MinIO LDAP Access Key (Lab-Scope)
- K8s Namespace-Referenzen

### 2.4 Neues Lab-Passwort

**Generiert:** `[LAB-PW-REDACTED]` (20 Zeichen, kryptographisch sicher)

**Verwendung:** Dieses Passwort ersetzt `[CLUSTER-PW-REDACTED]` fuer ALLE Lab-User-Verbindungen:
- PostgreSQL: `dedup-lab` / `[LAB-PW-REDACTED]`
- CockroachDB: `dedup_lab` / `[LAB-PW-REDACTED]`
- Redis: `dedup-lab` / `[LAB-PW-REDACTED]`
- Kafka SCRAM: `dedup-lab` / `[LAB-PW-REDACTED]`
- MinIO LDAP: Access Key `dedup-lab-s3` / Secret `dedup-lab-s3-secret` (unveraendert)

**Dokumentation:** `Projekte/Cluster/keys/database-credentials.md` (NUR dort, NICHT im Projekt)

### 2.5 Redaktions-Plan

```
Phase 1: Neues Passwort generieren ✅ ([LAB-PW-REDACTED])
Phase 2: k8s/base/secrets.yaml — Passwoerter durch CI-Variablen ersetzen
Phase 3: Sessions + Infra-Docs — [CLUSTER-PW-REDACTED] durch [REDACTED] ersetzen
Phase 4: GitLab Token aus Session-Setup entfernen
Phase 5: credentials.env loeschen (sollte gitignored sein)
Phase 6: .gitlab-ci.yml — CI-Variablen auf neues Passwort umstellen
Phase 7: Cluster-seitig: Lab-User Passwoerter auf allen 5 DBs aendern
Phase 8: git filter-branch oder BFG Repo Cleaner fuer Git-History
```

---

## 3. Gap-Analyse: Aktuelle Implementierung vs. doku.tex Spezifikation

### 3.1 Experiment-Dimensionen (GROSSE ABWEICHUNGEN)

```
                          AKTUELL                    doku.tex SPEC
                          ───────                    ─────────────
Dup-Ratios:               3 (U0/U50/U90)        →   5 (0/50/90/95/99%)
Placement:                keine                  →   2 (within/across)
Dataset-Groessen:         500 Files fix           →   3 (1/5/10 GB)
Cache-Zustand:            keine                  →   2 (cold/warm)
Wiederholungen:           1                      →   3
Stages:                   3 (bulk/perfile/del)   →   4 (bulk/incremental/delete-var/refill)
Delete-Varianten:         1 (DELETE)             →   3 (logical/truncate/drop+recreate)
DB-Varianten:             keine                  →   pro System konfigurierbar
Datentypen:               UUID nur               →   Mixed (structured/JSON/text/media)
Metriken:                 EDR, throughput, lat    →   EDR_cluster, EDR_replica, throughput,
                                                      latency (p50/p90/p95/p99), size-over-time
CSV-Schema:               minimal                →   Vollstaendiges Schema mit run_id
Repetitions:              1                      →   3 (raw + averaged curve)
```

### 3.2 Neue Run-Matrix (pro System)

```
Runs = |sizes| × |dup_ratios| × |placement| × |cache| × |repetitions| × |variants|
     = 3 × 5 × 2 × 2 × 3 × V
     = 180 × V (base, ohne DB-Varianten)
```

Fuer PostgreSQL (V=2: TOAST EXTENDED/EXTERNAL):
  → 360 Runs

Fuer ClickHouse (V=2: MergeTree/ReplacingMergeTree):
  → 360 Runs

**Gesamt (7 Systeme, geschaetzt 1400+ Runs)**

### 3.3 Fehlende Implementierung (nach Datei)

#### config.hpp — Erweiterungen noetig
```
- DupGrade: {U0, U50, U90} → {D0, D50, D90, D95, D99}
- Placement: NEUES Enum {WITHIN, ACROSS}
- CacheState: NEUES Enum {COLD, WARM}
- DatasetSize: NEUES Enum/Config {1GB, 5GB, 10GB}
- DeleteVariant: NEUES Enum {LOGICAL, TRUNCATE, DROP_RECREATE}
- DbVariant: NEUES Struct pro System (z.B. PostgresTOASTMode, ClickHouseEngine)
- RunId: NEUES Struct (kodiert alle Dimensionen)
- Stage: BULK_INSERT, INCREMENTAL_INSERT, DELETE_VARIANTS, REFILL
- ExperimentConfig: repetitions=3, dataset_sizes, placements, cache_states
```

#### dataset_generator.cpp — Kompletter Rewrite
```
AKTUELL: Generiert UUID-Dateien mit xoshiro256** PRNG
NEU:     Mixed Datentypen:
  - Structured: Time-series, numeric payloads, transaction-like rows
  - Semi-structured: GitHub Event Logs (JSON, GH Archive 2026)
  - Unstructured: Project Gutenberg Text, NASA Images, Blender Videos
  - Groesse: 1/5/10 GB Zielvolumen
  - Placement: within (alles in einem Container) vs across (ueber mehrere)
  - Dup-Ratios: 0/50/90/95/99% byte-identische Duplikate
```

#### data_loader.cpp — Stage-Erweiterung
```
AKTUELL: bulk_insert → perfile_insert → perfile_delete → maintenance
NEU:     4 Stages mit Varianten:
  Stage 1: Bulk Ingest (COPY/batch insert, Throughput + Latenz)
  Stage 2: Incremental Ingest (Einzelinserts, Compaction sichtbar)
  Stage 3: Delete Variants (3 Typen: logical/truncate/drop+recreate)
  Stage 4: Refill After Delete (gleicher Datensatz, Reclamation-Test)
```

#### Neue Dateien noetig
```
experiment/run_matrix.hpp       — Kombinatorische Versuchsplanung
experiment/cache_controller.hpp — Cold/Warm Cache Management (Pod Restart)
experiment/variant_config.hpp   — DB-spezifische Varianten-Konfiguration
experiment/csv_exporter.hpp     — Neues CSV-Schema mit run_id
```

#### Connector-Erweiterungen
```
PostgreSQL:  TOAST-Mode umschalten (EXTENDED/EXTERNAL), VACUUM vs VACUUM FULL
ClickHouse:  ReplacingMergeTree, OPTIMIZE FINAL, SYSTEM DROP CACHE
CockroachDB: gc.ttlseconds, Row-Level TTL
Kafka:       cleanup.policy=compact vs delete, Segment-Parameter
MariaDB:     Page/Table Compression toggles
Redis:       rdbcompression toggle
MinIO:       (keine Varianten, Baseline)
```

### 3.4 Implementierungs-Plan (priorisiert)

```
PHASE A: Fundament (config.hpp Redesign)
  A.1 Neue Enums: DupRatio, Placement, CacheState, DeleteVariant, DataType
  A.2 RunId Struct (kodiert alle Dimensionen)
  A.3 DbVariant System (pro DB konfigurierbar)
  A.4 RunMatrix Generator (kartesisches Produkt aller Dimensionen)
  A.5 CSV-Schema mit run_id, system, variant, data_type, etc.

PHASE B: Dataset-Generator Rewrite
  B.1 DataType-basierte Generierung (structured/semi/unstructured)
  B.2 Placement-Logik (within vs across Container)
  B.3 Dup-Ratio Controller (0/50/90/95/99%)
  B.4 Groessen-Skalierung (1/5/10 GB Zielvolumen)
  B.5 Source-Downloader (GH Archive, Gutenberg, NASA, Blender)

PHASE C: Experiment-Engine Erweiterung
  C.1 4-Stage Engine (bulk/incremental/delete-variants/refill)
  C.2 Delete-Varianten (logical/truncate/drop+recreate)
  C.3 Cache-Controller (cold=Pod-Restart, warm=sofort)
  C.4 Repetition-Loop (3× mit Averaging)
  C.5 DB-Variant-Switcher (TOAST, RMT, TTL, compaction)

PHASE D: Connector-Erweiterungen
  D.1 PostgreSQL: TOAST toggle, VACUUM/VACUUM FULL
  D.2 ClickHouse: ReplacingMergeTree, OPTIMIZE FINAL, Cache Drop
  D.3 CockroachDB: gc.ttlseconds, Row-Level TTL
  D.4 Kafka: cleanup.policy Umschaltung, Segment-Konfiguration
  D.5 MariaDB: Compression toggles
  D.6 Redis: rdbcompression toggle

PHASE E: Metriken + Export
  E.1 EDR_cluster und EDR_replica parallel berechnen
  E.2 Latenz-Percentile: p50/p90/p95/p99
  E.3 Size-over-time 100ms Sampling (Longhorn + MinIO Prometheus)
  E.4 CSV-Export mit vollstaendigem Schema
  E.5 Grafana Dashboard Update
```

---

## 4. Aktuelle C++ Code-Architektur (32 Dateien)

```
src/cpp/
├── CMakeLists.txt              — Build-Config (gcc:14, nlohmann-json, libpq, etc.)
├── config.hpp                  — ExperimentConfig, DupGrade, Stage, DbSystem enums
├── config.example.json         — Beispiel-Konfiguration
├── main.cpp                    — CLI Entry-Point (--systems, --grades, --dry-run)
├── connectors/
│   ├── db_connector.hpp        — Abstract Interface + MeasureResult
│   ├── postgres_connector.*    — libpq, COPY, VACUUM, pg_database_size()
│   ├── redis_connector.*       — hiredis, Key-Prefix dedup:*, SCAN+DEL
│   ├── kafka_connector.*       — librdkafka, Admin API, Producer/Consumer
│   ├── minio_connector.*       — libcurl, AWS SigV4, S3 API
│   ├── mariadb_connector.*     — libmysqlclient, InnoDB OPTIMIZE
│   ├── clickhouse_connector.*  — HTTP API, MergeTree
│   └── comdare_connector.*     — REST API (Stub)
├── experiment/
│   ├── dataset_generator.*     — xoshiro256** PRNG, UUID-basiert
│   ├── data_loader.*           — Experiment-Engine (4 Stages)
│   ├── schema_manager.*        — CREATE/DROP lab schema
│   ├── metrics_collector.*     — Longhorn + MinIO Prometheus queries
│   ├── metrics_trace.*         — 100ms Sampling Thread (7 DB collectors)
│   └── results_exporter.*      — Kafka→CSV + Git commit+push
└── utils/
    ├── logger.hpp              — LOG_INFO/DBG/WARN/ERR Macros
    ├── sha256.hpp              — FIPS 180-4 SHA-256
    └── timer.hpp               — steady_clock ScopedTimer
```

### Code-Metriken (geschaetzt)
- **Aktuell:** ~3.500 LOC (32 Dateien, C++20)
- **Nach Redesign:** ~6.000-8.000 LOC (geschaetzt +3.000 fuer Experiment-Erweiterung)
- **Connector-Erweiterungen:** ~500 LOC (DB-Varianten)
- **Neue Dateien:** ~1.500 LOC (RunMatrix, CacheController, VariantConfig, CSV)

---

## 5. Credential-Rotation Plan

### Phase 1: Im Projekt (SOFORT)

```
1. k8s/base/secrets.yaml
   - ALLE Passwoerter → ${CI_VARIABLE} Platzhalter
   - Kommentar mit [CLUSTER-PW-REDACTED] → entfernen

2. infrastructure/sessions/...session-10...md
   - ALLE 51 Instanzen → [REDACTED]

3. infrastructure/security/setup-lab-user.sh
   - LAB_PASS='[CLUSTER-PW-REDACTED]' → LAB_PASS="${DEDUP_LAB_PASSWORD}"

4. infrastructure/security/db-hardening-checklist.md
   - Passwort in Beispielbefehl → [REDACTED]

5. sessions/20260216-session-dedup-projekt-setup.md
   - 3× Passwort → [REDACTED]
   - 1× GitLab Token → [REDACTED-GITLAB-TOKEN]

6. credentials.env
   - Datei komplett loeschen (gitignored, sollte nicht im Repo sein)
```

### Phase 2: Im Cluster (NACH Projekt-Cleanup)

```
1. PostgreSQL: ALTER USER "dedup-lab" WITH PASSWORD '[LAB-PW-REDACTED]';
2. CockroachDB: ALTER USER dedup_lab WITH PASSWORD '[LAB-PW-REDACTED]';
3. Redis: ACL SETUSER dedup-lab >[LAB-PW-REDACTED]
4. Kafka: KafkaUser CRD update (SCRAM-SHA-512 Passwort)
5. GitLab CI-Variablen: DEDUP_*_PASSWORD auf neues Passwort aendern
6. Dokumentation: Cluster/keys/database-credentials.md aktualisieren
```

### Phase 3: Git-History bereinigen

```
Option A: BFG Repo Cleaner (empfohlen)
  bfg --replace-text passwords.txt dedup-database-analysis.git
  git reflog expire --expire=now --all && git gc --prune=now

Option B: git filter-repo
  git filter-repo --blob-callback '...'

ACHTUNG: Beide erfordern Force-Push, was auf GitLab + GitHub Impact hat.
User-Entscheidung noetig.
```

---

## 6. MinIO Credentials Update

### Aktueller Zustand
- Lokaler MinIO-User `dedup-lab` wurde ENTFERNT (LDAP-Aktivierung)
- Neuer Auth-Mechanismus: LDAP Access Key
  - Access Key: `dedup-lab-s3`
  - Secret Key: `dedup-lab-s3-secret`
  - Policy: `readwrite` (gebunden an Samba AD LDAP DN)

### Code-Aenderungen noetig
```cpp
// config.hpp: MinIO Verbindung
{DbSystem::MINIO, "minio-lb.minio.svc.cluster.local", 9000,
 "dedup-lab-s3",           // LDAP Access Key (war: "dedup-lab")
 "dedup-lab-s3-secret",    // LDAP Secret Key (war: leer)
 "", "dedup-lab",
 "", "minio"},

// minio_connector.cpp: AWS SigV4 mit LDAP Access Key
// Kein Code-Aenderung noetig — SigV4 nutzt bereits user/password als Access/Secret
```

---

## 7. Offene Aufgaben (priorisiert)

### P0: KRITISCH (vor Veroeffentlichung)
| # | Aufgabe | Status | Abhaengigkeit |
|---|---------|--------|---------------|
| 1 | Cluster-Passwort aus allen Dateien entfernen | GEPLANT | — |
| 2 | GitLab Token aus Session-Doku entfernen | GEPLANT | — |
| 3 | credentials.env loeschen | GEPLANT | — |
| 4 | k8s/base/secrets.yaml auf Platzhalter | GEPLANT | — |
| 5 | Neues Lab-Passwort dokumentieren (Cluster/keys) | GEPLANT | #1 |

### P0: KRITISCH (Experiment)
| # | Aufgabe | Status | Abhaengigkeit |
|---|---------|--------|---------------|
| 6 | config.hpp Redesign (neue Enums, RunId) | GEPLANT | — |
| 7 | Dataset-Generator Rewrite (Mixed Types) | GEPLANT | #6 |
| 8 | 4-Stage Experiment Engine | GEPLANT | #6, #7 |
| 9 | Delete-Varianten implementieren | GEPLANT | #8 |
| 10 | CSV-Export mit vollstaendigem Schema | GEPLANT | #6 |

### P1: HOCH
| # | Aufgabe | Status | Abhaengigkeit |
|---|---------|--------|---------------|
| 11 | MinIO LDAP Access Key im Code | GEPLANT | #1 |
| 12 | PostgreSQL TOAST-Mode toggle | GEPLANT | #8 |
| 13 | ClickHouse ReplacingMergeTree | GEPLANT | #8 |
| 14 | CockroachDB TTL-Varianten | GEPLANT | #8 |
| 15 | Kafka Log Compaction toggle | GEPLANT | #8 |
| 16 | Cold/Warm Cache Controller | GEPLANT | #8 |
| 17 | Repetition-Loop (3×) | GEPLANT | #8 |

### P2: MITTEL
| # | Aufgabe | Status | Abhaengigkeit |
|---|---------|--------|---------------|
| 18 | MariaDB deployen (K8s) | INFRA | — |
| 19 | ClickHouse deployen (K8s) | INFRA | — |
| 20 | Prometheus + Grafana deployen | INFRA | — |
| 21 | GH Archive Downloader | GEPLANT | #7 |
| 22 | Gutenberg/NASA/Blender Downloader | GEPLANT | #7 |

### P3: SPAETER
| # | Aufgabe | Status |
|---|---------|--------|
| 23 | Git-History bereinigen (BFG) | USER-ENTSCHEIDUNG |
| 24 | comdare-DB Connector fertigstellen | DEFERRED |
| 25 | NetworkPolicies fuer DB-Namespaces | INFRA |

---

## 8. Pipeline-Status (live)

```
Pipeline 1309 (push):    SUCCESS ✅ (latex:compile 39s)
Pipeline 1310 (API):     RUNNING
  latex:compile:          SUCCESS ✅ (39s)
  docker:build:           MANUAL (skip)
  cpp:build:              SUCCESS ✅ (271s)
  cpp:smoke-test:         RUNNING (MetricsTrace DRY @ 100ms)
  cpp:full-dry-test:      RUNNING
  experiment:*:           MANUAL (nicht ausgeloest)
```

---

## 9. Zusammenfassung der Entscheidungen

1. **Neues Lab-Passwort:** `[LAB-PW-REDACTED]` (dokumentiert in Cluster/keys)
2. **MinIO Auth:** LDAP Access Key `dedup-lab-s3` / `dedup-lab-s3-secret`
3. **Experiment-Redesign:** Vollstaendige Neuimplementierung nach doku.tex Spec
4. **Security-First:** Credentials-Cleanup VOR jeder weiteren Veroeffentlichung
5. **Push statt API:** Fuer Pipeline-Trigger, wegen Sidekiq-Bug

---

## 10. Session-Ende Update (17:15 UTC)

### Erledigte Tasks:
- [x] Task #57: Neues Lab-Passwort generiert (`[LAB-PW-REDACTED]`)
- [x] Task #58: 51+ Passwort-Instanzen + 1 GitLab-Token redaktiert (Commit `09b6feb`)
- [x] Task #60: MinIO LDAP Access Key (`dedup-lab-s3`) in config.hpp gesetzt
- [x] Task #61: Pipeline 1310 cpp:build SUCCESS (271s), smoke-tests laufen

### Pipeline-Status (final):
```
Pipeline 1309 (push, 46f958d):  SUCCESS (latex:compile 39s)
Pipeline 1310 (API, 46f958d):   RUNNING
  latex:compile:     SUCCESS (39s)
  cpp:build:         SUCCESS (271s)
  cpp:smoke-test:    RUNNING (~34min, MetricsTrace DRY 100ms Loop)
  cpp:full-dry-test: RUNNING (~34min, parallel)
Pipeline 1311:                  Push von 09b6feb erwartet (auto-trigger)
```

### Commits:
```
09b6feb Security audit: redact all cluster credentials for publication
46f958d Session 10-11: K8s credentials, security hardening, LaTeX docs
cd55cba Fix CI rules: separate push from web/api
a3e9f54 Fix pipeline triggers: disable Auto DevOps
```

### Naechste Aktion: Experiment-Redesign (Task #59)
User-Anweisung: "beginne direkt mit dem redesign, die Infrastruktur steht
und du kannst dort dry run die Verbindungen testen"

Redesign-Reihenfolge:
1. config.hpp komplett neu (5 DupRatios, Placement, CacheState, DeleteVariant, RunId)
2. dataset_generator.cpp Rewrite (Mixed Types, 1/5/10GB, Within/Across)
3. data_loader.cpp 4-Stage Engine (bulk/incremental/delete-variants/refill)
4. Connector-Erweiterungen (TOAST, RMT, TTL, compaction)
5. CSV-Export mit vollstaendigem Schema

---

## CONTEXT RECOVERY (fuer naechste Session)

### Sofort weitermachen mit:
1. **config.hpp Redesign** — Task #59 in Arbeit
   - Neue Enums: DupRatio{D0,D50,D90,D95,D99}, Placement{WITHIN,ACROSS}
   - CacheState{COLD,WARM}, DeleteVariant{LOGICAL,TRUNCATE,DROP_RECREATE}
   - RunId Struct, RunMatrix Generator, DB-Varianten
2. **Smoke-Test Ergebnis** pruefen (Pipeline 1310, Job 1210/1211)
3. **Pipeline 1311** pruefen (auto-triggered von Security-Commit 09b6feb)

### Befehle:
```bash
# Projekt-Verzeichnis
cd "C:\Users\benja\OneDrive\Desktop\Projekte\Research\dedup-database-analysis"

# Pipeline pruefen
curl -sk --header "PRIVATE-TOKEN: glpat-..." \
  "https://gitlab.comdare.de/api/v4/projects/280/pipelines?per_page=5"

# Git Status
git log --oneline -5

# Dry-Run Verbindungstest (Infrastruktur steht!)
ssh -i ~/.ssh/pve1 root@192.168.178.44 "kubectl get pods -n databases --no-headers"
```

### Dateipfade:
| Was | Pfad |
|-----|------|
| Session 12 | `sessions/20260218-session-12-security-audit-experiment-redesign.md` |
| Lab-Passwort | `[LAB-PW-REDACTED]` (Cluster/keys/database-credentials.md) |
| C++ Source | `src/cpp/` (32 Dateien, ~3.500 LOC) |
| doku.tex (Spec) | `docs/20260218 doku.tex` (AUTHORITATIVE) |
| CI Pipeline | `.gitlab-ci.yml` |
| Security-Commit | `09b6feb` (alle Credentials redaktiert) |

### Kritische Architektur-Infos fuer Redesign:
- **CockroachDB User:** `dedup_lab` (UNDERSCORE, kein Dash!)
- **Redis Key-Prefix:** ALLE Keys MUESSEN mit `dedup:` beginnen
- **Kafka Auth:** SCRAM-SHA-512 auf Port 9094, Plain auf 9092
- **MinIO:** LDAP Access Key `dedup-lab-s3`, NICHT lokaler User
- **Longhorn Replicas:** 4 (EDR_replica = physical/4)
- **MinIO = Direct Disk:** Kein Longhorn PVC, Prometheus-Endpoint fuer Size

---

# ===================================================================
# FORTSETZUNG: Session 12, Kontext 2 (nach Context-Compaction)
# Datum: 2026-02-18 ~19:00-19:35 UTC
# ===================================================================

## 11. doku.tex Infrastruktur-Alignment (Commit `6915178`)

Fuenf gezielte Aenderungen an doku.tex, um das Forschungsdokument mit der
tatsaechlichen K8s-Cluster-Infrastruktur in Einklang zu bringen:

### 11.1 PVC-Groesse korrigiert (Zeile 182)
- **Vorher:** "PVC of 100 GiB"
- **Nachher:** "PVC of 50 GiB" + MinIO-Ausnahme (Direct Disk, kein Longhorn PVC)

### 11.2 Longhorn Replica vs. DB Replica: Bruecke geschlagen (Zeile 184)
Neuer Absatz erklaert die Beziehung zwischen Longhorn-Storage-Replikation (N=4,
Block-Level, auf allen 4 Nodes synchron) und anwendungsseitiger DB-Replikation.
Kernaussage: Longhorn entkoppelt Storage-Replikation von der DB-eigenen Replikation
(PG Streaming, CRDB Raft, Kafka ISR). Jede DB sieht ein logisches Volume, Longhorn
verwaltet transparent 4 physische Kopien.

### 11.3 comdare-DB Beschreibung aktualisiert
- **Vorher:** "black box"
- **Nachher:** "REST API endpoint + empirical comparison"

### 11.4 CockroachDB 125 GiB PVC dokumentiert
CockroachDB teilt seinen 125 GiB PVC mit Produktionsdaten. Experimente sind auf
50 GiB begrenzt (`max_experiment_bytes = 53687091200`). Produktionsdaten werden
NIEMALS beruehrt.

### 11.5 Neuer Paragraph: Database-internal Instrumentation (doku.tex §6.5)
~30 Zeilen LaTeX mit per-DB internen Mess-Tools:

| System | Interne Messung |
|--------|----------------|
| PostgreSQL | `pg_total_relation_size()`, `pg_statio_all_tables` (TOAST), `pg_stat_statements` |
| CockroachDB | `crdb_internal.kv_store_status`, `SHOW RANGES span_stats` |
| Redis | `MEMORY STATS` Dekomposition, `LATENCY` Subsystem |
| Kafka | JMX `RequestMetrics` 5-Komponenten-Timing |
| MinIO | `minio_s3_ttfb_seconds_distribution` |
| MariaDB | `INNODB_TABLESPACES FILE_SIZE/ALLOCATED_SIZE`, `performance_schema` |
| ClickHouse | `system.columns` Kompressionsverhaeltnis, `system.query_log ProfileEvents` |

---

## 12. config.hpp + Datagen Komplett-Redesign (Commit `a67d234`)

### 12.1 config.hpp (268 → 477 LOC, +209 LOC)

**PayloadType Enum** (von dataset_generator.hpp hierher verschoben):
```
RANDOM_BINARY, STRUCTURED_JSON, TEXT_DOCUMENT, UUID_KEYS, JSONB_DOCUMENTS,
NASA_IMAGE, BLENDER_VIDEO, GUTENBERG_TEXT, GITHUB_EVENTS, MIXED
```
5 synthetische + 4 realweltliche + 1 Composite = 10 Typen.

**Neue Structs:**
- `DataSourceConfig` — URLs fuer NASA, Blender, Gutenberg, GH Archive + Cache-Dir
- `DbConnection.max_experiment_bytes` — Per-DB Experimentlimit (CRDB: 50 GiB)
- `ExperimentConfig.payload_types` — Konfigurierbare Payload-Typ-Liste
- `ExperimentConfig.data_sources` — Realweltdaten-Quellen
- `ExperimentConfig.db_internal_metrics` — Toggle fuer doku.tex §6.5

**Hilfsfunktionen:**
- `payload_type_str()`, `parse_payload_type()`, `is_real_world_payload()`, `dup_grade_ratio()`

### 12.2 dataset_generator.hpp (76 → 76 LOC, umstrukturiert)
- PayloadType Enum ENTFERNT (jetzt in config.hpp)
- `DatasetConfig` erhaelt `DataSourceConfig data_sources`
- Neue Methoden: `generate_uuid_keys()`, `generate_jsonb_document()`
- Neue Methoden: `load_cached_or_download()`, `load_nasa_image()`,
  `load_blender_video()`, `load_gutenberg_text()`, `load_github_events()`

### 12.3 dataset_generator.cpp (+161 LOC)
- UUID v4 Generator (128-bit Random mit Version/Variant Bits)
- Nested JSONB Document Generator (verschachtelte JSON-Objekte mit Arrays)
- `load_cached_or_download()`: Generischer Download+Cache via `curl` Systemprozess
- 4 realweltliche Loader (NASA, Blender, Gutenberg, GH Archive)
- Erweiterter `generate_payload()` Switch mit allen 10 PayloadType-Werten

### 12.4 data_loader.cpp (+12 LOC)
- `max_experiment_bytes` Guard am Anfang von `run_stage()`:
  Prueft aktuelle DB-Groesse gegen Limit, ueberspringt Stage bei Ueberschreitung

### 12.5 main.cpp (+26 LOC)
- Neues `--payload-types` CLI-Flag mit Komma-Parsing via `std::istringstream`
- `DatasetConfig.data_sources` wird aus `ExperimentConfig` befuellt

### 12.6 config.example.json (komplett neugeschrieben)
Alle neuen Felder dokumentiert: `max_experiment_bytes`, `payload_types`,
`data_sources` mit URLs, `db_internal_metrics`, `prometheus`, `grafana`,
`metrics_trace`, `git_export`.

---

## 13. DB-Internal Instrumentation Collectors (Commit `d23841f`, +703 LOC)

### 13.1 Neue Dateien: db_internal_metrics.hpp/cpp (44 + 593 = 637 LOC)

Implementiert Stage-Boundary-Snapshots fuer alle 7 Datenbanksysteme.
Diese werden ZWEIMAL pro Stage aufgerufen (vor + nach), ergaenzend
zur Longhorn-physischen Groessenmessung.

#### PostgreSQL (`snapshot_postgresql`, ~60 LOC)
1. `pg_total_relation_size()` fuer Lab-Schema (inkl. Indexes + TOAST)
2. `pg_statio_all_tables` Aggregate (heap_blks_read/hit, toast_blks_read/hit)
3. `pg_stat_statements` Top-5 Queries nach total_exec_time (erfordert Extension)

#### CockroachDB (`snapshot_cockroachdb`, ~55 LOC)
1. `pg_database_size('dedup_lab')` (via PG Wire Protokoll)
2. `crdb_internal.kv_store_status` Aggregate (live/key/val/intent/sys bytes)
3. `SHOW RANGES FROM DATABASE dedup_lab` (range_count, range_size_mb)

#### Redis (`snapshot_redis`, ~40 LOC, HAS_HIREDIS Guard)
1. `DBSIZE` (Gesamtanzahl Keys)
2. `MEMORY STATS` Dekomposition: peak/total/startup allocated, dataset.bytes,
   overhead.total, keys.count, fragmentation.bytes, Ratio-Felder

#### Kafka (`snapshot_kafka`, ~50 LOC)
JMX Prometheus Exporter (Port 9404):
1. 5-Komponenten-Timing fuer Produce-Requests (p50):
   TotalTimeMs, RequestQueueTimeMs, LocalTimeMs, RemoteTimeMs, ResponseSendTimeMs
2. Fetch-Request TotalTimeMs (p50)
3. Gesamt Log-Size und Messages-In Total

#### MinIO (`snapshot_minio`, ~35 LOC)
Prometheus Cluster-Endpoint `/minio/v2/metrics/cluster`:
1. Bucket-Gesamt-Bytes und Objekt-Anzahl
2. S3 TTFB Mean (sum/count aus Distribution)
3. RX/TX Byte-Zaehler

#### MariaDB (`snapshot_mariadb`, ~55 LOC, HAS_MYSQL Guard)
1. `information_schema.INNODB_TABLESPACES` — FILE_SIZE, ALLOCATED_SIZE fuer Lab-DB
2. `SHOW TABLE STATUS` — Per-Table Breakdown (rows, data_length, index_length, data_free)
3. `performance_schema.events_waits_summary_global_by_event_name` — Top-5 InnoDB IO Waits

#### ClickHouse (`snapshot_clickhouse`, ~50 LOC)
HTTP API POST (`FORMAT JSONCompact`):
1. `system.columns` — Compressed vs Uncompressed pro Table + Compression Ratio
2. `system.parts` — Active Parts Count, Total Rows, Bytes-on-Disk pro Table

### 13.2 data_loader.hpp Erweiterungen
- `ExperimentResult` erhaelt `db_internal_before` und `db_internal_after` (JSON)
- `DataLoader` erhaelt `db_internal_metrics_` Flag im Konstruktor

### 13.3 data_loader.cpp Integration
- Snapshot BEFORE: Nach Longhorn-Volume-Aufloesung, vor logischer Groessenmessung
- Snapshot AFTER: Nach Longhorn-AFTER-Messung, vor EDR-Berechnung
- `to_json()` serialisiert beide Snapshots ins Ergebnis
- Direkte `<algorithm>` und `<numeric>` Includes hinzugefuegt (nicht auf transitive abhaengig)

### 13.4 metrics_trace.cpp Erweiterungen (100ms-Collector, +19 LOC)
Leichtgewichtige Zusatzmetriken aus bestehenden Datenquellen:
- **Redis:** `used_memory_dataset`, `used_memory_overhead`, `mem_fragmentation_ratio`
- **MariaDB:** `Innodb_data_read`, `Innodb_rows_inserted`, `Innodb_rows_deleted`
- **MinIO:** `minio_s3_ttfb_seconds_distribution_sum/count`

### 13.5 main.cpp Bugfix
**KOMPILIERUNGS-BUG BEHOBEN:** `ExperimentConfig cfg` wurde VOR Deklaration in
`generate_data` Block verwendet (Zeilen 169, 173-174 referierten `cfg.data_sources`
und `cfg.payload_types`, aber `cfg` wurde erst auf Zeile 186 deklariert).
Fix: Konfigurationsladung vor Dataset-Generierung verschoben.

### 13.6 CMakeLists.txt
`experiment/db_internal_metrics.cpp` zur DEDUP_SOURCES Liste hinzugefuegt.

---

## 14. K8s ConfigMap Update (Commit `054ab0a`)

K8s ConfigMap (`k8s/base/configmap.yaml`) an redesigntes Config-Schema angepasst:
- `db_internal_metrics: true` hinzugefuegt
- `max_experiment_bytes` pro DB (CRDB: 53687091200 = 50 GiB)
- `payload_types` Array (5 synthetische Typen)
- `data_sources` Sektion mit allen realweltlichen URLs (NASA, Blender, Gutenberg, GH Archive)
- `_comment` Felder fuer Deploy-Status (MariaDB/ClickHouse: TODO)
- ClickHouse user/database Felder ergaenzt

---

## 15. Code-Metriken (Session 12 komplett)

### Commits dieser Session (chronologisch)
```
c072e72  Session 10-11: K8s credentials, security hardening, LaTeX docs
ff89071  Security audit: redact all cluster credentials for publication
6915178  docs(doku.tex): align Chapter 6 with actual K8s testbed infrastructure
a67d234  refactor(config+datagen): redesign experiment framework for doku.tex §6.3
916b6b7  security: remove credentials from tracked files, add .credentials.env template
41666ba  docs: restore session files with credentials redacted
d23841f  feat(instrumentation): add DB-internal metrics snapshots (doku.tex §6.5)
054ab0a  chore(k8s): update experiment ConfigMap with redesigned config fields
```

### Geaenderte Dateien (Session 12 Fortsetzung, ab ff89071)
```
18 Dateien geaendert, +1.418 / -116 Zeilen

Neue Dateien:
  db_internal_metrics.hpp     44 LOC
  db_internal_metrics.cpp    593 LOC
  credentials.env.example     24 LOC
  README.md                   35 LOC

Hauptaenderungen:
  config.hpp                +279 LOC (Redesign)
  dataset_generator.cpp     +161 LOC (Neue Generatoren)
  config.example.json        +73 LOC (Neugeschrieben)
  main.cpp                   +52 LOC (CLI Flags + Bugfix)
  configmap.yaml             +44 LOC (K8s Config)
  data_loader.cpp            +35 LOC (Snapshots + Guard)
  doku.tex                   +24 LOC (Infrastruktur-Alignment)
  metrics_trace.cpp          +19 LOC (Extra Collectors)
  data_loader.hpp            +12 LOC (DB-Internal Fields)
  dataset_generator.hpp       (Umstrukturiert)
  CMakeLists.txt              +1 LOC
```

### Aktuelle Code-Architektur (34 Dateien, ~5.200 LOC)
```
src/cpp/
├── CMakeLists.txt              — gcc:14, C++20, libpq/curl/hiredis/rdkafka/mysqlclient
├── config.hpp (477)            — REDESIGNED: 10 PayloadTypes, DataSourceConfig, DB limits
├── config.example.json (159)   — Vollstaendige Referenz-Konfiguration
├── main.cpp (~415)             — CLI: --payload-types, --config, --generate-data, etc.
├── connectors/
│   ├── db_connector.hpp (55)   — Abstract Interface + MeasureResult
│   ├── postgres_connector.*    — libpq, COPY, VACUUM, pg_database_size()
│   ├── redis_connector.*       — hiredis, Key-Prefix dedup:*, SCAN+DEL
│   ├── kafka_connector.*       — librdkafka, Admin API, Producer/Consumer
│   ├── minio_connector.*       — libcurl, AWS SigV4, S3 API
│   ├── mariadb_connector.*     — libmysqlclient, InnoDB OPTIMIZE
│   ├── clickhouse_connector.*  — HTTP API, MergeTree
│   └── comdare_connector.*     — REST API (conditional, -DENABLE_COMDARE_DB=ON)
├── experiment/
│   ├── dataset_generator.* (~400)    — 10 Payload-Generatoren + Download+Cache
│   ├── data_loader.* (~370)          — 4-Stage Engine + DB-Internal Snapshots
│   ├── db_internal_metrics.* (637)   — NEW: 7 DB-Snapshot-Funktionen
│   ├── schema_manager.*              — CREATE/DROP lab schema
│   ├── metrics_collector.*           — Longhorn + MinIO Prometheus queries
│   ├── metrics_trace.* (~690)        — 100ms Sampling Thread (7+3 Collectors)
│   └── results_exporter.*            — Kafka→CSV + Git commit+push
└── utils/
    ├── logger.hpp, sha256.hpp, timer.hpp
```

---

## 16. Task-Status (Session 12 Ende)

### Erledigte Tasks
| # | Task | Commit |
|---|------|--------|
| 57 | Lab-Passwort generiert | — (Cluster/keys) |
| 58 | 51+ Credentials redaktiert | `ff89071` |
| 59 | Experiment-Programm implementiert (Kern) | `a67d234`, `d23841f` |
| 60 | MinIO LDAP Credentials | `a67d234` |
| 61 | Pipeline 1310 Smoke-Test | `c072e72` |
| 65 | DB-Internal Instrumentation Collectors | `d23841f` |
| 66 | K8s Manifeste vorbereitet | `054ab0a` |
| 67 | DB-Internal in DataLoader integriert | `d23841f` |
| 68 | Kompilierung verifiziert + Commit | `d23841f` |

### Offene Tasks
| # | Task | Status | Naechster Schritt |
|---|------|--------|-------------------|
| 62 | MariaDB deployen (K8s) | BEREIT | `kubectl apply -f k8s/mariadb/` |
| 63 | ClickHouse deployen (K8s) | BEREIT | `kubectl apply -f k8s/clickhouse/` |
| 64 | Prometheus+Grafana+Pushgateway | BEREIT | `helm install -f k8s/monitoring/values.yaml` |

### Hinweis zu Tasks 62-64 (Infrastruktur-Deploy):
- Manifeste sind KOMPLETT und BEREIT zum Apply
- MariaDB: StatefulSet + ConfigMap + Services (mariadb:11.7, 50Gi Longhorn)
- ClickHouse: StatefulSet + ConfigMap + Services (clickhouse/clickhouse-server:24.12, 50Gi Longhorn)
- Prometheus: Helm values mit Longhorn/Kafka/ClickHouse/MinIO Scrape-Configs
- **WICHTIG:** Cluster wird aktuell von anderem Agent aktualisiert — temporaere Ausfaelle moeglich
- **METHODIK:** Erst Cluster-Update abwarten, dann deployen

---

## 17. CONTEXT RECOVERY (fuer naechste Session)

### Sofort weitermachen mit:
1. **Infrastruktur-Deploy** (Tasks #62-64) — Manifeste liegen bereit:
   ```bash
   # Warten bis Cluster stabil, dann:
   kubectl apply -f k8s/base/namespace.yaml
   kubectl apply -f k8s/base/rbac.yaml
   kubectl apply -f k8s/base/secrets.yaml  # CI-Variablen setzen!
   kubectl apply -f k8s/mariadb/statefulset.yaml
   kubectl apply -f k8s/clickhouse/statefulset.yaml
   helm install kube-prometheus-stack prometheus-community/kube-prometheus-stack \
     -n monitoring --create-namespace -f k8s/monitoring/values.yaml
   ```

2. **Verbindungstests** — Dry-Run mit MariaDB + ClickHouse:
   ```bash
   dedup-test --config config.json --systems mariadb,clickhouse --dry-run
   ```

3. **Erweiterte Experiment-Dimensionen** (Phase B des Redesigns):
   - 5 Dup-Ratios (D0/D50/D90/D95/D99) statt 3
   - Within/Across Placement
   - Cold/Warm Cache
   - Delete-Varianten (logical/truncate/drop+recreate)
   - 3× Wiederholungen mit Averaging
   Diese Dimensionen sind im doku.tex spezifiziert aber noch NICHT implementiert.

### Git-Status:
```
Branch:       development
Letzter Commit: 054ab0a (chore(k8s): update experiment ConfigMap)
Remote:       GitLab ✅ + GitHub ✅ (beide synchron)
Pipeline:     Erwartet nach Push (auto-trigger)
```

### Kritische Dateien:
| Was | Pfad | LOC |
|-----|------|-----|
| Hauptkonfiguration | `src/cpp/config.hpp` | 477 |
| DB-Internal Snapshots | `src/cpp/experiment/db_internal_metrics.cpp` | 593 |
| 100ms Collectors | `src/cpp/experiment/metrics_trace.cpp` | ~690 |
| Experiment-Engine | `src/cpp/experiment/data_loader.cpp` | ~370 |
| Dataset-Generator | `src/cpp/experiment/dataset_generator.cpp` | ~400 |
| K8s ConfigMap | `k8s/base/configmap.yaml` | ~115 |
| MariaDB Manifest | `k8s/mariadb/statefulset.yaml` | 176 |
| ClickHouse Manifest | `k8s/clickhouse/statefulset.yaml` | 221 |
| Monitoring Values | `k8s/monitoring/values.yaml` | 200 |
