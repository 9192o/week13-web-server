# 브라우저 라우팅 실습

VS Code의 개발 컨테이너나 `sh dev.sh shell`로 연 개발 셸에서 실행한다.
정적 루트는 저장소 전체가 아니라 실습 파일만 담은 `public/`으로 지정한다.

```sh
make
./webserver --port 8080 --root public
```

VS Code 개발 컨테이너에서는 **Ports** 탭에서 컨테이너 포트 `8080`을 전달한 뒤
**Open in Browser**를 누른다. 다른 로컬 포트가 배정되면 Ports 탭에 표시된 주소를 사용한다.
브라우저 개발자 도구의 **Network** 탭을 열고 새로고침하면 상태와 응답 헤더를 볼 수 있다.

| 요청 | 기대 결과 |
| --- | --- |
| `GET /`, `GET /index.html` | `200`, 같은 HTML |
| `GET /http-server-guide.html` | `200`, 현재 구현의 요청 흐름과 HTTP 개념 정리 |
| `GET /about` | `308`, `Location: /about/` (Network의 Preserve log로 확인) |
| `GET /about/` | `200`, 하위 디렉터리의 `index.html` |
| `GET /assets/style.css`, `GET /assets/demo.js` | `200`, `text/css`, `text/javascript` |
| `GET /assets/sample.png`, `GET /assets/sample.jpg` | `200`, `image/png`, `image/jpeg` |
| `GET /assets/aris_kei.gif` | `200`, `image/gif` (애니메이션 샘플) |
| `GET /assets/sample.json`, `GET /missing.html` | 각각 `403`(미지원 확장자), `404`(없는 파일) |
| `GET /echo` | `405`, `Allow: POST` |
| 에코 버튼 (텍스트 또는 PNG) | `POST /echo` → `200`, `application/octet-stream`, 바이트 일치 |

새 MIME 타입은 [IANA 등록명](https://www.iana.org/assignments/media-types)을 확인하고
필요한 확장자만 공개 목록에 추가한다. GET의 타입·본문 원본 일치, HEAD의 같은
타입·길이와 빈 본문, 대소문자 변형, 기존 미지원 확장자의 `403`을 확인한다.
이 서버는 파일 내용을 검사해서 타입을 정하지 않으므로 공개 루트에는 신뢰한 파일만
둔다. MIME 지정과 성능은 별개다. GIF 샘플은 약 11 MB를 내려받으므로 포트폴리오
경량화가 필요하면 별도의 작은 샘플로 비교할 수 있다.

브라우저를 열지 않아도 **컨테이너 안의 다른 터미널**에서 확인할 수 있다.

```sh
curl -I http://127.0.0.1:8080/assets/sample.png
curl -i http://127.0.0.1:8080/about
curl -i --data-binary 'hello' http://127.0.0.1:8080/echo
```
