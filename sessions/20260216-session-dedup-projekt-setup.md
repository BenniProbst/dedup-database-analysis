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

## Naechste Schritte (PRIORISIERT, ab Session 3)

1. **Agent a291b1d Ergebnis abholen** (Quellen-Webrecherche)
2. **INITIAL COMMIT + PUSH** auf GitHub + GitLab (alle Fixes + Kartografie)
3. **CI PIPELINE DUAL:** LaTeX-Kompilierung + DB-Test-Pipeline einrichten
4. **Phase 2 Inhalt:** Ungenutzte bib-Eintraege im Text einarbeiten (DuckDB, Cassandra, etc.)
5. **Redcomponent-DB → comdare-DB** Entscheidung einholen
6. **MESSSYSTEM:** Basierend auf konsolidierter EN-Version aufbauen

## Technische Notizen

### GitLab API via pve1 (funktionierender Aufruf)
```bash
ssh pve1 "curl -sk -X POST 'http://10.0.40.5/api/v4/projects' \
  --header 'PRIVATE-TOKEN:  [GITLAB-TOKEN-REDACTED]' \
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
