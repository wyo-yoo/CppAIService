// npm install --prefix tests; node tests/frontend.cjs [optional screenshot path]
const { chromium } = require("playwright");
const http = require("http"),
  fs = require("fs"),
  path = require("path"),
  assert = require("assert/strict");
const html = fs.readFileSync(
  path.join(__dirname, "../AIApps/ChatServer/resource/AI.html"),
);
const sessions = new Map([
  [
    "100",
    {
      sessionId: "100",
      name: "已有会话",
      history: [
        { is_user: true, content: "旧问题" },
        { is_user: false, content: "旧回答" },
      ],
    },
  ],
]);
const runs = new Map();
let nextId = 200,
  requests = [];
const server = http.createServer(async (req, res) => {
  let raw = "";
  for await (const part of req) raw += part;
  const body = raw ? JSON.parse(raw) : {};
  requests.push({ url: req.url, body });
  const reply = (value) => {
    res.setHeader("Content-Type", "application/json");
    res.end(JSON.stringify(value));
  };
  if (req.url === "/") {
    res.setHeader("Content-Type", "text/html; charset=utf-8");
    res.end(html);
    return;
  }
  if (req.url === "/chat/sessions")
    return reply({
      sessions: [...sessions.values()].map(({ history, ...s }) => s),
    });
  if (req.url === "/chat/history")
    return reply({ history: sessions.get(body.sessionId).history });
  if (req.url === "/chat/sessions/rename") {
    sessions.get(body.sessionId).name = body.name;
    return reply({ success: true, name: body.name });
  }
  if (req.url === "/chat/sessions/delete") {
    sessions.delete(body.sessionId);
    return reply({ success: true });
  }
  if (req.url === "/chat/cancel") {
    runs.get(body.requestId)?.finish(true);
    return reply({ success: true });
  }
  if (req.url === "/chat/stream") {
    const id = body.sessionId || String(nextId++);
    let s = sessions.get(id);
    if (!s) {
      s = { sessionId: id, name: body.question, history: [] };
      sessions.set(id, s);
    }
    res.setHeader("Content-Type", "text/event-stream");
    res.setHeader("Connection", "close");
    res.flushHeaders();
    const emit = (event, value) => {
      const buffer = Buffer.from(
        `event: ${event}\r\ndata: ${JSON.stringify(value)}\r\n\r\n`,
      );
      res.write(buffer.subarray(0, buffer.length - 3));
      res.write(buffer.subarray(buffer.length - 3));
    };
    const answer = { is_user: false, content: "" };
    s.history.push({ is_user: true, content: body.question }, answer);
    let timer,
      ended = false,
      index = 0;
    const pieces = ["第一段内容。", "这是随后到达的第二段。", "最后一段。"];
    const run = {
      finish(stopped) {
        if (ended) return;
        ended = true;
        clearInterval(timer);
        emit("done", { sessionId: id, text: answer.content, stopped });
        res.end();
        runs.delete(body.requestId);
      },
    };
    runs.set(body.requestId, run);
    emit("meta", { sessionId: id, requestId: body.requestId, name: s.name });
    timer = setInterval(() => {
      if (index < pieces.length) {
        const text = pieces[index++];
        answer.content += text;
        emit("delta", { text });
      } else run.finish(false);
    }, 650);
    res.on("close", () => {
      clearInterval(timer);
      runs.delete(body.requestId);
    });
    return;
  }
  if (req.url === "/chat/tts") return reply({ url: "/voice.wav" });
  res.statusCode = 404;
  res.end("{}");
});
(async () => {
  await new Promise((r) => server.listen(0, "127.0.0.1", r));
  const options = { headless: true };
  if (process.env.CHROME_PATH) options.executablePath = process.env.CHROME_PATH;
  const browser = await chromium.launch(options);
  let page;
  try {
    page = await browser.newPage({ viewport: { width: 1440, height: 960 } });
    const errors = [];
    page.on("pageerror", (e) => errors.push(e.message));
    await page.route("https://cdn.jsdelivr.net/**", (route) => route.abort());
    await page.goto("http://127.0.0.1:" + server.address().port);
    await page.getByRole("button", { name: "已有会话", exact: true }).waitFor();
    // A first send must work without pressing New Chat.
    await page.getByLabel("输入问题").fill("帮我制定学习计划");
    await page.getByRole("button", { name: "发送 ↑", exact: true }).click();
    await page.getByText("第一段内容。", { exact: true }).waitFor();
    assert.equal(runs.size, 1);
    assert.equal(
      await page.getByRole("button", { name: "■ 停止生成" }).isVisible(),
      true,
    );
    await page.getByRole("button", { name: "已有会话", exact: true }).click();
    await page.getByText("旧回答", { exact: true }).waitFor();
    await page.waitForTimeout(750);
    assert.equal(await page.getByText("旧回答", { exact: true }).count(), 1);
    assert.equal(
      await page.locator(".message-body").filter({ hasText: "第二段" }).count(),
      0,
    );
    await page.getByRole("button", { name: "返回生成中的会话 ↗" }).click();
    await page.getByRole("button", { name: "■ 停止生成" }).click();
    await page.getByText("已停止 · 已保留生成内容", { exact: true }).waitFor();
    assert.equal(runs.size, 0);
    assert(requests.some((r) => r.url === "/chat/cancel"));
    await page.getByLabel("输入问题").fill("继续说明");
    await page.getByRole("button", { name: "发送 ↑", exact: true }).click();
    await page.waitForFunction(
      () => !document.querySelector("#send-message").disabled,
    );
    assert.equal(
      requests.filter((r) => r.url === "/chat/stream")[1].body.sessionId,
      "200",
    );
    await page
      .getByRole("button", { name: "重命名 帮我制定学习计划", exact: true })
      .click();
    await page
      .getByRole("textbox", { name: "会话名称", exact: true })
      .fill("我的学习计划");
    await page.getByRole("button", { name: "保存", exact: true }).click();
    await page
      .getByRole("heading", { name: "我的学习计划", exact: true })
      .waitFor();
    await page.reload();
    await page
      .getByRole("button", { name: "我的学习计划", exact: true })
      .click();
    await page.getByText("继续说明", { exact: true }).waitFor();
    if (process.argv[2])
      await page.screenshot({ path: process.argv[2], fullPage: true });
    await page
      .getByRole("button", { name: "删除 我的学习计划", exact: true })
      .click();
    await page.getByRole("button", { name: "删除", exact: true }).click();
    await page.getByRole("heading", { name: "新会话", exact: true }).waitFor();
    await page.reload();
    assert.equal(
      await page
        .getByRole("button", { name: "我的学习计划", exact: true })
        .count(),
      0,
    );
    await page.setViewportSize({ width: 390, height: 844 });
    await page
      .getByRole("button", { name: "打开会话列表", exact: true })
      .click();
    await page.getByRole("button", { name: "已有会话", exact: true }).click();
    await page.getByText("旧回答", { exact: true }).waitFor();
    assert.equal(
      await page.evaluate(
        () => document.documentElement.scrollWidth > window.innerWidth,
      ),
      false,
    );
    await page.waitForTimeout(300); // allow the mobile sidebar transition to finish
    if (process.argv[2])
      await page.screenshot({
        path: process.argv[2].replace(/\.png$/, "-mobile.png"),
        fullPage: true,
      });
    assert.deepEqual(errors, []);
    console.log(
      "frontend: first send, incremental display, switching during generation, stop/continue, rename/delete reload and mobile layout passed",
    );
  } finally {
    await browser.close();
    await new Promise((r) => server.close(r));
  }
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
  server.close();
});
