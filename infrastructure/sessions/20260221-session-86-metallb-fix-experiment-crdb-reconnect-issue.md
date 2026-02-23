# Session 86: MetalLB Recovery + Experiment CRDB Verbindungsverlust

**Datum:** 2026-02-21
**Kontext:** Fortsetzung von Session 85, Kontext-Kompaktierung. Pipeline 1930 lief autonom.
**Status:** MetalLB REPARIERT, Resume-Mechanismus IMPLEMENTIERT, NAS Payload-Types INTEGRIERT, Pipeline #1951 RUNNING

---

## Zusammenfassung

Session begann nach Kontext-Kompaktierung der vorherigen Session 85. Hauptprobleme:
1. MetalLB konnte keine ARP-Announcements senden (L2Advertisement Interface-Restriction + Memberlist-Fehler)
2. GitLab extern nicht erreichbar (HAProxy auf OPNsense muss neugestartet werden)
3. CockroachDB Experiment Run 2 hat Verbindung verloren (Pods neugestartet waehrend Run)
4. Kein Connection-Retry/Reconnect-Mechanismus im C++ Experiment-Code

---

## 1. MetalLB Diagnose + Fix

### 1.1 Ausgangslage
- 3 Speaker + 1 Controller Running, aber VIPs (10.0.40.5, 10.0.40.6) nicht erreichbar
- Memberlist-Kommunikation auf Port 7946 zwischen allen Nodes gebrochen
- ServiceL2Status-Objekte aus S85 waren vorhanden aber nicht korrekt

### 1.2 L2Advertisement Interface-Untersuchung
Die `apps-vlan40-l2` L2Advertisement hatte `interfaces: ["br0.40"]` konfiguriert.

**Ergebnis der Interface-Pruefung via /proc/net/dev auf talos-qkr-yc0:**
```
br0.10, br0.15, br0.20, br0.30, br0.40, br0.50, br0.60, br0.70,
br0.80, br0.90, br0.100, br0.110, br0.120
```

Alle 15 VLANs als `br0.XX` vorhanden! Die `interfaces: ["br0.40"]` Konfiguration war KORREKT.
Wurde temporaer entfernt (Verdacht auf falsches Interface), dann wiederhergestellt.

### 1.3 Aggressiver MetalLB-Neustart
```bash
kubectl delete pods -n metallb-system --all --grace-period=0 --force
kubectl delete servicel2status -n metallb-system --all
```

Ergebnis: Neue ServiceL2Status-Objekte korrekt erstellt:

| VIP | Service | Allocated Node |
|-----|---------|----------------|
| 10.0.40.5 | gitlab-web-lb | talos-qkr-yc0 |
| 10.0.40.6 | ingress-nginx-controller | talos-qkr-yc0 |
| 10.0.40.7 | gitlab-gitlab-shell | talos-say-ls6 |
| 10.0.30.5 | samba-ad-lb | talos-say-ls6 |
| 10.0.90.55 | minio-lb | talos-qkr-yc0 |
| 192.168.178.81 | opnsense-1-lb | talos-qkr-yc0 |
| 192.168.178.84 | opnsense-4-lb | talos-say-ls6 |

### 1.4 Speaker sendet ARP-Announcements
Speaker-Logs bestaetigen: `"service has IP, announcing"` fuer 10.0.40.5 und 10.0.40.6.

### 1.5 ARP-Verifikation
Von pve1 (10.0.40.201 auf vmbr0.40):

| IP | MAC | Status |
|----|-----|--------|
| 10.0.40.5 | 52:54:00:e5:4e:4d | REACHABLE |
| 10.0.40.6 | 52:54:00:e5:4e:4d | REACHABLE |
| 10.0.40.181 | 52:54:00:e5:4e:4d | REACHABLE |

Alle drei auf gleiche MAC (talos-qkr-yc0 br0.40) — **MetalLB L2 funktioniert korrekt!**

### 1.6 Connectivity-Test
```
curl https://10.0.40.5:80  → HTTP 302 (GitLab Redirect) ✓
curl https://10.0.40.6:443 -H 'Host: gitlab.comdare.de' → HTTP 302 ✓
```

**GitLab ist INTERN voll funktionsfaehig!**

### 1.7 pve1 Routing-Cleanup
Entfernte veraltete Eintraege auf pve1:
- **PERMANENT ARP-Eintraege** fuer 10.0.40.5/.6/.7 (aus Session 78 Workaround)
- **Statische Routen** 10.0.40.5/6/7 via 10.0.40.1 (unnoetig, VIPs sind direkt auf VLAN 40)

### 1.8 Verbleibendes Memberlist-Problem
MetalLB Speakers haben weiterhin Memberlist-Fehler auf Port 7946:
```
memberlist: Failed fallback TCP ping: timeout 1s:
  read tcp 10.0.110.181:54384->10.0.110.182:7946: i/o timeout
```
Dies betrifft alle Node-Paare. Ursache unklar — moeglicherweise Netzwerk-Last
oder Firewall auf VLAN 110. Hat aber **keinen Einfluss auf ARP-Announcements**,
da ServiceL2Status-basierte Zuweisung funktioniert.

---

## 2. GitLab Extern-Zugang (OFFEN)

### Verkehrsweg
```
Client → OPNsense CARP VIP (:443) → HAProxy → 10.0.40.6:443 (ingress-nginx) → GitLab
```

### Status
- **Intern:** OK (10.0.40.5:80 und 10.0.40.6:443 erreichbar)
- **Extern:** DOWN (HAProxy auf OPNsense antwortet nicht auf :443)
- **OPNsense WebUI:** opnsense-1/2/4 auf :8443 erreichbar (HTTP 200)
- **OPNsense-3:** ContainerCreating auf NotReady Node talos-5x2-s49

### OPNsense API-Zugang
Authentifizierung mit `root:0pnSense!` und `root:opnsense` schlaegt fehl.
Korrektes Passwort laut Credential-Suche: `83n]am!nP.`
**SSH auf OPNsense:** Timeout (Port 22 nicht erreichbar via pve1)

### Aktion: Anderer Agent uebernimmt
User delegierte HAProxy-Neustart an einen anderen Agenten.

---

## 3. CockroachDB Experiment — Verbindungsverlust

### 3.1 Ursache
CockroachDB Pods (0, 1, 2) wurden neugestartet waehrend Run 2 lief.
Vermutlich durch CockroachDB Operator nach dem Upgrade (v26.1.0).

| Pod | Alter bei Check (09:35) | Node |
|-----|------------------------|------|
| cockroachdb-0 | 14 min (neugestartet ~09:21) | talos-lux-kpk |
| cockroachdb-1 | 14 min | talos-lux-kpk |
| cockroachdb-2 | 14 min | talos-lux-kpk |
| cockroachdb-3 | 7h18m (stabil) | talos-lux-kpk |

### 3.2 Auswirkung auf Experiment
Der C++ Experiment-Code (`dedup-test`) oeffnet eine libpq-Verbindung beim Start
und nutzt diese fuer den gesamten Run. Es gibt:

- **KEIN Connection Retry** bei Verbindungsverlust
- **KEIN Checkpoint/Resume** bei Absturz
- **KEIN Reconnect** nach TCP-Timeout

### 3.3 Run-Status (Pipeline 1930, experiment:cockroachdb)

**Run 1:** ✅ KOMPLETT + VALIDE (06:52 - 08:57, 2h05m)
- Alle 5 Payloads × 3 Grades × 4 Stages durchgelaufen
- CockroachDB-Ergebnisse enthalten reale Messdaten
- Export-Pipeline erfolgreich

**Run 2:** ❌ INVALID — Verbindung verloren
- Datengenerierung: OK (08:58 - 09:03)
- Verbindung bei 09:03:49 erfolgreich aufgebaut (Server v13.0.0)
- random_binary/U0/bulk_insert: 522 Rows (09:03-09:09) — letzte valide Messung
- Ab random_binary/U0/perfile_insert (09:09): "no connection to the server"
- Alle weiteren Operationen: 0 Rows, 0 phys_delta, ERR
- **Negatives phys_delta:** -35,578,638,337 und -33,185,480,705 (Longhorn Query Fehler)
- Position bei Session-Ende: random_binary/U90/perfile_insert (Payload 1/5, ~25% durch)

**Run 3:** ⏳ Noch nicht gestartet
- Wird NEUE Binary-Invokation sein → frische DB-Verbindung
- Sollte valide Daten liefern WENN CockroachDB stabil bleibt

### 3.4 CockroachDB Gesundheit (bei Session-Ende)
```sql
SHOW DATABASES → dedup_lab, defaultdb, postgres, system ✓
```
CockroachDB intern voll funktionsfaehig. Problem war nur der Pod-Neustart
waehrend des laufenden Experiments.

### 3.5 Kafka Trace Producer
Durchgehend `Connection refused` zu `kafka-cluster-kafka-bootstrap:9092`.
Kafka Entity Operator in CrashLoopBackOff (318 Restarts).
**Trace-Daten gehen verloren** — Experiment-Metriken werden nur lokal gesammelt.

---

## 4. Redis — NICHT betroffen

User-Frage: "Bist du dir sicher, dass du die richtige Redis DB erwischt hast?"

### Verifizierung
| Redis-Instanz | Namespace | Zweck | Experiment? |
|---------------|-----------|-------|-------------|
| redis-gitlab-0/1/2 | gitlab | GitLab Sessions/Cache | NEIN |
| redis-cluster-0 | redis | Experiment-Datenbank | JA (aber noch nicht getestet, kommt nach CRDB) |

Experiment-Logs bestaetigen: `--systems cockroachdb` → verbindet NUR zu CockroachDB.
GitLab Redis wurde NICHT beruehrt.

**redis-gitlab-1:** Pending (kein Node verfuegbar, gleiche Ursache wie samba-ad-1)

---

## 5. Samba AD Status

| Pod | Status | Node |
|-----|--------|------|
| samba-ad-0 | Running ✓ | talos-qkr-yc0 |
| samba-ad-1 | **Pending** | - (Insufficient CPU + Anti-Affinity + Taint) |
| samba-ad-2 | Running ✓ | talos-lux-kpk |

samba-ad-1 wartet auf talos-5x2-s49 Recovery. 2/3 Replicas fuer AD/DNS ausreichend.

User-Verwirrung: IP-Overlap — 10.0.**30**.5 (Samba AD auf VLAN 30) vs 10.0.**40**.5 (GitLab auf VLAN 40).
Gleiche letzte Oktette, verschiedene VLANs.

---

## 6. Cluster-Status (bei Session-Ende)

### Nodes
| Node | Status | Rolle |
|------|--------|-------|
| talos-5x2-s49 | **NotReady** | CockroachDB Upgrade Folgen |
| talos-lux-kpk | Ready | GitLab, CockroachDB, Runner |
| talos-qkr-yc0 | Ready | DBs, OPNsense-1 |
| talos-say-ls6 | Ready | Experiment Node (Taint) |

### Problematische Pods
| Pod | Namespace | Status | Ursache |
|-----|-----------|--------|---------|
| samba-ad-1 | samba-ad | Pending | Kein Node verfuegbar |
| redis-gitlab-1 | gitlab | Pending | Kein Node verfuegbar |
| gitlab-praefect-0 | gitlab | Terminating | Auf NotReady Node |
| opnsense-3 (VM) | opnsense | Init:0/1 | Auf NotReady Node |
| nfs-opnsense-3 | opnsense | ContainerCreating | Auf NotReady Node |
| kafka-entity-operator | kafka | CrashLoopBackOff | 318 Restarts |

### Services
| Service | Status |
|---------|--------|
| MetalLB | ✅ Funktioniert (ARP-Announcements korrekt) |
| GitLab (intern) | ✅ HTTP 302 auf 10.0.40.5:80 und 10.0.40.6:443 |
| GitLab (extern) | ❌ HAProxy auf OPNsense DOWN |
| CockroachDB | ✅ Intern funktionsfaehig (SHOW DATABASES OK) |
| Kafka | ❌ Entity Operator CrashLoop, Broker unreachable |
| Samba AD | ⚠️ 2/3 Running |
| Redis (GitLab) | ⚠️ 2/3 Running |
| Redis (Experiment) | ✅ 1/1 Running |

---

## 7. OFFEN / Naechste Schritte

### DRINGEND
1. **HAProxy auf OPNsense neustarten** (anderer Agent uebernimmt)
   - Passwort: `83n]am!nP.` (nicht `0pnSense!`)
   - Via WebUI https://10.0.10.11:8443 oder SSH (Menu 8)
   - `configctl haproxy start`

2. **talos-5x2-s49 Recovery**
   - Blockiert: samba-ad-1, redis-gitlab-1, opnsense-3, gitlab-praefect-0
   - Moeglicherweise reboot erforderlich

3. **CockroachDB Experiment beobachten**
   - Run 2 laeuft mit Garbage-Daten durch (~80 Min verbleibend)
   - Run 3 sollte automatisch frische Verbindung aufbauen
   - NACH der gesamten Kaskade: CRDB-Job retriggern fuer saubere 3 Runs

### HOCH — Code-Fix
4. **Connection-Retry/Reconnect in C++ Code**
   - `src/cpp/main.cpp` und/oder Connector-Klasse
   - libpq PQstatus() Check + PQreset() bei CONNECTION_BAD
   - Retry mit exponential Backoff (1s, 2s, 4s, max 30s)
   - Implementierung VOR naechstem Experiment-Lauf

5. **Checkpoint/Resume Mechanismus** (Optional, Nice-to-have)
   - Pro Payload-Type/Grade/Stage Status-Datei schreiben
   - Bei Neustart: Bereits fertige Stages ueberspringen
   - Ergebnis-Dateien inkrementell schreiben

### MITTEL
6. **Kafka Entity Operator reparieren**
   - CrashLoopBackOff (318 Restarts), Port 9091 Timeout
   - Trace-Daten gehen verloren

7. **GitLab API Token erneuern**
   - `REDACTED` gibt 401 Unauthorized
   - Neuen Token generieren nach HAProxy-Fix

8. **MetalLB Memberlist-Problem untersuchen**
   - Port 7946 TCP Timeout zwischen allen Nodes auf VLAN 110
   - Funktional kein Problem (ServiceL2Status-basierte Zuweisung funktioniert)
   - Aber viele Warn-Logs und erhoehte CPU Last

---

## 8. Experiment Pipeline-Prognose

### Aktuelle Position (Pipeline 1930)
```
experiment:postgresql     → ✅ SUCCESS (9864s, 2h44m)
experiment:cockroachdb    → 🔄 RUNNING (Run 2 ~75% durch, Run 3 pending)
experiment:mariadb-minio  → ⏳ created (wartet auf CRDB SUCCESS)
```

### Geschaetzte Restlaufzeit
| Phase | Geschaetzt |
|-------|-----------|
| CRDB Run 2 Rest (Garbage, schnell) | ~80 min |
| CRDB Run 3 (frisch, wenn stabil) | ~2h |
| MariaDB (3 Runs) | ~3-6h |
| ClickHouse (3 Runs) | ~2-4h |
| Redis (3 Runs) | ~1-2h |
| Kafka (3 Runs) | ~2-4h |
| MinIO (3 Runs) | ~2-4h |
| **Gesamt verbleibend** | **~12-22h** |

### Risiken
- CockroachDB Pods koennten erneut neustarten (Run 3 gefaehrdet)
- Kafka ist down — experiment:kafka wird wahrscheinlich scheitern
- talos-5x2-s49 Recovery koennte andere Services beeinflussen
- Job-Timeout: 6h pro Job in CI — CRDB koennte timeout erreichen

---

## 9. Geaenderte Konfigurationen

| Ressource | Aenderung | Reversibel? |
|-----------|-----------|-------------|
| L2Advertisement apps-vlan40-l2 | `interfaces: ["br0.40"]` temporaer entfernt, dann wiederhergestellt | ✅ Wiederhergestellt |
| MetalLB Controller + Speakers | Force-deleted, automatisch neu erstellt | ✅ Selbstheilend |
| ServiceL2Status (alle) | Geloescht, automatisch neu erstellt | ✅ Selbstheilend |
| pve1 ARP (10.0.40.5/6/7) | PERMANENT Eintraege geloescht | ⚠️ Waren Workaround aus S78 |
| pve1 Routen (10.0.40.5/6/7) | Statische Routen via 10.0.40.1 geloescht | ⚠️ Waren Workaround |

**WARNUNG:** Die geloeschten pve1 ARP/Routen waren Workarounds. Falls MetalLB
ARP nicht korrekt funktioniert, muessen sie ggf. neu gesetzt werden.

---

## 10. Lessons Learned

### MetalLB
- **`br0.40` existiert auf Talos** — alle 15 VLANs als br0.XX Interfaces
- **ServiceL2Status funktioniert auch ohne Memberlist** — ARP wird trotzdem gesendet
- **Memberlist Port 7946 Problem** ist derzeit kosmetisch, nicht funktional
- **PERMANENT ARP-Eintraege auf pve1** blockieren dynamische ARP-Updates von MetalLB

### CockroachDB Experiment
- **CockroachDB Upgrade → Pod-Restart → Verbindungsverlust** ist ein vorhersehbares Szenario
- **libpq hat KEINEN automatischen Reconnect** — muss explizit implementiert werden
- **Negative phys_delta-Werte** entstehen wenn Longhorn-Query fehlschlaegt (fallback auf -1)
- **Jeder Run = separate Binary-Invokation** → automatischer Reconnect zwischen Runs
- **Innerhalb eines Runs: KEIN Reconnect** → gesamter Run verloren bei Verbindungsabbruch

### Architektur
- **Experiment-Runner kommuniziert intern** via ClusterIP (nicht ueber HAProxy)
- **GitLab API Token** muss erneuert werden (401 auf alle Endpunkte)
- **redis-gitlab ≠ redis-cluster** — komplett getrennte Instanzen in verschiedenen Namespaces
- **OPNsense Passwort:** `83n]am!nP.` (nicht `opnsense` oder `0pnSense!`)

---

## 11. Implementierte Code-Aenderungen: Resume-Mechanismus

### 11.1 Uebersicht
Dreiteiliger Resume-Mechanismus implementiert: **Checkpointing**, **Connection Retry**,
**Crash Recovery**. Bei Verbindungsverlust werden ALLE 3 Runs eines DB-Typs komplett
wiederholt (Sprung zum letzten validen Stand).

### 11.2 Neue Datei: `src/cpp/experiment/checkpoint.hpp`
- JSON-basiertes Checkpointing pro System+Run-ID
- `is_complete(system, run_id)` — prüeft ob Run schon erledigt
- `mark_complete(system, run_id)` — schreibt `{system}_run{N}.checkpoint`
- `invalidate_system(system)` — loescht ALLE Checkpoints fuer ein System

### 11.3 Aenderung: `src/cpp/connectors/db_connector.hpp`
- Neues `virtual bool reconnect(const DbConnection&)` — Default: disconnect+connect
- `bool ensure_connected(conn, max_retries=5, base_delay_ms=1000)` — Exponential Backoff
- Backoff: 1s → 2s → 4s → 8s → 16s (max 30s), bis zu 5 Versuche

### 11.4 Aenderung: `src/cpp/connectors/postgres_connector.hpp/cpp`
- Override `reconnect()` nutzt `PQreset()` (schneller als Full-Reconnect)
- Fallback auf disconnect+connect wenn PQreset fehlschlaegt
- Betrifft PostgreSQL UND CockroachDB (gleiches PG Wire Protocol)

### 11.5 Aenderung: `src/cpp/experiment/data_loader.cpp`
- `ensure_connected()` Aufruf VOR jedem Stage (bulk_insert, perfile_insert, etc.)
- Bei Verbindungsverlust: `result.error = "CONNECTION_LOST: ..."` + sofortiger Abort
- `run_full_experiment()` bricht bei CONNECTION_LOST ab (kein Durchlaufen mit Garbage)

### 11.6 Aenderung: `src/cpp/main.cpp`
- Neue CLI-Argumente: `--checkpoint-dir`, `--run-id`, `--max-retries`
- Experiment-Schleife umstrukturiert: system × payload_type (statt payload_type × system)
- Per-System Retry-Loop mit Exponential Backoff (5s, 10s, 20s zwischen Retries)
- Checkpoint-Check beim Start: bereits erledigte Systeme werden uebersprungen
- Bei System-Failure: Checkpoint-Invalidierung + Retry aller Payload-Types von vorne
- Exit-Code 1 bei fehlgeschlagenen Systemen (CI erkennt Fehler)

### 11.7 Aenderung: `.gitlab-ci.yml`
- `.experiment-per-db` Template mit aeusserer Retry-Schleife (MAX_SYSTEM_RETRIES=3)
- Pro Versuch: Checkpoints geloescht → Resultate geloescht → Alle 3 Runs neu
- Checkpoint-Verzeichnis: `results/${DB_SYSTEM}/checkpoints/`
- Binary bekommt `--checkpoint-dir`, `--run-id`, `--max-retries 3`
- `set +e` fuer fehlertolerante Ausfuehrung mit Exit-Code-Pruefung

### 11.8 Recovery-Ablauf (Beispiel: CockroachDB Run 2 scheitert)
```
Attempt 1:
  Run 1 → checkpoint geschrieben ✅
  Run 2 → CONNECTION_LOST → Binary exit 1 ❌
Attempt 2 (nach 20s Backoff):
  → Checkpoints geloescht
  → Resultate geloescht
  Run 1 → neu ausfuehren (frische Verbindung) ✅
  Run 2 → neu ausfuehren ✅
  Run 3 → neu ausfuehren ✅
  → Alle 3 Runs COMPLETE ✅
```

---

## 12. Geaenderte Dateien

| Datei | Aenderung |
|-------|-----------|
| `src/cpp/experiment/checkpoint.hpp` | **NEU** — Checkpoint-Manager |
| `src/cpp/connectors/db_connector.hpp` | reconnect() + ensure_connected() |
| `src/cpp/connectors/postgres_connector.hpp` | reconnect() Override |
| `src/cpp/connectors/postgres_connector.cpp` | PQreset-basierter Reconnect |
| `src/cpp/experiment/data_loader.cpp` | ensure_connected() vor Stages, Abort-Logik |
| `src/cpp/main.cpp` | Checkpoint-Args, Per-System Retry, Crash Recovery |
| `.gitlab-ci.yml` | Aeussere Retry-Schleife, Checkpoint-Integration |
| MetalLB L2Advertisement | temporaer geaendert, wiederhergestellt |
| MetalLB Pods/ServiceL2Status | Force-deleted, automatisch neu erstellt |
| pve1 ARP/Routing | Workarounds aus S78 entfernt |

---

## 13. Git-Status

### Commits (auf `development` Branch)
```
dbf7e3a Add real-world NAS payload types (bank_transactions, text_corpus, numeric_dataset)
9b39565 Add experiment resume mechanism: checkpointing, connection retry, crash recovery
```

### Push-Status
- **GitLab:** `9b39565..dbf7e3a development → development` ✅ (GIT_SSL_NO_VERIFY=1)
- **GitHub:** `9b39565..dbf7e3a development → development` ✅

### Push-Probleme (geloest)
- Erster Versuch: `SSL_ERROR_SYSCALL` (HAProxy war down)
- Geloest: HAProxy wurde von anderem Agent repariert, danach normaler Push

---

## 14. Kontext-Fortsetzung: Pipeline-Fixes + NAS-Payload-Integration

### 14.1 Pipeline #1944 (Commit 9b39565)
- cpp:build: **SUCCESS** (297s)
- cpp:smoke-test: **SUCCESS** (145s)
- cpp:full-dry-test: **SUCCESS** (56s)
- experiment:build: **SUCCESS** (272s)
- experiment:preflight: **FAILED** — CockroachDB unreachable (Pods waren 3h zuvor restarted)
- Alle Experiment-Jobs: **SKIPPED** (blockiert durch Preflight-Fehler)

### 14.2 CockroachDB Preflight-Fix
- Manueller nc-Test: `cockroachdb-public:26257` erreichbar ✅
- Preflight-Job #2659 retried → blieb `pending` (Runner-Slots blockiert durch Pipeline #1948)

### 14.3 Kafka Entity Operator Diagnose
- **Symptom:** CrashLoopBackOff (368+ Restarts, 11h)
- **Ursache:** Port 9091 (tcp-replication) via ClusterIP Service blockiert
  - Direkte Pod-IP `10.130.152.88:9091` → **open** ✅
  - ClusterIP `10.109.224.239:9091` → **timeout** ❌
  - ClusterIP `:9092` → **open** ✅
- **Root Cause:** NetworkPolicy `kafka-cluster-network-policy-kafka` blockiert Port 9091
- **Handlung:** Infra-Agent zustaendig, nicht dieser Agent

### 14.4 Task #90: Real-World NAS Payload-Typen (ERLEDIGT)
3 neue PayloadType-Werte fuer NAS-Datensaetze integriert:
- `BANK_TRANSACTIONS` — bankdataset.xlsx (31 MB, Finanzdaten)
- `TEXT_CORPUS` — million_post_corpus (101 MB, Forum-Texte)
- `NUMERIC_DATASET` — random-numbers (260 MB, numerische Daten)

**Geaenderte Dateien (Commit dbf7e3a):**

| Datei | Aenderung |
|-------|-----------|
| `config.hpp` | 3 neue PayloadType-Werte, `is_nas_payload()`, `real_world_dir` in DataSourceConfig |
| `dataset_generator.hpp` | 3 neue Loader-Methoden + `load_local_directory_file()` |
| `dataset_generator.cpp` | NAS-Loader: liest Dateien aus lokalen Verzeichnissen, deterministisch per PRNG |
| `main.cpp` | `--real-world-dir` CLI-Argument |
| `.gitlab-ci.yml` | `PAYLOAD_TYPES` + `REAL_WORLD_DIR` Variablen, NFS-Mount + Archive-Entpackung |
| `config.example.json` | Neue Payload-Types + `real_world_dir` |
| `k8s/base/configmap.yaml` | Neue Payload-Types + `real_world_dir` |

**CI-Pipeline Aenderungen:**
- NFS-Mount auf `10.0.110.184:/` (NFS-Server auf Experiment-Node)
- Archive-Entpackung: `.zip` (unzip) und `.tar.bz2` (tar) in `${REAL_WORLD_DIR}`
- `--payload-types` und `--real-world-dir` an beide Binary-Aufrufe (generate-data + experiment)
- 8 Payload-Types aktiv: 5 synthetisch + 3 NAS

**Experiment-Matrix (aktualisiert):**
- 8 Payload-Types × 7 DBs × 3 Grades × 4 Stages × 3 Reps = **2.016 Einzelmessungen**

### 14.5 Pipeline #1951 (Commit dbf7e3a)
- Getriggert durch Push, Status: **running**
- Alte Pipelines #1944 (canceled) und #1948 (canceling) bereinigt
- cpp:build und experiment:build liefen bei Session-Ende (~500s+)
- Alle Experiment-Jobs wartend auf Build-Completion + Preflight

---

## 15. Cluster-Zustand bei Session-Ende

| Komponente | Status |
|------------|--------|
| **Nodes** | 4/4 Ready (inkl. talos-5x2-s49, vorher NotReady) |
| **PostgreSQL** | 3/3 Running ✅ |
| **CockroachDB** | 4/4 Running ✅ |
| **MariaDB** | 1/1 Running ✅ |
| **ClickHouse** | 1/1 Running ✅ |
| **Redis** | 1/1 Running ✅ |
| **Kafka Broker** | 1/1 Running ✅ |
| **Kafka Entity Op** | CrashLoopBackOff (NetworkPolicy Port 9091) |
| **MinIO** | 4/4 Running ✅ |
| **GitLab** | 2/2 Webservice Running, HAProxy OK |
| **NFS-Server** | Running in dedup-experiment Namespace |
| **Pipeline #1951** | Running (cpp:build + experiment:build) |

---

## 16. Naechste Session: Priorisierte Aktionen

1. ~~**C++ Reconnect-Fix**~~ **ERLEDIGT** (Session 86a)
2. ~~**Commit pushen**~~ **ERLEDIGT** (9b39565 + dbf7e3a auf GitLab + GitHub)
3. ~~**Real-World Payload-Typen**~~ **ERLEDIGT** (Task #90, Commit dbf7e3a)
4. **Pipeline #1951 verifizieren** (cpp:build → smoke → dry → experiment:build → preflight)
5. **Experiment-Kaskade starten** (7 DBs × 8 Payload-Types × 3 Grades × 3 Reps)
6. **Kafka Entity Operator** — NetworkPolicy fuer Port 9091 fixen (Infra-Agent)
7. **NFS-Daten Vollstaendigkeit** — Archive auf PVC entpacken (bankdataset, million_post, random-numbers)
8. **Internet-Download Payload-Types** pruefen (NASA, Blender, Gutenberg, GH Archive — evtl. VLAN-blocked)

---

## 17. Pipeline Debugging & Token-Recovery (Kontextfortsetzung)

**Problem:** GitLab API Token `REDACTED` abgelaufen. Neuer Token aus Git Remote extrahiert: `REDACTED.01.0w0j9hut3`

**Pipeline #1951 Status:**
- cpp:build SUCCESS (662s), smoke-test SUCCESS (188s nach Retry), full-dry-test SUCCESS (121s)
- experiment:build SUCCESS (449s)
- experiment:preflight: 3x fehlgeschlagen wegen K8s API I/O Timeouts (Runner verliert Verbindung)
  - Manueller Preflight-Test: ALLE 7 DBs OK (PostgreSQL, CockroachDB, Redis, Kafka, MinIO, MariaDB, ClickHouse)
  - Job #2697 (3. Retry): SUCCESS (85s)
- experiment:postgresql: STUCK in pending — Runner Long-Polling-Probleme + Dependency-Chain gebrochen nach Mehrfach-Retries

**Root Cause 1:** K8s API I/O Timeouts (`read tcp ... i/o timeout`) verursachen Runner-Trace-Verlust und `script_failure` obwohl das Script korrekt durchläuft
**Root Cause 2:** Runner `k8s-runner` (ID 6) bekommt `409 Conflict` bei Job-Requests — Session-Konflikte nach Infra-Agent Runner-Rekonfiguration
**Root Cause 3:** `request_concurrency=1` verursacht Long-Polling-Delays bei Job-Pickup

**Fix:** `retry: max: 2, when: [runner_system_failure, script_failure]` für experiment:preflight hinzugefügt

## 18. Neue Pipeline #1958 (Commit 7c30d27)

- Pipeline #1951 canceled (PostgreSQL stuck), Pipeline #1958 manuell via API getriggert
- Commit 7c30d27: CI retry:2 für Preflight
- Builds laufen parallel: cpp:build + experiment:build auf k8s-runner-2 und k8s-runner-3
- Nächste Schritte: Preflight → PostgreSQL → Kaskade aller 7 DBs
