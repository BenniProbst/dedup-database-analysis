# Deduplikation in Datenhaltungssystemen - Experimentelles Testbed

**Forschungsarbeit:** Analyse eines Forschungsthemas -- Deduplikation in Datenhaltungssystemen
**Autor:** Benjamin-Elias Probst (Matr.-Nr. 4510512)
**Betreuer:** Dr. Sebastian Goetz, TU Dresden
**Projektkennung:** p_llm_compile (ZIH)

## Forschungsfrage

> Wo findet Deduplikation statt -- auf der Datenbankebene, auf der Storage-Ebene oder gar nicht?

## Methodik

Empirische Messung des physischen Speicherverbrauchs (Longhorn-Metriken) vs. logischer Datengroesse
mit kontrollierten Duplikationsgraden:

| Grad | Beschreibung |
|------|-------------|
| **U0** | 0% Duplikate (unique) |
| **U50** | 50% Duplikate |
| **U90** | 90% Duplikate (hochredundant) |

## Testbed

- **Kubernetes-Cluster:** 4 Worker Nodes (Talos v1.11.6, K8s v1.34.0)
- **Storage:** Longhorn (Replica 4 ueber 4 Nodes, thin-provisioned)
- **PVC:** 100 GiB pro System unter Test
- **Metriken:** `longhorn_volume_actual_size_bytes` via Prometheus

## Systeme unter Test

### Primaer (bereits im Cluster)

| System | Typ | Deduplikationsmechanismus |
|--------|-----|--------------------------|
| PostgreSQL | Relationales DBMS | B-Tree Index Dedup, TOAST Kompression |
| MariaDB | Relationales DBMS | Keine native Dedup, InnoDB Page Compression |
| Apache Kafka | Distributed Log | Log Compaction (Key-basiert), EOS |
| MinIO | S3 Object Storage | Keine Inhalts-Dedup |
| CockroachDB | Geo-verteiltes SQL | RocksDB Compaction (Key-Dedup) |
| comdare-db | Experimentell (Blackbox) | Empirisch zu bestimmen |

### Zusaetzlich einzurichten

| System | Typ | Deduplikationsmechanismus | Prioritaet |
|--------|-----|--------------------------|------------|
| ClickHouse | OLAP spaltenorientiert | ReplacingMergeTree (Zeilen-Dedup bei Merge) | HOCH |
| QuestDB | Zeitreihen-DBMS | Explizite Ingestion-Dedup (seit v7.3) | HOCH |
| InfluxDB v3 (IOx) | Zeitreihen-DBMS | Sort-Merge-Dedup auf Parquet/Arrow | HOCH |
| TimescaleDB | Zeitreihen (PG-Extension) | Columnar Compression (implizite Dedup) | HOCH |
| Cassandra / ScyllaDB | Wide-Column | LSM-Compaction (STCS/LCS/UCS) | HOCH |
| DuckDB | Embedded OLAP | Columnar Encoding (DELTA, FSST) | MITTEL |
| Apache Doris | OLAP | Unique/Aggregate/Duplicate Key Models | MITTEL |
| NATS JetStream | Message Streaming | Msg-ID-basierte Ingestion-Dedup | MITTEL |
| MongoDB | Dokument-DB | WiredTiger Block-Kompression | OPTIONAL |
| Redis / Valkey | In-Memory KV | Key-Overwrite (last-write-wins) | OPTIONAL |

## Vier Deduplikationsebenen

1. **Ingestion-Time Dedup** (QuestDB, InfluxDB IOx, NATS JetStream)
   - Duplikate werden VOR Persistierung abgewiesen/gemergt
   - Sofortige, garantierte Speicherersparnis

2. **Compaction-Time Dedup** (ClickHouse RMT, Cassandra, CockroachDB, Kafka Compaction)
   - Duplikate werden geschrieben, aber bei Background-Merge entfernt
   - Eventuelle, nichtdeterministische Einsparung

3. **Compression-Implicit Dedup** (TimescaleDB, DuckDB, Druid Dictionary Encoding)
   - Keine explizite Dedup, aber Encoding nutzt Wertwiederholung
   - Datenabhaengige Einsparung

4. **Storage-Layer Dedup** (ZFS Fast Dedup, Ceph, LINSTOR/LVM)
   - Filesystem/Block-Storage dedupliziert transparent
   - DB-unabhaengige Einsparung

## Experiment-Stufen

### Stufe 1: Bulk-Insert
- Schema/Topic/Bucket anlegen
- Bulk-Load U0/U50/U90 Varianten
- Wall-Clock-Zeit + Longhorn-Metriken messen

### Stufe 2: Per-File Insert
- Jede Datei als atomare Einheit speichern
- Latenzen pro Datei + Gesamtdurchsatz
- EDR (Effective Deduplication Ratio) berechnen: `EDR = B_logical / (B_phys / N)`

### Stufe 3: Per-File Delete + Reclamation
- Einzeln loeschen + Maintenance (VACUUM FULL, Compaction, etc.)
- TRIM/Discard fuer Block-Storage-Reclamation
- Physische Groesse erneut messen

## Datensaetze

| Typ | Quelle | Beschreibung |
|-----|--------|-------------|
| Event-Streams | GH Archive | GitHub-Timeline Events |
| Bilder | NASA (Hubble Ultra Deep Field) | Grosse Binaerdaten |
| Video | Blender Open Movies (Big Buck Bunny, Sintel, Tears of Steel) | Sehr grosse Binaerdaten |
| Volltext | Project Gutenberg (Pride and Prejudice, Moby-Dick) | Plain-Text |
| SQL-Datentypen | Synthetisch | UUID, JSON/JSONB, Zeitstempel, Numerik |
| Banktransaktionen | Synthetisch | Gemischte Workloads |

## Projektstruktur

```
dedup-database-analysis/
  docs/                  # LaTeX-Quelldateien (ZIH Vorlage)
  src/
    loaders/             # Daten-Lader pro System
    reporters/           # Metrik-Sammlung + Reporting
    cleanup/             # Maintenance/Reclamation-Skripte
  k8s/
    base/                # Gemeinsame K8s-Ressourcen (Namespace, RBAC, PVCs)
    jobs/                # CronJobs/Jobs pro Experiment-Stufe
  scripts/               # Hilfsskripte (Daten-Download, Duplikation-Generator)
  .gitlab-ci.yml         # CI/CD Pipeline fuer automatisierte Experimente
  credentials.env.example  # Template fuer lokale Credentials
  .credentials.env       # Echte Credentials (gitignored, NICHT im Repo!)
```

## Credentials einrichten

Die Datenbank-Credentials werden NICHT im Repository gespeichert. Stattdessen:

1. **Template kopieren:**
   ```bash
   cp credentials.env.example .credentials.env
   ```

2. **Passwörter eintragen** (aus GitLab CI/CD Variables oder Cluster-Dokumentation):
   ```bash
   # .credentials.env ausfuellen (KEY=VALUE Format)
   DEDUP_LAB_PASSWORD=<dein-passwort>
   DEDUP_PG_PASSWORD=<dein-passwort>
   # ...
   ```

3. **Verwendung im Code:**
   ```bash
   # Shell: Variablen laden
   source .credentials.env

   # K8s: secrets.yaml mit envsubst befuellen
   envsubst < k8s/base/secrets.yaml | kubectl apply -f -
   ```

4. **In GitLab CI/CD:** Die Variablen werden unter Settings > CI/CD > Variables
   als Protected/Masked Variables konfiguriert und automatisch injiziert.

**Sicherheit:** `.credentials.env` ist in `.gitignore` eingetragen und wird
niemals committed. Das Template `credentials.env.example` enthaelt nur leere
Schluessel ohne Werte und ist sicher versionierbar.

## Remotes

| Remote | URL | Zweck |
|--------|-----|-------|
| github | https://github.com/BenniProbst/dedup-database-analysis.git | Backup + Public |
| gitlab | git@gitlab-push:comdare/research/dedup-database-analysis.git | CI/CD + K8s Integration |

## Relevante Forschung (2025-2026)

- **DEDUPKV** (ICS 2025): Fine-Grained Dedup in LSM-tree KV Stores
- **Fu et al.** (ACM Computing Surveys 2025): Distributed Data Deduplication Survey
- **EDBT 2025**: Benchmarking Write Amplification in LSM Engines
- **OpenZFS Fast Dedup** (2024-2025): Storage-Layer Dedup mit DDT Log
- **arXiv 2602.12669** (Feb 2026): LSM-tree Compaction via LLM Inference
