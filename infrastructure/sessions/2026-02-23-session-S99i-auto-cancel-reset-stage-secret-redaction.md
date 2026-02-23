# Session S99i: Auto-Cancel, Reset-Stage, Secret-Redaction, Dual-Remote Sync

**Datum:** 2026-02-23 (Fortsetzung von S99h)
**Agent:** system32 (Projekte/Kahan-Plan)

## Zusammenfassung

Fortsetzung von S99h. Drei grosse Aufgaben erledigt:
1. **CI Auto-Cancel + Parallele Reset-Stage** implementiert (workflow:auto_cancel, API-Cancel im Preflight, experiment:reset Stage mit 7 parallelen DB-Resets)
2. **Secret Redaction** aller `glpat-*` Tokens aus gesamter Git-History via `git-filter-repo` (Regex: `glpat-[A-Za-z0-9_-]{10,}`)
3. **Dual-Remote Sync** — identische redaktierte History auf GitLab UND GitHub, jeweils `development` + `master` Branch

---

## Erledigte Aufgaben

### 1. CI Auto-Cancel alter Pipelines (3 Mechanismen)

**User-Anforderung:** "eine neue Pipeline auch immer ALLE alten zu diesem Projekt canceln soll"

**a) workflow:auto_cancel (GitLab-nativ):**
```yaml
workflow:
  auto_cancel:
    on_new_commit: interruptible
```
Cancelt automatisch Build-Jobs mit `interruptible: true` in aelteren Pipelines desselben Branches wenn ein neuer Commit gepusht wird.

**b) API-Cancel im Preflight:**
Im `experiment:preflight` Job wird VOR den Health-Checks die GitLab API aufgerufen:
```bash
API_URL="http://gitlab-webservice-default.gitlab.svc:8181/api/v4/projects/${CI_PROJECT_ID}/pipelines"
for status in running pending; do
  RESPONSE=$(curl -sf --header "JOB-TOKEN: ${CI_JOB_TOKEN}" "${API_URL}?status=${status}&per_page=100")
  # Alle Pipelines ausser der aktuellen canceln
done
```
Nutzt `JOB-TOKEN` Auth (CI_JOB_TOKEN), braucht keine zusaetzlichen Secrets.

**c) interruptible: true auf Build-Jobs:**
- `build-k8s`, `build-debian-x86`, `build-ubuntu-x86`: `interruptible: true`
- `.exotic-base` (Lanes 4-7): `interruptible: true`
- `experiment:preflight`: `interruptible: true`
- `experiment:reset`: `interruptible: true`
- Experiment per-DB Jobs: NICHT interruptible (langlebig, werden via API gecancelt)

### 2. Parallele Reset-Stage

**User-Anforderung:** "auto reset als sequentieller eigener pipeline schritt vor der Ausfuehrung der Datenbanken... die einzelnen reset skripte parallelisieren"

**Neue Stage `experiment-reset`** zwischen `experiment-preflight` und `experiment-run`:

```yaml
experiment:reset:
  stage: experiment-reset
  image: bitnami/kubectl:latest
  needs: ["experiment:preflight"]
  # KEIN allow_failure — Resets sind PFLICHT (User-Direktive)
```

**Ausfuehrung:** Ein einzelner Job startet alle 7 Reset-Scripts als Bash-Background-Prozesse:
```bash
for db in mariadb clickhouse redis kafka minio postgresql cockroachdb; do
  bash "scripts/reset/reset_${db}.sh" > "/tmp/reset_${db}.log" 2>&1 &
done
# Warten, Logs sammeln, pass/fail zaehlen
```

**Dependency-Kette (AKTUELL):**
```
experiment:build → experiment:preflight (+ cancel alte Pipelines)
  → experiment:reset (7 DB-Resets parallel, PFLICHT)
    → experiment:mariadb → clickhouse → redis → kafka → minio → postgresql → cockroachdb
      → experiment:upload-results + cleanup
```

**WICHTIG:** `experiment:mariadb` Needs geaendert von `["experiment:preflight", "experiment:build"]` zu `["experiment:reset", "experiment:build"]`.

### 3. allow_failure entfernt (User-Direktive)

**User:** "Die resets sind nicht allow failure, weil sonst das experiment scheitert, alle resets sind pflicht"

`experiment:reset` hat KEIN `allow_failure: true`. Wenn Resets scheitern, stoppt die Pipeline.

### 4. Secret Redaction (git-filter-repo)

**Problem:** GitHub Push Protection blockierte Push wegen 3 `glpat-*` Tokens in Session-Dok S86:
- `REDACTED` (aktiver GitLab Token)
- `REDACTED` (abgelaufener Token)
- `REDACTED` (abgelaufener Token)

**Loesung:**
```bash
# Regex-Replacement fuer ALLE glpat-* Tokens
printf 'regex:glpat-[A-Za-z0-9_-]{10,}==>REDACTED\n' > /tmp/replace-tokens.txt
git filter-repo --replace-text /tmp/replace-tokens.txt --force
```

**User-Direktive:** "Du musst die secrets direkt in jedem betroffenen commit redacten, sodass es historisch nie existiert hat"

**Ergebnis:** Gesamte Git-History bereinigt. Kein `glpat-*` Token mehr in KEINEM Commit.

### 5. Dual-Remote Sync (GitLab + GitHub, konsistente History)

**User-Direktive:** "Bitte pushe die GitHub history konsistent auf die GitLab history, ueberschreibe die gitlab history"

**Ablauf:**
1. Temp-Clone erstellt → filter-repo mit Regex → alle Tokens entfernt
2. GitLab `development` Branch-Schutz temporaer via API aufgehoben (DELETE /protected_branches/development)
3. Force-Push redaktierte History auf GitLab (development + master)
4. Force-Push redaktierte History auf GitHub (development + master)
5. Branch-Schutz auf GitLab wiederhergestellt (POST /protected_branches, push_access_level=40, merge_access_level=40)
6. Lokales Repo mit `git fetch gitlab && git reset --hard gitlab/development` synchronisiert

**Finaler Stand:**

| Remote | Branch | HEAD | Tokens |
|--------|--------|------|--------|
| GitLab | development | `e30b383` | REDACTED |
| GitLab | master | `e30b383` | REDACTED |
| GitHub | development | `e30b383` | REDACTED |
| GitHub | master | `e30b383` | REDACTED |
| Lokal | development | `e30b383` | REDACTED |

### 6. Alte Pipelines gecancelt

- Pipeline #6921: canceling (manuell gecancelt)
- Pipeline #6922: canceled (manuell gecancelt)
- Pipeline #6923: canceled (manuell gecancelt)

---

## OFFENE AUFGABEN (Naechste Session)

### KRITISCH: Verwaiste Pipelines #6924 und #6925 canceln

**Status:** Durch den History-Rewrite (force-push) referenzieren #6924 und #6925 die alten Commit-Hashes (`99cc936`), die auf GitLab nicht mehr existieren. Sie laufen aber noch (`running`).

**Pipelines #6926 und #6927** (auf `e30b383`) sind die korrekten neuen Pipelines.

**Schritte:**
```bash
ssh root@10.0.10.201 'TOOLBOX=$(kubectl -n gitlab get pod -l app=toolbox -o jsonpath="{.items[0].metadata.name}") && kubectl -n gitlab exec $TOOLBOX -- python3 -c "
import urllib.request, json
TOKEN = \"REDACTED\"
for pid in [6921, 6924, 6925]:
    req = urllib.request.Request(
        f\"http://gitlab-webservice-default.gitlab.svc:8181/api/v4/projects/280/pipelines/{pid}/cancel\",
        method=\"POST\", headers={\"PRIVATE-TOKEN\": TOKEN}
    )
    try:
        resp = urllib.request.urlopen(req)
        data = json.loads(resp.read())
        st = data.get(\"status\", \"?\")
        print(f\"Pipeline {pid}: -> {st}\")
    except Exception as e:
        print(f\"Pipeline {pid}: {e}\")
"'
```

Falls `canceling` nicht zu `canceled` wechselt: Einzelne Jobs via API canceln:
```bash
# Jobs der Pipeline auflisten
# GET /api/v4/projects/280/pipelines/{PID}/jobs
# Jeden running Job einzeln canceln
# POST /api/v4/projects/280/jobs/{JOB_ID}/cancel
```

### KRITISCH: Pipeline #6926 (development) verifizieren

**Status:** Pipeline #6926 ist `pending` auf Commit `e30b383` (redaktierte History). Dies ist die KORREKTE Pipeline.

**Erwarteter Ablauf:**
1. Build-Stage: 7 parallele Builds (interruptible)
2. experiment:build → experiment:preflight (cancelt alte Pipelines via API)
3. experiment:reset (7 DB-Resets parallel via bitnami/kubectl)
4. experiment:mariadb → clickhouse → redis → kafka → minio → postgresql → cockroachdb

**Verifikationspunkte:**
- experiment:reset Job: Pruefen ob `bitnami/kubectl` Image gepullt werden kann
- experiment:reset Job: Pruefen ob kubectl pods/exec RBAC-Berechtigung hat
- experiment:mariadb Log: `[INF] [metrics_trace] Kafka producer ready (...:9094, user=dedup-lab)` (NICHT 9092!)
- Kafka MetricsTrace: Keine `SASL FAIL` oder `brokers are down` Meldungen

**Runner-4 Polling-Bug:** Nach JEDEM Job muss der Runner-4 Pod neugestartet werden:
```bash
kubectl -n gitlab-runner delete pod -l app=gitlab-runner-4 --grace-period=5
```

### KRITISCH: experiment:reset RBAC pruefen

**Problem:** Der `experiment:reset` Job laeuft auf `bitnami/kubectl:latest` im K8s Runner. Die Reset-Scripts nutzen `kubectl exec` um in DB-Pods zu gelangen. Dafuer braucht der Service Account des Runner-Pods `pods/exec` Permission in ALLEN DB-Namespaces.

**Betroffene Namespaces:**
- `databases` (MariaDB, ClickHouse, PostgreSQL)
- `redis`
- `kafka`
- `minio`
- `cockroach-operator-system`
- `gitlab-runner` (fuer NFS Checkpoint-Cleanup)

**Pruefen:**
```bash
# SA des K8s Runners ermitteln
kubectl -n gitlab-runner get pod -l app=gitlab-runner-4 -o jsonpath='{.items[0].spec.serviceAccountName}'

# RBAC pruefen
kubectl auth can-i create pods/exec -n databases --as=system:serviceaccount:gitlab-runner:<SA_NAME>
```

**Fix falls fehlend:**
```yaml
apiVersion: rbac.authorization.k8s.io/v1
kind: ClusterRole
metadata:
  name: experiment-reset-access
rules:
  - apiGroups: [""]
    resources: ["pods/exec"]
    verbs: ["create"]
  - apiGroups: [""]
    resources: ["pods"]
    verbs: ["get", "list"]
  - apiGroups: [""]
    resources: ["secrets"]
    verbs: ["get"]
---
apiVersion: rbac.authorization.k8s.io/v1
kind: ClusterRoleBinding
metadata:
  name: experiment-reset-binding
subjects:
  - kind: ServiceAccount
    name: <SA_NAME>
    namespace: gitlab-runner
roleRef:
  kind: ClusterRole
  name: experiment-reset-access
  apiGroup: rbac.authorization.k8s.io
```

**WICHTIG:** Falls RBAC scheitert, schlaegt der gesamte Reset fehl und die Pipeline stoppt (kein allow_failure!). Entweder RBAC fixen oder temporaer `allow_failure: true` setzen bis RBAC steht.

### MITTEL: Kafka super.users CRD-Patch revertieren

**Status:** `User:ANONYMOUS` als super.user hinzugefuegt als Backup-Loesung fuer Kafka-Zugriff.

**Wann revertieren:** Sobald Kafka SASL auf Port 9094 in der Pipeline verifiziert funktioniert (Log: `Kafka producer ready (...:9094, user=dedup-lab)` und Daten in `dedup-lab-metrics` Topic ankommen).

**Revert-Befehl:**
```bash
kubectl -n kafka patch kafka kafka-cluster --type merge \
  -p '{"spec":{"kafka":{"config":{"super.users":null}}}}'
```

**Verifikation nach Revert:**
```bash
# Pruefen ob Kafka-Broker den Patch angenommen hat
kubectl -n kafka get kafka kafka-cluster -o jsonpath='{.spec.kafka.config.super\.users}'
# Sollte leer sein

# Pruefen ob SASL-User dedup-lab noch funktioniert
kubectl -n kafka exec kafka-cluster-broker-0 -- /opt/kafka/bin/kafka-acls.sh \
  --bootstrap-server localhost:9092 --list --principal User:dedup-lab
```

### MITTEL: Kafka DB-Experiment Port (databases.kafka.port)

**Status:** `config.example.json` hat `databases[].kafka.port: 9092` (plain Listener).
Der `metrics_trace.kafka_bootstrap` wurde auf 9094 gefixt, aber der Kafka-DB-Connector nutzt den `databases[].kafka` Eintrag.

**Abhaengigkeit:** Haengt von der super.users Entscheidung ab:
- Falls `super.users=User:ANONYMOUS` BLEIBT: Port 9092 ist OK (ANONYMOUS hat full access)
- Falls `super.users` REVERTIERT wird: Port muss auf 9094 UND SASL-Config im Kafka-DB-Connector noetig

**Fix falls noetig:**
```json
// config.example.json databases[].kafka
{
    "system": "kafka",
    "host": "kafka-cluster-kafka-bootstrap.kafka.svc.cluster.local",
    "port": 9094,  // war 9092
    ...
}
```
PLUS: C++ Kafka-DB-Connector muss SASL-Support haben (wie MetricsTrace).

### MITTEL: Pipeline #6927 (master) canceln oder laufen lassen

**Status:** Pipeline #6927 wurde durch den Push auf `master` Branch ausgeloest. CI-Jobs laufen fuer BEIDE Branches (development + master), was Runner-Ressourcen verdoppelt.

**Optionen:**
1. Pipeline #6927 canceln (master braucht keine CI, nur Archiv-Branch)
2. `.gitlab-ci.yml` anpassen: `rules:` nur fuer `development` Branch
3. GitLab Branch-Settings: CI fuer `master` deaktivieren

**Empfehlung:** Pipeline canceln und `master` als reinen Archiv-Branch nutzen. CI laeuft nur auf `development`.

### NIEDRIG: Runner-Tag Permanent Fix

CronJob `runner-tag-enforcer` laeuft jede Minute als Workaround. Permanenter Fix:
```bash
TOOLBOX=$(kubectl -n gitlab get pod -l app=toolbox -o jsonpath='{.items[0].metadata.name}')
kubectl -n gitlab exec $TOOLBOX -- gitlab-rake db:migrate:down VERSION=20241219100359
kubectl -n gitlab exec $TOOLBOX -- gitlab-rake db:migrate:up VERSION=20241219100359
```

### NIEDRIG: GitHub Default Branch auf `development` setzen

**Status:** GitHub hat jetzt `master` UND `development`. Default Branch sollte `development` sein (dort passiert die aktive Entwicklung).

```bash
gh repo edit BenniProbst/dedup-database-analysis --default-branch development
```

---

## Commits dieser Session (S99i)

| Commit (redaktiert) | Beschreibung | Remote |
|---------------------|-------------|--------|
| `e4bebbd` (war `aa73d1c`) | fix: Kafka SCRAM port 9092->9094, reset scripts fix, CI checkpoint reset | GitLab + GitHub |
| `97f0fff` (war `e36fbdd`) | feat: auto-cancel old pipelines, parallel reset stage, interruptible builds | GitLab + GitHub |
| `e30b383` (war `99cc936`) | fix: experiment:reset is mandatory, remove allow_failure | GitLab + GitHub |

**HINWEIS:** Commit-Hashes haben sich durch `git-filter-repo` geaendert. Die Commits ab `b2db072` (war `c68c01a`) haben neue Hashes weil der Token entfernt wurde. Aeltere Commits (vor dem Token-Commit) haben dieselben Hashes.

## Dateien geaendert (S99i, zusaetzlich zu S99h)

| Datei | Aenderung |
|-------|-----------|
| `.gitlab-ci.yml` | workflow:auto_cancel:on_new_commit:interruptible |
| `.gitlab-ci.yml` | experiment-reset Stage hinzugefuegt |
| `.gitlab-ci.yml` | Cancel-API im Preflight-Job |
| `.gitlab-ci.yml` | interruptible: true auf 5 Build-Jobs + Preflight + Reset |
| `.gitlab-ci.yml` | experiment:mariadb needs → experiment:reset statt preflight |
| `.gitlab-ci.yml` | allow_failure entfernt von experiment:reset (User-Direktive) |
| `infrastructure/sessions/S99i` | Diese Session-Dok |

---

## Kontext fuer naechste Session

1. **Pipelines #6924 + #6925 CANCELN** — laufen auf alten Commit-Hashes (pre-redaction), verbrauchen Runner-Ressourcen
2. **Pipeline #6926 (development, e30b383) ist die KORREKTE Pipeline** — muss verifiziert werden
3. **Pipeline #6927 (master) CANCELN** — master braucht keine CI
4. **experiment:reset RBAC** — bitnami/kubectl braucht pods/exec in 6+ Namespaces
5. **Kafka SASL verifizieren** — Log muss `:9094, user=dedup-lab` zeigen (nicht 9092!)
6. **Runner-4 Polling-Bug** — nach JEDEM Job Pod restarten!
7. **Kafka super.users revertieren** wenn SASL funktioniert
8. **databases.kafka.port** entscheiden (9092 plain vs 9094 SCRAM, abhaengig von super.users)
9. **ALLE Remotes KONSISTENT** — GitLab + GitHub haben identische redaktierte History auf development + master

### Git Remote Uebersicht (nach Redaction)

| Remote | URL | Branches |
|--------|-----|----------|
| gitlab | `https://oauth2:REDACTED@gitlab.comdare.de/comdare/research/dedup-database-analysis.git` | development, master |
| github | `https://github.com/BenniProbst/dedup-database-analysis.git` | development, master |

### Pipeline-Status (Stand Session-Ende)

| Pipeline | Status | Branch | Commit | Aktion |
|----------|--------|--------|--------|--------|
| #6927 | pending | master | e30b383 | CANCELN (master braucht keine CI) |
| #6926 | pending | development | e30b383 | VERIFIZIEREN (korrekte Pipeline) |
| #6925 | running | master | 99cc936 | CANCELN (alter Commit, pre-redaction) |
| #6924 | running | development | 99cc936 | CANCELN (alter Commit, pre-redaction) |
| #6923 | canceled | development | e36fbdd | OK |
| #6922 | canceled | development | aa73d1c | OK |
| #6921 | canceling | development | 0571a00 | Sollte bald canceled sein |

### CI Pipeline Stages (AKTUELL)

```
stages:
  - build                    # 7 parallele Lanes (interruptible)
  - aggregate               # Ergebnisse sammeln
  - upload                  # MinIO Artefakte
  - experiment-build        # dedup-test Binary
  - experiment-preflight    # Health-Checks + Auto-Cancel (interruptible)
  - experiment-reset        # 7 DB-Resets parallel (PFLICHT, interruptible)
  - experiment-run          # MariaDB → CH → Redis → Kafka → MinIO → PG → CRDB
  - experiment-cleanup      # Upload Results + Manual Cleanup
```

### Kafka Listener Uebersicht
| Name | Port | TLS | Auth |
|------|------|-----|------|
| plain | 9092 | No | none |
| tls | 9093 | Yes | none |
| scram | 9094 | No | SCRAM-SHA-512 |

### Experiment-Job Config-Pfade
| Kontext | Config-Datei | Beschreibung |
|---------|-------------|--------------|
| CI Pipeline | `src/cpp/config.example.json` | Wird von `.gitlab-ci.yml` genutzt |
| K8s Job | `k8s/base/configmap.yaml` -> ConfigMap | Wird von `experiment-job.yaml` gemountet |

**WICHTIG:** Beide Configs muessen synchron gehalten werden (gleiche Ports, Hosts, etc.)!

### Secret Redaction Methode (fuer Zukunft)
```bash
# Bei neuen Tokens in History:
git clone <REPO> /tmp/redact-clone
cd /tmp/redact-clone
printf 'regex:<PATTERN>==>REDACTED\n' > /tmp/replace.txt
git filter-repo --replace-text /tmp/replace.txt --force
# Remotes hinzufuegen + force-push
# GitLab Branch-Schutz temporaer aufheben: DELETE /protected_branches/{branch}
# Nach Push: POST /protected_branches (push_access_level=40, merge_access_level=40)
```
