# Session 95c - Experiment-Pipeline Fix: JSON Config + apt-get Deadlock

**Datum:** 2026-02-22
**Typ:** Experiment-Pipeline Debugging & Fixes
**Vorgaenger:** S95/S95b (Experiment-Pipeline), S74c/S74d (Experiment-Spezifikation + Node-Isolation)
**Status:** Pipeline #5059 LAEUFT (postgresql aktiv, Kaskade ueber Nacht)

---

## Kontext

TU Dresden Forschungsprojekt "Deduplikation in Datenhaltungssystemen" (GitLab Project 280, `dedup-database-analysis`).
7 Datenbanksysteme (PostgreSQL, CockroachDB, MariaDB, ClickHouse, Redis, Kafka, MinIO) werden auf einer dedizierten Node (talos-say-ls6) mit Taint `experiment=dedicated:NoSchedule` getestet.
Runner-4 (ID 17) ist exklusiv fuer das Experiment konfiguriert (`experiment` Tag, `run_untagged=false`).

Vorherige Sessions S95/S95b hatten mit konkurrierenden CI-Template-Updates durch den Kahan-Plan Agent zu kaempfen (v5.0 -> v5.1 -> v5.2 -> v5.3). User-Direktive: "Wenn Config ueberschrieben wird, einfach Experiment-Stages wieder ergaenzen."

---

## Behobene Probleme

### 1. JSON Config Parse Error (KRITISCH)

**Fehler:**
```
nlohmann::json parse_error at line 1, column 1: invalid literal '#'
```

**Ursache:** `.gitlab-ci.yml` Zeile 368 verwendete `--config k8s/base/configmap.yaml` - eine K8s ConfigMap YAML-Datei. Das C++ Binary erwartet jedoch reines JSON.

**Fix:** Pfad geaendert zu `--config src/cpp/config.example.json`

**Commit:** `ea094ff`

**Lektion:** K8s ConfigMap YAML ist nicht JSON. Niemals direkt als `--config` Parameter verwenden.

---

### 2. Merge-Konflikt mit Kahan-Plan v5.4.1

Der Kahan-Plan Agent pushte CI Template v5.4.1 (python3 S3v4 Upload, 15min K8s Timeout) waehrend der Experiment-Arbeit.

**Aufloesung:**
- v5.4.1 Aenderungen behalten
- Experiment-Stages (`experiment-build`, `experiment-preflight`, `experiment-run`, `experiment-cleanup`) in `stages:`-Liste wieder angehaengt
- Experiment-Job-Definitionen am Ende der Datei beibehalten

**Commit:** `e5bf3ac` (Merge)

---

### 3. apt-get Haenger auf Experiment-Node (KRITISCH)

**Problem:** `apt-get update` hing UNENDLICH auf talos-say-ls6 (>15 Minuten, keine Ausgabe).

**Root Cause:** Fastly CDN SRV-Redirect + IPv6 Timeout.
- Die apt HTTP-Methode versuchte IPv6-Verbindungen zu `cdn-fastly.deb.debian.org`
- IPv6 auf der isolierten Node nicht korrekt geroutet -> Endlos-Timeout
- Beweis: `/dev/tcp` TCP-Check zu `cdn-fastly.deb.debian.org` = FAIL, aber `curl deb.debian.org` = OK

**Diagnose:**
```bash
# Pod exec auf haengendem Job
kubectl exec -it <pod> -- ps aux
# Ergebnis: apt-get PID 33 seit Start, 0 Bytes heruntergeladen
```

**Fix (Preflight-Job):** Komplette Umschreibung - bash `/dev/tcp` TCP-Checks statt apt-basierte Connectivity-Tests.
- PostgreSQL: `/dev/tcp` Port 5432 + `pg_isready` (aus Base-Image)
- Redis: `/dev/tcp` Port 6379 + `redis-cli ping`
- Andere: Reine TCP-Port-Checks
- **Ergebnis:** 16 Sekunden statt 300+ Sekunden Timeout

**Fix (Build/Per-DB Jobs):**
```bash
# In before_script:
echo 'Acquire::ForceIPv4 "true";' > /etc/apt/apt.conf.d/99force-ipv4
sed -i 's/deb.debian.org/cdn-fastly.deb.debian.org/g' /etc/apt/sources.list*
apt-get update 2>&1 | tail -3  # Sichtbare Ausgabe statt /dev/null
```

**Commits:** `e7503d5` (Preflight), `3afb82a` (IPv4+CDN)

**Lektion:** apt-get auf isolierten Nodes: IPv6 + Fastly CDN = Deadlock. IMMER `ForceIPv4` setzen!

---

### 4. Push-Triggered Pipeline: Experiment-Jobs bleiben "created"

**Problem:** Pipeline #5058 (source=push) hatte alle Experiment-Run Jobs im Status "created" - nicht pending oder running.

**Ursache:** `changes:`-Bedingung in den Rules funktioniert nicht zuverlaessig bei Push-Pipelines, wenn nur `.gitlab-ci.yml` geaendert wurde. Die Experiment-Daten/Code-Dateien waren unveraendert.

**Workaround:** API-triggered Pipeline (#5059) erstellen:
```bash
curl --request POST --header "PRIVATE-TOKEN: $TOKEN" \
  "https://gitlab.comdare.de/api/v4/projects/280/pipeline" \
  --form "ref=development"
```
Bei API-Pipelines greifen die Rules ohne `changes:`-Bedingung korrekt.

---

### 5. Konkurrierende Pipelines

Bis zu 4 Pipelines gleichzeitig auf dem `development` Branch (Push + API x 2 SHAs), da `auto_cancel_pending_pipelines = disabled` auf Projekt 280.

**Loesung:**
- Alle ueberfluessigen Pipelines per API gecancelled
- Lingering Pods manuell geloescht:
  ```bash
  kubectl delete pod <pod-name> -n gitlab --grace-period=10
  ```

**Lektion:** `auto_cancel_pending_pipelines=disabled` bedeutet ALLE Pipelines laufen parallel. Manuelles Aufraeumen noetig.

---

## Konfigurationsaenderungen

| Datei | Aenderung | Commit |
|-------|-----------|--------|
| `.gitlab-ci.yml` | `--config src/cpp/config.example.json` (statt configmap.yaml) | `ea094ff` |
| `.gitlab-ci.yml` | Preflight: `/dev/tcp` TCP-Checks (kein apt-get mehr) | `e7503d5` |
| `.gitlab-ci.yml` | Before_script: `ForceIPv4` + `cdn-fastly` Mirror | `3afb82a` |
| `.gitlab-ci.yml` | v5.4.1 Template + Experiment-Stages (Merge) | `e5bf3ac` |

---

## Commits dieser Session

| SHA | Beschreibung |
|-----|-------------|
| `ea094ff` | fix(ci): use JSON config instead of K8s ConfigMap YAML |
| `e5bf3ac` | merge: v5.4.1 CI template + experiment stages with JSON config fix |
| `e7503d5` | fix(ci): preflight uses bash /dev/tcp instead of apt-get |
| `3afb82a` | fix(ci): force IPv4 + direct CDN for apt on experiment node |

---

## Aktueller Status (Ende S95c)

### Pipeline #5059 (API-triggered, SHA `3afb82af`)

| Job | Status | Dauer |
|-----|--------|-------|
| experiment:build | SUCCESS | 138s |
| experiment:preflight | SUCCESS | 26s |
| experiment:postgresql | RUNNING | 16min+ (laeuft ueber Nacht) |
| experiment:cockroachdb | CREATED | Wartet auf postgresql |
| experiment:mariadb | CREATED | Wartet auf cockroachdb |
| experiment:clickhouse | CREATED | Wartet auf mariadb |
| experiment:redis | CREATED | Wartet auf clickhouse |
| experiment:kafka | CREATED | Wartet auf redis |
| experiment:minio | CREATED | Wartet auf kafka |

### PostgreSQL Job Details
- Run 1/3 aktiv (seed=43)
- Datengenerierung laeuft: random_binary, structured_json, text_document, uuid_keys, jsonb_documents abgeschlossen (~786 MB pro Typ)
- nasa_image, blender_video werden generiert (Downloads aktiv)
- 8 Payload-Typen insgesamt
- DB-Insert-Phase steht bevor (nach Datengenerierung aller Typen)

### Geschaetzte Laufzeit
- 7-21 Stunden (ueber Nacht), abhaengig von Datengenerierung und DB-Performance
- Kaskade: postgresql -> cockroachdb -> mariadb -> clickhouse -> redis -> kafka -> minio

---

## Infrastruktur-Zustand

| Komponente | Status |
|------------|--------|
| talos-say-ls6 | Isoliert (Taint aktiv), NFS-Server 10.0.110.184 |
| Runner-4 (ID 17) | Exklusiv Experiment, `run_untagged=false` |
| Services | 3 Replicas (PostgreSQL, MinIO, Samba-AD, Redis-GitLab) |
| ClickHouse | Auf qkr-yc0 (umgezogen von say-ls6) |
| OPNsense-4 | Tolerations fuer Experiment-Taint gesetzt |
| Longhorn | `taint-toleration = experiment=dedicated:NoSchedule` (NACH EXPERIMENT ZURUECKSETZEN!) |

---

## Gelernte Lektionen (Zusammenfassung)

1. **K8s ConfigMap YAML ist nicht JSON** - Niemals direkt als `--config` verwenden
2. **apt-get auf isolierten Nodes:** IPv6 + Fastly CDN = Deadlock. IMMER `ForceIPv4` setzen!
3. **Push-Pipelines + `changes:` Rule:** Funktioniert nicht zuverlaessig fuer Experiment-Jobs. API-Trigger bevorzugen.
4. **`auto_cancel_pending_pipelines=disabled`:** Alle Pipelines laufen parallel. Manuelles Aufraeumen noetig.
5. **Runner Pod-Cleanup:** Gecancelte Jobs hinterlassen Pods. Manuell loeschen mit `kubectl delete pod --grace-period=10`.

---

## Naechste Schritte (S96)

1. **Pipeline #5059 ueberwachen** - postgresql -> ... -> minio Kaskade pruefen
2. **Bei Fehlern:** Logs pruefen, ggf. einzelne Jobs per API retrigern
3. **Nach Experiment-Abschluss:** Ergebnisse in `results/` Verzeichnis pruefen
4. **Post-Experiment Cleanup (Plan Phase 9):**
   - Taint von talos-say-ls6 entfernen
   - Services 3 -> 4 Replicas skalieren
   - Longhorn `taint-toleration` ZURUECKSETZEN
5. **CI-Robustheit:** Immer API-triggered Pipeline fuer Experiment verwenden
6. **Kahan-Plan Koexistenz:** Bei v5.x Updates Experiment-Stages automatisch wiederherstellen (oder separates CI-Include)
