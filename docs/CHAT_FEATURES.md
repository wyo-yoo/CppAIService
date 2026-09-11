# 流式聊天、停止生成与会话管理

聊天页 `/chat` 现在通过 `POST /chat/stream` 接收真实模型 SSE 增量。新增停止按钮、生成状态、会话自动命名／重命名／删除、按名称搜索、Markdown 导出及手机侧栏。首次直接发送即可创建会话；生成期间切换会话不会把回复写到其他会话。

## 行为

- 阿里百炼、豆包的最终回答使用兼容接口流式协议；百炼 RAG 使用 DashScope SSE 和 `incremental_output`。工具助手的判断阶段使用普通请求，工具执行后的回答支持流式输出；判断结果不需要工具时一次性展示。
- 停止会取消上游 HTTP 请求，并保存已经生成的文字。取消检查间隔约 100ms；已有天气工具本身最多等待 5 秒。断开浏览器连接也会结束模型请求。
- 单个用户同时允许一个流式生成任务；后台默认 4 个工作线程、最多 32 个排队任务。读取历史不会等待模型生成。连接超时 10 秒、请求总超时 180 秒。
- 新会话以首条问题生成标题。重命名和删除写入 MySQL。删除采用 `chat_sessions.deleted` 标记，界面与历史接口不再返回该会话；原始消息行保留，暂未实现物理清理或恢复入口。新增格式的迟到队列消息会跳过已删除会话。
- 成功或主动停止的轮次保存成完整的用户／助手消息对；模型请求失败时不提交该轮次到服务端历史，界面显示错误。

## 构建

要求 C++17、CMake、Muduo、SimpleAmqpClient/rabbitmq-c、MySQL Connector/C++、MySQL C 客户端、OpenSSL、libcurl、Boost chrono/system、nlohmann_json。

在当前虚拟机中，Muduo 和 SimpleAmqpClient 的安装目录是 `/home/wy/project/.cppaiservice-deps/install`。使用该路径配置构建：

```bash
cd /home/wy/project/CppAIService
cmake -S . -B build-chat \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=/home/wy/project/.cppaiservice-deps/install \
  -DCHAT_BUILD_TESTS=ON
cmake --build build-chat -j2
ctest --test-dir build-chat --output-on-failure
```

原运行方式仍需 MySQL、已有 `ChatHttpServer.users` 和 `chat_message` 表、RabbitMQ 及相应模型环境变量。程序启动时自动创建新的 `chat_sessions` 表，并迁移旧消息对应的会话名称。启动账户需要该库的建表权限。数据库连接读取 `CHAT_MYSQL_URL`、`CHAT_MYSQL_USER`、`CHAT_MYSQL_PASSWORD` 和 `CHAT_MYSQL_DATABASE`，可在本机 `.env` 中配置。

```bash
# 先在启动服务的环境中配置实际使用的模型凭据和原有数据库/MQ服务。
# 百炼：DASHSCOPE_API_KEY；豆包：DOUBAO_API_KEY；RAG 另需 Knowledge_Base_ID。
cd /home/wy/project/CppAIService
bash scripts/run.sh 8080
```

启动脚本会切换到构建目录，以兼容原有 `../AIApps/ChatServer/resource/` 资源路径。如有反向代理，须允许流式转发、关闭响应缓冲并配置足够长的读取超时。

## 接口

所有接口均使用现有登录 Cookie，且只操作当前用户的会话。

| 接口 | 请求／结果 |
| --- | --- |
| `POST /chat/stream` | `{question, modelType, requestId, sessionId?}`；SSE 事件依次为 `meta`、`status`、多个 `delta`、`done` 或 `error` |
| `POST /chat/cancel` | `{requestId}`；取消当前用户对应任务 |
| `GET /chat/sessions` | 返回会话 ID 与已保存的名称 |
| `POST /chat/history` | `{sessionId}`；不存在或已删除返回 404 |
| `POST /chat/sessions/rename` | `{sessionId, name}` |
| `POST /chat/sessions/delete` | `{sessionId}`；会话生成中返回 409 |

`delta` 内容是 `{text}`；`done` 包含 `{sessionId, text, stopped}`。流式连接采用 HTTP/1.1 关闭连接定界，不发送 Content-Length；终止事件之后关闭连接。

## 验证及范围

- CTest：SSE 分片／中文、首段延迟、兼容接口与 RAG、上游错误、提前断流、静默上游取消、历史回滚与并发保护、实际 HTTP 流式报文、生成中其他请求、客户端断开。
- 浏览器：真实 Chromium 中验证首次发送、逐段显示、切换会话、停止后继续、重命名与删除后刷新、手机布局。测试模型与会话存储由本地 HTTP 测试服务器提供。
- MySQL 集成：生产 ChatServer 和真实临时 MySQL，验证登录／用户隔离、生成中查看历史、停止／继续、模型失败回滚、进程重启恢复名称和历史、删除后重启仍不可访问。模型为本地模拟服务，MQ 交付替换为同步调用入库逻辑；未验证真实 RabbitMQ 故障恢复或真实付费模型。

```bash
# 可选的独立 MySQL 集成测试；临时数据目录自动创建和清理。
python3 tests/test_sessions.py build-chat/chat_session_fixture /usr/sbin/mysqld

# 浏览器测试；首次安装依赖和 Chromium。
npm install --prefix tests
npx --prefix tests playwright install chromium
node tests/frontend.cjs
# 也可设置 CHROME_PATH 指向已安装的 Chrome 可执行文件。
```

此次未重构原有登录密码存储、TTS 延迟响应和 RabbitMQ 整体可靠性机制。生产 MQ 仍异步保存消息；断电、队列不可用等情况下的数据可靠性仍受原系统设计限制。
