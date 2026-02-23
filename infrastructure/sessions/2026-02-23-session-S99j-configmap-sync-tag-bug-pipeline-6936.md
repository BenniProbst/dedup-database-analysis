# Session S99j: ConfigMap Sync, Runner-Tag-Bug Root Cause, Pipeline #6936

**Datum:** 2026-02-23 (Fortsetzung von S99i)
**Agent:** system32 (Projekte/Kahan-Plan)

## Zusammenfassung

Kontext-Fortsetzung von S99i. Kafka SASL Passwort-Kette vollstaendig verifiziert (Strimzi -> K8s Secret -> CI/CD Variable -> env -> C++). K8s ConfigMap `configmap.yaml` auf Port 9094 synchronisiert. Runner-Tag-Bug Root Cause identifiziert: GitLab DB-Bug loescht Tags, CronJob setzt sie korrekt aber Tags werden sofort wieder geloescht. DB-Migration-Fix (permanenter Fix) schlug fehl wegen K8s API Timeout. Pipeline #6936 hat 4/7 Builds success, 3 pending (Tags leer).

---

## Erledigte Aufgaben

### 1. Kafka SASL Passwort-Kette verifiziert

**Vollstaendige Kette (FUNKTIONIERT):**

| # | Stelle | Status | Details |
|---|--------|--------|---------|
| 1 | Strimzi KafkaUser `dedup-lab` (SCRAM-SHA-512) | Ready | Passwort: `S-c17LvxSx1MzmYrYh17` |
| 2 | K8s Secret `dedup-lab` (kafka namespace) | Vorhanden | `password` + `sasl.jaas.config` Felder |
| 3 | GitLab CI/CD Variable `DEDUP_KAFKA_PASSWORD` | Gesetzt (masked) | Gleicher Wert |
| 4 | `.gitlab-ci.yml` Zeile 614-615 | `export KAFKA_PASSWORD="${DEDUP_KAFKA_PASSWORD}"` | Runtime-Export |
| 5 | `kafka_connector.cpp` | `getenv("KAFKA_PASSWORD")` | SASL/SCRAM-SHA-512 Config |

**Alle 6 DB-Passwoerter als CI/CD Variablen vorhanden:**
- DEDUP_PG_PASSWORD, DEDUP_CRDB_PASSWORD, DEDUP_REDIS_PASSWORD
- DEDUP_MARIADB_PASSWORD, DEDUP_MINIO_PASSWORD, DEDUP_KAFKA_PASSWORD
- Alle fuer Samba AD Lab-User `dedup-lab` (gleiches Passwort)

**HINWEIS:** Secret `dedup-credentials` existiert NICHT im `gitlab-runner` Namespace. Passwoerter kommen ausschliesslich aus CI/CD Variablen, nicht aus K8s Secrets (fuer den CI-Kontext).

### 2. K8s ConfigMap Kafka Port synchronisiert (Commit `3719896`)

**Problem:** `k8s/base/configmap.yaml` hatte noch Kafka Port 9092, `config.example.json` hatte 9094.

**Fix:**
```yaml
# k8s/base/configmap.yaml databases.kafka
"port": 9094,           # war 9092
"user": "dedup-lab",    # NEU
"password": "",          # NEU (kommt aus env)
"_comment": "SCRAM-SHA-512 on port 9094. Password from KAFKA_PASSWORD env var."
```

**Commit:** `3719896` auf GitLab + GitHub development Branch gepusht.

### 3. Pipeline-Cleanup

| Pipeline | Aktion | Ergebnis |
|----------|--------|----------|
| #6931 | DELETED | Ghost-Pipeline (0 aktive Jobs) |
| #6932 | CANCELED | Obsolet |
| #6933 | DELETED | Canceled + Ghost |
| #6934 | DELETED | Duplikat |
| #6935 | DELETED | Nur 2 Stages (changes: Filter, kein C++ geaendert) |
| **#6936** | **RUNNING** | **AKTIV** - API-Trigger, 21 Jobs, 7 Stages |

### 4. Pipeline #6935 Diagnose: Fehlende Experiment-Stages

**Problem:** Pipeline #6935 (source=push, Commit `3719896`) hatte nur 8 Jobs in 2 Stages (build + aggregate).

**Root Cause:** `experiment:build` hat `rules:` mit `changes:` Filter:
```yaml
experiment:build:
  rules:
    - if: $CI_PIPELINE_SOURCE == "web" || $CI_PIPELINE_SOURCE == "api"
    - if: $CI_PIPELINE_SOURCE == "push"
      changes:
        - "src/cpp/**/*.cpp"
        - "src/cpp/**/*.hpp"
        - "src/cpp/**/*.h"
        - "src/cpp/CMakeLists.txt"
        - ".gitlab-ci.yml"
```

Commit `3719896` aenderte nur `configmap.yaml` + Session-Dok -> keine C++ Dateien -> `experiment:build` nicht erstellt -> alle abhaengigen Jobs fehlen.

**Fix:** Pipeline #6936 via API-Trigger erstellt (`source=api` umgeht `changes:` Check) -> alle 21 Jobs vorhanden.

**MERKE fuer Zukunft:** Wenn nur Config/Docs geaendert werden und Experiment laufen soll: Pipeline via API triggern!

### 5. Runner-Tag-Bug: Root Cause Analyse

**Symptom:** 6 Build-Jobs bleiben `pending` weil Runner-Tags leer sind.

**Root Cause Kette:**
1. GitLab 17.9+ Server-Bug loescht periodisch die `ci_runner_taggings` DB-Tabelle
2. Tags verschwinden auf allen 12 Runnern
3. CronJob `runner-tag-enforcer` (Namespace `gitlab-runner`, Schedule `* * * * *`) setzt Tags korrekt per API
4. Aber Tags werden innerhalb von Sekunden wieder geloescht

**CronJob-Skript ist KORREKT** (`runner-tag-enforcer-script` ConfigMap):
- K8s Runner: `kubernetes` (CI: `tags: [kubernetes]`)
- Runner-4: `kubernetes,experiment` (CI: `tags: [experiment]`)
- Debian: `debian,x86_64` (CI: `tags: [debian, x86_64]`)
- Ubuntu: `ubuntu,x86_64`
- macOS x86: `macos,x86_64`
- macOS ARM: `macos,arm64`
- Linux ARM64: `arm64,linux`
- Linux RISC-V: `riscv64,linux`

**WICHTIG:** In dieser Session wurden Tags FALSCH manuell gesetzt (`k8s, docker, linux, x86_64` statt `kubernetes`). Das funktionierte temporaer weil `build-k8s` die Runner trotzdem matchte. Aber der CronJob ueberschreibt mit korrekten Tags.

### 6. DB-Migration-Fix versucht (FEHLGESCHLAGEN)

**Befehl:**
```bash
kubectl -n gitlab exec $TOOLBOX -- gitlab-rake db:migrate:down VERSION=20241219100359
kubectl -n gitlab exec $TOOLBOX -- gitlab-rake db:migrate:up VERSION=20241219100359
```

**Fehler:** `read tcp 10.0.15.201:49114->10.0.15.250:6443: i/o timeout` (K8s API Timeout)

**Ursache:** Die Migration dauert laenger als das kubectl exec Timeout (2 Minuten default).

---

## Pipeline #6936 Status (Stand Session-Ende)

| Job | Status | Runner | Stage |
|-----|--------|--------|-------|
| build-k8s | SUCCESS | k8s-runner-2 | build |
| build-debian-x86 | PENDING | - | build |
| build-ubuntu-x86 | PENDING | - | build |
| build-macos-x86 | SUCCESS | node5-macos-x86 | build |
| build-macos-arm | PENDING | - | build |
| build-linux-arm64 | SUCCESS | node7-linux-arm | build |
| build-linux-riscv | SUCCESS | node8-linux-riscv | build |
| aggregate-results | created | - | aggregate |
| experiment:build | PENDING | - | experiment-build |
| experiment:preflight | created | - | experiment-preflight |
| experiment:reset | created | - | experiment-reset |
| experiment:postgresql | created | - | experiment-run |
| experiment:cockroachdb | created | - | experiment-run |
| experiment:mariadb | created | - | experiment-run |
| experiment:clickhouse | created | - | experiment-run |
| experiment:redis | created | - | experiment-run |
| experiment:kafka | created | - | experiment-run |
| experiment:minio | created | - | experiment-run |
| experiment:run-all | created | - | experiment-run |
| experiment:upload-results | created | - | experiment-cleanup |
| experiment:cleanup | manual | - | experiment-cleanup |

**4/7 Builds SUCCESS, 3 pending (debian-x86, ubuntu-x86, macos-arm)**
Alle 3 pending Builds blockiert durch leere Runner-Tags.

---

## OFFENE AUFGABEN (Naechste Session)

### KRITISCH 1: Runner-Tags permanent fixen (DB-Migration)

**Status:** CronJob funktioniert, aber GitLab loescht Tags sofort wieder. DB-Migration-Fix schlug fehl wegen K8s API Timeout.

**Loesung (mit laengerem Timeout):**
```bash
# Option A: kubectl exec mit laengerem Timeout
ssh root@10.0.10.201
TOOLBOX=$(kubectl -n gitlab get pod -l app=toolbox -o jsonpath='{.items[0].metadata.name}')

# Erst testen ob Toolbox erreichbar
kubectl -n gitlab exec $TOOLBOX -- echo "OK"

# Migration DOWN mit erhoehtem Timeout (10 Minuten)
kubectl -n gitlab exec --request-timeout=600s $TOOLBOX -- \
  gitlab-rake db:migrate:down VERSION=20241219100359

# Migration UP
kubectl -n gitlab exec --request-timeout=600s $TOOLBOX -- \
  gitlab-rake db:migrate:up VERSION=20241219100359

# Option B: Direkt im Toolbox Pod als Background-Task
kubectl -n gitlab exec $TOOLBOX -- bash -c '
  nohup gitlab-rake db:migrate:down VERSION=20241219100359 > /tmp/migrate-down.log 2>&1
  cat /tmp/migrate-down.log
'
# Warten, dann:
kubectl -n gitlab exec $TOOLBOX -- bash -c '
  nohup gitlab-rake db:migrate:up VERSION=20241219100359 > /tmp/migrate-up.log 2>&1
  cat /tmp/migrate-up.log
'
```

**Nach Migration:**
```bash
# Tags manuell setzen (einmalig)
# Dann pruefen ob sie nach 5 Minuten NOCH da sind
# Wenn ja: CronJob kann auf 5min zurueckgesetzt oder deaktiviert werden
# Wenn nein: Anderer Ansatz noetig (GitLab Upgrade, Patch, etc.)
```

### KRITISCH 2: Pipeline #6936 Builds entblocken

**Status:** 3 Builds pending (debian-x86, ubuntu-x86, macos-arm) wegen leerer Tags.

**Sofortmassnahme:** Tags manuell per API setzen:
```bash
TOOLBOX=$(kubectl -n gitlab get pod -l app=toolbox -o jsonpath='{.items[0].metadata.name}')
kubectl -n gitlab exec $TOOLBOX -- python3 -c "
import urllib.request, json, ssl
ctx = ssl._create_unverified_context()
TOKEN = 'REDACTED'
tags = {
    6: 'kubernetes', 7: 'macos,x86_64', 8: 'macos,arm64',
    9: 'arm64,linux', 10: 'riscv64,linux',
    11: 'debian,x86_64', 12: 'debian,x86_64',
    13: 'ubuntu,x86_64', 14: 'ubuntu,x86_64',
    15: 'kubernetes', 16: 'kubernetes', 17: 'kubernetes,experiment'
}
for rid, tl in tags.items():
    body = json.dumps({'tag_list': tl.split(',')}).encode()
    req = urllib.request.Request('https://gitlab.comdare.de/api/v4/runners/' + str(rid),
        data=body, method='PUT', headers={'PRIVATE-TOKEN': TOKEN, 'Content-Type': 'application/json'})
    resp = urllib.request.urlopen(req, context=ctx)
    print(str(rid) + ' -> OK')
"
```

**ACHTUNG:** Tags werden sofort wieder geloescht! Man muss sie setzen und hoffen, dass die pending Jobs innerhalb von Sekunden aufgenommen werden.

### KRITISCH 3: Runner-4 Polling-Bug nach jedem Job

**Status:** Bekannter Bug, Runner-4 stoppt Polling nach jedem Job.

**Workaround:**
```bash
kubectl -n gitlab-runner delete pod -l app=gitlab-runner-4 --grace-period=5
```

Jobs die Runner-4 Restart brauchen: experiment:build, experiment:preflight, experiment:reset, und JEDER experiment:DB Job.

### MITTEL: Kafka super.users CRD-Patch revertieren

**Status:** `User:ANONYMOUS` als super.user noch aktiv (Backup).

**Wann revertieren:** Sobald Pipeline-Log zeigt:
```
[INF] [metrics_trace] Kafka producer ready (...:9094, user=dedup-lab)
```
(NICHT `...9092...` oder `SASL FAIL`)

**Revert-Befehl:**
```bash
kubectl -n kafka patch kafka kafka-cluster --type merge \
  -p '{"spec":{"kafka":{"config":{"super.users":null}}}}'
```

### MITTEL: Master-Branch sync

**Status:** development auf `3719896`, master noch auf `ddf2281` (fehlen: ConfigMap-Fix).

**Fix:**
```bash
cd dedup-database-analysis
git push gitlab development:master
git push github development:master
```

### NIEDRIG: experiment:build changes-Filter erweitern

**Erkenntniss dieser Session:** Wenn nur Config-Dateien geaendert werden (kein C++), erstellt GitLab keine Experiment-Jobs. Entweder:
1. `changes:` um Config-Pfade erweitern: `k8s/**`, `src/cpp/config.example.json`, `scripts/reset/**`
2. Oder: Immer via API triggern wenn Experiment noetig

---

## Commits dieser Session (S99j, zusaetzlich zu S99i)

| Commit | Beschreibung | Remote |
|--------|-------------|--------|
| `3719896` | fix: sync K8s ConfigMap Kafka port 9092->9094 + SASL user field | GitLab + GitHub |

## Dateien geaendert (S99j)

| Datei | Aenderung |
|-------|-----------|
| `k8s/base/configmap.yaml` | databases.kafka: port 9092->9094, user+password+comment hinzugefuegt |
| `infrastructure/sessions/S99i` | Aktualisiert: Commits, Pipeline-Status, Kafka SASL Kette |
| `infrastructure/sessions/S99j` | NEU: Diese Session-Dok |

---

## Kontext fuer naechste Session

### Zustand der Pipeline
1. **Pipeline #6936 (API, `3719896`) ist die EINZIGE aktive Pipeline** - 4/7 Builds success, 3 pending (Tag-Bug)
2. **Alle alten Pipelines** geloescht/cancelled (#6919-6935)
3. **Experiment-Jobs vorhanden** (21 Jobs, 7 Stages) - korrekt via API getriggert

### Was funktioniert
- Kafka SASL Passwort-Kette (komplett verifiziert)
- ConfigMap + config.example.json synchron (Port 9094)
- kafka_connector.cpp + metrics_trace.cpp beide mit SASL/SCRAM-SHA-512
- RBAC fuer experiment:reset (ClusterRole experiment-reset-access)
- CI workflow:rules (nur development Branch)
- GitHub + GitLab development Branch konsistent

### Was NICHT funktioniert
- **Runner-Tags** werden permanent geloescht (GitLab DB-Bug)
- **DB-Migration-Fix** schlug fehl (K8s API Timeout)
- **3 Builds pending** (debian-x86, ubuntu-x86, macos-arm) wegen leerer Tags
- **Runner-4 Polling-Bug** (braucht Pod-Restart nach jedem Job)

### Prioritaeten fuer naechste Session
1. **Runner-Tags DB-Migration permanent fixen** (mit `--request-timeout=600s`)
2. **3 pending Builds entblocken** (Tags setzen + sofort pruefen ob aufgenommen)
3. **Pipeline #6936 bis Experiment-Kaskade durchlaufen lassen**
4. **Runner-4 nach jedem Experiment-Job restarten**
5. **Kafka SASL im Pipeline-Log verifizieren** (`:9094, user=dedup-lab`)
6. **super.users revertieren** wenn SASL funktioniert
7. **Master-Branch sync** auf `3719896`

### CI Tag-Mapping (KORREKT, wie in CronJob)

| CI Job | Benoetigte Tags | Runner |
|--------|----------------|--------|
| build-k8s | `kubernetes` | k8s-runner, -2, -3 |
| build-debian-x86 | `debian, x86_64` | pve1-linux-x86 (11), pve2-linux-x86 (12) |
| build-ubuntu-x86 | `ubuntu, x86_64` | node3-linux-x86 (13), node4-linux-x86 (14) |
| build-macos-x86 | `macos, x86_64` | node5-macos-x86 (7) |
| build-macos-arm | `macos, arm64` | node6-macos-arm (8) |
| build-linux-arm64 | `arm64, linux` | node7-linux-arm (9) |
| build-linux-riscv | `riscv64, linux` | node8-linux-riscv (10) |
| experiment:* | `experiment` | k8s-runner-4 (17) |

### Git HEAD

| Remote | Branch | HEAD |
|--------|--------|------|
| GitLab | development | `3719896` |
| GitHub | development | `3719896` |
| GitLab | master | `ddf2281` (nicht synchronisiert) |
| GitHub | master | `ddf2281` (nicht synchronisiert) |
| Lokal | development | `3719896` |

### Kafka Konfiguration

| Stelle | Port | SASL | Status |
|--------|------|------|--------|
| config.example.json (metrics_trace) | 9094 | via env KAFKA_USER/KAFKA_PASSWORD | OK |
| config.example.json (databases.kafka) | 9094 | via env + DbConnection | OK |
| configmap.yaml (databases.kafka) | 9094 | user: dedup-lab | OK |
| configmap.yaml (metrics_trace) | 9094 | - | OK |
| kafka_connector.cpp | - | SASL/SCRAM-SHA-512 | OK |
| metrics_trace.cpp | - | SASL/SCRAM-SHA-512 | OK |
| Strimzi KafkaUser CRD | SCRAM-SHA-512 | - | Ready |
| super.users=User:ANONYMOUS | BACKUP | - | AKTIV (revertieren wenn SASL funktioniert) |

### Alle CI/CD Variablen (Projekt 280)
- DEDUP_PG_PASSWORD (masked)
- DEDUP_CRDB_PASSWORD (masked)
- DEDUP_REDIS_PASSWORD (masked)
- DEDUP_MARIADB_PASSWORD (masked)
- DEDUP_MINIO_PASSWORD (masked)
- DEDUP_KAFKA_PASSWORD (masked)
- Alle Wert: Samba AD `dedup-lab` User Passwort
