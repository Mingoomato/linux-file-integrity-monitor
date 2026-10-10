# Linux File Integrity Monitor

[:kr: 한국어](#한국어) · [:us: English](#english)

## 한국어

`fimctl`은 사용자가 명시한 실습 디렉터리의 일반 파일을 SHA-256 기준선으로 기록하고, 나중의 전체 스캔과 비교하는 읽기 전용 파일 무결성 모니터입니다. `watch`는 inotify 신호를 로컬 NDJSON으로 남깁니다. 목적은 디렉터리 FD 기반 파일 순회, 안정적인 해시 계산, SQLite 상태 저장, Linux 파일 이벤트의 한계와 복구 필요성을 학습하는 것입니다. 차단·자동 복구·행위자 식별 제품이 아닙니다.

### 빌드 및 사용

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
mkdir -p "$HOME/fim-lab"
./build/fimctl init --root "$HOME/fim-lab" --db ./state.sqlite
./build/fimctl verify --db ./state.sqlite --full
./build/fimctl watch --root "$HOME/fim-lab" --events ./events.ndjson
```

C17 컴파일러, CMake, OpenSSL libcrypto, SQLite 개발 헤더가 필요합니다. `init`은 기존 DB를 덮어쓰지 않습니다. `scan`은 기준선을 갱신하고, `verify`는 차이를 찾으면 종료 코드 2를 반환합니다. 감시 프로세스는 Ctrl-C 또는 `fimctl stop --pidfile ./fimd.pid`로 종료합니다.

### 검증 및 한계

현재 Linux 호스트에서 GCC Release, Clang Release, ASan/UBSan CTest가 각각 1/1 통과했고 Clang 정적 분석도 경고 없이 끝났습니다. 스모크는 기준선 생성/비교, JSON 경로 내보내기, 심볼릭 링크 제외, 이벤트 burst coalescing과 원본 신호 수·시각 범위, rename 짝짓기·타임아웃, 신규 디렉터리 생성·이동 후 재감시, 큐 오버플로 주입 뒤 전체 재검사와 생성/삭제/메타데이터 차이 보고, PID 파일 종료를 확인합니다. 실제 커널 큐 오버플로, watch 자원 고갈, mount 경계, 동시 파일 변경, 디스크 오류, 10,000개 파일 성능, Ubuntu/Rocky 런타임 매트릭스는 확인하지 않았습니다.

스캔은 심볼릭 링크를 따라가지 않고 선택한 루트와 다른 장치의 파일을 제외합니다. 감시기는 256개 항목의 pending rename/coalescing 표와 100ms quiet window를 사용해 신호 burst를 묶고, `observation_count` 및 최초·마지막 관찰 시각을 기록합니다. rename 신호는 쿠키 짝짓기를 위해 즉시 별도 기록합니다. 큐 오버플로, 감시 무효화, 디렉터리 추가·이동 때 `recovering` 상태를 기록하고 감시 트리와 파일 상태를 다시 검사합니다. 전체 watch 등록과 스캔이 성공하면 `healthy`, 실패하면 `degraded`를 기록하며, 재검사 차이는 별도 reconciliation 증거로 남깁니다. `--inject-overflow-once`는 이 경로의 합성 회귀 테스트 전용이며 실제 커널 큐 오버플로 시험은 아닙니다. inotify 이벤트는 유실되거나 합쳐질 수 있고 프로세스 행위자를 알 수 없습니다. 기준선 비교를 위한 hash debounce와 주기적 전체 검사는 아직 없습니다. 데이터베이스와 로그는 같은 사용자 또는 root에 의한 변조를 막지 못합니다. 민감한 경로가 로그에 기록될 수 있으니 개인 파일 대신 폐기 가능한 실습 경로를 사용하세요.

상세 설계와 한계는 [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md), [`docs/THREAT_MODEL.md`](docs/THREAT_MODEL.md), [`docs/TEST_RESULTS.md`](docs/TEST_RESULTS.md)를 참고하세요.

## English

`fimctl` is an educational, read-only file integrity monitor. It records a SHA-256 and metadata baseline for ordinary files beneath an explicitly selected lab directory, compares later scans, and writes local NDJSON observations from inotify. The project demonstrates descriptor-relative traversal, stable-file hashing, transactional SQLite storage, event-loss handling, and the limits of a best-effort Linux notification stream. It does not prevent changes, identify the process that made them, or recover files.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Dependencies: a C17 compiler, CMake, OpenSSL libcrypto, and SQLite3 development headers.

## Use

```sh
mkdir -p "$HOME/fim-lab"
./build/fimctl init --root "$HOME/fim-lab" --db ./state.sqlite
./build/fimctl verify --db ./state.sqlite --full
./build/fimctl watch --root "$HOME/fim-lab" --events ./events.ndjson
```

`init` refuses to replace an existing database. `scan` deliberately replaces the current baseline. `verify` returns 2 when it finds differences. Run `watch` in a separate terminal and stop it with Ctrl-C or `fimctl stop --pidfile ./fimd.pid`. `watch` coalesces non-rename signals for the same path over a bounded 100 ms quiet window and records signal count plus first/last observation timestamps. The explicit lab-only `--inject-overflow-once` option exercises reconciliation in tests; omit it during normal observation.

## Scope and limitations

Traversal refuses symlinks and skips filesystems mounted below the selected root during baseline scanning. A bounded table correlates rename cookies and coalesces other signals for each path over a 100 ms quiet window; the journal retains the combined mask, signal count, and first/last wall-clock nanoseconds. On queue overflow, invalidated watches, or directory-tree changes, the watcher records `recovering`, rebuilds recursive watches, scans current file state, emits reconciliation differences, and records `healthy` only when both operations succeed. The smoke test injects an overflow condition through a clearly named option; it does not force the kernel queue itself to overflow. Inotify does not identify the process that changed a file and can coalesce events in the kernel. Baseline-aware hash debounce, periodic verification, watch-limit fault injection, mount-boundary and concurrent-mutation tests, a 10k-file benchmark, and the Ubuntu/Rocky runtime matrix remain open. Export paths are local data and may contain personal information.

Supported target is a Linux filesystem and an unprivileged lab directory. No Ubuntu/Rocky runtime matrix has been executed. Run `ctest --test-dir build --output-on-failure`; the smoke test covers baseline creation/comparison, JSON path escaping, symlink exclusion, coalesced event bursts, paired and unmatched rename observations, move timeout, recursive watch recovery, synthetic overflow reconciliation, and monitor shutdown. See `docs/TEST_RESULTS.md` for the evidence and remaining gates.

## Safety

The program never modifies monitored files. It is not a complete audit log, does not attribute process actors, does not protect its database from a privileged attacker, and does not provide a point-in-time filesystem snapshot.
