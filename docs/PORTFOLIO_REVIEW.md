# Portfolio review / 포트폴리오 검토 안내

## 문제와 제작 의도

파일의 현재 상태를 신뢰 가능한 기준선과 비교하고 이벤트 유실을 드러내기. 이 저장소는 Linux 시스템 호출의 결과와 실패를 관측 가능한 계약으로 연결하는 교육용 구현입니다.

## 핵심 설계

SQLite 기준선은 감시 중 읽기 전용입니다. 두 hash worker가 계산하고 main loop만 순서대로 결과와 journal을 확정합니다. dirfd와 no-follow 접근은 경로 해석 범위를 줄이고, 전후 stat은 읽는 동안의 변화를 발견합니다.

## 코드 검토 경로

1. [`src/fimctl.c`](../src/fimctl.c)의 CLI 진입점에서 자원 획득과 오류 반환을 찾습니다.
2. [코드 해설](CODE_WALKTHROUGH.md)에서 핵심 함수와 소유권을 따라갑니다.
3. [최신 검증 기록](VALIDATION_CHECKPOINT.md)과 [요구사항 추적표](REQUIREMENTS_TRACEABILITY.md)를 비교합니다.
4. [기술 질문](TECHNICAL_INTERVIEW.md)과 [정답 없는 실습](DEBUGGING_EXERCISES.md)으로 직접 설명·재현합니다.

검토 질문: inotify 이벤트와 현재 상태의 차이를 어떻게 구분하는가? worker가 늦게 끝났을 때 순서를 누가 소유하는가? overflow 이후 무엇을 복구할 수 없는가?

## English

**Purpose:** Compare current file state with a recorded baseline and expose lost coverage.

The baseline stays read-only during watching. Two workers calculate hashes while the event loop commits ordered results and journal records. Descriptor-relative, no-follow access limits path resolution; before/after stat checks detect changes during reads.

Start with the linked source entry point, follow `CODE_WALKTHROUGH.md`, and compare the dated checkpoint with requirement traceability. Review questions: How do events differ from state? Who owns ordering when a worker finishes late? Which history cannot be recovered after overflow?

The five study documents cover subsystem purpose, data/control flow, Linux interfaces, memory/resource lifetime, concurrency, security assumptions, failures and alternatives. Three reproduction exercises remain unsolved. Repository checks and the owner's demonstrated competence are tracked separately; neither AI assistance nor automated test completion establishes personal mastery.
