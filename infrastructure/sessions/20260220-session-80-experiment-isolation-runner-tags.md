# Session 80: Experiment-Node-Isolation + Runner-Tags Fix

**Datum:** 2026-02-20
**Agent:** Claude Opus 4.6 (Cluster-Infrastruktur + Kahan-Plan CI)
**Vorgaenger:** Session 79 (Monitoring Start + Cluster-Stabilisierung)
**Typ:** Cluster-Wartung + CI/CD Konfiguration
**Status:** ABGESCHLOSSEN

---

## 1. Kontext-Recovery

Fortgesetzt aus Session 72 Kontext 7 (via Kontext-Komprimierung). Monitoring-Start war bereits in Session 79 erledigt.

---

## 2. Erledigte Arbeit

### 2.1 Monitoring Stack gestartet (aus Vorgaenger-Kontext)
- **10 Monitoring-Pods** erfolgreich gestartet (Prometheus, Alertmanager, Grafana, node-exporter 4x, pushgateway, operator, kube-state-metrics)
- **3 Longhorn Volumes** auf 1 Replika (alertmanager 2Gi, grafana 5Gi, prometheus 20Gi)
- **Fixes:** crane Image-Cache, Grafana Datasource-Konflikt (isDefault), node-exporter nodeSelector

### 2.2 Swap-Verifikation
- Alle 4 Hosts: **0 Swap** (pve1, pve2, node3, node4) — bereits erledigt

### 2.3 CalicoIsDown Taint aufgeloest
- qkr-yc0 hatte `network-unavailable` Taint — spontan verschwunden
- **Alle 4 Nodes: 0 Taints** (ausser say-ls6 experiment-Taint = beabsichtigt)

### 2.4 Experiment-Node-Isolation (say-ls6)

**User-Auftrag:** say-ls6 fuer Experiment reserviert, Produktion auf 3 Replika

1. **Longhorn Replika 4→3:** 29 Production-Volumes von 4 auf 3 Replika gepatcht
2. **Longhorn Scheduling deaktiviert:** `allowScheduling=false` auf say-ls6
   - Wichtig: Longhorn ignoriert K8s-Taints fuer Replika-Scheduling!
   - Erst nach Deaktivierung des Longhorn-Scheduling blieben Replika weg
3. **say-ls6 Replika geloescht:** 29 Replika von Production-Volumes entfernt
   - **GESCHUETZT:** pvc-e7d3e701 (Experiment-NFS, 1 Replika auf say-ls6 = einzige Replika)
   - **GESCHUETZT:** 3 Monitoring-Volumes (nicht auf say-ls6)
4. **Rebuild laeuft:** 14 degraded → rebuilden auf 3 Nodes (5x2-s49, lux-kpk, qkr-yc0)

### 2.5 Kafka Entity Operator Diagnose

**Ursache:** KRaft Controller-Quorum kaputt
- Original 3+ Controller, nach Scale-Down nur controller-4 uebrig
- controller-4 versucht controller-5/6/7 zu erreichen (geloescht, UnknownHostException)
- Strimzi Status: `UnforceableProblem` — KRaft Controller-Scaling NICHT unterstuetzt
- **Loesung:** Kafka-Cluster komplett loeschen und mit 1 Controller neu erstellen (Experiment-App)
- **NICHT UMGESETZT** — auf User-Wunsch verschoben

**Quellen:**
- [Strimzi KRaft Controller Scaling Issue #9429](https://github.com/strimzi/strimzi-kafka-operator/issues/9429)
- [KIP-853 Dynamic Quorum](https://developers.redhat.com/articles/2024/11/27/dynamic-kafka-controller-quorum)

### 2.6 GitLab Runner-Tags Fix (Kahan-Plan K13)

**Problem:** Shell-Runner mit `run_untagged=True` nahmen Docker-Jobs an → Pipeline FAILED

**Fix:** Alle 9 Runner per GitLab API konfiguriert:

| ID | Runner | Tags | run_untagged |
|---|---|---|---|
| 6 | k8s-runner | k8s, docker, linux, x86_64 | **true** |
| 7 | node5-macos-x86 | bare-metal, shell, macos, x86_64 | false |
| 8 | node6-macos-arm | bare-metal, shell, macos, arm64 | false |
| 9 | node7-linux-arm | bare-metal, shell, linux, arm64 | false |
| 10 | node8-linux-riscv | bare-metal, shell, linux, riscv64 | false |
| 11 | pve1-linux-x86 | bare-metal, shell, linux, x86_64 | false |
| 12 | pve2-linux-x86 | bare-metal, shell, linux, x86_64 | false |
| 13 | node3-linux-x86 | bare-metal, shell, linux, x86_64 | false |
| 14 | node4-linux-x86 | bare-metal, shell, linux, x86_64 | false |

**Pipeline #1894 gestartet** auf comdare-simd (development branch) zur Verifikation.

### 2.7 pve1 OPNsense LV Status
- pve1 root LV: 176.25G (pve2: 150.25G) — 26G OPNsense LV fehlt
- **Erfordert Rescue-Boot** (resize2fs + lvreduce + lvcreate)
- NICHT remote moeglich → auf physischen Zugang verschoben

---

## 3. Cluster-Endzustand

### 3.1 Nodes
| Node | Status | Taints | Longhorn Sched |
|---|---|---|---|
| talos-5x2-s49 | Ready | none | true |
| talos-lux-kpk | Ready | none | true |
| talos-qkr-yc0 | Ready | none | true |
| talos-say-ls6 | Ready | experiment=dedicated:NoSchedule | **false** |

### 3.2 Longhorn Volumes (Endzustand)
- **33 total:** 30 attached, 3 detached
- **12 healthy (3 Replika)** + 4 healthy (1 Replika) = 16 healthy
- **14 degraded** (rebuilding auf 3 Nodes, say-ls6 ausgeschlossen)
- **3 detached** (Pods von say-ls6 evicted, warten auf Reschedule)
- Rebuild-Ziel: ALLE 29 Production-Volumes 3/3 auf 3 Nodes

### 3.3 Pods
- **164 Running**, 1 CrashLoopBackOff (Kafka), 1 ContainerCreating (nfs-server)

### 3.4 GitLab CI
- **9/12 Runner ONLINE**, Tags konfiguriert
- **Pipeline #1894** gestartet (Verifizierung Runner-Routing)
- **NAECHSTER SCHRITT:** CI Template v4.0 mit Tag-basiertem Job-Routing

---

## 4. Lessons Learned

1. **Longhorn ignoriert K8s Taints** fuer Replika-Scheduling — `allowScheduling=false` auf Node-CRD setzen!
2. **KRaft Controller Scaling** in Strimzi NICHT moeglich (nur Kafka 4.x+ mit KIP-853)
3. **GitLab Runner API:** `run_untagged` ist der Schlussel — NUR der Default-Runner (K8s) darf `true` haben
4. **crane Image-Cache:** Einfache Loesung fuer Talos Nodes ohne direkten Registry-Zugang

---

## 5. Offene Punkte

| # | Aufgabe | Prioritaet | Blockiert durch |
|---|---|---|---|
| 1 | Longhorn Rebuild abwarten (14 degraded) | HOCH | Automatisch |
| 2 | Pipeline #1894 Ergebnis pruefen | HOCH | Laeuft |
| 3 | CI Template v4.0 (Tag-basiertes Routing) | MITTEL | Runner-Tags done |
| 4 | Kafka Cluster Neuerstellen | NIEDRIG | User-Entscheidung |
| 5 | pve1 OPNsense LV | MITTEL | Rescue-Boot |
| 6 | 3 detached Volumes reattachen | MITTEL | Pods reschedule |
| 7 | Longhorn Scheduling auf say-ls6 RE-ENABLE | NACH EXPERIMENT | Experiment-Ende |

---

## 6. Referenzen

| Dokument | Pfad |
|---|---|
| Session 79 | `sessions/20260220-session-79-monitoring-start-cluster-stabilisierung.md` |
| K13 CI Strategie | `_temp/Present/20260220-multi-platform-ci-strategy.md` |
| Experiment Plan | `~/.claude/plans/experiment-node-isolation-plan.md` |
| Masterplan v7 | `~/.claude/plans/scalable-imagining-torvalds.md` |
