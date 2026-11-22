# Phase 3 요구사항 — 프록시

> 상위 인덱스: [requirements.md](requirements.md)
> Phase 1의 HTTP 서버와 Phase 2의 동시성을 기반으로 Proxy 또는 Reverse Proxy를 구현한다.

## 0. 범위

- **Forward Proxy 또는 Reverse Proxy 중 최소 1개** (둘 다 구현하면 더 좋음).
- Reverse: 경로/호스트 라우팅, Round-Robin 로드 밸런싱, 백엔드 타임아웃.
- Forward: 절대 URL 파싱, 외부 서버 연결, 응답 중계, 선택적 캐싱.
- `Transfer-Encoding: chunked` 중계 (프록시에서 필수).
- gzip은 **Phase 3에서 결정**한다.
- **설정 파일 (D6):** Phase 3에서 라우팅 규칙/백엔드/타임아웃을 설정 파일로 받는다. Phase 1 CLI를 유지하고, CLI override를 허용한다.
- 제외: TLS 종료/발급, HTTP/2, HTTP/3, 전체 캐시 계층.

## 1. 결정 요약

| ID | 결정 | 상태 | 이유 |
|---|---|---|---|
| D3 | 프록시 종류 | **미확정** (추천: Reverse) | 과제 예시(`/api/*` → 8001)와 로드 밸런싱에 적합 |
| D4 | Reverse 라우팅 | **미확정** (추천: 경로 기반) | 구현/테스트 명확. 호스트 기반은 선택 |
| D5 | 프록시 정적 서빙 | **미확정** (추천: 프록시 직접 서빙) | 정적/동적 라우팅을 모두 보여줄 수 있음 |
| D8 | 캐싱 | **미확정** (선택) | 필수 아님. 시간 남으면 구현 |
| D6 | 설정 파일 (Phase 3) | **결정: Phase 3에서 추가** | Phase 1 CLI 유지 + 라우팅 규칙용 설정 파일. 규칙이 많아 CLI로 관리하기 어려움 |
| gzip | Phase 3에서 결정 | **미확정** | 중계만 할지, 압축 해제/재압축할지 |

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

- **D3**: Reverse / Forward / 둘 다.
- **D4**: 경로 기반 vs 호스트 기반 라우팅.
- **D5**: 프록시가 정적 파일을 직접 서빙할지, 백엔드로 넘길지.
- **D8**: 캐싱 여부와 방식(메모리 LRU/디스크).
- **gzip**: 중계만 vs 압축 해제/재압축.
- **CONNECT**: 지원 여부.
- **D6 설정 파일 형식**: INI/JSON/자체 형식.
- 라우팅 규칙/백엔드 포트/타임아웃 기본값.
