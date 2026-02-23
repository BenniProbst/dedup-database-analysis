# Session 85: Experiment Pipeline Fixes + MetalLB Recovery

**Datum:** 2026-02-21
**Kontext:** Fortsetzung von Session 74e (Experiment-Pipeline), Pipeline 1930
**Status:** Experiment laeuft autonom, GitLab down (HAProxy/MetalLB)

## Zusammenfassung

Zwei kritische C++ Bugs in der Experiment-Pipeline gefunden und gefixt, die das
Experiment daran gehindert haben korrekt zu laufen. Pipeline 1930 laeuft jetzt
autonom mit korrektem per-DB Filtering. Zusaetzlich MetalLB-Ausfall diagnostiziert
und teilweise repariert.

## C++ Bug Fixes (Commit 5cd8542)

### Bug 1: --generate-data Fall-through (main.cpp:203)
- **Problem:** `--generate-data` Modus hatte kein `return 0` nach Datengenerierung
- **Auswirkung:** Erste Invokation (Datengenerierung) fiel durch in die
  Connector-Initialisierung und den Experiment-Loop. Die zweite Invokation
  (`--systems "postgresql"`) wurde NIEMALS erreicht!
- **Symptom:** `EXPERIMENT MATRIX: 5 payload types x 4 systems` statt `x 1 systems`
- **Fix:** `return 0;` nach der generate_data Block

### Bug 2: Fehlende Passwort-Umgebungsvariablen (config.hpp)
- **Problem:** DB-Passwoerter waren hardcoded als leere Strings `""`
- **Auswirkung:** PostgreSQL, CockroachDB, MariaDB Verbindung schlug fehl
  (`password authentication failed for user "dedup-lab"`)
- **Fix:** `std::getenv()` fuer DEDUP_PG_PASSWORD, DEDUP_CRDB_PASSWORD,
  DEDUP_REDIS_PASSWORD, DEDUP_MARIADB_PASSWORD, DEDUP_MINIO_PASSWORD
- **Neuer Include:** `<cstdlib>` fuer std::getenv

## DB Permissions Fixes

| DB | Fix | Befehl |
|----|-----|--------|
| PostgreSQL | GRANT CREATE auf postgres DB | `GRANT CREATE ON DATABASE postgres TO "dedup-lab"` |
| CockroachDB | User + DB erstellt (Secure Mode nach Upgrade) | `CREATE USER dedup_lab WITH PASSWORD '...'` |
| MariaDB | Bereits korrekt konfiguriert | dedup-lab User funktioniert |

## CI Variable Updates

Alle 5 DB-Passwoerter in GitLab CI/CD Variables auf `S-c17LvxSx1MzmYrYh17` gesetzt:
- DEDUP_PG_PASSWORD, DEDUP_CRDB_PASSWORD, DEDUP_REDIS_PASSWORD
- DEDUP_MARIADB_PASSWORD, DEDUP_MINIO_PASSWORD

## Pipeline 1930 Status (bei Session-Ende)

| Job | Status | Dauer |
|-----|--------|-------|
| cpp:build | SUCCESS | 1556s |
| cpp:smoke-test | SUCCESS | 205s |
| cpp:full-dry-test | SUCCESS | 249s |
| experiment:build | SUCCESS | 1097s |
| experiment:preflight | SUCCESS | 151s |
| experiment:postgresql | **SUCCESS** | 9864s (2h44m) |
| experiment:cockroachdb | **RUNNING** | ~80min, Run 1 Payload 3/5 |
| experiment:mariadb-minio | created | Warten in Kaskade |

### PostgreSQL Ergebnisse (Auszug Run 1)

| Payload | Grade | Stage | Rows | Logical | Phys Delta | EDR | Speed |
|---------|-------|-------|------|---------|-----------|-----|-------|
| random_binary | U0 | perfile_insert | 500 | 251 MB | 447 MB | 2.253 | 2.9 MB/s |
| random_binary | U50 | bulk_insert | 500 | 256 MB | 493 MB | 2.078 | 3.5 MB/s |
| random_binary | U50 | perfile_insert | 500 | 256 MB | 164 MB | 6.254 | 2.8 MB/s |
| structured_json | U0 | perfile_insert | 500 | 262 MB | 877 KB | 1193.8 | 3.3 MB/s |

**Bekanntes Problem:** bulk_insert meldet oft 0 Rows wegen fehlendem Schema.
perfile_insert erstellt Tabelle automatisch und funktioniert korrekt.

## MetalLB-Ausfall

### Ursache
- talos-5x2-s49 NotReady (CockroachDB Upgrade durch anderen Agent)
- MetalLB Speaker versuchte Memberlist-Join zu 10.0.110.183 (NotReady Node)
- ServiceL2Status Objekte hatten immutable Node-Referenzen auf alten Node
- MetalLB Controller: 42 Restarts in 17h

### Fixes durchgefuehrt
1. Speaker-Pod auf NotReady Node geloescht
2. Controller neugestartet
3. Alle ServiceL2Status Objekte geloescht (automatisch neu erstellt)
4. Speaker DaemonSet: Toleration `experiment=dedicated:NoSchedule` hinzugefuegt
5. Rolling Restart aller Speaker

### Verbleibender Status
- MetalLB: 3/3 Speaker + 1 Controller Running
- **GitLab WebUI immer noch nicht erreichbar** (Port 443)
- HAProxy auf opnsense-1 antwortet nicht auf Port 443
- opnsense-2 gerade neugestartet, opnsense-3 Pending
- **HAProxy muss manuell auf opnsense-1 neugestartet werden**

## OPNsense Status

| VM | Status | Node | HAProxy :443 | WebUI :8443 |
|----|--------|------|-------------|-------------|
| opnsense-1 | Running | talos-qkr-yc0 | DOWN | OK (200) |
| opnsense-2 | JUST STARTED | talos-lux-kpk | DOWN | DOWN (booting) |
| opnsense-3 | PENDING | - | DOWN | DOWN |
| opnsense-4 | Running | talos-say-ls6 | 404 | OK (200) |

## Cluster Status

| Node | Status | Rolle |
|------|--------|-------|
| talos-5x2-s49 | **NotReady** | CockroachDB Upgrade |
| talos-lux-kpk | Ready | GitLab, Runner |
| talos-qkr-yc0 | Ready | DBs, OPNsense-1 |
| talos-say-ls6 | Ready | Experiment Node |

## OFFEN / Naechste Schritte

1. **DRINGEND:** HAProxy auf opnsense-1 (und 4) neu starten
   - Via OPNsense WebUI oder SSH: `configctl haproxy start`
2. **DRINGEND:** talos-5x2-s49 Recovery (nach CockroachDB Upgrade)
3. **Experiment laeuft autonom** — CockroachDB Job bei Run 1, ~15h Restlaufzeit
4. **bulk_insert 0 Rows Fix** — Schema-Erstellung im C++ Code verbessern
5. **Kafka Trace Producer** — Connection refused (Entity Operator CrashLoopBackOff)
6. **Runner Stabilität** — Runner stoppt periodisch polling, braucht Restart

## Dateien geaendert

- `src/cpp/main.cpp` — return 0 nach generate-data
- `src/cpp/config.hpp` — getenv fuer Passwoerter, cstdlib include
- MetalLB Speaker DaemonSet — experiment Toleration
- MetalLB ServiceL2Status — alle geloescht/neu erstellt

## Lessons Learned

- **--generate-data MUSS standalone sein** — kein Fall-through in Experiment-Code
- **CI Variables werden als ENV exportiert** — C++ Code muss sie explizit lesen
- **MetalLB ServiceL2Status node ist IMMUTABLE** — bei Node-Wechsel loeschen!
- **MetalLB Speaker braucht Tolerations** fuer ALLE Node-Taints (experiment=dedicated)
- **OPNsense CARP VIP funktioniert nur wenn HAProxy auf CARP Master laeuft**
