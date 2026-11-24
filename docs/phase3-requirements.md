# Phase 3 요구사항 — 프록시

> 상위 인덱스: [requirements.md](requirements.md)
> Phase 1의 HTTP 서버와 Phase 2의 동시성을 기반으로 Proxy 또는 Reverse Proxy를 구현한다.

## 0. 범위

- **Forward Proxy 또는 Reverse Proxy 중 최소 1개** (둘 다 구현하면 더 좋음).
- Reverse: 경로/호스트 라우팅, Round-Robin 로드 밸런싱, 백엔드 타임아웃.
- Forward: 절대 URL 파싱, 외부 서버 연결, 응답 중계, 선택적 캐싱.
- `Transfer-Encoding: chunked` 중계 (프록시에서 필수).
- gzip은 **그대로 중계**한다 (Phase 3 결정).
- **설정 파일 (D6):** Phase 3에서 라우팅 규칙/백엔드/타임아웃을 설정 파일로 받는다. Phase 1 CLI를 유지하고, CLI override를 허용한다.
- 제외: TLS 종료/발급, HTTP/2, HTTP/3, 전체 캐시 계층.

## 1. 결정 요약

| ID | 결정 | 상태 | 이유 |
|---|---|---|---|
| D3 | 프록시 종류 | **결정: Reverse Proxy** | 과제 예시(`/api/*` → 8001)와 로드 밸런싱에 적합 |
| D4 | Reverse 라우팅 | **결정: 경로 기반** | 구현/테스트 명확. `/api/*` 예시와 일치. 호스트 기반은 future |
| D5 | 프록시 정적 서빙 | **결정: 프록시 직접 서빙** | Phase 2 정적 파일 재사용, 정적/동적 라우팅을 모두 시연 |
| D8 | 캐싱 | **결정: Phase 3 MVP는 캐싱 없음** | 정확성/무효화 복잡. 작은 LRU는 future |
| D6 | 설정 파일 (Phase 3) | **결정: INI 유사 형식** | 파서가 단순하고 fail fast가 쉬움. JSON은 과함 |
| D16 | 업스트림 연결 | **결정: keep-alive 풀 (Phase 3 필수)** | 확장성/지연 개선. 기본 중계와 함께 Phase 3 완료 조건에 포함 |
| D17 | 헬스체크 | **결정: passive only** | dead backend 회피를 저비용으로. active `/health`는 future |
| D18 | DNS | **결정: 시작 시 1회 resolve** | hostname 허용, 이벤트 루프 블로킹 방지. DNS 변경 시 재시작 |
| gzip | Phase 3 처리 | **결정: 그대로 중계** | zlib 없이 `Content-Encoding`/`Content-Length` 보존 |
| chunked | Phase 3 처리 | **결정: stream-through** | 원본 chunk를 중계, 큰 body 버퍼링 없음 |

### D3 선택지

| 방안 | 장점 | 단점 | 적합 |
|---|---|---|---|
| Reverse Proxy | 과제 예시와 일치, 경로/호스트 라우팅, 로드 밸런싱, 로컬 테스트 쉬움 | 백엔드 서버 필요, 외부 중계는 아님 | 안정적이고 테스트 가능한 프록시 |
| Forward Proxy | 진짜 프록시, 외부 서버, absolute URL, 캐싱 | 외부 네트워크 의존, CONNECT/HTTPS 복잡, 테스트 어려움 | 네트워크 프록시/캐싱 학습 |
| 둘 다 | 최대 평가, 두 모델 경험 | 작업량 2배, 테스트 복잡 | 시간 충분, 포트폴리오 최대 |

### D6 — 설정 파일 (Phase 3)

- **결정:** Phase 3에서 라우팅 규칙/백엔드/타임아웃을 설정 파일로 받는다. Phase 1의 CLI는 유지하고, **CLI가 설정 파일보다 우선**한다.
- **왜 Phase 3인가:**
  - Phase 1 설정은 포트/docroot/cgi_dir/한계값 정도라 CLI로 충분하다.
  - Phase 3는 경로/호스트 라우팅, 백엔드 목록, 타임아웃, 정적 서빙 정책 등 규칙이 많아 CLI 인자로 관리하기 어렵다.
  - 설정 파일은 읽기 쉽고, 버전 관리/환경별 교체가 쉽다.
- **포함할 항목 (예):**
  - listen port, docroot (선택)
  - route: path prefix → backend 목록
  - backend: host, port, weight (선택)
  - timeout: connect/read/write
  - static policy (D5)
- **우선순위:** CLI > 설정 파일 > 기본값.
- **형식:** INI/JSON/자체 형식 중 Phase 3 설계에서 결정.
- **에러 처리:** 잘못된 설정은 시작 시 명확한 메시지로 실패(fail fast). 설정에 실행 코드/스크립트를 넣지 않는다.
- **Phase 1과의 관계:** Phase 1에서는 파서를 만들지 않는다. Phase 3에서 추가한다.

### D6 형식 — INI 유사 (확정)

- **결정:** `key value` 형태의 줄 단위 설정 파일을 쓴다. `#` 주석과 빈 줄을 허용한다.
- **이유:** Phase 3의 설정은 listen/docroot/route/backend/timeout 정도라 JSON parser를 새로 만들 가치가 없다. 줄 번호를 붙여 fail fast 하기도 쉽고, CLI override 순서(CLI > 파일 > 기본값)를 구현하기도 단순하다.
- **예:**
  ```text
  listen 8080
  docroot ./www
  route /api/ backend 127.0.0.1:8001
  route /api/ backend 127.0.0.1:8002
  route /static/ static
  timeout connect 2
  timeout read 10
  ```

### D4 — 경로 기반 라우팅 (확정)

- **결정:** path prefix의 longest-match로 backend를 고른다.
- **이유:** `/api/*` → 8001 예시와 일치하고, `curl`/통합 테스트가 가장 단순하다. 호스트 기반은 `/etc/hosts`/`Host` 헤더 조작이 필요해 Phase 3 MVP의 테스트 비용을 높인다.
- **규칙:** `/api/v1/`이 `/api/`보다 우선. 매칭 실패 시 정적 서빙 또는 404.

### D5 — 프록시 직접 정적 서빙 (확정)

- **결정:** `/api/*`만 backend로 보내고, 그 외 정적 경로는 Phase 2 `static_file_prepare`로 직접 서빙한다.
- **이유:** Phase 2 정적 파일 코드를 재사용하고, “정적은 프록시, 동적은 backend” 구조를 한 번에 보여줄 수 있다. backend가 정적 파일을 갖지 않아도 된다.

### D8 — Phase 3 MVP 캐싱 없음 (확정)

- **결정:** 캐싱은 구현하지 않고 문서에만 남긴다. future로 “GET + `Cache-Control: max-age`만 지원하는 작은 메모리 LRU”를 검토한다.
- **이유:** cache key/`Vary`/freshness/conditional/무효화/보안(`Set-Cookie`, `Authorization`)까지 범위가 커진다. Phase 3의 핵심인 라우팅/중계/청크/타임아웃/백프레셔를 먼저 완성하는 편이 안전하다.

### D16 — 업스트림 keep-alive 풀 (확정, Phase 3 필수)

- **결정:** Phase 3 완료 조건에 backend별 keep-alive 연결 풀을 포함한다. “2차/future”로 미루지 않는다.
- **keep-alive와 풀의 차이:**
  - keep-alive는 “응답 후 연결을 닫지 않는 HTTP 동작”이다.
  - 풀은 그렇게 남은 idle 연결을 backend별로 저장해 다음 요청에 재사용하는 **프록시 내부 자료구조**다.
  - keep-alive만 켜고 풀이 없으면, 응답 후 연결을 보관할 곳이 없어 결국 close한다.
- **이유:** 연결 재사용은 latency/throughput/backend 연결 수에서 확장성 이점이 크다.
- **관리 항목:** backend별 idle 목록, max idle, idle timeout, 사용 중 연결 제외, backend down 시 풀 close.
- **정확성 규칙:** 응답 body를 끝까지 읽은 뒤에만 풀에 반납한다. `Connection: close` 응답, 파싱 실패, 에러 응답은 풀에 넣지 않는다. 한 연결에 동시에 두 요청을 보내지 않는다(HTTP pipelining 금지).
- **구현 순서:** 기본 중계(connect → send → read → headers/body relay)를 먼저 동작시킨 뒤 같은 Phase 3 안에서 풀을 붙인다. 풀은 릴리스 범위에서 제외하지 않는다.

### D17 — passive 헬스체크 (확정)

- **결정:** Phase 3 MVP는 passive만 한다. connect 실패/timeout 시 backend를 `down_until = now + cooldown`으로 표시하고 Round-Robin에서 건너뛴다.
- **이유:** 추가 타이머/스레드 없이 dead backend를 회피할 수 있다. active `/health`는 backend endpoint 계약/주기/오탐/threshold가 필요하므로 future로 둔다.

### gzip — 그대로 중계 (확정)

- **결정:** `Content-Encoding: gzip` 응답을 해제하지 않고 바이트 그대로 중계한다. `Content-Encoding`/`Content-Length`/`Vary: Accept-Encoding`을 보존한다.
- **이유:** zlib 의존성과 압축/해제 CPU 비용이 없다. 프록시는 body 내용을 검사할 필요가 없다.
- **대안:** 압축 해제/재압축은 body 변형/캐싱/보안 검사가 필요할 때만. Phase 3에서는 하지 않는다.

### D18 — 시작 시 1회 resolve (확정)

- **결정:** 설정의 backend hostname을 서버 시작 시 `getaddrinfo()`로 한 번 resolve하고, 요청 경로에서는 캐시된 IP를 사용한다.
- **이유:** hostname을 허용하면서도 이벤트 루프가 DNS로 블로킹되지 않는다. DNS가 바뀌면 재시작이 필요하지만, Phase 3 데모/로컬 backend에는 충분하다.
- **참고:** `127.0.0.1` 같은 IP literal은 resolve 없이 그대로 사용한다.

### chunked — stream-through (확정)

- **결정:** 요청/응답 chunk를 파싱해 원본 바이트를 중계하고 `Transfer-Encoding: chunked`를 유지한다.
- **이유:** 큰 body를 메모리에 모으지 않아도 되고 프록시 semantics를 보존한다.
- **주의:** chunk size/data/CRLF/trailer 상태 머신, `Content-Length`와 `Transfer-Encoding` 충돌 거부, 요청 chunked parser 확장이 필요하다.
- **대안:** dechunk+`Content-Length`는 큰 body 버퍼링이 필요해 프록시에는 덜 적합하다.

## 2. 기능 요구사항

| ID | 요구사항 | 우선순위 | 검증 |
|---|---|---|---|
| FR-16 | Forward 또는 Reverse 중 1개 이상 | Must | 프록시 동작 테스트 |
| FR-17 | Reverse: 경로/호스트 라우팅, 백엔드 1..N | Must (D3=Reverse) | `/api/*` → 8001 |
| FR-18 | Round-Robin 로드 밸런싱 | Should | 백엔드 분배 확인 |
| FR-19 | Forward: 절대 URL 파싱, 외부 연결, 중계 | Must (D3=Forward) | 외부 사이트 요청 |
| FR-20 | 캐싱 | 선택 | 캐시 hit/miss |
| FR-21 | 정적 요청을 프록시가 직접 서빙할지 결정 | Must (D5) | 라우팅 표 |
| FR-28 | `Transfer-Encoding: chunked` 중계 | Must | chunked 응답 프록시 |
| FR-32 | hop-by-hop 헤더 제거 | Must | 헤더 검사 |
| FR-33 | 백엔드/업스트림 타임아웃 | Must | 지연 백엔드 → 504 |
| FR-34 | gzip 처리 결정/문서화 | Should | gzip 응답 중계/변환 |
| FR-35 | Phase 3 설정 파일: 라우팅/백엔드/타임아웃 | Must (D6) | 설정 파일 로드 + CLI override |

### Reverse Proxy 상세

- 라우팅 규칙:
  - `/api/*` → `127.0.0.1:8001`
  - `/static/*` → `127.0.0.1:8002`
  - 그 외 → 정적 파일 또는 404 (D5/D12)
- 백엔드 1..N. Round-Robin(Should), 헬스 체크는 선택.
- 요청 헤더:
  - `Host` 유지.
  - `X-Forwarded-For`, `X-Forwarded-Proto`, `X-Forwarded-Host` 추가(선택/정책).
- 응답 헤더:
  - hop-by-hop 헤더 제거: `Connection`, `Keep-Alive`, `Proxy-Authenticate`, `Proxy-Authorization`, `TE`, `Trailer`, `Transfer-Encoding`, `Upgrade`.
- 에러 매핑:
  - 백엔드 연결 실패 → `502 Bad Gateway`
  - 백엔드 타임아웃 → `504 Gateway Timeout`
- `Transfer-Encoding: chunked` 응답을 그대로 중계하거나 dechunk/rechunk.
- `Content-Length`와 `Transfer-Encoding`이 모순되면 `502`/`400` 정책 결정.

### Forward Proxy 상세

- 요청 라인 absolute-form(`GET http://host/path HTTP/1.1`) 파싱.
- URL/`Host`에서 호스트·포트 추출, 기본 80.
- `getaddrinfo()`로 업스트림 연결.
- 요청/응답 중계. `Connection`/hop-by-hop 헤더 정리.
- `CONNECT`는 선택(HTTPS 터널). 지원하지 않으면 `501`.
- 캐싱(선택): 메모리 LRU, `Cache-Control`/`Expires`/`Vary` 최소 존중, `Age`/`Date` 처리.
- 보안: SSRF 방지(사설 IP/루프백 차단 정책), 헤더 주입, 요청 스머글링.

### gzip (Phase 3 결정)

- 업스트림이 `Content-Encoding: gzip`으로 응답할 수 있다.
- 프록시가 **그대로 중계**하면 zlib 없이 통과 가능. `Content-Encoding`/`Content-Length`/`Vary`를 보존.
- 캐싱/변형/압축 해제가 필요하면 zlib으로 디코드/재인코드.
- 클라이언트 `Accept-Encoding`과 캐시 키(`Vary`) 정책을 결정.
- Phase 3에서 “중계만” vs “변환” 중 선택하고 문서화.

## 3. 비기능 요구사항

| ID | 요구사항 | 검증 |
|---|---|---|
| NFR-08 | Forward vs Reverse의 관점/보안/로드밸런싱/캐싱 차이 문서화 | 설계 문서 |
| NFR-09 | 프록시 경로에서도 메모리/fd 회수 | ASan/valgrind |
| NFR-13 | 프록시 부하 테스트 | `ab`/`wrk` 결과 |
| NFR-15 | 백엔드 다운/타임아웃, 클라이언트 끊김, chunked, gzip | 테스트 케이스 |

## 4. 테스트 / 인수 기준

### Reverse Proxy (D3=Reverse)

- [ ] `/api/*` 요청이 백엔드 8001로 전달되고 응답이 그대로 반환됨
- [ ] `/static/*` 요청이 백엔드 8002 또는 프록시 정적 서빙으로 처리됨
- [ ] Round-Robin으로 백엔드에 분배됨 (Should)
- [ ] 백엔드가 꺼져 있으면 `502`
- [ ] 백엔드가 느리면 `504`
- [ ] hop-by-hop 헤더가 클라이언트/백엔드로 새지 않음
- [ ] chunked 응답이 정상 중계됨
- [ ] keep-alive/동시성 아래에서 정상 동작

### Forward Proxy (D3=Forward)

- [ ] `curl -x http://127.0.0.1:8080 http://example.com/` 정상
- [ ] absolute-form 요청 파싱
- [ ] `Host`/URL에서 호스트·포트 추출
- [ ] hop-by-hop 헤더 제거
- [ ] `CONNECT` 미지원 시 `501` (지원하면 터널 테스트)
- [ ] 캐싱(선택) hit/miss, `Cache-Control` 최소 존중
- [ ] SSRF/헤더 주입 방어 정책 문서화

### gzip

- [ ] gzip 응답을 중계하거나 변환하는 정책이 문서화됨
- [ ] `Content-Encoding`/`Content-Length`/`Vary`가 올바르게 처리됨

## 5. 범위 제외

- TLS 종료/인증서 발급
- HTTP/2, HTTP/3
- 전체 캐시 계층/고급 캐시 무효화
- 인증/인가 프록시
- `CONNECT`는 선택 (미지원 시 501)

## 6. Phase 1/2에서 넘어오는 것

- HTTP 파서/응답 빌더, 정적 파일, CGI.
- keep-alive와 동시성 모델.
- 에러 처리/로깅/타임아웃 뼈대.
- `getaddrinfo`/소켓 연결 패턴.

## 7. 열린 결정

- **D3**: **결정: Reverse Proxy** (2026-11-24).
- **D4**: **결정: 경로 기반 (longest prefix)** (2026-11-24).
- **D5**: **결정: 프록시 직접 정적 서빙** (2026-11-24).
- **D8**: **결정: Phase 3 MVP는 캐싱 없음** (future: 작은 LRU) (2026-11-24).
- **D6 형식**: **결정: INI 유사** (2026-11-24).
- **D16 업스트림 연결**: **결정: keep-alive 풀, Phase 3 필수** (2026-11-24).
- **D17 헬스체크**: **결정: passive only, active는 future** (2026-11-24).
- **D18 DNS**: **결정: 시작 시 1회 resolve** (2026-11-24).
- **gzip**: **결정: 그대로 중계** (2026-11-24).
- **chunked**: **결정: stream-through** (2026-11-24).
- **CONNECT**: D3=Reverse이므로 Phase 3 범위에서 제외.
- 라우팅 규칙/백엔드 포트/타임아웃 기본값.
