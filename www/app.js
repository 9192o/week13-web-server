// 데모 페이지 스크립트: 정적 파일 헤더 표, CGI POST, 프록시 라운드로빈 확인.

function fmtSize(n) {
  if (n === null) return '-';
  n = Number(n);
  if (n >= 1 << 20) return (n / (1 << 20)).toFixed(1) + ' MB';
  if (n >= 1024) return (n / 1024).toFixed(1) + ' KB';
  return n + ' B';
}

// 서버가 GET/POST 만 지원하므로(HEAD 는 501) GET 으로 받아 헤더를 표에 채운다.
// 1MB 넘는 파일(big.bin)은 헤더만 보고 끊는다. 서버 로그에 클라이언트 끊김이 한 줄 남는다.
const BIG = 1 << 20;

async function fillStaticRow(tr) {
  const path = tr.dataset.path;
  const ctl = new AbortController();
  let cells;
  try {
    const r = await fetch(path, { cache: 'no-store', signal: ctl.signal });
    if (Number(r.headers.get('Content-Length')) > BIG) ctl.abort();
    else await r.arrayBuffer();     // 본문을 다 읽어야 연결을 keep-alive 로 다시 쓴다
    cells = [path, r.status, r.headers.get('Content-Type') || '-',
             fmtSize(r.headers.get('Content-Length'))];
    tr.innerHTML = cells.map(c => '<td></td>').join('');
    tr.children[1].className = r.ok ? 'ok' : 'bad';
  } catch (e) {
    cells = [path, 'ERR', String(e), '-'];
    tr.innerHTML = cells.map(c => '<td></td>').join('');
    tr.children[1].className = 'bad';
  }
  cells.forEach((c, i) => { tr.children[i].textContent = c; });
}

document.querySelectorAll('#static tbody tr').forEach(fillStaticRow);

// CGI POST: 폼 값을 a=..&b=.. 본문으로 보낸다.
document.getElementById('post-form').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const out = document.getElementById('post-result');
  const body = new URLSearchParams(new FormData(ev.target)).toString();
  try {
    const r = await fetch('/cgi-bin/adder', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body,
    });
    out.textContent = ' → ' + r.status + ' ' + (await r.text()).trim();
  } catch (e) {
    out.textContent = ' → ' + e;
  }
});

// 프록시: 같은 경로를 여러 번 요청해 어느 백엔드가 답했는지 순서대로 찍는다.
document.getElementById('api-btn').addEventListener('click', async () => {
  const log = document.getElementById('api-log');
  log.innerHTML = '';
  for (let i = 0; i < 6; i++) {
    const li = document.createElement('li');
    try {
      const r = await fetch('/api/whoami.txt', { cache: 'no-store' });
      li.textContent = r.status + '  ' + (r.ok ? (await r.text()).trim() : r.statusText);
    } catch (e) {
      li.textContent = 'ERR ' + e;
    }
    log.appendChild(li);
  }
});
