# Session 97e: Native Adapter Implementierung (Phase 1-6)
**Datum:** 2026-02-22
**Agent:** Infrastruktur (system32 Kontext)
**Projekt:** dedup-database-analysis (GitLab Project 280)
**Vorgaenger:** Session 97d (Plan-Erstellung + Download-Agent)

## Kontext

Fortsetzung von Session 97d. User hat den "Strukturierte Adapter: Native Insertion pro Datensatz x DB"-Plan genehmigt. Pipeline #6087 laeuft auf `development` Branch (BLOB-Modus). Native Adapter werden auf separatem Feature-Branch implementiert.

## Plan-Referenz

Vollstaendiger Plan: `~/.claude/plans/jazzy-gliding-fiddle.md`
- 8 Phasen, ~1.645 neue Zeilen C++
- InsertionMode Enum (BLOB | NATIVE | BOTH)
- NativeRecord mit variant-basiertem Typsystem
- 12 PayloadType-spezifische Parser
- 7 Connector-Erweiterungen mit nativen Methoden

## Feature Branch

**Branch:** `feature/native-adapters` (erstellt von `development` bei Commit `fe6e569`)
**Commits:** 19 neue Commits (5ef68af → b4f6318)

## Implementierte Phasen

### Phase 1: native_record.hpp (DONE, Commit 5ef68af)
**Neue Datei:** `src/cpp/experiment/native_record.hpp`
**Inhalt:**
- `InsertionMode` Enum: BLOB, NATIVE, BOTH
- `ColumnValue` = `std::variant<monostate, bool, int64_t, double, string, vector<char>>`
- `NativeRecord` Struct: `map<string, ColumnValue>` mit Convenience-Settern + `estimated_size_bytes()`
- `ColumnDef` Struct: name, type_hint, is_primary_key, is_not_null, default_expr
- `NativeSchema` Struct: table_name + columns + Hilfsmethoden (column_list, param_placeholders, question_placeholders)
- `get_native_schema(PayloadType)`: Gibt typspezifisches Schema zurueck fuer alle 13 PayloadTypes

**Schema-Registry:**

| PayloadType | Tabelle | Spalten |
|-------------|---------|---------|
| BANK_TRANSACTIONS | bank_transactions | id SERIAL, amount DOUBLE, currency CHAR(3), description TEXT, category TEXT, timestamp TIMESTAMPTZ |
| TEXT_CORPUS | posts | id SERIAL, article_id BIGINT, user_id BIGINT, headline TEXT, body TEXT, positive_votes INT, negative_votes INT, created_at TIMESTAMPTZ |
| NUMERIC_DATASET | numbers | id SERIAL, f1..f10 DOUBLE |
| GITHUB_EVENTS | github_events | id TEXT PK, type TEXT, actor_login TEXT, repo_name TEXT, payload JSONB, created_at TIMESTAMPTZ |
| STRUCTURED_JSON | json_records | id SERIAL, name TEXT, email TEXT, data JSONB |
| JSONB_DOCUMENTS | jsonb_documents | event_id TEXT PK, type TEXT, data JSONB, ts TIMESTAMPTZ |
| TEXT_DOCUMENT | text_documents | id SERIAL, content TEXT |
| UUID_KEYS | uuid_records | uuid TEXT PK |
| GUTENBERG_TEXT | gutenberg_texts | id SERIAL, title TEXT, content TEXT |
| NASA_IMAGE/BLENDER_VIDEO/RANDOM_BINARY | binary_objects | id UUID, filename TEXT, mime TEXT, size_bytes BIGINT, sha256 BYTEA, payload BYTEA |
| MIXED | files | (gleich wie BLOB-Schema) |

### Phase 2: native_data_parser (DONE, Commits f0d4592 + 6323f3c)
**Neue Dateien:**
- `src/cpp/experiment/native_data_parser.hpp` (~60 Zeilen)
- `src/cpp/experiment/native_data_parser.cpp` (~350 Zeilen)

**Parser pro PayloadType:**

| PayloadType | Parser-Methode | Logik |
|-------------|---------------|-------|
| BANK_TRANSACTIONS | parse_bank_csv() | CSV-Header-Detection, Spalten-Mapping (DE/EN), stod() fuer Amount |
| TEXT_CORPUS | parse_million_post_json() | nlohmann::json Array-Parsing, Feld-Mapping ID_Post/Headline/Body |
| NUMERIC_DATASET | parse_numeric_csv() | CSV mit 10 Double-Spalten, Header-Detection |
| GITHUB_EVENTS | parse_github_events_json() | JSON Lines, actor.login + repo.name + payload.dump() |
| STRUCTURED_JSON | parse_structured_json() | name/email/data Extraktion |
| JSONB_DOCUMENTS | parse_jsonb_document() | event_id/type/data Extraktion |
| TEXT_DOCUMENT | parse_text_document() | Content als ganzer String |
| UUID_KEYS | parse_uuid_keys() | Eine UUID pro Zeile |
| GUTENBERG_TEXT | parse_gutenberg_text() | Titel aus Filename/Zeile 1, Rest als Content |
| Binaer (NASA/Blender/Random) | parse_binary_blob() | MIME-Detection, SHA256, Payload |

**Public API:**
- `parse_file(data, type, filename)` → `vector<NativeRecord>`
- `parse_directory(dir_path, type, max_files)` → `vector<NativeRecord>` (sortiert!)
- `parse_single(data, type, filename)` → `NativeRecord`

### Phase 3+4: db_connector.hpp Erweiterung (DONE, Commit 595f371)
**Geaenderte Datei:** `src/cpp/connectors/db_connector.hpp`
**Neue virtuelle Methoden (mit Default-Fallback):**

```cpp
virtual bool create_native_schema(schema_name, PayloadType type);
virtual bool drop_native_schema(schema_name, PayloadType type);
virtual bool reset_native_schema(schema_name, PayloadType type);
virtual MeasureResult native_bulk_insert(records, PayloadType type);
virtual MeasureResult native_perfile_insert(records, PayloadType type);
virtual MeasureResult native_perfile_delete(PayloadType type);
virtual int64_t get_native_logical_size_bytes(PayloadType type);
```

**Wichtig:** Include von `native_record.hpp` hinzugefuegt, `schema_name_` als protected Member.

### Phase 5: 7 Connector-Implementierungen (DONE, 12 Commits)

#### PostgreSQL + CockroachDB (Commits ea0c14d + 2b17970)
**Geaenderte Dateien:**
- `postgres_connector.hpp`: 6 neue Methoden-Deklarationen + 3 private Helpers
- `postgres_connector.cpp`: ~200 neue Zeilen

**Implementierung:**
- `pg_type_for(ColumnDef)`: Mappt generische Typen auf PG-Typen (SERIAL, JSONB, TIMESTAMPTZ, etc.)
- `build_create_table_sql(NativeSchema)`: Dynamisches DDL mit PK/NOT NULL/DEFAULT
- `build_insert_sql(NativeSchema)`: INSERT mit $N Placeholders, SERIAL-Spalten uebersprungen
- `bind_and_exec_native()`: std::visit ueber ColumnValue, PQexecParams mit typisierten Parametern
  - text: als String (format 0)
  - binary: als Binary (format 1) mit lengths
  - int64_t/double/bool: als String-Repraesentation
- `native_bulk_insert()`: BEGIN → N x PQexecParams → COMMIT
- `native_perfile_insert()`: Einzelne Inserts mit ScopedTimer pro Record
- `native_perfile_delete()`: SELECT PK::text → DELETE per PK (UUID/BIGINT cast)
- `get_native_logical_size_bytes()`: pg_total_relation_size (PG) / crdb_internal.ranges (CRDB)

**CockroachDB:** Erbt von PostgresConnector, JSONB wird unterstuetzt.

#### MariaDB (Commits 21e68d6 + 82b8473)
**Geaenderte Dateien:** `mariadb_connector.hpp/.cpp`
**Implementierung:**
- MariaDB Typ-Mapping: SERIAL→BIGINT AUTO_INCREMENT, JSONB→JSON, BYTEA→LONGBLOB, TIMESTAMPTZ→DATETIME(6)
- ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
- mysql_stmt_prepare/mysql_stmt_bind_param/mysql_stmt_execute
- MYSQL_BIND Typen: MYSQL_TYPE_LONGLONG, MYSQL_TYPE_DOUBLE, MYSQL_TYPE_STRING, MYSQL_TYPE_BLOB
- #ifdef HAS_MYSQL Guards
- Delete: SELECT PK → DELETE mit LIMIT 1

#### ClickHouse (Commits 20b989d + 91bacca)
**Geaenderte Dateien:** `clickhouse_connector.hpp/.cpp`
**Implementierung:**
- ClickHouse Typ-Mapping: SERIAL→UInt64, DOUBLE→Float64, BOOLEAN→UInt8, JSONB→String, CHAR(N)→FixedString(N)
- ENGINE = MergeTree() ORDER BY pk_column
- Bulk Insert via HTTP API: TSV Format (Tab-Separated, INSERT ... FORMAT TabSeparated)
- Per-File Insert via einzelne INSERT VALUES SQL
- Delete: ALTER TABLE DELETE WHERE 1=1 (ClickHouse Lightweight DELETE)
- Size: system.parts (bytes_on_disk WHERE active)

#### Redis (Commits f549355 + b8afe97)
**Geaenderte Dateien:** `redis_connector.hpp/.cpp`
**Implementierung:**
- Schema: No-Op (schemaless)
- Key-Pattern: `dedup:TABLE_NAME:IDX` (z.B. `dedup:posts:42`)
- Insert: HSET mit allen Spalten als Felder
- redisCommandArgv mit typed argv/argvlen Arrays
- Delete: Reuse von BLOB SCAN+DEL Logik
- #ifdef HAS_HIREDIS Guards

#### Kafka (Commits 6825d5 + f52e570)
**Geaenderte Dateien:** `kafka_connector.hpp/.cpp`
**Implementierung:**
- Topic: `topic_prefix_-TABLE_NAME` (z.B. `dedup-lab-posts`)
- Schema: No-Op (auto-created topics)
- Insert: rd_kafka_producev mit JSON-serialisiertem Record
- JSON-Escaping: Manuelle Escape-Logik fuer \", \\, \n, \t
- Binaere Daten: `"(binary:Nbytes)"`
- rd_kafka_flush(10s) nach Bulk Insert
- Delete: Reuse von BLOB Topic-Deletion
- #ifdef HAS_RDKAFKA Guards

#### MinIO (Commits a0b3ab4 + 8efa0f2)
**Geaenderte Dateien:** `minio_connector.hpp/.cpp`
**Implementierung:**
- Bucket: `bucket_prefix_-TABLE_NAME` (z.B. `dedup-lab-posts`)
- Schema: create_bucket(bucket)
- Strukturierte Daten: JSON-serialisiert, Content-Type: application/json, Key: `record_IDX.json`
- Binaere Daten: Original-MIME und Filename, Key: Originalname oder `obj_IDX.bin`
- put_object(bucket, key, body, content_type)
- Delete: Reuse von BLOB list+delete Logik

### Phase 6: main.cpp + data_loader.cpp (DONE, Commits 88fc5f8 + 8f02d74 + b4f6318)

#### data_loader.hpp Aenderungen:
- Include von `native_record.hpp` und `native_data_parser.hpp`
- `ExperimentResult`: Neues Feld `insertion_mode` ("blob" oder "native")
- Neue Methoden: `run_native_stage()` und `run_native_experiment()`

#### data_loader.cpp Aenderungen:
- `to_json()`: insertion_mode im JSON-Output
- `run_native_stage()`: Identisch zu run_stage(), aber:
  - Verwendet `native_bulk_insert/native_perfile_insert/native_perfile_delete` statt BLOB-Methoden
  - Verwendet `get_native_logical_size_bytes()` fuer Messung
  - Setzt `insertion_mode = "native"` im Result
  - Separate Grafana-Metriken: `dedup_native_duration_ms`, `dedup_native_edr`
- `run_native_experiment()`:
  - Parsed Datenverzeichnis via `NativeDataParser::parse_directory()`
  - `reset_native_schema()` vor jedem Grade
  - 4 Stages: BULK_INSERT → PERFILE_INSERT → PERFILE_DELETE → MAINTENANCE
  - Ergebnis-Datei: `results/native_SYSTEM_PAYLOAD_results.json`

#### main.cpp Aenderungen:
- Include `native_record.hpp` und `native_data_parser.hpp`
- Neuer CLI-Parameter: `--insertion-mode blob|native|both` (Default: blob)
- BLOB-Loop in `if (insertion_mode == BLOB || BOTH)` gewrappt
- Neuer NATIVE-Loop nach BLOB-Loop:
  - Gleiche Retry-Logik (exponential backoff)
  - Aufruft `loader.run_native_experiment()` statt `loader.run_full_experiment()`
  - MetricsTrace Events mit `native_system_start/end` Tags
- Ergebnisse beider Modi in `all_results` zusammengefuehrt

## Pipeline #6087 Status (BLOB-Pipeline auf development)

| Job | Status | Dauer | Notizen |
|-----|--------|-------|---------|
| experiment:build | SUCCESS | 117s | |
| experiment:preflight | SUCCESS | 20s | |
| experiment:postgresql | **SUCCESS** | 12.975s (216min) | BLOB-Modus komplett |
| experiment:cockroachdb | **FAILED** | 4.303s (71min) | NFS `/datasets/real-world/random_numbers` nicht gefunden |
| experiment:mariadb | SKIPPED | | Durch CRDB-Failure blockiert |
| experiment:clickhouse | SKIPPED | | |
| experiment:redis | SKIPPED | | |
| experiment:kafka | SKIPPED | | |
| experiment:minio | SKIPPED | | |
| experiment:upload-results | SKIPPED | | |
| build-debian-x86 | SUCCESS | 46s | |
| build-ubuntu-x86 | SUCCESS | 24s | |
| build-macos-x86 | SUCCESS | 15s | |
| build-macos-arm | SUCCESS | 17s | |
| build-linux-arm64 | SUCCESS | 28s | |

**CockroachDB Failure Root Cause:** NFS-Mount `/datasets/real-world/random_numbers` existiert nicht.
Die Daten wurden in Session 97b/97c unter `/nfs/real-world/random_numbers` (Ganesha NFS, IP 10.0.110.184) abgelegt.
Der CI-Job erwartet sie unter `/datasets/real-world/random_numbers` (Runner-4 K8s Volume Mount).
**Fix:** Pruefen ob Runner-4 ConfigMap den NFS-Volume korrekt mounted → Pfad-Diskrepanz fixen.

## CMakeLists.txt TODO

Die neuen `.cpp` Dateien (`native_data_parser.cpp`) muessen noch in `CMakeLists.txt` aufgenommen werden.
Aktuell nicht gemacht, da CI-Config-Sperre aktiv (User-Direktive S95b).
**Muss vor der ersten Kompilation auf feature/native-adapters passieren.**

## Offene Tasks

| Task | Status | Beschreibung |
|------|--------|-------------|
| #118 | IN_PROGRESS | bankdataset Excel→CSV + gharchive Sample |
| #119 | PENDING | CI/CD INSERTION_MODE Variable + Pipeline B |
| #87 | IN_PROGRESS | BLOB-Pipeline #6087 ueberwachen (CRDB failed) |
| #99 | IN_PROGRESS | Experiment-Kaskade (nur PG SUCCESS, Rest blocked) |

## Zusammenfassung der Aenderungen

| # | Datei | Typ | Zeilen (ca.) |
|---|-------|-----|-------------|
| 1 | `src/cpp/experiment/native_record.hpp` | NEU | ~280 |
| 2 | `src/cpp/experiment/native_data_parser.hpp` | NEU | ~60 |
| 3 | `src/cpp/experiment/native_data_parser.cpp` | NEU | ~350 |
| 4 | `src/cpp/connectors/db_connector.hpp` | UPDATE | +60 |
| 5 | `src/cpp/connectors/postgres_connector.hpp` | UPDATE | +15 |
| 6 | `src/cpp/connectors/postgres_connector.cpp` | UPDATE | +200 |
| 7 | `src/cpp/connectors/mariadb_connector.hpp` | UPDATE | +10 |
| 8 | `src/cpp/connectors/mariadb_connector.cpp` | UPDATE | +200 |
| 9 | `src/cpp/connectors/clickhouse_connector.hpp` | UPDATE | +10 |
| 10 | `src/cpp/connectors/clickhouse_connector.cpp` | UPDATE | +180 |
| 11 | `src/cpp/connectors/redis_connector.hpp` | UPDATE | +10 |
| 12 | `src/cpp/connectors/redis_connector.cpp` | UPDATE | +130 |
| 13 | `src/cpp/connectors/kafka_connector.hpp` | UPDATE | +10 |
| 14 | `src/cpp/connectors/kafka_connector.cpp` | UPDATE | +120 |
| 15 | `src/cpp/connectors/minio_connector.hpp` | UPDATE | +10 |
| 16 | `src/cpp/connectors/minio_connector.cpp` | UPDATE | +150 |
| 17 | `src/cpp/experiment/data_loader.hpp` | UPDATE | +20 |
| 18 | `src/cpp/experiment/data_loader.cpp` | UPDATE | +180 |
| 19 | `src/cpp/main.cpp` | UPDATE | +60 |
| **Total** | | **3 NEU + 16 UPDATE** | **~2.045** |

## Naechste Schritte (fuer naechste Session)

### Prio 1: BLOB-Pipeline #6087 Fix
1. NFS-Volume Pfad-Diskrepanz fixen: `/nfs/real-world/` vs `/datasets/real-world/`
2. Pruefen ob Daten korrekt in NFS liegen (bankdataset, million_post, random_numbers)
3. Runner-4 ConfigMap NFS Volume Mount verifizieren
4. Pipeline #6087 retriggen oder neue Pipeline starten

### Prio 2: Feature Branch kompilierbar machen
1. `CMakeLists.txt` aktualisieren: `native_data_parser.cpp` hinzufuegen
2. Sicherstellen dass Include-Pfade korrekt sind (relative Pfade `../` Check)
3. Compile-Test auf feature/native-adapters

### Prio 3: Testdaten-Aufbereitung (#118)
1. bankdataset Excel→CSV Python-Konvertierung (auf data-copier Pod)
2. gharchive Sample: 24 .json.gz von NAS kopieren + entpacken
3. NASA/Gutenberg/Blender Downloads verifizieren

### Prio 4: CI/CD Anpassung (#119)
1. INSERTION_MODE Variable in .gitlab-ci.yml
2. Experiment-Jobs fuer native Modus konfigurieren
3. Separate Ergebnis-Sammlung fuer blob/ und native/

## Architektur-Entscheidungen (User-Direktiven)

1. **Getrennte Pipelines:** BLOB-Pipeline laeuft zuerst (bereits gestartet), NATIVE-Pipeline separat
2. **bankdataset:** Python Pre-Processing (Excel→CSV mit echten Spalten)
3. **Infra-Agent:** Alle C++ Code-Aenderungen direkt durch diesen Agent (kein Handoff)
4. **Branch-Strategie:** feature/native-adapters erst nach BLOB-Pipeline-Abschluss mergen
5. **Backward-Kompatibilitaet:** `--insertion-mode blob` ist Default, bestehende Pipeline unveraendert

## Wichtige Referenzen

| Referenz | Pfad/Wert |
|----------|-----------|
| Plan-Datei | `~/.claude/plans/jazzy-gliding-fiddle.md` |
| Feature-Branch | `feature/native-adapters` (19 Commits) |
| GitLab Token | `REDACTED.01.0w0j9hut3` |
| GitLab Projekt | 280 (dedup-database-analysis) |
| BLOB-Pipeline | #6087 (PG SUCCESS, CRDB FAILED, Rest SKIPPED) |
| NFS-Server | 10.0.110.184:/ (Ganesha auf talos-say-ls6) |
| NFS-Pfade | `/nfs/real-world/{bankdataset,million_post,random_numbers}` |
| CI Volume | `/datasets/real-world/` (Runner-4 K8s Volume Mount) |
| doku.tex | `docs/doku.tex` (Stage 1 "Idiomatic" + Stage 2 "Blob") |

## Geaenderte Dateien (Git-Summary)

```
Branch: feature/native-adapters (19 commits ahead of development)

Neue Dateien:
  src/cpp/experiment/native_record.hpp
  src/cpp/experiment/native_data_parser.hpp
  src/cpp/experiment/native_data_parser.cpp

Geaenderte Dateien:
  src/cpp/connectors/db_connector.hpp
  src/cpp/connectors/postgres_connector.hpp
  src/cpp/connectors/postgres_connector.cpp
  src/cpp/connectors/mariadb_connector.hpp
  src/cpp/connectors/mariadb_connector.cpp
  src/cpp/connectors/clickhouse_connector.hpp
  src/cpp/connectors/clickhouse_connector.cpp
  src/cpp/connectors/redis_connector.hpp
  src/cpp/connectors/redis_connector.cpp
  src/cpp/connectors/kafka_connector.hpp
  src/cpp/connectors/kafka_connector.cpp
  src/cpp/connectors/minio_connector.hpp
  src/cpp/connectors/minio_connector.cpp
  src/cpp/experiment/data_loader.hpp
  src/cpp/experiment/data_loader.cpp
  src/cpp/main.cpp
```
