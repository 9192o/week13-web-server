const tests = [
  {
    id: 'static-root',
    label: 'GET /',
    hint: '정적 index.html',
    kind: 'static',
    run: () => request('/'),
  },
  {
    id: 'static-json',
    label: 'GET /demo/data.json',
    hint: 'JSON MIME 타입',
    kind: 'static',
    run: () => request('/demo/data.json'),
  },
  {
    id: 'not-found',
    label: 'GET /missing',
    hint: '404 Not Found',
    kind: 'error',
    run: () => request('/missing'),
  },
  {
    id: 'head',
    label: 'HEAD /demo/index.html',
    hint: '본문 없이 헤더만',
    kind: 'static',
    run: () => request('/demo/index.html', { method: 'HEAD' }),
  },
  {
    id: 'range',
    label: 'Range: bytes=0-4',
    hint: '206 Partial Content',
    kind: 'static',
    run: async () => {
      const result = await request('/demo/index.html', {
        headers: { Range: 'bytes=0-4' },
      });
      if (result.status !== 206) {
        result.note =
          '206이 아니면 브라우저가 Range 헤더를 제거했을 수 있습니다. curl로 확인하세요.';
      }
      return result;
    },
  },
  {
    id: 'options',
    label: 'OPTIONS /demo/',
    hint: 'Allow 헤더',
    kind: 'static',
    run: () => request('/demo/', { method: 'OPTIONS' }),
  },
  {
    id: 'cgi-get',
    label: 'GET /cgi-bin/echo?x=1',
    hint: 'CGI 환경변수 + 쿼리',
    kind: 'cgi',
    run: () => request('/cgi-bin/echo?x=1'),
  },
  {
    id: 'cgi-post',
    label: 'POST /cgi-bin/echo',
    hint: '폼 본문 전달',
    kind: 'cgi',
    run: () =>
      request('/cgi-bin/echo', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({ name: 'jungle', message: 'hello' }),
      }),
  },
  {
    id: 'cgi-crash',
    label: 'GET /cgi-bin/crash',
    hint: 'CGI 비정상 종료 → 500',
    kind: 'error',
    run: () => request('/cgi-bin/crash'),
  },
  {
    id: 'cgi-sleep',
    label: 'GET /cgi-bin/sleep',
    hint: '느린 CGI (2초)',
    kind: 'cgi',
    run: () => request('/cgi-bin/sleep'),
  },
  {
    id: 'redirect',
    label: 'GET /demo',
    hint: '301 → /demo/',
    kind: 'static',
    run: () => redirectTest(),
  },
  {
    id: 'keep-alive',
    label: 'keep-alive x5',
    hint: '연속 요청 5회',
    kind: 'static',
    run: () => keepAliveTest(),
  },
];

function headerValue(headers, name) {
  const found = headers.find(([key]) => key.toLowerCase() === name);
  return found ? found[1] : '';
}

function sleepNote(path, status) {
  if (path !== '/cgi-bin/sleep') return '';
  if (status === 504) return '서버가 CGI를 타임아웃으로 종료했습니다.';
  if (status === 200) {
    return 'CGI가 2초 뒤 정상 종료했습니다. 504를 보려면 서버를 --timeout 1로 실행하세요.';
  }
  return '';
}

async function request(path, options = {}) {
  const started = performance.now();
  const res = await fetch(path, { cache: 'no-store', ...options });
  const elapsed = Math.round(performance.now() - started);
  const headers = [...res.headers.entries()];
  const isHead = options.method === 'HEAD';

  let body = '';
  if (!isHead) {
    const text = await res.text();
    body =
      text.length > 4000
        ? `${text.slice(0, 4000)}\n… (전체 ${text.length}자 중 4000자만 표시)`
        : text;
  }

  return {
    status: res.status,
    statusText: res.statusText,
    elapsed,
    headers,
    body,
    isHead,
    note: sleepNote(path, res.status),
  };
}

async function redirectTest() {
  const started = performance.now();
  const res = await fetch('/demo', { redirect: 'manual', cache: 'no-store' });
  const elapsed = Math.round(performance.now() - started);

  if (res.type === 'opaqueredirect' || res.status === 0) {
    return {
      status: 0,
      statusText: 'opaqueredirect',
      elapsed,
      headers: [],
      body: '',
      note:
        '브라우저가 301 응답을 숨겼습니다. 터미널에서 `curl -i http://localhost:8080/demo`로 확인하세요.',
    };
  }

  return {
    status: res.status,
    statusText: res.statusText,
    elapsed,
    headers: [...res.headers.entries()],
    body: '',
  };
}

async function keepAliveTest() {
  const times = [];
  let last = null;

  for (let i = 0; i < 5; i++) {
    const started = performance.now();
    last = await request('/demo/data.json');
    times.push(Math.round(performance.now() - started));
  }

  return {
    ...last,
    note: `5회 연속 요청: ${times.join('ms, ')}ms (브라우저가 연결을 재사용하면 짧게 나옵니다)`,
  };
}

function statusTone(status) {
  if (!status) return 'info';
  if (status >= 500) return 'bad';
  if (status >= 400) return 'warn';
  if (status >= 300) return 'info';
  return 'ok';
}

function errorResult(err) {
  return {
    status: 0,
    statusText: 'network error',
    elapsed: 0,
    headers: [],
    body: '',
    error: String(err),
  };
}

function render(test, result) {
  document.getElementById('result-title').textContent = test.label;

  const root = document.getElementById('result');
  root.classList.remove('empty');
  root.replaceChildren();

  const head = document.createElement('div');
  head.className = 'result-head';

  const badge = document.createElement('span');
  badge.className = `status ${statusTone(result.status)}`;
  badge.textContent = result.status
    ? `${result.status} ${result.statusText}`.trim()
    : result.statusText || 'no status';
  head.append(badge);

  const size = headerValue(result.headers, 'content-length');
  const meta = document.createElement('span');
  meta.className = 'hint';
  meta.textContent = `${result.elapsed}ms${size ? ` · ${size}B` : ''}`;
  head.append(meta);

  root.append(head);

  if (result.note) {
    const note = document.createElement('p');
    note.className = 'note';
    note.textContent = result.note;
    root.append(note);
  }

  if (result.error) {
    const pre = document.createElement('pre');
    pre.textContent = result.error;
    root.append(pre);
    return;
  }

  if (result.headers.length > 0) {
    const title = document.createElement('h3');
    title.textContent = 'Response headers';
    root.append(title);

    const table = document.createElement('table');
    const tbody = document.createElement('tbody');

    for (const [name, value] of result.headers) {
      const row = document.createElement('tr');
      const th = document.createElement('th');
      th.textContent = name;
      const td = document.createElement('td');
      td.textContent = value;
      row.append(th, td);
      tbody.append(row);
    }

    table.append(tbody);
    root.append(table);
  }

  const bodyTitle = document.createElement('h3');
  bodyTitle.textContent = 'Body';
  root.append(bodyTitle);

  const pre = document.createElement('pre');
  if (result.isHead) {
    pre.textContent = '(HEAD 요청 — 본문 없음)';
  } else if (result.body) {
    pre.textContent = result.body;
  } else {
    pre.textContent = '(빈 본문)';
  }
  root.append(pre);
}

function renderTests() {
  const root = document.getElementById('tests');

  for (const test of tests) {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = `btn test ${test.kind}`;
    button.dataset.id = test.id;

    const label = document.createElement('span');
    label.className = 'test-label';
    label.textContent = test.label;

    const hint = document.createElement('span');
    hint.className = 'test-hint';
    hint.textContent = test.hint;

    button.append(label, hint);
    button.addEventListener('click', () => runOne(test, button));
    root.append(button);
  }
}

async function runOne(test, button) {
  button.disabled = true;
  try {
    render(test, await test.run());
  } catch (err) {
    render(test, errorResult(err));
  } finally {
    button.disabled = false;
  }
}

async function runAll() {
  const allButton = document.getElementById('run-all');
  const buttons = [...document.querySelectorAll('.test')];

  allButton.disabled = true;
  buttons.forEach((button) => {
    button.disabled = true;
  });

  for (const test of tests) {
    try {
      render(test, await test.run());
    } catch (err) {
      render(test, errorResult(err));
    }
  }

  allButton.disabled = false;
  buttons.forEach((button) => {
    button.disabled = false;
  });
}

document.addEventListener('DOMContentLoaded', () => {
  renderTests();
  document.getElementById('run-all').addEventListener('click', runAll);
});
