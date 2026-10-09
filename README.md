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

현재 Linux 호스트에서 Release 빌드, CTest 스모크 1/1, 별도 ASan/UBSan 스모크가 통과했습니다. 스모크는 기준선 생성/비교, JSON 경로 내보내기, 심볼릭 링크 제외, 재귀 inotify 이벤트, 쿠키 기반 rename 짝짓기, 짝 없는 이동 진입과 이동 타임아웃 기록, PID 파일을 이용한 종료를 확인합니다. 큐 오버플로 재조정, mount 경계, 동시 파일 변경, 디스크 오류, 10,000개 파일 성능, Ubuntu/Rocky 런타임 매트릭스는 확인하지 않았습니다.

스캔은 심볼릭 링크를 따라가지 않고 선택한 루트와 다른 장치의 파일을 제외합니다. 감시기는 제한된 256개 항목의 pending rename 표로 inotify 쿠키를 짝짓고, 짝 없는 진입과 시간 초과를 별도 기록합니다. 이벤트는 유실되거나 합쳐질 수 있고 프로세스 행위자를 알 수 없습니다. 현재 감시는 기준선을 자동으로 재조정하지 않으며, 오버플로 이후 자동 전체 재검사를 보장하지 않습니다. 데이터베이스와 로그는 같은 사용자 또는 root에 의한 변조를 막지 못합니다. 민감한 경로가 로그에 기록될 수 있으니 개인 파일 대신 폐기 가능한 실습 경로를 사용하세요.

상세 설계와 한계는 [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md), [`docs/THREAT_MODEL.md`](docs/THREAT_MODEL.md), [`docs/TEST_RESULTS.md`](docs/TEST_RESULTS.md)를 참고하세요.

## English

`fimctl` stores a SHA-256 baseline for ordinary files beneath an explicitly selected lab directory and compares later scans with that baseline. Its `watch` command emits local NDJSON notifications from inotify. It is an educational, read-only observer, not a prevention or recovery product.

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

`init` refuses to replace an existing database. `scan` deliberately replaces the current baseline. `verify` returns 2 when it finds differences. Run `watch` in a separate terminal and stop it with Ctrl-C or `fimctl stop --pidfile ./fimd.pid`.

## Scope and limitations

Traversal refuses symlinks and skips filesystems mounted below the selected root during baseline scanning. A bounded 256-entry pending-move table correlates inotify cookies; unmatched move-in and timed-out move-out events are kept distinct. Inotify does not identify the process that changed a file, can coalesce events, and can overflow. The live watcher is a notification stream; it does not yet reconcile automatically or update the baseline. A degraded event directs the operator to run `verify --full`. Export paths are local data and may contain personal information.

Supported target is a Linux filesystem and an unprivileged lab directory. No Ubuntu/Rocky runtime matrix has been executed. Run `ctest --test-dir build --output-on-failure`; the smoke test covers baseline creation/comparison, JSON path escaping, symlink exclusion, paired and unmatched rename observations, move timeout, inotify event output, and monitor shutdown. It does not simulate queue overflow/reconciliation, mount boundaries, concurrent mutation, or filesystem errors; see `docs/TEST_RESULTS.md`.

## Safety

The program never modifies monitored files. It is not a complete audit log, does not attribute process actors, does not protect its database from a privileged attacker, and does not provide a point-in-time filesystem snapshot.
