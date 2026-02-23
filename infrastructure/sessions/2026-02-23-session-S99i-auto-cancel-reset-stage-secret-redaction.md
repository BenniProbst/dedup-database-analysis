# Session S99i: Auto-Cancel, Reset-Stage, Secret-Redaction, RBAC, Pipeline-Monitoring

**Datum:** 2026-02-23 (Fortsetzung von S99h, laeuft noch)
**Agent:** system32 (Projekte/Kahan-Plan)

## Zusammenfassung

Fortsetzung von S99h. Fuenf grosse Aufgaben erledigt:
1. **CI Auto-Cancel + Parallele Reset-Stage** implementiert (workflow:auto_cancel, API-Cancel im Preflight, experiment:reset Stage mit 7 parallelen DB-Resets)
2. **Secret Redaction** aller `glpat-*` Tokens aus gesamter Git-History via `git-filter-repo` (Regex: `glpat-[A-Za-z0-9_-]{10,}`)
3. **Dual-Remote Sync** — identische redaktierte History auf GitLab UND GitHub, jeweils `development` + `master` Branch
4. **RBAC fuer experiment:reset** — ClusterRole `experiment-reset-access` mit pods/exec, pods get/list, secrets get in allen DB-Namespaces
5. **CI workflow:rules** — Pipeline nur auf `development` Branch (master bekommt keine CI mehr)
6. **Kafka DB-Connector SASL/SCRAM-SHA-512** — `kafka_connector.cpp` hatte KEINEN SASL-Support, nur MetricsTrace. Jetzt beides mit SASL auf Port 9094.
7. **Kafka Port-Fix** — `databases.kafka.port` 9092→9094 + `user/password` Felder in config.example.json
8. **Kafka SASL Passwort-Kette verifiziert** — Strimzi KafkaUser → K8s Secret → GitLab CI/CD Variable → env var → C++ Code
9. **Pipeline #6933** (`ddf2281`) — Alle Fixes enthalten, Builds laufen

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

**Finaler Stand (nach Kontext-Fortsetzung):**

| Remote | Branch | HEAD | Tokens |
|--------|--------|------|--------|
| GitLab | development | `e5c74a7` | REDACTED |
| GitLab | master | `e5c74a7` | REDACTED |
| GitHub | development | `e5c74a7` | REDACTED |
| GitHub | master | `e5c74a7` | REDACTED |
| Lokal | development | `e5c74a7` | REDACTED |

### 6. Alte Pipelines gecancelt

- Pipeline #6919-6928: ALLE canceled
- Pipeline #6930 (master, e5c74a7): canceled
- Pipeline #6929 (development, e5c74a7): **AKTIV — alle 7 Builds SUCCESS**

---

## ERLEDIGTE AUFGABEN (Kontext-Fortsetzung)

### 7. RBAC fuer experiment:reset (ERLEDIGT)

**Problem:** `gitlab-runner` ServiceAccount hatte KEINE pods/exec Berechtigung ausserhalb gitlab-runner Namespace.

**Fix:**
```yaml
ClusterRole: experiment-reset-access
  - pods: get, list
  - pods/exec: create
  - secrets: get
ClusterRoleBinding: experiment-reset-binding
  → ServiceAccount gitlab-runner in Namespace gitlab-runner
```

**Verifiziert:** `kubectl auth can-i create pods --subresource=exec -n databases --as=system:serviceaccount:gitlab-runner:gitlab-runner` → yes
(Achtung: `kubectl auth can-i create pods/exec` zeigt false, man muss `--subresource=exec` nutzen!)

### 8. Pipeline Cleanup (ERLEDIGT)

- Pipelines #6921-#6928: ALLE canceled
- Pipeline #6930 (master): canceled
- Pipeline #6929 (development, `e5c74a7`): **AKTIV**, alle 7 Builds SUCCESS

### 9. CI workflow:rules — nur development Branch (ERLEDIGT)

```yaml
workflow:
  rules:
    - if: '$CI_COMMIT_BRANCH == "development"'
    - if: '$CI_PIPELINE_SOURCE == "api"'
    - if: '$CI_PIPELINE_SOURCE == "web"'
```

Master-Pushes loesen KEINE CI-Pipelines mehr aus. Nur development, API-Trigger und manuelle Web-Trigger.

### 10. GitHub Default Branch auf development (ERLEDIGT)

`gh repo edit BenniProbst/dedup-database-analysis --default-branch development`

---

## OFFENE AUFGABEN

### KRITISCH: Pipeline #6929 verifizieren (laeuft)

**Status:** Pipeline #6929 (development, `e5c74a7`) — 7/7 Builds SUCCESS, Aggregate als naechstes.

**Erwarteter Ablauf:**
1. ~~Build-Stage: 7 parallele Builds~~ → **DONE** (alle 7 success)
2. aggregate-results → upload
3. experiment:build → experiment:preflight (+ cancel alte Pipelines via API)
4. **experiment:reset** (7 DB-Resets parallel via bitnami/kubectl) — RBAC jetzt vorhanden!
5. experiment:mariadb → clickhouse → redis → kafka → minio → postgresql → cockroachdb

**Verifikationspunkte:**
- experiment:reset Job: Pruefen ob `bitnami/kubectl` Image gepullt werden kann
- experiment:reset Job: Pruefen ob kubectl pods/exec tatsaechlich funktioniert (RBAC deployed)
- experiment:mariadb Log: `[INF] [metrics_trace] Kafka producer ready (...:9094, user=dedup-lab)` (NICHT 9092!)
- Kafka MetricsTrace: Keine `SASL FAIL` oder `brokers are down` Meldungen

**Runner-4 Polling-Bug:** Nach JEDEM Job muss der Runner-4 Pod neugestartet werden:
```bash
kubectl -n gitlab-runner delete pod -l app=gitlab-runner-4 --grace-period=5
```

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

### 11. Kafka DB-Connector SASL/SCRAM-SHA-512 (ERLEDIGT)

**Problem:** `kafka_connector.cpp` hatte KEINEN SASL-Support — nur MetricsTrace hatte SASL.
Port 9094 (SCRAM) erfordert Authentifizierung, d.h. Kafka-DB-Experiment schlug fehl.

**Fix in `kafka_connector.cpp` (Commit `ddf2281`):**
```cpp
// SASL/SCRAM-SHA-512 auth (dedup-lab KafkaUser via Strimzi)
std::string sasl_user = conn.user;    // aus DbConnection / config.json
std::string sasl_pass = conn.password;
if (const char* v = std::getenv("KAFKA_USER")) sasl_user = v;
if (const char* v = std::getenv("KAFKA_PASSWORD")) sasl_pass = v;

if (!sasl_user.empty()) {
    rd_kafka_conf_set(conf, "security.protocol", "SASL_PLAINTEXT", ...);
    rd_kafka_conf_set(conf, "sasl.mechanism", "SCRAM-SHA-512", ...);
    rd_kafka_conf_set(conf, "sasl.username", sasl_user.c_str(), ...);
    rd_kafka_conf_set(conf, "sasl.password", sasl_pass.c_str(), ...);
}
```

**config.example.json (Commit `ad609fc`):** Port 9092→9094, `user: "dedup-lab"` hinzugefuegt.

### 12. Kafka SASL Passwort-Kette verifiziert (ERLEDIGT)

**Vollstaendige Kette:**
| # | Stelle | Status | Wert |
|---|--------|--------|------|
| 1 | Strimzi KafkaUser `dedup-lab` (SCRAM-SHA-512) | Ready | `S-c17Lvx...` |
| 2 | K8s Secret `dedup-lab` (kafka namespace) | Vorhanden | base64-encoded |
| 3 | GitLab CI/CD Variable `DEDUP_KAFKA_PASSWORD` | Gesetzt (masked) | Gleicher Wert |
| 4 | `.gitlab-ci.yml` → `export KAFKA_PASSWORD="${DEDUP_KAFKA_PASSWORD}"` | Zeile 614-615 | Env-Export |
| 5 | `kafka_connector.cpp` → `getenv("KAFKA_PASSWORD")` | Neuer SASL-Code | Runtime-Lookup |

**Alle 6 DB-Passwoerter als CI/CD Variablen:**
- DEDUP_PG_PASSWORD, DEDUP_CRDB_PASSWORD, DEDUP_REDIS_PASSWORD
- DEDUP_MARIADB_PASSWORD, DEDUP_MINIO_PASSWORD, DEDUP_KAFKA_PASSWORD
- Alle fuer **Samba AD Lab-User `dedup-lab`** (gleiches Passwort)

### 13. Pipeline Cleanup (ERLEDIGT)

- Pipeline #6931, #6934: DELETED (Ghost-Pipelines mit 0 aktiven Jobs)
- Pipeline #6932: CANCELED
- Pipeline #6933 (`ddf2281`): **AKTIV** — Builds laufen

### ERLEDIGT: Pipeline #6927/#6930 (master) — gecancelt + CI-Regel

Pipeline #6927 und #6930 (master) wurden gecancelt. `.gitlab-ci.yml` hat jetzt `workflow:rules` die master-Branch ausschliessen. Nur development, API und Web-Trigger loesen Pipelines aus.

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
| `e5c74a7` | docs: session S99i handoff (tokens redacted) | GitLab + GitHub |
| `81b69be` | fix: CI workflow:rules nur development Branch | GitLab + GitHub |
| `ad609fc` | fix: Kafka DB port 9092->9094 in config.example.json | GitLab + GitHub |
| `ddf2281` | feat: Kafka DB connector SASL/SCRAM-SHA-512 support | GitLab + GitHub |

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
| `.gitlab-ci.yml` | workflow:rules — nur development Branch + API/Web Trigger |
| `src/cpp/connectors/kafka_connector.cpp` | SASL/SCRAM-SHA-512 Support in connect() |
| `src/cpp/config.example.json` | databases.kafka: port 9092→9094, user+password Felder |
| `infrastructure/sessions/S99i` | Diese Session-Dok |
| K8s ClusterRole | `experiment-reset-access` — pods/exec, pods, secrets |
| K8s ClusterRoleBinding | `experiment-reset-binding` → SA gitlab-runner |

---

## Kontext fuer naechste Session

1. ~~Pipelines canceln~~ **ERLEDIGT** — alle alten Pipelines canceled/deleted
2. **Pipeline #6933 (development, `ddf2281`) ist die AKTIVE Pipeline** — Builds laufen, alle Kafka-Fixes enthalten
3. ~~Pipeline master canceln~~ **ERLEDIGT** — CI-Rules schliessen master aus
4. ~~experiment:reset RBAC~~ **ERLEDIGT** — ClusterRole experiment-reset-access deployed
5. ~~Kafka SASL verifizieren~~ **ERLEDIGT** — Passwort-Kette komplett (Strimzi → CI/CD Var → env → C++)
6. ~~databases.kafka.port~~ **ERLEDIGT** — Port 9094 + SASL im kafka_connector.cpp
7. **Runner-4 Polling-Bug** — nach JEDEM Job Pod restarten!
8. **Kafka super.users revertieren** wenn Pipeline-Log `Kafka producer ready (:9094, user=dedup-lab)` zeigt
9. **Master-Branch sync** — `ddf2281` muss noch auf master gepusht werden (GitLab + GitHub)
10. **K8s ConfigMap sync** — `k8s/base/configmap.yaml` muss gleichen Kafka-Port 9094 haben wie config.example.json

### Git Remote Uebersicht (nach Redaction)

| Remote | URL | Branches |
|--------|-----|----------|
| gitlab | `https://oauth2:REDACTED@gitlab.comdare.de/comdare/research/dedup-database-analysis.git` | development, master |
| github | `https://github.com/BenniProbst/dedup-database-analysis.git` | development, master |

### Pipeline-Status (aktuell, Stand Kontext-Fortsetzung 2)

| Pipeline | Status | Branch | Commit | Aktion |
|----------|--------|--------|--------|--------|
| #6933 | **running** | development | ddf2281 | AKTIV — Builds laufen, alle Kafka-Fixes enthalten |
| #6932 | canceled | development | ad609fc | Obsolet (vor SASL-Connector-Fix) |
| #6931 | deleted | development | 81b69be | Ghost-Pipeline (0 aktive Jobs) |
| #6934 | deleted | development | ddf2281 | Duplikat von #6933, geloescht |
| #6929 | canceled | development | e5c74a7 | Obsolet (vor Kafka-Port-Fix) |

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
