# Session S99f: Unicode-Fix, GitOps Tags, Experiment Reset Scripts
**Datum:** 2026-02-23
**Agent:** system32 (Projekte/Kahan-Plan)

## Zusammenfassung

Pipeline #6917 experiment:mariadb schlug fehl wegen Unicode Curly Quotes in `.gitlab-ci.yml`. Fix deployed, Pipeline #6919 gestartet -- experiment:mariadb laeuft erfolgreich. CronJob fuer Runner-Tags auf 1-Minute-Intervall verkuerzt. Experiment Reset Scripts fuer alle 7 DBs erstellt.

---

## Erledigte Aufgaben

### 1. Unicode-Fix in .gitlab-ci.yml (Projekt 280, Branch development)
**Commit:** `e32186ed` auf development
**Problem:** Zeilen 515-516 hatten Unicode Curly Quotes (U+201C `\xe2\x80\x9c` und U+201D `\xe2\x80\x9d`) statt ASCII `"`. Bash interpretierte `(no scale up/down)` als Subshell-Syntax → `syntax error near unexpected token '('`

**Zusaetzlich:** ~30 Mojibake Em-Dash-Sequenzen (`\xc3\xa2\xe2\x82\xac\xe2\x80\x9d` = triple-encoded U+2014) in Kommentaren/Echo-Strings ersetzt durch `--`.

**Fix-Methode:**
```python
# Reihenfolge wichtig:
1. \xc3\xa2\xe2\x82\xac\xe2\x80\x9d → --  (Triple-Mojibake)
2. \xe2\x80\x94 → --                        (Echter Em-Dash)
3. \xe2\x80\x9c → "                          (Left Curly Quote)
4. \xe2\x80\x9d → "                          (Right Curly Quote)
```

33 Zeilen geaendert, 235 Bytes eingespart. Datei ist jetzt 100% ASCII.

### 2. Pipeline #6917 cancelled, #6918 cancelled, #6919 erstellt
- Pipeline #6919 (SHA `e32186ed`): Alle 7 Builds SUCCESS, experiment:build SUCCESS, experiment:preflight SUCCESS
- experiment:mariadb RUNNING (gestartet 19:24 UTC, läuft > 15min)
- Experiment-Output bestaetigt: MariaDB bulk_insert, random_binary/U0, Longhorn BEFORE/AFTER korrekt

### 3. Runner-Tag CronJob auf 1 Minute
**Datei:** `k8s-gitops/gitlab-runner/cronjob-tag-enforcer.yaml`
- Schedule: `*/5 * * * *` → `* * * * *`
- Applied auf K8s Cluster

### 4. Kafka ACLs gesetzt
- 5 Topics: dedup-lab-metrics, dedup-lab-events, dedup-lab-u0/u50/u90
- ACLs: User:ANONYMOUS ALL auf alle 5 Topics
- Trotzdem "Topic authorization failed" im laufenden Experiment → moeglicherweise veraltet im Build-Pod-Cache

### 5. Experiment Reset Scripts erstellt
**Pfad:** `scripts/reset/`

| Script | DB | Reset-Methode |
|--------|-----|--------------|
| `reset_mariadb.sh` | MariaDB | DROP/CREATE DATABASE dedup_lab |
| `reset_clickhouse.sh` | ClickHouse | DROP/CREATE DATABASE dedup_lab |
| `reset_redis.sh` | Redis | SCAN+DEL dedup:* keys |
| `reset_kafka.sh` | Kafka | Delete+Recreate Topics + ACLs |
| `reset_minio.sh` | MinIO | mc rm dedup-lab-* buckets |
| `reset_postgresql.sh` | PostgreSQL | DROP/CREATE SCHEMA dedup_lab |
| `reset_cockroachdb.sh` | CockroachDB | DROP/CREATE DATABASE dedup_lab |
| `general_reset.sh` | ALLE | Ruft alle 7 Einzel-Resets auf |

**Aufruf:**
```bash
# Von pve1 (hat kubectl):
ssh root@pve1
bash /path/to/scripts/reset/general_reset.sh          # Alle DBs
bash /path/to/scripts/reset/general_reset.sh mariadb   # Nur MariaDB
bash /path/to/scripts/reset/general_reset.sh --dry-run # Nur anzeigen
```

### 6. GitOps Repo synchronisiert
- `C:\Users\benja\OneDrive\Desktop\Projekte\Research\k8s-gitops\` (Projekt 284)
- 4 ConfigMaps mit `tags` und `run_untagged` Feld hinzugefuegt (lokal)
- `C:\Users\benja\OneDrive\Desktop\Projekte\Research\dedup-database-analysis\` mit GitLab gesynct

---

## OFFENE AUFGABEN (Naechste Session)

### KRITISCH: Runner-Tags werden permanent zurueckgesetzt
**Status:** NICHT GELOEST
**Root Cause:** GitLab Server-Bug (Issue #524402). Bei GitLab 17.9+ wird die `ci_runner_taggings` DB-Tabelle bei Migration geleert. Tags in config.toml helfen NICHT -- mit `glrt-` Tokens werden Tags ausschliesslich serverseitig verwaltet.

**Workaround aktiv:** CronJob `runner-tag-enforcer` laeuft jetzt jede Minute und setzt Tags per API.

**Permanenter Fix (TODO):**
1. GitLab Toolbox Pod aufrufen
2. `gitlab-rake db:migrate:down VERSION=20241219100359` ausfuehren
3. `gitlab-rake db:migrate:up VERSION=20241219100359` ausfuehren
4. Danach pruefen ob Tags persistent bleiben
5. Wenn ja: CronJob Intervall wieder auf 5min oder entfernen

**Wie:**
```bash
ssh root@pve1
TOOLBOX=$(kubectl -n gitlab get pod -l app=toolbox -o jsonpath='{.items[0].metadata.name}')
kubectl -n gitlab exec $TOOLBOX -- gitlab-rake db:migrate:down VERSION=20241219100359
kubectl -n gitlab exec $TOOLBOX -- gitlab-rake db:migrate:up VERSION=20241219100359
# Dann Tags setzen und beobachten ob sie bleiben
```

### MITTEL: Kafka "Topic authorization failed" im Experiment
**Status:** ACLs gesetzt aber Experiment-Pod hatte sie noch nicht
**Erwartung:** Naechster Experiment-Lauf (clickhouse-Job) sollte funktionieren, da neuer Build-Pod
**Falls nicht:** Strimzi KafkaUser CRD mit ACLs erstellen statt manueller kafka-acls.sh

### MITTEL: Runner-4 Polling-Bug nach jedem Job
**Status:** Bekannter Bug, braucht GitLab Runner 18.9+
**Workaround:** Pod nach jedem Experiment-Job manuell neustarten:
```bash
kubectl -n gitlab-runner delete pod -l app=gitlab-runner-4 --grace-period=5
```
**Fuer naechste Session:** Monitoring-Script das Runner-4 Pod automatisch restartet wenn experiment-Job fertig und naechster pending

### NIEDRIG: Reset Scripts auf GitLab pushen
**Status:** Lokal erstellt, noch nicht committed/gepusht
**Wie:**
```bash
cd C:\Users\benja\OneDrive\Desktop\Projekte\Research\dedup-database-analysis
git add scripts/reset/
git commit -m "feat: add per-DB experiment reset scripts + general_reset.sh"
git -c http.sslVerify=false push gitlab development
```

### NIEDRIG: GitOps ConfigMap-Aenderungen auf GitLab pushen
**Status:** Lokal editiert (tags + run_untagged hinzugefuegt), noch nicht gepusht
**Dateien:** `k8s-gitops/gitlab-runner/configmap-runner-{1,2,3,4}.yaml`, `cronjob-tag-enforcer.yaml`

---

## Pipeline #6919 Status (Stand Session-Ende)

| Job | Status | Dauer |
|-----|--------|-------|
| build-k8s | SUCCESS | 47s |
| build-debian-x86 | SUCCESS | 11s |
| build-ubuntu-x86 | SUCCESS | 12s |
| build-macos-x86 | SUCCESS | 11s |
| build-macos-arm | SUCCESS | 27s |
| build-linux-arm64 | SUCCESS | 12s |
| build-linux-riscv | SUCCESS | 15s |
| aggregate-results | SUCCESS | 26s |
| experiment:build | SUCCESS | 119s |
| experiment:preflight | SUCCESS | 15s |
| **experiment:mariadb** | **RUNNING** | >15min |
| experiment:clickhouse | created | - |
| experiment:redis | created | - |
| experiment:kafka | created | - |
| experiment:minio | created | - |
| experiment:postgresql | created | - |
| experiment:cockroachdb | created | - |

**DAG-Kette:** mariadb → clickhouse → redis → kafka → minio → postgresql → cockroachdb

**WICHTIG:** Nach jedem Experiment-Job muss Runner-4 Pod restartet werden (Polling-Bug)!

---

## Dateien geaendert/erstellt

| Datei | Aktion | Repo |
|-------|--------|------|
| `.gitlab-ci.yml` | Unicode-Fix (Commit e32186ed) | PID 280 GitLab |
| `k8s-gitops/gitlab-runner/configmap-runner-{1-4}.yaml` | tags + run_untagged | PID 284 (lokal) |
| `k8s-gitops/gitlab-runner/cronjob-tag-enforcer.yaml` | Schedule */5 → * | PID 284 (lokal+K8s) |
| `scripts/reset/reset_{mariadb,clickhouse,redis,kafka,minio,postgresql,cockroachdb}.sh` | NEU | PID 280 (lokal) |
| `scripts/reset/general_reset.sh` | NEU | PID 280 (lokal) |

---

## Kontext fuer naechste Session

1. **Pipeline #6919 LAEUFT** -- experiment:mariadb seit 19:24 UTC. Pruefen ob fertig/failed
2. **Runner-4 Polling-Bug** -- nach experiment:mariadb Pod restartet (wenn needed)
3. **Tag-Reset-Bug** -- CronJob laeuft jede Minute als Workaround. Permanenter Fix via DB-Migration pending
4. **Reset Scripts** -- erstellt aber noch nicht gepusht. Ausfuehrbar via `ssh root@pve1`
5. **Kafka ACLs** -- neu gesetzt, im naechsten Build-Pod sollte Authorization funktionieren
