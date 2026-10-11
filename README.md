# Linux File Integrity Monitor

[검토 안내 / Reviewer guide](docs/PORTFOLIO_REVIEW.md) · [최신 검증 / Latest checkpoint](docs/VALIDATION_CHECKPOINT.md)

[:kr: 한국어](#한국어) · [:us: English](#english)

## 한국어

`fimctl`은 지정한 실습 디렉터리에서 일반 파일의 SHA-256과 메타데이터 기준선을 만들고, `verify`와 inotify 감시로 이후 상태 변화를 관찰하는 교육용 도구입니다. 이 프로젝트는 디렉터리 FD 기반 순회, 안정적인 파일 해시, SQLite 트랜잭션, 이벤트 coalescing, 기준선 인식 hash debounce, 감시 범위 복구, Linux 이벤트 유실 한계를 설명하기 위해 제작했습니다. 변경을 차단하거나, 행위자를 식별하거나, 파일을 복구하는 보안 제품은 아닙니다.

### 빌드

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

필요 항목: Linux, C17 컴파일러, CMake, OpenSSL libcrypto, SQLite3 개발 헤더.

### 사용

```sh
mkdir -p "$HOME/fim-lab"
./build/fimctl init --root "$HOME/fim-lab" --db ./state.sqlite
./build/fimctl verify --db ./state.sqlite --full
./build/fimctl watch --root "$HOME/fim-lab" --db ./state.sqlite \
  --events ./events.ndjson --pidfile ./fimd.pid --verify-interval-seconds 3600
```

`init`은 이미 존재하는 DB를 덮어쓰지 않습니다. `scan`은 저장된 root를 다시 스캔해 기준선을 갱신하고, `verify`는 차이를 발견하면 종료 코드 2를 반환합니다. `watch`는 DB를 읽기 전용으로 열며 `--root`가 기준선 root와 일치해야 합니다. Ctrl-C 또는 `./build/fimctl stop --pidfile ./fimd.pid`로 종료할 수 있습니다. 주기 검사 간격은 1~86,400초이고 기본값은 3,600초입니다.

### 구현 및 검증 현황

현재 Linux 호스트에서 GCC Release, Clang Release, Clang ASan/UBSan, ThreadSanitizer CTest가 각각 3회 연속 통과했습니다. Clang 정적 분석은 경고 없이 끝났습니다. 스모크는 기준선 생성/읽기 전용 비교, 파일 해시 증거, 100ms coalescing과 관찰 수·시간 범위, 1,100개 파일 이벤트 부하, 잘못된 검사 간격 거부, 주기 전체 검사, 합성 오버플로 복구, rename, 디렉터리 재감시, PID 파일 종료를 확인합니다. Draft 2020-12 스키마가 worker가 만든 파일 이벤트를 포함한 실제 이벤트 샘플에 적용됐고, `/dev/full` 로그 쓰기 실패도 비정상 종료 코드와 함께 처리됩니다. CTest는 각 빌드 디렉터리의 실행 파일을 사용합니다.

아직 검증하지 않은 항목은 실제 커널 inotify 큐 오버플로, watch 자원 고갈과 재시도, mount 경계, 동시 파일 변경 fault injection, 실제 파일시스템의 디스크 부족·부분 쓰기, 대규모 성능, Ubuntu/RockyOS 매트릭스입니다. 합성 오버플로 테스트는 커널에서 `IN_Q_OVERFLOW`를 발생시키지 않습니다. 따라서 저장소는 SRS 전체 완료가 아니라 구현 일부와 선택된 기술 게이트 통과 상태입니다.

### 동작과 보안 범위

스캐너는 심볼릭 링크와 다른 장치의 파일시스템을 따라가지 않습니다. 감시기는 256개 항목의 메모리 내 이벤트 표와 100ms quiet window로 비-rename 신호를 합칩니다. quiet window 뒤에는 root FD 기준 `openat`으로 파일을 다시 열고, 해시 전후 `fstat` 상태가 안정적인지 확인한 뒤 시작 시 읽은 기준선/마지막 상태와 비교합니다. NDJSON에는 전후 SHA-256, 이벤트 신호 수·시간 범위, 오류 errno를 기록할 수 있습니다. 감시 중 SQLite 기준선은 변경하지 않습니다.

오버플로, 감시 무효화, 디렉터리 트리 변경에는 recursive watch와 현재 상태를 다시 확인합니다. 또한 기본 1시간 간격의 주기 검사는 마지막 성공 스냅샷과 현재 트리를 비교합니다. 이 검사는 현재 상태를 찾을 뿐 중간에 일어난 모든 작업을 복원하지는 못합니다. 두 hash worker가 최대 1,024개의 진행 중 job을 처리하고, 감시 loop만 결과 순서 정리·스냅샷 변경·로그 기록을 담당합니다. 한도에 도달하면 완료 결과를 순서대로 소비하느라 inotify 읽기가 잠시 늦어질 수 있으며, 이 동안 커널 큐가 넘치면 전체 재검사로 현재 상태를 맞춥니다. DB와 이벤트 로그는 같은 사용자 또는 root의 변조를 막지 못하고, 로그 경로에는 개인정보가 포함될 수 있습니다. 폐기 가능한 실습 경로에서 실행하세요.

### 학습 자료

`docs/ENGINEERING_STUDY_GUIDE.md`는 subsystem 목적과 보안 가정을, `docs/CODE_WALKTHROUGH.md`는 함수별 흐름과 자원 수명을, `docs/LINUX_INTERNALS_EXPLAINED.md`는 syscall·inotify 의미를 설명합니다. `docs/DEBUGGING_EXERCISES.md`에는 정답을 넣지 않은 세 가지 독립 구현 연습이 있고, `docs/TECHNICAL_INTERVIEW.md`에는 저장소별 기술 방어 질문이 있습니다. 자동 검증 결과는 소유자의 학습·재현 증거가 아닙니다.

## English

`fimctl` is an educational tool that records SHA-256 and metadata baselines for ordinary files under an explicitly selected lab directory, then observes later state through `verify` and inotify. It was built to teach descriptor-relative traversal, stable file hashing, SQLite transactions, event coalescing, baseline-aware hash debounce, coverage recovery, and the limits of Linux event streams. It is not a prevention, actor-attribution, or file-recovery product.

### Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Requirements: Linux, a C17 compiler, CMake, OpenSSL libcrypto, and SQLite3 development headers.

### Use

```sh
mkdir -p "$HOME/fim-lab"
./build/fimctl init --root "$HOME/fim-lab" --db ./state.sqlite
./build/fimctl verify --db ./state.sqlite --full
./build/fimctl watch --root "$HOME/fim-lab" --db ./state.sqlite \
  --events ./events.ndjson --pidfile ./fimd.pid --verify-interval-seconds 3600
```

`init` refuses to replace an existing database. `scan` refreshes the baseline for its stored root. `verify` returns exit code 2 when it finds differences. `watch` opens the database read-only and requires `--root` to match the baseline root. Stop it with Ctrl-C or `./build/fimctl stop --pidfile ./fimd.pid`. The verification interval accepts 1 through 86,400 seconds and defaults to 3,600 seconds.

### Implementation and verification status

GCC Release, Clang Release, ASan/UBSan, and ThreadSanitizer CTest each passed three consecutive runs on the current Linux host. Clang static analysis produced no diagnostics. The smoke test covers read-only baseline startup comparison; hashed create/delete/modify/metadata evidence; the 100 ms coalescing window and signal count/time range; a 1,100-file event burst; invalid interval rejection; periodic full verification; synthetic overflow recovery; rename handling; recursive watch recovery; and PID-file shutdown. Draft 2020-12 validation passed for actual emitted events, including a worker-generated file event; `/dev/full` journal-write failure returns a nonzero status. CTest uses the executable from the build directory under test.

Still unverified: genuine kernel inotify queue overflow, watch-resource exhaustion and retry, mount boundaries, concurrent-mutation fault injection, real filesystem disk-full/partial-write behavior, large-tree performance, and the Ubuntu/RockyOS matrix. The synthetic overflow test does not make the kernel emit `IN_Q_OVERFLOW`. This repository has passed selected technical gates; it is not a complete SRS implementation.

### Behavior and security scope

The scanner does not follow symlinks or cross to another device. The watcher uses a bounded 256-entry in-memory event table and a 100 ms quiet window to coalesce non-rename signals. After the quiet window, it reopens the path relative to the root FD with `openat`, checks pre/post `fstat` state around hashing, and compares the result with the baseline/last accepted state loaded at startup. NDJSON can include before/after SHA-256 values, the raw signal count and time range, and an error errno. The watcher does not update the SQLite baseline.

Overflow, invalidated watches, and directory-tree changes trigger recursive watch recovery and a current-state scan. A periodic scan also compares the current tree with the last successful in-memory snapshot; its default interval is one hour. These scans recover current state, not every action that happened between scans. Two hash workers process up to 1,024 outstanding jobs. The event loop alone commits results to the snapshot and journal in sequence order. When that bound is reached, the loop waits while consuming completed results, which can delay inotify reads; a kernel queue overflow then triggers a full current-state scan. The database and journal are not protected from the same user or root, and logged paths may contain personal information. Use a disposable lab tree.

### Study materials

`docs/ENGINEERING_STUDY_GUIDE.md` explains subsystem purpose and security assumptions; `docs/CODE_WALKTHROUGH.md` follows functions and resource lifetimes; `docs/LINUX_INTERNALS_EXPLAINED.md` explains syscall and inotify behavior. `docs/DEBUGGING_EXERCISES.md` contains three independent exercises without solutions, and `docs/TECHNICAL_INTERVIEW.md` contains repository-specific defense questions. Automated verification is not evidence of the owner's personal study or reproduction.
