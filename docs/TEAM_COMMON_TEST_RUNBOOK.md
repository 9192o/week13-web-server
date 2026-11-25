# 팀 공통 테스트 재실행 가이드

이 문서는 [공통 테스트 계획](TEAM_COMMON_TEST_PLAN.md)을 다른 컴퓨터에서 실행하기 위한 가이드다. **합격 기준은 계획 문서가 원본**이다. 여기서 후보별로 다른 것은 기동 인자, 시험할 입력 한도의 단위·수치, 마감 시간 및 선언된 연결 정책뿐이다. 기존 결과나 머지 순위를 새 실행의 기준으로 쓰지 않는다.

## 1. 깨끗한 소스와 공통 환경 준비

저장소에 접근할 수 있는 Bash 셸에서 실행한다. Windows에서는 WSL Bash를 사용하고 Docker Desktop이 Linux 컨테이너를 실행할 수 있는지 확인한다. `docker`가 연결되지 않고 `docker.exe`가 연결된다면 `D=docker.exe`를 쓴다. 아래 명령은 현재 브랜치를 체크아웃하거나 수정하지 않는다.

```bash
set -o pipefail
D=docker                         # 필요하면 docker.exe
IMAGE=week13-web-server-dev:trixie
BOX=week13-team-qa-$$
git fetch origin

eun=f4f33c7b843c52e27b3115a1359f5ee40ee5b910
woong=01a2e8580eef90d118745ddf7f22e9804396f777
ldc=11080e4abee013a71245a7de8168e9d47e89c45b

"$D" info --format '{{.OSType}}/{{.Architecture}}'
"$D" image inspect --format '{{.Id}} {{.Os}}/{{.Architecture}}' "$IMAGE"
```

이전 비교 이미지 ID는 `sha256:8542426ab0d1d4b49eced209564c1162aa807d5a9667d0335f786a9b50559bb9`이었다. 같은 이미지를 가진 팀원은 `"$D" save "$IMAGE" > team-qa-image.tar`로 내보내고, 받는 팀원은 `"$D" load < team-qa-image.tar`로 가져온 뒤 ID를 확인한다. 이미지가 공유되지 않으면 아래처럼 같은 Dockerfile로 **새 공통 이미지**를 만들 수 있다. 패키지 해석 결과가 달라질 수 있으므로 같은 이미지로 간주하지 말고, 새 ID로 세 후보 모두를 실행한다. 빌드 파일은 테스트 환경에만 쓰며 후보 코드나 후보별 빌드 옵션에 영향을 주지 않는다.

```bash
git show "$woong:Dockerfile" | "$D" build --platform linux/amd64 -t "$IMAGE" -
```

팀원 간 기능 결과가 다르면 **같은 SHA와 이미지 ID로 재실행**한다. 이미지가 같아도 커널·Docker 백엔드·CPU는 다를 수 있다. ARM 호스트의 amd64 에뮬레이션 여부도 기록하고, 에뮬레이션과 네이티브 부하 수치를 한 순위표로 합치지 않는다.

```bash
"$D" run --pull never --platform linux/amd64 --network none \
  --cpus 2 --memory 1g --ulimit nofile=4096:4096 --user 1000:1000 \
  --name "$BOX" -dit "$IMAGE" bash
"$D" exec "$BOX" mkdir -p /workspace/{eun,woong,ldc,qa/logs}
for candidate in eun woong ldc; do
  sha=${!candidate}
  git archive "$sha" | "$D" exec -i "$BOX" tar -xf - -C "/workspace/$candidate" || break
  "$D" exec "$BOX" sh -c 'printf "%s %s\n" "$1" "$2" >> /workspace/qa/logs/sources.txt' sh "$candidate" "$sha"
done
"$D" exec -it "$BOX" bash
```

아래부터는 컨테이너 내부다. 포트를 호스트로 공개하지 않는다. 추가 터미널이 필요하면 **같은 컨테이너**에 `"$D" exec -it "$BOX" bash`로 들어간다. `/workspace/qa/logs/sources.txt`에 세 SHA가 모두 있는지 확인한다.

```bash
export QA=/workspace/qa
set -o pipefail
{
  uname -smr; id; ulimit -n
  cc --version; make --version; python3 --version; curl --version; valgrind --version
  printf 'cpu.max '; head -n 1 /sys/fs/cgroup/cpu.max
  printf 'memory.max '; head -n 1 /sys/fs/cgroup/memory.max
} 2>&1 | tee "$QA/logs/environment.log"
test "$(id -u)" -ne 0
```

`cpu.max`·`memory.max`를 읽을 수 없는 Docker 환경에서는 호스트의 컨테이너 설정값과 그 제약을 기록한다. 호스트 OS/CPU, Docker 버전, 이미지 ID와 도구 버전은 결과에 함께 남긴다.

## 2. 공통 파일과 백엔드 준비

다음 블록은 정적 루트, 바이너리 POST 입력, 백엔드 A/B를 준비한다. 이미지 확장자 파일은 MIME·바이트 검사용 합성 입력이지 렌더링 테스트용 이미지가 아니다. `qa` 셸에서 실행한다.

```bash
cd "$QA"
python3 - <<'PY'
from pathlib import Path

for name in ("one", "two"):
    root = Path(name)
    root.mkdir(exist_ok=True)
    for ext in ("html", "css", "js", "png", "jpg"):
        suffix = b"\0\xff" if ext in ("png", "jpg") else b""
        (root / f"file.{ext}").write_bytes((ext.encode() + suffix) * 21 if name == "one" else b"other root")
Path("one/small.html").write_bytes(b"x" * 1024)
Path("one/big.png").write_bytes(b"\0\xde\xff\x01" * (2 * 1024 * 1024))
locked = Path("one/locked.html")
locked.touch()
locked.chmod(0)
Path("post-text.bin").write_bytes(b"text-1")
Path("post-binary.bin").write_bytes(b"\0\xffbinary")
PY
sha256sum one/file.* one/small.html one/big.png two/file.html post-*.bin | tee logs/fixtures.sha256

cat > "$QA/backend.py" <<'PY'
import hashlib
import http.server
import json
import socket
import sys
import time

identity = sys.argv[1].encode()
port = int(sys.argv[2])

class Backend(http.server.ThreadingHTTPServer):
    request_queue_size = 128

    def get_request(self):
        conn, address = super().get_request()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return conn, address

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def serve(self):
        declared = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(declared)
        if "--quiet" not in sys.argv:
            print(json.dumps({"backend": identity.decode(), "method": self.command,
                              "path": self.path, "declared": declared, "received": len(body),
                              "sha256": hashlib.sha256(body).hexdigest()}), flush=True)
        if len(body) != declared:
            self.close_connection = True
            return
        endpoint = self.path.rsplit("/", 1)[-1]
        if endpoint == "silent":
            time.sleep(14)  # 이 가이드의 고정 후보 응답 헤더 마감(최대 10초)보다 길다.
            self.close_connection = True
            return
        payload = {
            "small": lambda: b"x" * 1024,
            "big": lambda: b"\0\xde\xff\x01" * (2 * 1024 * 1024),
        }.get(endpoint, lambda: b"|".join((identity, self.command.encode(), self.path.encode(), body)))()
        self.send_response(404 if endpoint == "missing" else 200)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        if endpoint == "truncated":
            payload = payload[:2]
            self.close_connection = True
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass

    do_GET = do_POST = serve

with Backend(("127.0.0.1", port), Handler) as service:
    service.serve_forever()
PY

python3 "$QA/backend.py" A 20803 > "$QA/logs/backend-a.log" 2>&1 & A_PID=$!
python3 "$QA/backend.py" B 20804 > "$QA/logs/backend-b.log" 2>&1 & B_PID=$!
```

다음 명령은 배열과 PID를 같은 Bash 세션에서 사용한다. 다른 터미널에서는 `export QA=/workspace/qa`를 다시 설정한다. 한 후보씩 검증하고, 다음 후보 전에는 기존 서버·프록시를 종료하고 백엔드도 다시 띄운다. PID를 잃었다면 컨테이너를 통째로 새로 시작하는 편이 안전하다.

## 3. 기본 빌드·단위 시험 및 기동 어댑터 — F01, U01–U06

`CANDIDATE` 값을 바꿔 반복한다. 사용자 환경의 `CFLAGS` 등을 의도하지 않게 상속하지 않도록 초기화한다. 파이프라인이 아니라 **`make`의 종료 코드**를 기록한다. 기본 빌드가 실패하면 런타임 항목은 해당 기본 빌드에서 `미실행`으로 남긴다. 원인을 분리하려고 플래그를 바꾸는 경우는 `진단` 결과로 별도 기록한다.

```bash
CANDIDATE=woong                    # eun / woong / ldc
cd "/workspace/$CANDIDATE"
unset MAKEFLAGS MFLAGS CC CFLAGS CPPFLAGS LDFLAGS LDLIBS
LOG="$QA/logs/$CANDIDATE"; mkdir -p "$LOG"
make all 2>&1 | tee "$LOG/build.log"
printf 'build exit=%s\n' "${PIPESTATUS[0]}" | tee "$LOG/status.log"

# make all이 성공한 경우. 자체 통합 테스트는 공통 U 항목의 대체 증빙이 아니다.
case "$CANDIDATE" in
  woong) unit=(test-unit test-proxy-unit) ;;
  eun|ldc) unit=(test) ;;
esac
make "${unit[@]}" 2>&1 | tee "$LOG/unit.log"
printf 'unit exit=%s\n' "${PIPESTATUS[0]}" | tee -a "$LOG/status.log"
```

U01–U06별로 `테스트 파일:함수`, 기대 계약 중 실제로 검사한 범위, 결과를 기록한다. 스위트가 성공해도 검사하지 않은 단위 계약은 `커버리지 없음`이다. `make all`의 성공만으로 F01을 통과 처리하지 말고, 소스·빌드 설정과 아래 포트·루트 전환도 확인한다.

```bash
printf 'listen 20802\ndocroot %s/one\n[route /a/]\ntype proxy\nbackend 127.0.0.1:20803\n[route /b/]\ntype proxy\nbackend 127.0.0.1:20804\n[route /]\ntype static\n' "$QA" > "$QA/proxy.conf"

case "$CANDIDATE" in
  eun)
    direct=(./server -p 20801 -r "$QA/one")
    alternate=(./server -p 20807 -r "$QA/two")
    proxy=(./server -p 20802 -r "$QA/one" -R /a/=127.0.0.1:20803 -R /b/=127.0.0.1:20804)
    ;;
  woong)
    direct=(./webserver --port 20801 --root "$QA/one")
    alternate=(./webserver --port 20807 --root "$QA/two")
    proxy=(./reverse-proxy --port 20802 --route /a/=127.0.0.1:20803 --route /b/=127.0.0.1:20804)
    ;;
  ldc)
    direct=(./server 20801 "$QA/one")
    alternate=(./server 20807 "$QA/two")
    proxy=(./server --config "$QA/proxy.conf")
    ;;
esac
"${direct[@]}" > "$LOG/direct.log" 2>&1 & S_PID=$!
"${proxy[@]}" > "$LOG/proxy.log" 2>&1 & P_PID=$!
```

`ldc` 설정에서는 `strip_prefix`를 지정하지 않아 세 후보 모두 `/a/...`, `/b/...` 그대로를 백엔드에 보내는 구성이다. 실제 경로는 추정하지 말고 F06에서 확인한다. 미래 후보가 Host 기준 라우팅이라면 Host만 바꿔 같은 A/B 관측 기준을 적용한다.

## 4. 공통 기능과 경계 — F02–F07, E01–E07

기동 로그와 정상 GET으로 준비 상태부터 확인한다. 보통 요청의 클라이언트 한도는 5초다. 요청 지연·백엔드 지연·종료 시험은 후보의 **해당 단계 마감 + 2초**를 미리 기록해 적용한다. 시험 도구가 먼저 포기한 경우 서버가 제한을 지켰다는 증거가 아니다.

```bash
for ext in html css js png jpg; do
  curl --noproxy '*' --max-time 5 -sS -D "$LOG/$ext.headers" -o "$LOG/$ext.body" \
    "http://127.0.0.1:20801/file.$ext" &&
    cmp "$QA/one/file.$ext" "$LOG/$ext.body" || break
done

"${alternate[@]}" > "$LOG/alternate.log" 2>&1 & ALT_PID=$!
# 기동 확인 후:
curl --noproxy '*' --max-time 5 -fsS http://127.0.0.1:20807/file.html -o "$LOG/alternate.body"
cmp "$QA/two/file.html" "$LOG/alternate.body"
kill -TERM "$ALT_PID"; wait "$ALT_PID"

for path in a/ok b/missing; do
  name=${path//\//-}
  curl --noproxy '*' --max-time 5 -sS -D "$LOG/$name-get.headers" \
    -o "$LOG/$name-get.body" "http://127.0.0.1:20802/$path"
  curl --noproxy '*' --max-time 5 -sS --data-binary "@$QA/post-binary.bin" \
    -D "$LOG/$name-post.headers" -o "$LOG/$name-post.body" "http://127.0.0.1:20802/$path"
done
```

F02는 `cmp` 외에 저장된 헤더의 `200`, MIME, 선언한 경우 `Content-Length`를 판정한다. F06은 `/a/ok` → A·`200`, `/b/missing` → B·`404`와 메서드·경로·바이너리 본문 보존을 비교한다. 예를 들어 A의 POST 본문은 `A|POST|/a/ok|` 뒤에 원본 입력 바이트가 붙어야 하고, 백엔드 로그의 길이·SHA-256도 원본과 일치해야 한다. `curl`의 종료 코드만으로 HTTP 상태를 판정하지 않는다.

아래 소켓 예시는 **같은 입력을 한 번에 쓰거나 여러 번에 나눠 쓰는 F03**을 실행한다. TCP 패킷의 물리적 분할을 보장하는 시험은 아니다. 백엔드 위임 대신 로컬 동적 경로를 시험할 경우 포트·경로·기대 응답만 기록하여 바꾼다.

```bash
python3 - <<'PY'
from http.client import HTTPResponse
from pathlib import Path
from socket import create_connection
import time

for filename in ("post-text.bin", "post-binary.bin"):
    data = Path("/workspace/qa", filename).read_bytes()
    head = (f"POST /a/echo HTTP/1.1\r\nHost: localhost\r\n"
            f"Connection: close\r\nContent-Length: {len(data)}\r\n\r\n").encode()
    for pieces in ((head + data,), (head[:-1], head[-1:] + data[:1], data[1:])):
        with create_connection(("127.0.0.1", 20802), timeout=5) as sock:
            for index, part in enumerate(pieces):
                if index:
                    time.sleep(0.02)
                sock.sendall(part)
            with HTTPResponse(sock) as response:
                response.begin()
                assert response.status == 200, response.status
                assert response.read() == b"A|POST|/a/echo|" + data
    print(filename, "complete/split identical")
PY
```

나머지는 아래 절차대로 실행한다. 소켓 기반 케이스는 위 예시처럼 `socket.create_connection`·`sendall`·`HTTPResponse`를 사용하거나 동일한 동작을 관측하는 로컬 클라이언트를 쓴다. **각 경계 뒤 새 연결 정상 GET**을 기록한다. 종료 시험은 재기동 후 확인한다.

| ID | 실행과 기록 |
| --- | --- |
| F04 | 직접 서버에서 `/missing.html`, `/locked.html`을 요청해 각각 `404`, `403`을 관측한다. 권한 검사는 비특권 계정 여부부터 확인한다. 통제된 내부 I/O 실패로 `500`을 관측하지 못하면 이 부분은 `미실행`; U05에서 해당 실패를 주입한 테스트 이름·실행 로그가 있을 때만 대체 증빙을 연결한다. |
| F05 | 정상 연결 16개에서 직접 `GET /file.png`, 프록시 `POST /a/echo`(요청별 다른 식별자), `GET /b/ok`를 섞는다. 별도 직접 연결에 헤더가 끝나지 않은 정상 요청의 앞부분을 두고 같은 묶음을 확인한다. 16개 응답의 상태·바이트와 식별자를 대조한다. |
| F07 | 포트 20801·20802 각각 HTTP/1.1 소켓에서 한 응답을 끝까지 읽고 다음 요청을 보낸다. 재사용 모드는 응답 두 개, 종료 모드는 첫 응답의 닫기 정책과 실제 EOF를 확인한다. 후보가 지원하지 않는 모드를 강제하지 않는다. |
| E01 | 문법이 틀린 요청 줄, 지원하지 않는 메서드·버전, 숫자가 아닌 본문 길이를 각각 별도 연결에서 검사한다. 지정된 오류·종료를 확인하며 특정 오류 코드로 고정하지 않는다. |
| E02 | 헤더 끝나기 전에 쓰기를 닫는 경우와 열린 채 대기하는 경우를 분리한다. POST는 정상 경로에 본문 일부만 쓰고 `sock.shutdown(socket.SHUT_WR)`로 쓰기를 끝낸다. 완성된 요청과 같은 성공으로 처리되지 않는지, 해당 백엔드에 전달을 시작했다면 그 작업도 정리되는지 확인한다. |
| E03 | 아래 상한별 `-1 / 동일 / +1`을 따로 시험한다. URI·요청 줄은 정상 경로의 쿼리 길이로, 전체 헤드는 여러 개의 개별 상한 내 일반 헤더로 맞춘다. 본문은 `/a/echo`로 시험한다. 초과 길이 선언만 전송했다면 `선언 단계 거부`로 기록하고 실제 본문 처리로 과장하지 않는다. |
| E04 | 직접 `/big.png`, 프록시 `/a/big`를 작은 덩어리로 천천히 읽고 `logs/fixtures.sha256`의 큰 파일 해시와 대조한다. 별도 연결에서 응답 첫 부분을 읽은 뒤 닫고 새 GET으로 복구 확인. |
| E05 | 동일 설정의 두 번째 프로세스 기동 실패와 종료 코드를 기록한다. 별도 실행에서 큰 응답 전송 중 `kill -INT "$S_PID"` 또는 `kill -TERM "$S_PID"`를 보내고 `wait`; 각각 새로 기동해 시험한다. 프록시도 반복한다. 신규 연결, 진행 중 요청, 종료 후 프로세스 및 재기동 GET을 각각 관측한다. |
| E06 | `/b/`는 백엔드 B 하나만 사용한다. `kill -TERM "$B_PID"; wait "$B_PID"`로 B 프로세스와 열린 소켓을 함께 정리한 뒤 `/b/ok`를 시험한다. 같은 포트에 B를 다시 띄워 정상 GET과 복구까지 경과 시간을 기록한다. A에서 `/a/silent`와 `/a/truncated`를 별도로 요청하고 종료 방식과 수신 바이트를 남긴다. 불완전한 본문을 완전한 성공으로 기록하지 않는다. |
| E07 | 공통 계획의 **컨테이너 내부 합성 파일**만 사용해 정적 루트 경계를 확인한다. 시험 파일 바이트가 응답에 없는지 비교하고, 실제 호스트 파일이나 개인 데이터는 사용하지 않는다. |

아래 수치는 고정 스냅샷의 재실행용 입력 치수다. 상한 의미와 포함 규칙은 소스·문서와 대조한다. 갱신 커밋은 먼저 치수를 갱신한다. 특히 `woong`의 **요청 줄**과 다른 후보의 **URI**는 단위가 다르고, `ldc`의 URI는 설정 기본값 4096이 아니라 고정 버퍼의 NUL 공간을 뺀 **유효 상한**을 사용한다.

| 스냅샷 | URI 또는 요청 줄 상한 | 요청 줄 포함 전체 헤드 | 본문 | 미완성 헤드 / 백엔드 응답 헤더 마감 |
| --- | ---: | ---: | ---: | --- |
| `eun` | URI 8,192 B | 16,384 B | 1,048,576 B | 5초 / 5초 |
| `woong` | 요청 줄 8,192 B (CRLF 포함) | 32,768 B | 1,048,576 B | 10초 / 5초 |
| `ldc` | 유효 URI 4,095 B | 16,384 B | 8,388,608 B | 20초 / 10초 |

## 5. 부하 — L01·L02

공통 계획의 **1·8·32 동시성, 100건 워밍업, 2,000건 측정, 3회 반복**을 사용한다. 일반 빌드만 최적화 수준을 `-O2`로 맞추되 기존의 표준·기능 매크로·링크 옵션은 유지한다. `make clean` 대신 새로 추출한 소스에서 빌드하거나, 깨끗한 빌드 여부를 기록한다. 한 후보만 동작하게 하고 세 후보 모두 서버 로그는 파일로 보낸다. 백엔드 요청마다 JSON 기록을 쓰면 부하 차이가 커지므로 이 단계에만 A를 `--quiet`로 재기동한다. 백엔드 재사용 정책을 끄거나 켜서 특정 후보에 맞추지 않는다.

```bash
# 일반 기능 실행을 종료한 뒤, 후보 디렉터리에서 실행한다.
case "$CANDIDATE" in
  eun)   make -B all CFLAGS='-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -O2 -g' ;;
  woong) make -B all CFLAGS='-std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -g' ;;
  ldc)   make -B all CFLAGS='-std=c11 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra -Wpedantic -g -O2 -MMD -MP -pthread' ;;
esac 2>&1 | tee "$LOG/build-load.log"
printf 'load build exit=%s\n' "${PIPESTATUS[0]}" | tee -a "$LOG/status.log"
```

부하 빌드가 성공한 후보만 새 실행 파일로 기동한다. 실패한 후보는 기본 비교에서 `미실행(빌드 실패)`로 남긴다. 진단 빌드의 부하값을 참고로 함께 수집할 때는 정확한 추가 옵션과 별도 로그 이름을 사용한다. M01/M02로 넘어갈 때는 해당 단계에서 쓰기로 한 일반 빌드를 다시 만든다.

```bash
kill -TERM "$A_PID"; wait "$A_PID"  # 실행 중인 A에만 적용. 신호 종료 코드는 별도 기록.
python3 "$QA/backend.py" A 20803 --quiet > "$LOG/load-backend.log" 2>&1 & A_PID=$!
```

아래 Python 도구는 오직 `127.0.0.1`에 연결하며 요청마다 TCP 연결을 새로 만든다. HTTP `200`과 응답 길이 1,024 B를 성공 조건으로 쓰고, **성공 요청만** p50/p95에 넣는다. 각 워크로드 시작 전과 끝난 뒤 응답 바이트를 검증한다. 부하 전용 로그는 일반 시험 및 메모리 로그와 분리한다.

```bash
cat > "$QA/load.py" <<'PY'
from concurrent.futures import ThreadPoolExecutor
from http.client import HTTPConnection
import json
import time

def get(port, path, verify_body=False):
    start = time.monotonic()
    conn = HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        conn.request("GET", path, headers={"Connection": "close"})
        with conn.getresponse() as response:
            data = response.read()
            if response.status != 200 or len(data) != 1024:
                raise ValueError(f"expected 200/1024, got {response.status}/{len(data)}")
            if verify_body and data != b"x" * 1024:
                raise ValueError("fixture bytes differ")
        return (time.monotonic() - start) * 1000, None
    except Exception as exc:
        return None, f"{type(exc).__name__}: {exc}"
    finally:
        conn.close()

def run(port, path, workers, count):
    start = time.monotonic()
    with ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(lambda _: get(port, path), range(count)))
    elapsed = time.monotonic() - start
    good = sorted(ms for ms, error in results if error is None)
    quantile = lambda q: good[int((len(good) - 1) * q)] if good else None
    return {"count": count, "success": len(good), "seconds": elapsed,
            "success_rps": len(good) / elapsed, "p50_ms": quantile(0.5),
            "p95_ms": quantile(0.95), "error_rate": 1 - len(good) / count,
            "errors": sorted({error for _, error in results if error is not None})[:5]}

for work, port, path in (("L01", 20801, "/small.html"), ("L02", 20802, "/a/small"),
                         ("backend", 20803, "/a/small")):
    for workers in (1, 8, 32):
        _, before = get(port, path, verify_body=True)
        if before:
            raise SystemExit(f"{work}: precheck failed: {before}")
        warm = run(port, path, workers, 100)
        measured = run(port, path, workers, 2000)
        _, after = get(port, path, verify_body=True)
        print(json.dumps({"work": work, "concurrency": workers, "warmup": warm,
                          "measurement": measured, "postcheck_error": after}), flush=True)
        if after:
            raise SystemExit(f"{work}: postcheck failed: {after}")
PY
```

후보당 1회 측정 명령은 `python3 "$QA/load.py" | tee "$LOG/load-$ROUND.jsonl"`이다. `set -o pipefail`을 유지하고 `ROUND=1`, `2`, `3`을 사용한다. 예를 들어 후보 순서는 1회차 `eun → woong → ldc`, 2회차 `woong → ldc → eun`, 3회차 `ldc → eun → woong`이다. 매 회차 같은 백엔드 설정과 로그 조건을 사용한다. JSON의 오류율과 성공 건수를 직접 확인한다. 실행기 종료 코드가 0이어도 개별 요청이 성공했다는 뜻은 아니다. 같은 호스트 안에서 회차별 수치와 중앙값을 비교하고, 직접 백엔드가 병목이거나 호스트 혼잡·에뮬레이션이 확인된 결과는 별도로 표시한다.

## 6. 메모리·FD — M01·M02

**M01:** 이전 비교와 동일한 도구로 재실행하려면 세 후보 모두 Valgrind Memcheck를 사용한다. 정상 프로세스를 내린 뒤 **일반 빌드**의 기동 배열에 아래 옵션만 붙인다. 부하와 동시에 실행하지 않는다. 단위 계측 및 F02·F03·F04·F06·E02·E04·E06을 반복하고, 실제 검사 대상 PID와 종료 코드를 남긴다.

```bash
vg=(valgrind --leak-check=full --show-leak-kinds=all
    --errors-for-leak-kinds=definite,indirect,possible --error-exitcode=77)
"${vg[@]}" --log-file="$LOG/mem-direct-%p.log" "${direct[@]}" > "$LOG/mem-direct.server.log" 2>&1 & S_PID=$!
"${vg[@]}" --log-file="$LOG/mem-proxy-%p.log" "${proxy[@]}" > "$LOG/mem-proxy.server.log" 2>&1 & P_PID=$!
# 기능·경계 케이스 재실행 후:
kill -TERM "$S_PID" "$P_PID"
wait "$S_PID"; printf 'mem-direct exit=%s\n' "$?" | tee -a "$LOG/status.log"
wait "$P_PID"; printf 'mem-proxy exit=%s\n' "$?" | tee -a "$LOG/status.log"
```

단위 경로는 `eun`은 성공한 `make test` 뒤 `build/tests/test_*` 실행 파일 각각, `woong`은 `make UNIT_RUNNER="${vg[*]}" test-unit test-proxy-unit`, `ldc`는 `make run_tests` 뒤 `"${vg[@]}" ./run_tests`다. 실행 파일별 요약과 종료 상태를 남긴다. 이 명령은 외부 CGI 실행 파일까지 추적하는 설정이 아니다. CGI를 검증 범위에 넣었다면 피검사 자식과 실제 계측 여부를 별도로 기록한다. `possibly lost`와 `definitely lost`를 구분하고, 결과가 없거나 강제 종료된 검사는 통과가 아니다. 도구 하에서만 재현되면 일반 실행 결과와 분리하고, 관측 시간을 조정했으면 조정 이유·값을 남겨 같은 원칙으로 모든 후보에 적용한다.

**M02:** 일반 빌드에서 직접 서버와 프록시 각각 정상 GET 100건과 응답 중단 100건을 한 묶음으로 3회 반복한다. 프록시는 묶음마다 백엔드 장애·복구도 포함한다. 측정은 예열 직후와 각 묶음 뒤 **같은 유휴 조건**에서 한다.

```bash
for pid in "$S_PID" "$P_PID"; do
  printf 'pid=%s fds=' "$pid"
  ls -1 "/proc/$pid/fd" | wc -l
  printf 'child PIDs: '
  awk '{for (i=1;i<=NF;i++) print $i}' "/proc/$pid"/task/*/children | sort -nu | xargs -r echo
done
```

관측한 자식이 있으면 각 자식에서도 FD와 자식 수를 재귀적으로 확인한다. 의도된 유휴 연결 풀은 설정 상한 안에서 안정화되는지, 종료 뒤에는 부모·자식이 남지 않는지 별도로 확인한다.

## 7. 결과 공유와 환경 차이 판별

| 기록 키 | 값 |
| --- | --- |
| 기준 환경 | 호스트·CPU·에뮬레이션 여부, Docker·커널, 이미지 ID, 도구 버전, UID, 자원 제한 |
| 후보 설정 | 후보·전체 SHA, 빌드/기동 명령, 포트와 루트, 라우팅·연결 정책, 마감 및 빌드 플래그 |
| 케이스 기록 | `ID / 하위 입력 / 기본·진단·계측 / 판정 / 관측값 / 실제 명령·종료 상태 / 로그 경로` |
| 판정 | `통과`, `실패`, `미실행`, `해당 없음`. 환경으로 시작하지 못한 경우 `미실행(환경 차단)`과 원인을 적는다. |

1. 다른 팀원의 결과와 다르면 이미지·SHA·권한·플래그·제한값·타임아웃을 먼저 비교한다. 한 번에 하나만 바꿔 **같은 케이스와 복구 요청**을 재실행하고 이전 로그도 보존한다. 플래그를 추가해 성공한 빌드는 기본 빌드 성공으로 바꾸지 않는다.
2. 성능이나 선택 기능은 필수 실패·미검증을 상쇄하지 않는다. 처리량은 동일 호스트·조건의 회차끼리 비교한다.
3. 컨테이너 안에서 프로세스를 종료하고 종료 상태를 기록한다. 컨테이너를 지우기 전에 **호스트**에서 로그를 복사한다. 전송 전에는 시험용 데이터 외에 개인 데이터가 없는지 확인한다.

```bash
"$D" exec "$BOX" tar -C /workspace/qa -cf - logs > "$BOX-logs.tar"
tar -tf "$BOX-logs.tar"             # 파일 목록 확인 후
"$D" stop "$BOX"
"$D" rm "$BOX"
```
