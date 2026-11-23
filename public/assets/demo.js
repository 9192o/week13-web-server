const jsResult = document.getElementById("js-result");
const form = document.getElementById("echo-form");
const text = document.getElementById("echo-text");
const png = document.getElementById("echo-png");
const result = document.getElementById("echo-result");

if (
  jsResult && form instanceof HTMLFormElement && text instanceof HTMLTextAreaElement
  && png instanceof HTMLButtonElement && result
) {
  jsResult.textContent = "JavaScript 로드 및 실행 완료";
  const send = form.querySelector('button[type="submit"]');

  /** @param {() => Promise<Uint8Array<ArrayBuffer>>} payload */
  async function echo(payload, contentType = "text/plain; charset=utf-8") {
    if (!(send instanceof HTMLButtonElement)) return;
    send.disabled = png.disabled = true;
    result.textContent = "요청 중…";
    try {
      const bytes = await payload();
      const response = await fetch("/echo", {
        method: "POST",
        headers: { "Content-Type": contentType },
        body: bytes,
      });
      const returned = new Uint8Array(await response.arrayBuffer());
      const matches = returned.length === bytes.length
        && bytes.every((byte, index) => byte === returned[index]);
      result.textContent = [
        `HTTP ${response.status}`,
        `Content-Type: ${response.headers.get("Content-Type")}`,
        `보낸 본문: ${bytes.length} B / 받은 본문: ${returned.length} B`,
        `바이트 비교: ${matches ? "일치" : "불일치"}`,
      ].join("\n");
    } catch (error) {
      result.textContent = error instanceof Error ? `요청 실패: ${error.message}` : "요청 실패";
    } finally {
      send.disabled = png.disabled = false;
    }
  }

  form.addEventListener("submit", (event) => {
    event.preventDefault();
    void echo(async () => new TextEncoder().encode(text.value));
  });

  png.addEventListener("click", () => {
    void echo(async () => {
      const response = await fetch("/assets/sample.png");
      if (!response.ok) throw new Error(`PNG 샘플: HTTP ${response.status}`);
      return new Uint8Array(await response.arrayBuffer());
    }, "image/png");
  });
}
