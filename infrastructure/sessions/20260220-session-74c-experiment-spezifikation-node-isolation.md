# Session 74c: Experiment-Spezifikation — Node-Isolation + Sequentielle DB-Tests

**Datum:** 2026-02-20
**Kontext:** Fortsetzung von Session 74b (Experiment-DB-Rebuild + CI-Scheduling)
**Projekt:** dedup-database-analysis (GitLab Project ID 280)
**Pipeline:** #1882 (C++ Jobs SUCCESS, experiment:run MANUAL bereit)

---

## 1. User-Anforderungen (Experiment-Ausfuehrungsspezifikation)

### 1.1 Datenbank-Isolation
- Fuer den Zeitraum JEDES Tests wird eine K8s-Node **komplett befreit** (drain)
- Deren Pods werden auf andere Nodes verteilt
- **Inklusive** Monitoring und Programme, die mit der DB interferieren koennen
- NUR die getestete Datenbank und das NFS fuer Testdaten bleiben auf der Node

### 1.2 Dediziertes NFS
- Ein dediziertes NFS mit **Replika 1** und **Pod 1**
- Von dem die Daten auf der Node geladen und integriert werden
- Das NFS ist als einziges Programm neben der DB auf der Node

### 1.3 Testdaten-Vorbereitung
- Daten von externer NFS-Platte (NAS: 192.168.178.31) kopieren
- Auf Longhorn temporaeren Speicher entpacken
- Groesse der Daten je Datensatz vorbereiten
- Datensaetze: GH Archive, GitHub Repos BigQuery, Bankdataset, Million Post Corpus,
  NASA Imagery, Gutenberg Texte, Blender Videos, Random Numbers

### 1.4 Sequentielle Testdurchfuehrung
- Test jeder einzelnen Datenbank **SEQUENTIELL** mit dem Experiment-Programm
- Die Last wird je Datenbank **isoliert** ueberwacht
- Ueber eine GitLab Pipeline in sequentiellen Batches laufen lassen
- Nach JEDEM Run die Messwerte exportieren

### 1.5 Wiederholungen
- Jeder Einzeltest laeuft **3 mal**

### 1.6 Runner-Deaktivierung
- GitLab Runner auf K8s und Bare Metal fuer die Zeit der Experimente
  **temporaer deaktiviert**
- Nur der Experiment-Runner selbst laeuft auf der isolierten Node

### 1.7 Build-Tests (NICHT betroffen)
- Der automatisierte Build-Test (500 Files, 1 MB) bleibt unveraendert
- Dies ist NICHT Teil des Experiments

---

## 2. Experiment-Matrix (aus doku.tex)

### 2.1 Datenbank-Systeme (7)
1. PostgreSQL (4 independent primaries, HA)
2. CockroachDB (4 Pods, TLS, shared production)
3. MariaDB (3 Replicas)
4. ClickHouse (1 Replica)
5. Redis (Standalone, ACL)
6. Kafka (Strimzi, KRaft, 1 Broker)
7. MinIO (4 Pods)

### 2.2 Payload-Typen (9+)
Synthetisch: random_binary, structured_json, text_document, uuid_keys, jsonb_documents
Real-World: github_events, nasa_image, gutenberg_text, blender_video
NAS: github_repos_content, bank_transactions, forum_posts

### 2.3 Duplikationsgrade (3)
U0 (0%), U50 (50%), U90 (90%)

### 2.4 Stages pro Test (4)
1. Bulk Insert
2. Per-File Insert (mit Latenz-Messung)
3. Per-File Delete
4. Maintenance (VACUUM/OPTIMIZE/COMPACT)

### 2.5 Wiederholungen: 3

### 2.6 Gesamtmatrix
- 7 DBs × (9+ Payload-Typen × 3 Grades × 4 Stages) × 3 Wiederholungen
- = 7 × 108+ × 3 = **2.268+ Einzelmessungen**

---

## 3. Infrastruktur-Anforderungen

### 3.1 Node-Isolation Workflow (pro DB-Test)
```
1. Node auswaehlen (z.B. talos-lux-kpk)
2. kubectl cordon <node>    # Keine neuen Pods
3. kubectl drain <node> --ignore-daemonsets --delete-emptydir-data
   # Alle Pods evakuieren (ausser DaemonSets)
4. DB-Pod + NFS-Pod auf isolierter Node starten (nodeSelector/affinity)
5. Experiment ausfuehren (3x)
6. Messwerte exportieren
7. kubectl uncordon <node>   # Node wieder freigeben
8. Naechste DB...
```

### 3.2 Dediziertes NFS-Setup
- Longhorn PVC: 100 Gi, Replika 1 (NICHT 4!)
- NFS-Pod: nfs-ganesha oder nfs-server-provisioner
- nodeSelector: auf isolierte Node pinnen
- Testdaten vorab draufkopieren

### 3.3 Runner-Management
- K8s Runner (ID 6): Deaktivieren via GitLab API
- Bare-Metal Runner (ID 7, 8, 9): Deaktivieren via GitLab API
- Experiment laeuft ueber dediziertes Script, NICHT ueber GitLab CI Runner
  ODER: spezieller Runner nur auf isolierter Node

---

## 4. Offene Fragen
- Welche Node soll isoliert werden? (Empfehlung: talos-lux-kpk, meiste Kapazitaet)
- Soll Longhorn auf der isolierten Node auch gestoppt werden?
  (Waere konsequent, interferiert aber mit dem NFS-PVC)
- Wie werden die Messwerte exportiert? (Kafka Topics, JSON Dateien, Git Push?)
- Soll ein eigener Runner auf der isolierten Node laufen?

---

## 5. Aktueller Status
- **Pipeline #1882:** C++ Build+Tests SUCCESS, experiment:build SUCCESS
- **7/7 DB-Systeme:** Verifiziert, Connectivity OK
- **Lab User:** dedup-lab (Passwort: S-c17LvxSx1MzmYrYh17) auf allen DBs
- **CI-Variablen:** 5 masked Variables gesetzt
- **Auth-Fixes:** Redis ACL + ClickHouse HTTP (Commit 6eed9d4)
- **CI-Fix:** docker:build Blocking (Commit 46cf078)
- **Testdaten:** NAS verfuegbar (~17 GB), PVC noch NICHT erstellt

---

## 6. Fortschritt (Kontext-Ende)

### 6.1 Erledigt in dieser Session
- Pipeline #1879: docker:build blockierte experiment:build → Fix: `needs: []` + docker:build Rules
- Pipeline #1882: ALLE C++ Jobs SUCCESS (build 292s, smoke 81s, dry-test 51s)
- experiment:build SUCCESS, experiment:run = MANUAL (bereit)
- doku.tex Analyse: Signifikante Luecken identifiziert (Real-World-Daten fehlen)
- Session-Doku 74c erstellt mit Experiment-Spezifikation
- Neuer Plan `experiment-node-isolation-plan.md` erstellt (bestehende Plaene NICHT ueberschrieben)

### 6.2 Infrastruktur erstellt
- **StorageClass `longhorn-experiment`:** Replika 1, Delete-Policy, dataLocality best-effort
- **PVC `dedup-nfs-data`:** 100 Gi, Bound, longhorn-experiment
  - Volume: pvc-8a92f1aa-e368-471f-a7e7-d7c6bb380554
- **Pod `dedup-staging`:** Running auf talos-qkr-yc0, /data gemountet
  - IP: 10.130.50.151

### 6.3 Naechste Schritte (fuer naechste Session)
1. **NAS-Daten auf PVC kopieren** (~17 GB):
   - gharchive (500 Dateien, ~10 GB)
   - github-repos-bigquery/sample_contents (5.9 GB)
   - bankdataset, million_post_corpus, random-numbers (extrahieren)
   - Internet-Downloads: NASA TIFF, Gutenberg, Blender
2. **C++ Code anpassen:**
   - Neue PayloadTypes (GITHUB_REPOS_CONTENT, BANK_TRANSACTIONS, FORUM_POSTS)
   - load_from_cache_dir() Funktion
   - Chunking fuer grosse Dateien
   - --repetitions 3 CLI-Flag
3. **CI Pipeline anpassen:**
   - PVC-Mount in experiment:run
   - Timeout 8h
   - Sequential DB-Testing
4. **Node-Isolation Workflow implementieren:**
   - Drain/Cordon Script
   - NFS-Server Pod mit nodeSelector
   - Runner-Deaktivierung via GitLab API

### 6.4 Git Status
```
46cf078 fix: remove src/cpp/** from docker:build trigger to prevent pipeline blocking
99a6af8 fix: decouple experiment:build from docker-build stage with needs: []
6eed9d4 fix: add auth support for Redis ACL + ClickHouse HTTP API
```

### 6.5 Plaene (NICHT ueberschreiben!)
- `~/.claude/plans/jazzy-gliding-fiddle.md` — Erster Testdaten-Plan (veraltet, aber behalten)
- `~/.claude/plans/experiment-node-isolation-plan.md` — Neuer Plan mit Node-Isolation
- `~/.claude/plans/scalable-imagining-torvalds.md` — Torvalds v7 (Cluster-Infra)
- `~/.claude/plans/typed-bubbling-kahan.md` — Kahan-Plan (Projekte/Impl)
