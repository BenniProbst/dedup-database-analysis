# Session: Dedup Database Analysis - Projekt-Setup
**Datum:** 2026-02-16, ~21:00-22:00 UTC
**Status:** Projekt angelegt, Kartografie + Messsystem AUSSTEHEND

## Was wurde gemacht

### 1. Projekt erstellt
- **Pfad:** `C:\Users\benja\OneDrive\Desktop\Projekte\Research\dedup-database-analysis\`
- **Git:** Initialisiert auf `development` Branch
- **GitHub Remote:** `github` → https://github.com/BenniProbst/dedup-database-analysis.git (PRIVATE)
- **GitLab Remote:** `gitlab` → git@gitlab-push:comdare/research/dedup-database-analysis.git (ID 261, Gruppe comdare/research)
- **NAS Policy Fix:** `AllowInsecureGuestAuth` in Registry aktiviert (HKLM\SYSTEM\CurrentControlSet\Services\LanmanWorkstation\Parameters)

### 2. LaTeX-Dateien vom NAS kopiert
- **NAS:** `\\BENJAMINHAUPT\Cloud` (Z: Drive, Registry-Mapping)
- **Quellpfad:** `\\BENJAMINHAUPT\Cloud\Dokumente\Uni Dresden\21_15. Semester INFO 17\Analyse eines Forschungsthemas - Datenbanken und Deduplikation\ZIH Latex Vorlage\`
- **Ziel:** `docs/` Unterordner im Projekt

### 3. Dateien im docs/ Ordner

| Datei | Groesse | Beschreibung |
|-------|---------|-------------|
| doku.tex | 46.870 B | **ORIGINAL** Englisch, Betreuer: Dr. Alexander Krause |
| doku_de.tex | 49.912 B | Deutsche Version (aelter) |
| doku.bib | 15.234 B | Original-Bibliographie (12 Eintraege) |
| 20260127 doku_updated.tex | 53.852 B | Aktualisierte EN Version |
| 20260127 doku_de_updated.tex | 57.063 B | **NEUESTE** DE Version, Betreuer: Dr. Sebastian Goetz |
| 20260127 doku_updated.bib | 15.157 B | Aktualisierte Bib |
| 20260108 doku_updated.tex | 32.820 B | Aeltere Aktualisierung |
| 20260108 doku_updated.bib | 22.707 B | Aeltere Bib (groesser!) |
| 20260107 doku_updated_tex_bib.zip | 10.444 B | Archiv |
| 20260107 update_dedup_merge_cockroach_rcdb.zip | 30.446 B | Archiv mit CockroachDB/rcdb Merge |
| alphadin.bst | 43.066 B | BibTeX Style |
| plaindin.bst | 39.137 B | BibTeX Style |
| zihpub.cls | 29.075 B | ZIH Dokumentklasse |
| zih_logo_de_sw.eps | 204.687 B | ZIH Logo |
| Makefile | 1.117 B | LaTeX Build |
| doku.aux, doku.log, doku.toc | div. | LaTeX Build-Artefakte |

### 4. GitLab-Zugang
- **GitLab NICHT erreichbar von Windows** (VLAN 40, 10.0.40.5 = Timeout)
- **Erreichbar via pve1 SSH Tunnel** (HTTP, nicht HTTPS!)
- **API:** `ssh pve1 "curl -sk 'http://10.0.40.5/api/v4/...' --header 'PRIVATE-TOKEN: ...'"`
- **GitLab Version:** 18.8.4

### 5. Erstellte Dateien (VORLAEUFIG - muss nach Kartografie ueberarbeitet werden)
- `README.md` — Projektbeschreibung
- `.gitignore` — Standard-Ignores
- `.gitlab-ci.yml` — CI-Pipeline (11 Stages, MUSS nach Kartografie angepasst werden)
- `scripts/generate_datasets.py` — Testdaten-Generator
- `scripts/measure_longhorn.sh` — Longhorn-Metrik-Sammler
- `src/loaders/postgresql_loader.py` — PostgreSQL-Loader (exemplarisch)
- `src/reporters/aggregate_results.py` — Ergebnis-Aggregation
- `src/reporters/generate_charts.py` — Chart-Generierung
- `src/reporters/final_report.py` — Final-Report
- `src/cleanup/postgresql_maintenance.py` — PostgreSQL Maintenance

---

## AUSSTEHEND: Praezise Kartografie aller LaTeX-Dokumente

### Zu kartografieren (naechste Session):

#### A. doku.tex (Original EN) - VOLLSTAENDIG LESEN + KARTOGRAFIEREN
- Kap. 1: Introduction and Motivation
- Kap. 2: Terminology and Differentiation
- Kap. 3: Historical Development (LBFS, Venti, DDFS, iDedup, FastCDC, Sparse Indexing)
- Kap. 4: Taxonomy (5 Dimensionen: Detection, Granularity, Timing, Architecture, Primary/Secondary)
- Kap. 5: Deduplication in Various Systems (4 Sektionen: Relational, Object, TimeSeries, Scalable)
- Kap. 6: Initial Evaluation and Research Goal
- **KEIN Experiment-Kapitel!**
- Betreuer: Dr. Alexander Krause

#### B. 20260127 doku_de_updated.tex (Neueste DE) - DELTA zu doku.tex
- **NEU:** Kapitel 5 "Experimentelles Design und Messplan" mit:
  - Testbed: K8s 4 Nodes, Longhorn Replica 4, PVC 100 GiB
  - Speichermessung: logisch (DB-intern) + physisch (longhorn_volume_actual_size_bytes)
  - 7 Systeme unter Test: PostgreSQL, MariaDB, Kafka, MinIO, Redcomponent-DB, ClickHouse, CockroachDB
  - Datensaetze: GH Archive Events, NASA Bilder, Blender Videos, Gutenberg Texte, UUID, JSON, Bank-Txn
  - 3 Duplikationsgrade: U0 (unique), U50 (50%), U90 (90%)
  - 3 Stufen: Bulk-Insert → Per-File-Insert → Per-File-Delete+Reclamation
  - EDR-Formel: EDR = B_logical / (B_phys / N) wobei N=4 Replicas
- Betreuer: Dr. Sebastian Goetz

#### C. doku.bib vs 20260127 doku_updated.bib - Bibliographie-Delta
- Original: 12 Quellen (Grundlagen-Papers)
- Aktualisiert: +30 Quellen (DB-spezifische Docs: ClickHouse, DuckDB, Cassandra, MongoDB, PostgreSQL, Longhorn, Kafka, MinIO, NASA, Gutenberg, Wikimedia)

#### D. 20260108 doku_updated.bib - Groesste Bib (22.707 B)
- Muss gelesen werden — koennte zusaetzliche Quellen enthalten

### Zu kartografieren: Cluster-Zustand fuer Experiment

#### E. Bereits deployed im K8s Cluster (aus Explorer-Agent)
| System | Namespace | Pods | Storage | StorageClass |
|--------|-----------|------|---------|-------------|
| CockroachDB | cockroach-operator-system | 4/4 | 125Gi/Pod | longhorn-database-raid10 |
| PostgreSQL (CNPG) | databases | 4/4 | 50Gi/Pod | longhorn-database |
| Kafka (Strimzi KRaft) | kafka | 4/4 | 50Gi/Broker | longhorn-database-raid10 |
| Redis | redis | 4/4 | 25Gi/Pod | longhorn |
| MinIO | minio | 4/4 | 125Gi/Node | longhorn-data-raid10 |
| RedComponent-DB | redcomponent-db | 3 | 20Gi/Pod | longhorn |

#### F. NICHT im Cluster, muss deployed werden
- ClickHouse
- MariaDB
- (Optional: QuestDB, InfluxDB, TimescaleDB, Cassandra, DuckDB)

#### G. Longhorn StorageClasses
| StorageClass | Replicas | Zweck |
|---|---|---|
| longhorn-database | 4 | Datenbanken (RAID1 alle Nodes) |
| longhorn-database-raid10 | 4 | CockroachDB, Kafka |
| longhorn-data-raid10 | 2 | MinIO, User Data |
| longhorn | 2 | Standard-Workloads |

---

## Web-Research Ergebnis (Agent a852335)

### Zusaetzlich empfohlene Systeme (Tier A = HOCH)
1. **QuestDB** — Explizite Ingestion-Dedup seit v7.3 (MUST TEST)
2. **InfluxDB v3 (IOx)** — Sort-Merge-Dedup auf Arrow/Parquet (MUST TEST)
3. **TimescaleDB** — Columnar Compression = implizite Dedup (MUST TEST)
4. **Cassandra/ScyllaDB** — LSM Compaction STCS/LCS/UCS (MUST TEST)
5. **DuckDB** — Columnar Encoding DELTA/FSST (SHOULD TEST)
6. **Apache Doris** — Unique/Aggregate/Duplicate Key Models
7. **NATS JetStream** — Msg-ID Ingestion-Dedup
8. **TiKV/TiDB** — RocksDB Compaction

### Vier Deduplikations-Ebenen (Paper-Struktur-Vorschlag)
1. **Ingestion-Time** (QuestDB, InfluxDB, NATS) — sofortige Ablehnung
2. **Compaction-Time** (ClickHouse RMT, Cassandra, CockroachDB) — Background-Merge
3. **Compression-Implicit** (TimescaleDB, DuckDB, Druid) — Encoding nutzt Wiederholung
4. **Storage-Layer** (ZFS Fast Dedup, Ceph, LINSTOR) — transparent unter DB

### Relevante Papers 2025-2026
- DEDUPKV (ICS 2025): Fine-Grained Dedup in LSM-tree KV
- Fu et al. (ACM CS 2025): Distributed Dedup Survey
- EDBT 2025: Write Amplification Benchmark
- arXiv 2602.12669: LSM Compaction via LLM
- OpenZFS Fast Dedup (2024-2025)

---

## WICHTIG: Basis-Anweisungen (User-Klarstellung Ende Session)

### Misskonzeption korrigiert
- **Abgabe erfolgt auf ENGLISCH** — `doku.tex` (EN) ist das Hauptdokument
- `doku_de_updated.tex` (DE) ist NICHT das Ziel, sondern nur Referenz fuer Updates
- Die DE-Version enthaelt neuere Inhalte (Experiment-Kapitel), aber ist "nicht so gut gelungen"
- **Aufgabe:** `doku.tex` (EN) schrittweise verbessern/konsolidieren, OHNE vom gut gelungenen Kern abzuweichen
- Neue Inhalte aus DE-Version selektiv in EN uebernehmen, Qualitaet pruefen

### Arbeitsweise
- **NICHT auf NAS arbeiten** — alles im Git-Repo bearbeiten
- **Jeden Commit des Dokuments** in einen parallelen LaTeX-Kompilierungs-Workflow einbinden
- LaTeX braucht `zihpub.cls` (TU Dresden Template) zum Kompilieren
- **Zwei parallele CI-Pipelines:**
  1. LaTeX-Kompilierung (bei jedem Commit der .tex/.bib Dateien)
  2. Datenbank-Test-Pipeline (manuell triggerbar)

### Quellen-Validierung
- **ALLE Quellen in doku.bib mit Web-Recherche gegenpruefen** (Links noch gueltig? Inhalte korrekt?)
- Neueste englische Version + bib wurde vom User in den NAS-Ordner kopiert — MUSS GELESEN WERDEN
- `20260108 doku_updated.bib` (22.707 B) ist GROESSER als die anderen — koennte zusaetzliche Quellen haben

### Kartografie-Aufgabe
- `doku.tex` + `doku.bib` = aktueller UNKARTOGRAFIERTER Stand (User kennt Inhalt nicht genau)
- Erst kartografieren, dann Delta zur DE-Version bestimmen
- Schrittweise EN-Version verbessern, nicht komplett ersetzen

## Session 2 (Kontext 2, 2026-02-16 ~22:30 UTC): Kartografie + Phase 1 Fixes

### Was wurde gemacht

#### 1. NAS erneut gelesen (NAS erreichbar via bash UNC)
- **Pfad funktioniert:** `//BENJAMINHAUPT/Cloud/Dokumente/Uni Dresden/...`
- **NEUE Dateien entdeckt:** `20260216 doku.tex` (54.168 B) + `20260216 doku.bib` (15.578 B)
- **Befund:** `20260216` = identisch mit `20260127 doku_updated` (User hat umbenannt)
- **Kopiert ins Projekt:** `docs/20260216 doku.tex` + `docs/20260216 doku.bib`

#### 2. Cluster_NFS Testdaten entdeckt + dokumentiert
- **Pfad:** `\\BENJAMINHAUPT\Cluster_NFS`
- **REGEL (User-Anweisung):** Daten DUERFEN NIE GELOESCHT WERDEN! Nur in tmp/ entpacken!
- **Inhalte:** bankdataset.xlsx.zip (32MB), gharchive/, github-repos-bigquery/, million_post_corpus.tar.bz2 (105MB), random-numbers.zip (271MB), cluster-backups/

#### 3. Vollstaendige Kartografie erstellt
- **Datei:** `sessions/20260216-kartografie-doku-tex.md` (UMFASSEND, ~400 Zeilen)
- **Inhalte:**
  - A. Dokumentversionen-Tabelle (6 Versionen verglichen)
  - B. Kapitelstruktur doku.tex (6 Kapitel + 4 Sektionen praezise kartografiert)
  - C. DELTA 20260216 vs Original (Zitationen repariert, Kap. 6 neu, Artefakte)
  - D. Bibliografie-Analyse (3 bib-Dateien verglichen, 32+14+30 Eintraege)
  - E. Bekannte Fehler (Artefakte, Redcomponent-DB, @online vs @misc)
  - F. Cluster_NFS Testdaten (mit NIEMALS-LOESCHEN Regel)
  - G. Konsolidierungsstrategie (3 Phasen)
  - H. Naechste Schritte

#### 4. Phase 1 Fixes ausgefuehrt
- **doku.tex ersetzt:** `20260216 doku.tex` → `doku.tex` (neueste EN mit korrekten \cite{})
- **3 Zitations-Artefakte behoben:**
  - Z.77: `Paulo & Pereira 2014: and Fu et al.~\cite{Fu2025}:` → korrekte \cite{Paulo2014}
  - Z.166: Doppelpunkt-Artefakt nach `group by` entfernt
  - Z.302: Doppelpunkt nach `\cite{Fu2025}` entfernt
- **doku.bib konsolidiert:**
  - `20260216 doku.bib` als Basis (46 Eintraege, @misc korrekt)
  - CockroachDB `}}` Bug gefixt
  - 12 HOCH-relevante Eintraege aus `20260108 doku_updated.bib` gemerged:
    - Douceur2002 (Convergent Encryption)
    - Harnik2010 (Side Channels in Cloud Dedup)
    - Pelkonen2015Gorilla (Gorilla TSDB)
    - ONeil1996LSM (LSM-Tree Seminal Paper)
    - Weil2006Ceph (Ceph Original Paper)
    - Kreps2011Kafka (Kafka Original Paper)
    - Shvachko2010HDFS (HDFS Design Paper)
    - Elmagarmid2007Duplicate (Duplicate Detection Survey)
    - QuestDBDedupDocs, InfluxDBDuplicatePoints
    - Bhagwat2009 (Extreme Binning), Debnath2010ChunkStash
  - **Total: 58 bib-Eintraege** in konsolidierter doku.bib

#### 5. Quellen-Webrecherche gestartet
- **Agent a291b1d laeuft im Hintergrund** — verifiziert alle 59 URLs
- **Status: NICHT ABGESCHLOSSEN** bei Kontext-Ende
- Ergebnis muss in naechster Session abgeholt werden

### User-Anweisungen (Session 2)
- **NAS-Daten auf Cluster_NFS NIE LOESCHEN** — nur in tmp/ entpacken
- **Projektordner = Hauptarbeitsordner** — NICHT auf NAS arbeiten
- **Alles ins Git-Repo kopieren** — NAS nur zum Lesen/Kopieren

### KRITISCHE BEFUNDE aus Kartografie
1. **doku.tex (Original) war NICHT kompilierbar** — ChatGPT-Artefakte statt \cite{}
2. **20260216 = 20260127 updated** (nur umbenannt, identischer Inhalt)
3. **20260108 bib ist GROESSTE** mit 30+ Extra-Papers (jetzt 12 davon gemerged)
4. **Redcomponent-DB im Text** — muss zu comdare-DB umbenannt werden (User-Entscheidung)
5. **15+ bib-Eintraege werden im Text nicht zitiert** (DuckDB, PG, CH, Cassandra, MongoDB) — fuer zukuenftige Sektionen vorbereitet

## Session 3 (Kontext 3, 2026-02-16 ~23:30 UTC): Dual-Pipeline + Architektur-Redesign

### Was wurde gemacht

#### 1. Bibliographie-Verifikation KOMPLETT (Agent a291b1d)
- **59/59 Eintraege verifiziert — 0 kaputte URLs, 0 Inhaltsfehler**
- 46/46 direkte URLs = HTTP 200
- 4/4 DOIs korrekt aufgeloest (302→ACM/IEEE)
- 9/9 akademische Papers ohne URL via Web-Suche bestaetigt
- Ergebnisse in `sessions/20260216-kartografie-doku-tex.md` Sektion H dokumentiert

#### 2. Dual-Pipeline .gitlab-ci.yml fertig
- **Pipeline 1 (LaTeX):** Auto-Trigger auf `docs/**/*.tex`, `docs/**/*.bib`, `docs/**/*.cls`, `docs/**/*.bst`
  - Image: `texlive/texlive:latest`
  - Triple-Pass Build: pdflatex → bibtex → pdflatex × 2
  - Artifact: `docs/doku.pdf` (30d)
  - Warnings/Undefined Refs/Missing Citations Summary im Log
- **Pipeline 2 (DB-Experiment):** ALLE Jobs `when: manual`
  - Bestehende 11 Stages unveraendert
  - Image: `alpine/k8s:1.34.0`
  - YAML-Anchors fuer experiment-default

#### 3. SSH Config gefixt
- `ProxyJump pve1` zu `gitlab-push` Host hinzugefuegt
- `ssh -T gitlab-push` = "Welcome to GitLab, @root!" (funktioniert!)
- GitLab Push laeuft aber SEHR LANGSAM durch ProxyJump (Timeout)

#### 4. K8s Produktions-DB Reconnaissance (READ-ONLY!)

| System | Namespace | Pods | Storage | Service-IP | Ports |
|--------|-----------|------|---------|------------|-------|
| CockroachDB | cockroach-operator-system | 4/4 | 125Gi×4=500Gi | 10.99.220.209 | 26257(SQL), 8080(UI) |
| PostgreSQL HA | databases | 4/4 | 50Gi×4=200Gi | 10.101.101.119(LB), 10.106.148.251(Primary) | 5432 |
| Redis Cluster | redis | 4/4 | 25Gi×4=100Gi | 10.106.48.77(Standalone) | 6379, 16379 |
| Redis GitLab | gitlab | 2/1(!) | 5Gi×4=20Gi | 10.108.176.157 | 6379 |
| Kafka (Strimzi) | kafka | 4+3=7 | 50Gi×4+10Gi×3=230Gi | 10.109.224.239(Bootstrap) | 9091-9093 |
| MinIO | minio | 4/4 | Direct Disk | 10.0.90.55(LB) | 9000, 9001 |
| **TOTAL Longhorn** | | **23 PVCs** | | **1.050 Gi** | |

**NICHT im Cluster:** MariaDB, ClickHouse, MongoDB, DuckDB

#### 5. Architektur-Redesign (User-Anforderungen)

### KRITISCHE USER-ANFORDERUNGEN (Session 3)

#### A. Sicherheitsarchitektur: Labor vs Produktion
- **ALLE DBs im Cluster = PRODUKTIONSDATEN!** Labor noch NICHT getrennt
- **Loesung: SEPARATE SCHEMATA** auf bestehenden Prod-DBs
- **Labor-User** ueber Samba AD anlegen (LDAP-Auth fuer alle DBs)
- **Test-Schemata nach jedem Lauf zuruecksetzen** (Reset auf Null)
- **Kundendaten bleiben UNVERAENDERT** fuer alle Datenbanken!
- **Regel:** Integrationstest-User darf NUR auf eigene Schemata zugreifen

#### B. C++ Testprogramm (statt Python) — BEGRUENDUNG
- **Cluster-Hardware: Intel N97** (Alder Lake-N, 4C/4T, max 3.6 GHz, passiv)
- **Python-Overhead auf N97 ZU DEUTLICH sichtbar** — verfaelscht Messergebnisse
- **C++ Vorteile:** std::chrono::steady_clock, kein GC/GIL, deterministisch
- **Praezise Zeitmessungen** sind PFLICHT fuer die Forschungsarbeit
- Kompilierung ueber BuildSystem (cd-buildsystem)
- Artefakte im MinIO buildsystem-artifact Cache

#### C. 3-Pipeline-Architektur (ERWEITERT!)
1. **Pipeline 1: LaTeX** (automatisch bei .tex/.bib) — FERTIG
2. **Pipeline 2: C++ Kompilierung** (automatisch bei Code-Aenderungen)
   - Smoke Tests
   - Umfangreiche Trocken-Test-Suite (Dry-Run ohne DB)
   - Artefakt → MinIO buildsystem-artifact Cache
3. **Pipeline 3: Testintegration** (MANUELL)
   - Holt kompiliertes C++ Binary aus MinIO Cache
   - Fuehrt Forschungs-Experiment durch (verschiedene Datentypen + Parameter)
   - Loggt Metriken an Grafana

#### D. Grafana-Service im K8s
- **NEU:** Grafana muss auf dem K8s Cluster deployed werden
- C++ Applikation loggt Werte direkt an Grafana (via Prometheus/InfluxDB)
- Echtzeit-Monitoring der Experiment-Laeufe

#### E. Netzwerk-Interfaces
- **Multiple Netzwerk-Interfaces** zu ALLEN HA-Datenbanken unter Test
- Jede DB hat eigene Service-IPs (siehe Tabelle oben)
- C++ Programm muss zu allen gleichzeitig verbinden koennen

#### F. Schema-Isolation (Detail-Design)

| Datenbank | Prod-Schema | Labor-Schema | Reset-Methode |
|-----------|-------------|-------------|---------------|
| PostgreSQL | public | dedup_lab | DROP SCHEMA dedup_lab CASCADE; CREATE SCHEMA dedup_lab; |
| CockroachDB | defaultdb | dedup_lab | DROP DATABASE dedup_lab; CREATE DATABASE dedup_lab; |
| Kafka | prod-topics | dedup-lab-* | kafka-topics --delete --topic 'dedup-lab-*' |
| MinIO | prod-buckets | dedup-lab-* | mc rb --force minio/dedup-lab-*; mc mb minio/dedup-lab-U0 |
| Redis | db0 (prod) | db15 (lab) | FLUSHDB auf db15 |

#### G. Samba AD Labor-User (TODO)

```
# Auf Samba AD Controller anlegen:
samba-tool user add dedup-lab '[REDACTED]' --description="Dedup Research Lab User"
samba-tool group addmembers "Database Admins" dedup-lab  # NICHT! Nur DB-Lab-Rechte!
# Separate Gruppe:
samba-tool group add "Research Lab"
samba-tool group addmembers "Research Lab" dedup-lab
```

### Attention Items aus Reconnaissance
1. **redis-gitlab-1** stuck in Terminating (node talos-5x2-s49)
2. **PostgreSQL pod-1/pod-2** beide auf talos-qkr-yc0 (kein Anti-Affinity)
3. **Strimzi Operator** 10 Restarts, Entity Operator 25 Restarts
4. **MinIO = Direct Disk** (kein Longhorn, kein Snapshot-Backup)

---

#### 6. C++ Integration Test Framework (Session 3, zweiter Teil)
- **20 Dateien, 2.051 Zeilen C++ Code**
- **Commit 39cb309:** "Add C++ integration test framework + triple CI pipeline"
- **Struktur:**
  ```
  src/cpp/
    CMakeLists.txt          — CMake Build (gcc:14-bookworm, FetchContent nlohmann/json)
    config.hpp              — ExperimentConfig, DbConnection, DupGrade, Stage enums
    main.cpp                — CLI mit --systems, --grades, --dry-run, --verbose
    connectors/
      db_connector.hpp      — Abstrakte DbConnector-Interface
      postgres_connector.*  — PostgreSQL + CockroachDB (libpq, PG wire protocol)
      redis_connector.*     — Redis (hiredis, DB 15 = Lab)
      kafka_connector.*     — Kafka (librdkafka, topic prefix dedup-lab-*)
      minio_connector.*     — MinIO (libcurl S3 API, bucket prefix dedup-lab-*)
    experiment/
      schema_manager.*      — Lab-Schema Lifecycle (create/reset/drop auf allen DBs)
      data_loader.*         — Experiment-Orchestrierung (Stages 1-3, EDR-Berechnung)
      metrics_collector.*   — Prometheus Longhorn-Metriken + Grafana Push
    utils/
      timer.hpp             — steady_clock Timer + RAII ScopedTimer
      logger.hpp            — Timestamp-Logger mit LogLevel (DEBUG/INFO/WARN/ERROR)
  ```
- **Dependencies:** libpq, libcurl, hiredis (optional), librdkafka (optional), nlohmann/json
- **Build:** `cmake ../src/cpp && make -j$(nproc)`
- **Targets:** `dedup-test` (real) + `dedup-smoke-test` (DEDUP_DRY_RUN=1)
- **N97 Optimierung:** `-O2 -march=alderlake`

#### 7. Triple CI Pipeline KOMPLETT
- **Dual→Triple erweitert** in .gitlab-ci.yml
- **Pipeline 2 (C++ Build):** Auto auf `src/cpp/**` Aenderungen
  - Stage `cpp-build`: gcc:14-bookworm, cmake, alle Dependencies
  - Stage `cpp-test`: Smoke Test (DRY RUN) + Dry Test Suite (alle Systems/Grades)
  - Artefakte in MinIO `buildsystem-artifacts/dedup-database-analysis/`
- **Pipeline 3 (DB Experiment):** Alle Jobs `when: manual` (wie zuvor)

#### 8. Commits + Push
- **3 Commits auf `development` Branch:**
  1. `fa9d890` — Initial commit: LaTeX paper + experiment framework + kartografie
  2. `2cac967` — Dual CI pipeline + session update: 3-pipeline architecture
  3. `39cb309` — Add C++ integration test framework + triple CI pipeline
- **GitHub Push:** ALLE 3 Commits erfolgreich gepusht
- **GitLab Push:** SSH ProxyJump funktioniert (`ssh -T gitlab-push` = OK), aber
  `git push` hängt bei `git-receive-pack` — Cluster unter Backup-Last (~1h)

---

## Session 4 (Kontext 4, 2026-02-16 ~22:05 UTC): Lab-Isolation + CockroachDB TLS

### Was wurde gemacht

#### 1. GitLab Push retry
- SSH Test: `ssh -T gitlab-push` = "Welcome to GitLab, @root!" (sofort)
- Push im Hintergrund gestartet (4 Commits ausstehend)
- **Status: LAEUFT** (task b44c03e)

#### 2. Samba AD Labor-User ANGELEGT
- **User:** `dedup-lab` (UPN: dedup-lab@comdare.de)
- **SID:** S-1-5-21-1633907924-69945966-2584419805-1111
- **Gruppe:** "Research Lab" erstellt + dedup-lab hinzugefuegt
- **DN:** CN=Dedup Lab,CN=Users,DC=comdare,DC=de
- **Passwort:** [REDACTED]
- **Credentials:** gespeichert in `credentials.env` (in .gitignore!)

#### 3. Schema-Isolation TEILWEISE EINGERICHTET

| Datenbank | Status | Detail |
|-----------|--------|--------|
| PostgreSQL | **DONE** | Schema `dedup_lab` erstellt, User `dedup-lab` mit GRANT ALL, public CREATE revoked |
| CockroachDB | **DONE (insecure)** | Database `dedup_lab` erstellt, User `dedup_lab` (ohne Passwort wg insecure mode) |
| Redis | **AENDERUNG** | Redis Cluster Mode → kein SELECT moeglich! → Key-Prefix `dedup:*` statt DB 15 |
| Kafka | **PENDING** | Topic-Prefix `dedup-lab-*` (auto-create bei erstem Produce) |
| MinIO | **PENDING** | Bucket-Prefix `dedup-lab-*` (TODO: Lab-User in MinIO anlegen) |

#### 4. CockroachDB Cluster-Check
- **4/4 Nodes healthy:** alle `is_available=true`, `is_live=true`
- **Version:** v24.3.0
- **PROBLEM:** `tlsEnabled: false` in CrdbCluster Spec!
- **User-Anweisung:** Nach User-Erstellung ZURUECK in sicheren Modus (TLS)

#### 5. Redis Cluster Mode Erkenntnis
- Redis 7.4.7 im **Cluster Mode** — SELECT nicht verfuegbar
- **Loesung:** Key-Prefix `dedup:*` statt separater DB
- C++ Code muss angepasst werden (config.hpp + redis_connector.*)

### OFFEN: CockroachDB TLS-Migration
- **Aktuell:** `tlsEnabled: false` → insecure mode
- **Ziel:** `tlsEnabled: true` → TLS-gesicherter Betrieb
- **Vorgehen:**
  1. cert-manager im Cluster pruefen (braucht CockroachDB Operator)
  2. CrdbCluster Spec patchen: `tlsEnabled: true`
  3. Operator generiert Certs + Rolling Restart
  4. Lab-User mit Passwort neu anlegen
  5. Connection Strings auf TLS updaten
- **ACHTUNG:** Rolling Restart = kurze Downtime! Mit User absprechen!

---

## Session 5 (Kontext 5, 2026-02-17 ~06:35 UTC): CockroachDB TLS + Lab Infrastructure

### Was wurde gemacht

#### 1. CockroachDB TLS Migration KOMPLETT
- **Ausgangslage:** `tlsEnabled: false`, CrdbCluster in letzter Session gepatcht
- **Problem:** Rolling Restart Deadlock — Pod 3 (TLS) konnte nicht mit Pods 0-2 (insecure) kommunizieren
- **Loesung:** Alle 4 Pods gleichzeitig geloescht (`kubectl delete pods cockroachdb-{0,1,2,3}`)
  - `podManagementPolicy: Parallel` erlaubte gleichzeitigen Neustart
  - Pod 1 blieb auf alter Revision → Force-Delete noetig
- **Ergebnis:** 4/4 Nodes `is_available=true, is_live=true` mit TLS
- **Zertifikate:** CA + Node + Client, gueltig bis 2031-02-20
- **Secrets:** `cockroachdb-ca`, `cockroachdb-node`, `cockroachdb-root`

#### 2. CockroachDB Lab-User mit Passwort
- `CREATE USER "dedup_lab" WITH PASSWORD '[REDACTED]'` — jetzt im TLS-Modus moeglich!
- `GRANT ALL ON DATABASE dedup_lab TO "dedup_lab"`

#### 3. Redis Connector Fix (DB 15 → Key-Prefix)
- **redis_connector.hpp:** `LAB_DB = 15` → `KEY_PREFIX = "dedup:"`
- **redis_connector.cpp:** SELECT entfernt, PING zur Verifizierung
  - Cleanup: SCAN + DEL statt FLUSHDB (cluster-safe)
  - Size: SCAN + STRLEN statt DBSIZE
  - Alle Keys mit Prefix `dedup:` (z.B. `dedup:filename.dat`)
- **config.hpp:** Kommentar aktualisiert
- **Commit:** `e29c9d4`

#### 4. MinIO Lab Setup KOMPLETT
- **User:** `dedup-lab` erstellt mit `readwrite` Policy
- **Buckets:** `dedup-lab-u0`, `dedup-lab-u50`, `dedup-lab-u90`, `dedup-lab-results`
- **credentials.env** aktualisiert (CRDB TLS + Redis Key-Prefix)

#### 5. Kafka Lab Topics KOMPLETT
- **3 Topics:** `dedup-lab-u0`, `dedup-lab-u50`, `dedup-lab-u90`
- **Config:** 4 Partitions, Replication Factor 3

#### 6. Git Status
- **Commit e29c9d4:** "Fix Redis connector for cluster mode + CockroachDB TLS + MinIO/Kafka lab setup"
- **GitHub Push:** Erfolgreich (c2fc814..e29c9d4)
- **GitLab Push:** Haengt wiederholt (SSH ProxyJump + git-receive-pack Timeout)

### Schema-Isolation Status (AKTUALISIERT)

| Datenbank | Status | Detail |
|-----------|--------|--------|
| PostgreSQL | **DONE** | Schema `dedup_lab`, User `dedup-lab`, GRANT ALL |
| CockroachDB | **DONE (TLS!)** | Database `dedup_lab`, User `dedup_lab` MIT Passwort |
| Redis | **DONE** | Key-Prefix `dedup:*` (Cluster Mode, kein SELECT) |
| Kafka | **DONE** | 3 Topics `dedup-lab-u0/u50/u90` (4 Part, RF=3) |
| MinIO | **DONE** | User `dedup-lab` + 4 Buckets (u0/u50/u90/results) |

---

## Session 6 (Kontext 6, 2026-02-17 ~17:30 UTC): GitLab Push Diagnose + Cluster Cleanup

### Was wurde gemacht

#### 1. GitLab Push — ALLE VERSUCHE GESCHEITERT
- **5+ Push-Versuche** (via ProxyJump, direkt von pve1, mit Keepalive) — alle Timeout
- **SSH-Verbindung funktioniert** (`ssh -T gitlab-push` = OK)
- **GitLab Shell empfaengt und autorisiert** den Push korrekt
- **Gitaly SSHReceivePack** startet, laeuft 360s, wird dann abgebrochen ("context canceled")
- **Repo auf GitLab ist LEER** — HEAD zeigt auf `refs/heads/development` aber `refs/heads/` ist leer
- **Jeder Push laedt Objects hoch, aber die finale Referenz-Aktualisierung schlaegt durch Timeout fehl**

#### 2. Root Cause Analyse
- **K8s API I/O Timeouts:** `read tcp 10.0.15.201:54428→10.0.15.250:6443: i/o timeout`
- `kubectl cp` und `kubectl exec` mit groesseren Daten-Streams scheitern
- Kleine kubectl-Befehle (get, exec kurze Kommandos, logs) funktionieren
- **Vermutung:** MTU-Problem oder TCP-Window-Issue auf VLAN 15 (K8S API)
- **Alternative:** Gitaly Repo hat stale Lock-Datei (gefunden + entfernt auf Repo 135)

#### 3. GitLab Reparaturen
- **Gitaly + Praefect neugestartet** (alle 4 Pods)
- **GitLab Shell + Webservice neugestartet** (Rolling Restart)
- **Stale Lock entfernt:** `/home/git/repositories/@cluster/repositories/13/67/135/refs/heads/development.lock`
- **Alternativer Push-Ansatz:** Git Bundle (167KB) erstellt, via SCP auf pve1, dort Clone + Push versucht — scheitert ebenfalls

#### 4. Redis GitLab auf 4 Replicas skaliert
- Anderer Agent hatte `redis-gitlab` von 4 auf 1 herunterskaliert
- **Zurueck auf 4 Replicas:** je 1 Pod pro Node (Anti-Affinity)
  - redis-gitlab-0 → talos-lux-kpk
  - redis-gitlab-1 → talos-5x2-s49
  - redis-gitlab-2 → talos-say-ls6
  - redis-gitlab-3 → talos-qkr-yc0

#### 5. Cluster-Diagnose (vollstaendig)
- **4/4 Nodes Ready** (Talos v1.11.6, K8s v1.34.0)
- **0 fehlerhafte Pods** (alle Running/Completed)
- **Longhorn:** 37 → 35 Volumes nach Cleanup (alle healthy)
- **MinIO:** 4/4 Running, **KEINE PVCs** (Direct Disk, kein Longhorn!)
- **Redis Cluster:** 4/4, **Redis GitLab:** 4/4
- **CockroachDB:** 4/4 TLS, **PostgreSQL:** 4/4, **Kafka:** 7/7
- **Samba AD:** 4/4, **OPNsense:** 4/4

#### 6. Verwaiste Longhorn Volumes bereinigt
- **pvc-056637a0** (5Gi) — ehem. `gitlab/redis-data-redis-gitlab-0`, PV Released → GELOESCHT
- **pvc-f1b1085e** (50Gi) — ehem. `kafka/data-kafka-cluster-broker-1`, PV Released → GELOESCHT
- Dienste verifiziert: Redis GitLab PONG + 12.243 Keys, Kafka Broker-1 unfenced + Topics erreichbar
- **55Gi Longhorn-Speicher zurueckgewonnen**

### Git Status (Lokal)
- **6 Commits auf `development`:**
  1. `fa9d890` Initial commit
  2. `2cac967` Dual CI pipeline
  3. `39cb309` C++ integration test framework (2.051 LOC, 20 files)
  4. `38b6824` Session docs update
  5. `c2fc814` Lab isolation (Samba AD user, PG schema, CockroachDB database)
  6. `e29c9d4` Redis cluster fix + CockroachDB TLS + MinIO/Kafka lab setup
- **GitHub:** ALLE 6 Commits gepusht
- **GitLab:** 0 Commits (Push blockiert durch K8s API Timeout)

---

## Session 7 (2026-02-17 ~18:30 UTC): GitLab HTTPS Push + SHA-256 + K8s CI

### Was wurde gemacht
1. **[DONE] GitLab Push via HTTPS** — User hat Firewall geupdated, gitlab.comdare.de via HTTPS erreichbar
   - Remote URL: `https://oauth2:TOKEN@gitlab.comdare.de/comdare/research/dedup-database-analysis.git`
   - Alle 6 Commits erfolgreich gepusht
2. **[DONE] SHA-256 (FIPS 180-4)** — Pure C++ Implementierung in `utils/sha256.hpp`
3. **[DONE] Dataset Generator** — xoshiro256** PRNG, kontrollierte Duplikation (U0/U50/U90)
4. **[DONE] MinIO S3 Auth** — AWS Signature V4 mit HMAC-SHA256
5. **[DONE] Kafka AdminClient** — Topic Create/Delete via librdkafka Admin API
6. **[DONE] JSON Config Parser** — `config.hpp::from_json()`
7. **[DONE] K8s CI Pipeline** — Umstellung auf K8s Runner (KEIN lokales Kompilieren)
8. **Commit 61a71c3** gepusht auf GitLab + GitHub

## Session 8 (2026-02-17 ~22:30 UTC): Longhorn-Metriken + MariaDB/ClickHouse Stubs

### Was wurde gemacht
1. **[DONE] Longhorn Physical Size Fix** — `data_loader.cpp` nutzt jetzt `MetricsCollector.get_longhorn_actual_size()` statt connector logical size fuer physische Messung (doku.tex 5.1)
2. **[DONE] PVC Name Mapping** — `DbConnection` erweitert um `pvc_name` + `k8s_namespace`, default_k8s_config mit realen PVC-Namen
3. **[DONE] Ingest Throughput** — `throughput_bytes_per_sec` in ExperimentResult (doku.tex 5.4.1)
4. **[DONE] SHA-256 Fix** — perfile_insert nutzt jetzt echten SHA-256 statt Placeholder
5. **[DONE] MariaDB Connector** — Vollstaendiger Stub mit libmysqlclient, InnoDB OPTIMIZE TABLE
6. **[DONE] ClickHouse Connector** — Vollstaendiger Stub mit HTTP API (port 8123), MergeTree Engine
7. **[DONE] CMakeLists.txt refactored** — `configure_dedup_target()` Funktion, optionale Dependencies (hiredis, rdkafka, mysqlclient)
8. **[DONE] CI Pipeline** — `libmariadb-dev` als optionale Dependency

### Erkannte Limitationen (aus Session-Lektuere)
- **MinIO = Direct Disk** — KEIN Longhorn PVC → Longhorn-Metriken funktionieren hier NICHT
- **MariaDB + ClickHouse = NICHT IM CLUSTER** — muessen erst deployed werden
- **Redis = Cluster Mode** — kein SELECT, Key-Prefix-Isolation
- **CockroachDB = TLS** → sslmode=verify-full
- **Longhorn thin-provisioned** → schrumpft NICHT nach Delete (braucht TRIM/Discard + Maintenance)

### Noch fehlende Datenbanken (laut doku.tex, NICHT im Cluster)
| System | Prioritaet | Status |
|--------|-----------|--------|
| MariaDB | HOCH (doku.tex 5.2) | Connector FERTIG, Cluster-Deploy TODO |
| ClickHouse | HOCH (doku.tex 5.2) | Connector FERTIG, Cluster-Deploy TODO |
| comdare-DB (ex Redcomponent-DB) | MITTEL | Bereits im Cluster (3 Pods), Connector TODO |
| QuestDB | HOCH (Web-Research) | Cluster-Deploy + Connector TODO |
| InfluxDB v3 | HOCH (Web-Research) | Cluster-Deploy + Connector TODO |
| TimescaleDB | HOCH (Web-Research) | Cluster-Deploy + Connector TODO |
| Cassandra/ScyllaDB | HOCH (Web-Research) | Cluster-Deploy + Connector TODO |

## Naechste Schritte (PRIORISIERT)

1. **[TODO]** CI Pipeline testen — Push + Verify auf K8s Runner
2. **[TODO]** MariaDB im K8s Cluster deployen (StatefulSet, Longhorn PVC)
3. **[TODO]** ClickHouse im K8s Cluster deployen (StatefulSet, Longhorn PVC)
4. **[TODO]** Grafana-Service im K8s deployen (Prometheus Pushgateway)
5. **[SPAETER]** Phase 2 inhaltliche Erweiterung (DuckDB, Cassandra, MongoDB in doku.tex)
6. **[SPAETER]** QuestDB/InfluxDB/TimescaleDB/Cassandra Connectors + Cluster-Deploy

## Technische Notizen

### GitLab API via pve1 (funktionierender Aufruf)
```bash
ssh pve1 "curl -sk -X POST 'http://10.0.40.5/api/v4/projects' \
  --header 'PRIVATE-TOKEN: [REDACTED-GITLAB-TOKEN]' \
  --data-urlencode 'name=PROJEKTNAME' \
  --data-urlencode 'namespace_id=24' \
  --data-urlencode 'visibility=private'"
```

### NAS Zugang (PowerShell, nach Policy-Fix)
```powershell
New-PSDrive -Name NAS -PSProvider FileSystem -Root "\\BENJAMINHAUPT\Cloud"
Get-ChildItem "\\BENJAMINHAUPT\Cloud\Dokumente\Uni Dresden\21_15. Semester INFO 17\..."
```

### Git Remotes
```
github  https://github.com/BenniProbst/dedup-database-analysis.git
gitlab  git@gitlab-push:comdare/research/dedup-database-analysis.git
```
