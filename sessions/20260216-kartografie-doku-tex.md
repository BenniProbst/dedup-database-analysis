# Kartografie: doku.tex (EN) — Praezise Dokumentanalyse
**Datum:** 2026-02-16, ~22:00 UTC
**Projekt:** dedup-database-analysis
**Basis-Dokument:** `doku.tex` (46.870 B, Original EN, Betreuer: Dr. Alexander Krause)
**Neueste Version:** `20260216 doku.tex` = `20260127 doku_updated.tex` (54.168 B, identischer Inhalt)

---

## A. DOKUMENTVERSIONEN IM UEBERBLICK

| Datei | Groesse | Datum | Sprache | Betreuer | Kapitel | Zitationen |
|-------|---------|-------|---------|----------|---------|------------|
| `doku.tex` | 46.870 B | Jan 27 21:28 | EN | Dr. Alexander Krause | 6 (kein Experiment) | KAPUTT: `contentReference[oaicite:N]` |
| `20260127 doku_updated.tex` | 53.852 B | Jan 27 22:55 | EN | Dr. Alexander Krause | 7 (+Experiment) | KORREKT: `\cite{}` |
| `20260216 doku.tex` | 54.168 B | Feb 16 21:44 | EN | Dr. Alexander Krause | 7 (+Experiment) | KORREKT: `\cite{}` (=0127 updated) |
| `doku_de.tex` | 49.912 B | Dez 7 16:44 | DE | (aelter) | ? | ? |
| `20260127 doku_de_updated.tex` | 57.063 B | Jan 27 22:55 | DE | Dr. Sebastian Goetz | 7 (+Experiment DE) | `\cite{}` |
| `20260108 doku_updated.tex` | 32.820 B | Jan 8 08:17 | EN | ? | ? (kleiner!) | ? |

### KRITISCHER BEFUND
- **`20260216 doku.tex` = `20260127 doku_updated.tex`** (User hat 0127 einfach umbenannt)
- **`doku.tex` (Original) ist NICHT kompilierbar!** Enthaelt `contentReference[oaicite:N]{index=N}` statt `\cite{}` — das sind ChatGPT-Artefakte, KEIN gueltiges LaTeX
- **Die "updated" Version hat die Zitate repariert** und Kapitel 6 (Experiment) hinzugefuegt
- **Betreuer-Wechsel nur in DE Version:** EN bleibt Dr. Alexander Krause, DE wechselt zu Dr. Sebastian Goetz

---

## B. KAPITELSTRUKTUR: `doku.tex` (Original EN, 46.870 B)

### Abstract (unnummeriert)
- 8 Saetze: Problem, Ziel, Ansatz, Ergebnisse, Auswirkungen
- Gut formuliert, deckt alle Aspekte ab
- Kernaussage: "adaptive, scalable deduplication strategies for future storage systems"

### Kapitel 1: Introduction and Motivation (Z.49-53)
- Label: `ch:einleitung`
- Motivation: Redundante Daten, Kostenersparnis, Bandbreite
- Roadmap des Papers: Terminologie → Historie → Taxonomie → Anwendungen → Evaluation
- **Qualitaet:** Solide, gut strukturierte Einfuehrung

### Kapitel 2: Terminology and Differentiation (Z.55-63)
- Label: `ch:begriffe`
- Deduplication vs. Compression (LZ77/LZ78 = innerhalb, Dedup = uebergreifend)
- Single Instance Storage (SIS) = File-Level Spezialfall
- Abgrenzung zu "Data Cleansing" / semantischer Duplikaterkennung
- Wirksamkeit abhaengig von Datennatur (verschluesselt = wenig Potenzial)
- **Qualitaet:** Praezise, klare Abgrenzungen

### Kapitel 3: Historical Development (Z.65-79)
- Label: `ch:historie`
- **LBFS** (Muthitacharoen 2001) — File-System-Level, Bandbreitenreduktion >10x
- **Venti** (Quinlan & Dorward 2002) — Content-Addressable Storage, Write-Once Store
- **DDFS/Data Domain** (Zhu 2008) — Summary Vector, Stream-Informed Segment Layout, Locality Preserved Caching; >99% Disk-Reads vermieden, >100 MB/s inline
- **iDedup** (Srinivasan 2012) — Latenz-sensitiv fuer Primary Storage; 60-70% Savings, 2-4% Latenz, <5% CPU
- **FastCDC** (Xia 2016) — 10x schneller als Rabin-based CDC, gleiche Dedup-Ratio
- **Sparse Indexing** (Lillibridge 2009) — Petabyte-Scale mit limitiertem RAM
- Surveys: Paulo & Pereira 2014, Fu et al. 2025
- **Qualitaet:** Hervorragend! Chronologisch, mit konkreten Zahlen, gut referenziert

### Kapitel 4: Taxonomy of Deduplication Techniques (Z.81-95)
- Label: `ch:taxonomie`
- **5 Dimensionen:**
  1. **Detection Principle:** Hash-based (SHA-1/256) vs. Content-Aware; Byte-Verify bei Kollision
  2. **Granularity:** File-Level (SIS) vs. Block/Chunk-Level (4-128 KB); Fixed vs. Content-Defined Chunking (Rabin, FastCDC)
  3. **Timing:** Inline (sofort, spart sofort Platz) vs. Post-Process (offline, kein Write-Path-Impact)
  4. **Architecture:** Source-Side (Client dedupliziert vor Senden) vs. Target-Side (Storage-System dedupliziert)
  5. **Primary vs. Secondary:** Backup (10:1-50:1, Performance zweitrangig) vs. Primary (latenz-sensitiv, inline, 2:1-5:1)
- **Qualitaet:** Exzellente Systematik, akademisch sauber

### Kapitel 5: Deduplication in Various Data Storage Systems (Z.97-172)
- Label: `ch:bereiche`
- **4 Sektionen:**

#### 5.1 Relational Database Systems (Z.101-112)
- Oracle SecureFiles Dedup (11g+): LOB-Level, 90% Savings bei Email-Attachments
- PostgreSQL/MySQL: Kein eingebauter LOB-Dedup, Storage-Layer moeglich (ZFS, NTFS)
- Dictionary Compression in Column-Stores als verwandtes Konzept
- Dedup in RDBMS = Nische (nur LOBs, Backups)

#### 5.2 Object-Based Storage (Z.114-131)
- Ceph: Experimental Cluster-Wide Dedup (Chunk Pool, Fingerprint-Index)
- TiDedup (Oh 2023): 34% Storage-Reduktion, weniger I/O-Impact
- WORM-Charakter ideal fuer Inline-Dedup
- Security: Encryption vs. Dedup Conflict, Cross-Tenant Side-Channel

#### 5.3 Time Series Databases (Z.133-148)
- Problem: Duplicate Data Points (at-least-once), Unchanged Values, Recurring Patterns
- TimescaleDB: PK auf (time, series_id)
- QuestDB/ClickHouse: Dedup-Optionen bei Ingestion
- InfluxDB 1.x: Merge bei identischen Points
- InfluxDB 3.0 (IOx): Sort-Merge-Dedup auf Parquet
- Gorilla-Compression = implizite Dedup
- Dedup hier = Datenqualitaet + Record-Level Optimierung

#### 5.4 Scalable Systems: Apache Kafka and Hadoop (Z.150-172)
- **Kafka:** Log Compaction (letzte Version pro Key), EOS (Idempotent Producers), KEIN Content-Dedup
- **Hadoop/HDFS:** 3x Replikation (absichtlich!), Extreme Binning, ChunkStash (SSD-Index)
- Application-Level Dedup (MapReduce distinct/group by)
- Backup-on-Hadoop mit externem Dedup
- Native HDFS-Dedup nicht im Mainline

### Kapitel 6: Initial Evaluation and Research Goal (Z.174-193)
- Label: `ch:bewertung`
- **Evaluation-Matrix:**
  - Backup: 10:1-50:1, State of the Art
  - Primary/DB: 2:1-5:1, selektiv, latenz-sensitiv
  - Cloud/BigData: Anfangsstadium, verteilte Indizes noetig
  - Kafka/TSDB: Limitiert, Konsistenz > Speicherersparnis
- **4 Forschungsfragen:**
  1. Adaptive Strategie fuer heterogene Workloads
  2. Verteilter globaler Dedup ohne zentralen Index
  3. Dedup + Encryption (Convergent Encryption)
  4. Fallstudie in konkretem System (Kafka Tiered Storage / HDFS)
- **Uebergeordnetes Ziel:** "adaptive, scalable deduplication concept...minimize redundancy without significantly impairing system performance or security"

---

## C. DELTA: `20260216 doku.tex` vs. `doku.tex` (Original)

### C.1 Zitationen REPARIERT
| Original (`doku.tex`) | Updated (`20260216 doku.tex`) |
|---|---|
| `:contentReference[oaicite:0]{index=0}` | Entfernt oder durch `\cite{...}` ersetzt |
| `:contentReference[oaicite:11]{index=11}` | `\cite{Muthitacharoen2001}` |
| `:contentReference[oaicite:13]{index=13}` | `\cite{Quinlan2002}` |
| `:contentReference[oaicite:15]{index=15}` | `\cite{Zhu2008}` |
| `:contentReference[oaicite:18]{index:18}` | `\cite{Srinivasan2012}` |
| `:contentReference[oaicite:25]{index:25}` | `\cite{Xia2016}` |
| Insgesamt 84 `contentReference` | Alle entfernt/ersetzt |

**ACHTUNG:** Einige Stellen haben noch Artefakte, z.B. Zeile 77:
```
Paulo & Pereira 2014: and Fu et al.~\cite{Fu2025}:
```
Die Doppelpunkte nach "2014" und nach `\cite{Fu2025}` sind Ueberreste der alten `contentReference`-Entfernung. **MUSS BEREINIGT WERDEN.**

### C.2 Formatierung korrigiert
- `**bold**` Markdown → `\textbf{bold}` LaTeX (in Kapitel 4 Taxonomie)
- Konsistentere LaTeX-Nutzung

### C.3 NEUES KAPITEL 6: Experimental Design and Measurement Plan (Z.175-293)
- **Eingefuegt zwischen Kapitel 5 (Various Systems) und bisherigem Kapitel 6 (Evaluation)**
- Bisheriges Kapitel 6 wird zu Kapitel 7

#### 6.1 Testbed and storage configuration
- K8s Cluster, 4 Worker Nodes
- PVC 100 GiB, Longhorn StorageClass, Replica N=4
- Thin Provisioned: physisch waechst, schrumpft NICHT automatisch
- **Messung:** Logisch (DB-intern) + Physisch (`longhorn_volume_actual_size_bytes`)

#### 6.2 Systems under test
- PostgreSQL, MariaDB, Kafka, MinIO, Redcomponent-DB (Basis)
- ClickHouse, CockroachDB (zusaetzlich)
- **HINWEIS:** "Redcomponent-DB" muss zu "comdare-DB" umbenannt werden

#### 6.3 Data sets and payload types
- GH Archive Events (Event-Stream)
- NASA Bilder (Large Binary Objects, Hubble Ultra Deep Field)
- Blender Videos (Big Buck Bunny, Sintel, Tears of Steel)
- Project Gutenberg Texte (Pride & Prejudice, Moby-Dick)
- UUID/GUID Identifiers
- JSON/JSONB Payloads
- (Bank-Transaktionen, Random Numbers — bereits vorhanden)

#### 6.4 Workload definition
- **3 Duplikationsgrade:** U0 (unique), U50 (50%), U90 (90%)
- **3 Stufen:**
  - Stage 1: Bulk-Insert (idiomatisch pro System)
  - Stage 2: Per-File Insert (einzelne Transaktionen, Latenz-Messung)
  - Stage 3: Per-File Delete + Reclamation (VACUUM FULL, Compaction, etc.)
- **EDR-Formel:** EDR = B_logical / (B_phys / N), N=4 Replicas
  - EDR ≈ 1 = kein Dedup
  - EDR > 1 = Storage-Layer Dedup aktiv

---

## D. BIBLIOGRAFIE-ANALYSE

### D.1 `doku.bib` (Original, 15.234 B, Jan 27)
**13 Kern-Papers + 19 DB-Docs = 32 Eintraege**

| Kategorie | Eintraege |
|-----------|-----------|
| Kern-Dedup-Papers | Muthitacharoen2001, Quinlan2002, Zhu2008, Srinivasan2012, Xia2016, Lillibridge2009, Paulo2014, Fu2025, Oh2023 |
| Oracle | Oracle2025 |
| InfluxDB | InfluxDB2023 |
| Kafka | Confluent2023 |
| Architektur | Hellerstein2007 |
| DuckDB | RaasveldtMuhleisen2019DuckDB, DuckDBConstraints, DuckDBIndexes, DuckDBStorageInternals, DuckDBFSSTPaper |
| PostgreSQL | PostgresTOAST, PostgresBtree, Postgres13ReleaseNotes |
| ClickHouse | SchulzeEtAl2024ClickHouse, ClickHouseLowCardinality, ClickHouseReplacingMergeTree, ClickHouseDedupRetries |
| Cassandra | LakshmanMalik2010Cassandra, CassandraDDLPrimaryKey, CassandraTombstones |
| MongoDB | SchultzDemirbas2025MongoTxns, MongoUniqueIndexes, MongoShardedUnique, MongoWiredTiger |

**PROBLEM:** Verwendet `@online{...}` Entry-Type — BibTeX kennt `@online` NICHT, nur BibLaTeX! Muss auf `@misc` geaendert werden fuer Standard-BibTeX-Kompilierung.

### D.2 `20260216 doku.bib` (= `20260127 doku_updated.bib`, 15.578 B)
**Gleiche 32 Eintraege wie doku.bib**, aber:
- `@online{...}` → `@misc{...}` (KORREKT fuer BibTeX!)
- Alphabetisch sortiert
- Zusaetzlich: GHArchive2026, KafkaTopicConfig2026, LonghornConcepts2026, LonghornMetrics2026, MinIOGitHub2026, NASAImagesLibrary2026, NASAMediaGuidelines2026, NASAUltraDeepField2026, PostgresVacuum2026, ProjectGutenbergTerms2026, TaftEtAl2020CockroachDB, WikimediaBigBuckBunny2026, WikimediaSintel2026, WikimediaTearsOfSteel2026
- **= 46 Eintraege total** (14 mehr als Original, alle fuer Kapitel 6 Experiment)

### D.3 `20260108 doku_updated.bib` (GROESSTE, 22.707 B)
**ENTHALT 30+ ZUSAETZLICHE EINTRAEGE** die in keiner anderen Bib sind:

| Key | Beschreibung | Relevanz |
|-----|-------------|----------|
| Douceur2002 | Convergent Encryption (ICDCS) | HOCH — Dedup+Encryption Thema |
| Harnik2010 | Side Channels in Cloud Dedup | HOCH — Security-Aspekt |
| Bhagwat2009 | Extreme Binning (MASCOTS) | MITTEL — im Text erwaehnt |
| Debnath2010ChunkStash | ChunkStash SSD-Index (USENIX ATC) | MITTEL — im Text erwaehnt |
| Pelkonen2015Gorilla | Gorilla TSDB (VLDB) | HOCH — TSDB-Kompression |
| ONeil1996LSM | LSM-Tree Seminal Paper (Acta Informatica) | HOCH — Grundlagen-Paper |
| Weil2006Ceph | Ceph Original Paper (OSDI) | HOCH — Ceph-Sektion |
| Kreps2011Kafka | Kafka Original Paper (LinkedIn) | HOCH — Kafka-Sektion |
| Shvachko2010HDFS | HDFS Design Paper (IEEE MSST) | HOCH — HDFS-Sektion |
| Elmagarmid2007Duplicate | Duplicate Detection Survey (IEEE TKDE) | HOCH — Terminologie-Kap |
| Cooper2010YCSB | YCSB Benchmark Paper (SoCC) | MITTEL — Benchmark |
| TPCHSpec | TPC-H Benchmark | MITTEL — Benchmark |
| TPCDSSpec | TPC-DS Benchmark | MITTEL — Benchmark |
| RCDBInternal2026 | RCDB Internal Notes (unpublished) | NIEDRIG — Placeholder |
| ProbstTask2025 | Aufgabenstellung (internal) | NIEDRIG — Meta |
| QuestDBDedupDocs | QuestDB Dedup Docs | HOCH — TSDB-Sektion |
| InfluxDBDuplicatePoints | InfluxDB Duplicate Handling | HOCH — TSDB-Sektion |
| InfluxDBIngestCompression2023 | InfluxDB 3.0 Compression | MITTEL — ergaenzt InfluxDB2023 |
| WindowsServerDedupOverview | Windows Server Dedup | NIEDRIG — Randnotiz |
| KafkaDesignCompaction | Kafka Log Compaction Design | MITTEL — ergaenzt Confluent2023 |
| HdfsDesign | HDFS Architecture Guide | MITTEL — HDFS-Sektion |
| OracleAdvancedCompression | Oracle Tech Brief (Duplikat?) | NIEDRIG — bereits Oracle2025 |
| PostgresMVCC | PostgreSQL MVCC Docs | NIEDRIG — nicht im Text |
| PostgresConstraints | PG Constraints Docs | NIEDRIG — nicht im Text |
| PostgresUniqueIndexes | PG Unique Indexes | NIEDRIG — nicht im Text |
| PostgresInsertOnConflict | PG INSERT ON CONFLICT | NIEDRIG — nicht im Text |
| ClickHouseMergeTree | CH MergeTree Docs | MITTEL — ergaenzt CH-Sektion |
| ClickHouseDedupStrategies | CH Dedup Strategies | MITTEL — ergaenzt CH-Sektion |
| KafkaProducerConfigs | Kafka Producer Configs | NIEDRIG — Detail |
| KafkaKIP98 | Kafka EOS Design (KIP-98) | MITTEL — EOS im Text erwaehnt |

**EMPFEHLUNG:** Alle HOCH-Eintraege in die konsolidierte `doku.bib` uebernehmen!

---

## E. BEKANNTE FEHLER / PROBLEME

### E.1 Zitations-Artefakte (MUSS BEHOBEN WERDEN)
Stellen in `20260216 doku.tex` wo `contentReference` nicht sauber entfernt wurde:
- Z.77: `Paulo & Pereira 2014: and Fu et al.~\cite{Fu2025}:` — Doppelpunkte + fehlende `\cite{Paulo2014}`
- Z.166: `...mappers/reducers (e.g. using a \texttt{distinct} or \texttt{group by}): –` — Doppelpunkt-Artefakt
- Z.302: `...Fu et al.~\cite{Fu2025}:)` — Doppelpunkt-Artefakt

### E.2 Redcomponent-DB → comdare-DB
- Z.201: `\textbf{Redcomponent-DB}` — alter Markenname
- Z.204: `beyond the new Redcomponent-DB` — alter Markenname
- **ENTSCHEIDUNG NOETIG:** Akademisch den aktuellen Namen verwenden? Oder historisch beibehalten?

### E.3 `\bibfiles{doku}` Referenz
- Beide .tex Dateien referenzieren `doku.bib` via `\bibfiles{doku}`
- Die konsolidierte bib MUSS `doku.bib` heissen

### E.4 BibTeX Entry-Type Kompatibilitaet
- Original `doku.bib` verwendet `@online{...}` — NUR BibLaTeX, NICHT BibTeX!
- `20260216 doku.bib` verwendet `@misc{...}` — KORREKT fuer Standard-BibTeX
- `zihpub.cls` verwendet wahrscheinlich Standard-BibTeX → `@misc` ist richtig

### E.5 Fehlende Zitationen im Text
Die folgenden bib-Eintraege werden NIRGENDWO im Text zitiert (sind aber in doku.bib):
- DuckDB-Eintraege (5x) — DuckDB wird im Text nicht erwaehnt!
- PostgreSQL-Eintraege (3x) — PG B-Tree Dedup etc. nicht im Text
- ClickHouse-Eintraege (3x) — CH Dedup etc. nicht im Text
- Cassandra-Eintraege (3x) — Cassandra nicht im Text
- MongoDB-Eintraege (4x) — MongoDB nicht im Text
- Hellerstein2007 — Architektur-Referenz, nicht zitiert
- **FAZIT:** Diese Quellen sind fuer zukuenftige Erweiterungen vorbereitet, aber derzeit ungenutzt

---

## F. CLUSTER_NFS TESTDATEN (VERBINDLICH!)

### Pfad: `\\BENJAMINHAUPT\Cluster_NFS`
### REGEL: Daten DUERFEN NIE GELOESCHT WERDEN! Nur in tmp/ Ordner entpacken und nutzen.

| Datei/Ordner | Groesse | Beschreibung |
|---|---|---|
| `bankdataset.xlsx.zip` | 32 MB | Bank-Transaktionsdaten |
| `gharchive/` | (Ordner) | GitHub Archive Event-Daten |
| `github-repos-bigquery/` | (Ordner) | GitHub Repos BigQuery Export |
| `million_post_corpus.tar.bz2` | 105 MB | Million Post Corpus (Text) |
| `random-numbers.zip` | 271 MB | Synthetische Zufallszahlen |
| `cluster-backups/` | (Ordner) | Cluster-Backups (NICHT ANFASSEN!) |

**Arbeitsweise:**
1. Nur in `\\BENJAMINHAUPT\Cluster_NFS\tmp\` temporaer entpacken
2. Entpackte Daten nach Nutzung aus tmp/ entfernen
3. Originaldaten NIEMALS loeschen oder ueberschreiben

---

## G. KONSOLIDIERUNGSSTRATEGIE

### Phase 1: Sofortige Fixes (vor erstem Commit)
1. `20260216 doku.tex` → `doku.tex` ersetzen (als Arbeitskopie)
2. Zitations-Artefakte bereinigen (Z.77, Z.166, Z.302)
3. HOCH-Eintraege aus `20260108 doku_updated.bib` in `doku.bib` mergen
4. `@online` → `@misc` sicherstellen in `doku.bib`

### Phase 2: Inhaltliche Verbesserung (schrittweise)
1. Ungenutzte bib-Eintraege (DuckDB, PG, CH, Cassandra, MongoDB) im Text einarbeiten:
   - Neue Sektion in Kap. 5 oder Erweiterung bestehender Sektionen
   - DuckDB als Column-Store mit FSST-Compression
   - Cassandra mit LSM-Tree Compaction
   - MongoDB mit WiredTiger Compression
2. "Redcomponent-DB" → "comdare-DB" (User-Entscheidung einholen)
3. Fehlende `\cite{Paulo2014}` in Z.77 einfuegen

### Phase 3: CI-Pipeline
1. LaTeX-Kompilierung bei jedem .tex/.bib Commit
2. DB-Test-Pipeline separat, manuell triggerbar

---

## H. NAECHSTE SCHRITTE (PRIORISIERT)

1. **[LAEUFT]** Quellen-Webrecherche (Agent a291b1d verifiziert alle URLs)
2. **[NAECHSTES]** Phase 1 Fixes ausfuehren (doku.tex ersetzen, Artefakte bereinigen, bib mergen)
3. **[DANN]** Initial Commit + Push (GitHub + GitLab)
4. **[DANN]** Dual CI-Pipeline einrichten (LaTeX + DB-Test)
5. **[SPAETER]** Phase 2 inhaltliche Erweiterung starten
