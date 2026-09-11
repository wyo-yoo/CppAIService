# 开放注册与公网部署

本版本允许访问者自行注册网站账号，登录后聊天。GitHub 保存源代码，网站需要另外部署到公网 Ubuntu 服务器。下面的配置面向一台服务器、一个应用进程的小规模网站；不需要把数据库、RabbitMQ 或 API Key 发给访问者。

## 已实现的保护与默认额度

| 项目 | 当前行为 |
| --- | --- |
| 注册 | 默认开放；用户名为 3–32 位字母、数字、下划线；新密码为 15–128 个字符，支持长句 |
| 密码 | OpenSSL PBKDF2-HMAC-SHA256，600,000 次迭代，随机 16 字节盐，恒定时间比较；不保存新用户的明文密码 |
| 旧账号升级 | 首次启动前将 `users.password` 中的旧明文密码转换成哈希；旧账号仍使用原密码。上线前先运行备份脚本 |
| 登录 | 按账号和来源地址限速；独立的有界工作线程处理密码计算；允许同一账号在不同设备登录 |
| Cookie | 32 字节安全随机 Session ID；登录时轮换；退出使该设备的 Session 失效；HttpOnly、SameSite=Lax；配置公网 HTTPS 地址后自动加 Secure |
| 浏览器请求 | 修改数据的接口要求 JSON，并检查 Origin；前端与 API 使用同一域名，不开放通配 CORS |
| 每人额度 | 每天 20 次生成，每分钟最多 6 次；同一账号同时只能有一个生成任务 |
| 全站额度 | 每天 200 次生成，同时最多处理 4 个生成任务 |
| 注册额度 | 全站每天最多 50 次、同一来源每天最多 5 次注册尝试；另有短期请求限速 |
| 生成成本 | 单个问题最多 8 KiB；发给模型的历史最多最近 10 轮、总计 32 KiB；默认最多生成 1,024 tokens |
| 会话数量 | 每人最多保留 100 个会话，每个会话最多 100 轮；达到上限时需要开始新会话或删除旧会话 |
| 默认能力 | 只开放 DeepSeek（模型编号 5），语音默认关闭；其他能力由管理员显式配置 |

每日额度以 **UTC 零点** 重置（北京时间上午 8 点）。计数保存在 MySQL 的 `usage_daily` 表里，重启应用、换浏览器或删除会话都不会重置。全站和账号额度在同一个事务中扣除，并发不能透支。请求获准调用模型后，即使生成失败或中途取消也计一次，因为上游可能已产生费用。这是次数和输出上限，不是精确的人民币余额控制；模型价格、输入长度和工具调用仍会影响实际费用。

`/chat/stream`、旧 `/chat/send`、`/chat/send-new-session` 和启用后的 `/chat/tts` 共用限制。未登录不能调用这些接口。伪造 `X-Forwarded-For` 不会更换限速身份。

所有配置见根目录 `.env.example`。例如调整为每人每天 10 次：修改 `.env` 的 `CHAT_USER_DAILY_LIMIT=10` 后重启服务。`CHAT_REGISTRATION_OPEN=0` 只暂停新用户注册，已有用户仍能登录。`CHAT_ALLOWED_MODELS=1,5` 可以开放已配好密钥的百炼与 DeepSeek；`CHAT_ENABLE_TTS=1` 需要配置百度凭据，语音也消耗每日次数。

## 先在当前虚拟机验证

```bash
cd /home/wy/project/CppAIService
python3 scripts/backup_database.py
bash scripts/build.sh
bash scripts/run.sh 8080
```

默认只监听 `127.0.0.1`。在 Windows 终端开一个 SSH 转发窗口：

```powershell
ssh -N -L 8080:127.0.0.1:8080 wy@192.168.135.129
```

然后在 Windows 浏览器打开 `http://localhost:8080`。这是开发环境入口，SSH 转发窗口需要保持打开。

## 在公网服务器部署

需要：一台有公网地址的 Ubuntu 24.04 服务器、可管理的域名。以下用 `chat.example.com` 表示你自己的域名，用普通 Linux 用户运行应用。先将该域名的 DNS A 记录指向服务器公网 IPv4；没有配置 IPv6 时不要添加 AAAA 记录。

1. 在服务器的普通用户目录克隆项目，按照 README 安装依赖、初始化数据库并构建。若仓库仍为私有，克隆时需要 GitHub 授权。只在服务器本地 `.env` 填入数据库凭据和 DeepSeek Key，权限设置为 `600`。
2. 服务器入口只开放网站的 TCP 80/443；SSH 按管理员需要开放。不要向公网开放应用的 8080、MySQL 3306/33060 或 RabbitMQ 5672/15672/25672。云平台的安全组中也要保持这些端口关闭。
3. 在项目目录生成配置，**把域名替换成你的真实域名**：

```bash
python3 scripts/prepare_deployment.py --domain chat.example.com
```

生成的 `.deployment/` 不纳入 Git。把 `.deployment/public.env` 中的配置合并到 `.env`，保留原有凭据。这会设置 `CHAT_PUBLIC_ORIGIN=https://chat.example.com`、`CHAT_TRUST_PROXY=1`、`CHAT_BIND_ADDRESS=127.0.0.1`。代理模式只信任同机 Nginx 覆盖的 `X-Real-IP`；不要在前面直接增加 CDN/第二层代理，需先重新配置真实来源地址的信任范围。

4. 先安装仅用于申请证书的 HTTP 配置；此时登录页面尚未对外开放：

```bash
sudo apt-get update
sudo apt-get install -y nginx certbot
sudo install -d -m 755 /var/www/letsencrypt
sudo install -m 644 .deployment/nginx-bootstrap.conf /etc/nginx/sites-available/cppaiservice.conf
sudo ln -sfn /etc/nginx/sites-available/cppaiservice.conf /etc/nginx/sites-enabled/cppaiservice.conf
sudo nginx -t
sudo systemctl reload nginx
sudo certbot certonly --webroot -w /var/www/letsencrypt -d chat.example.com
```

按 Certbot 提示填写联系邮箱。域名必须已正确解析，80 端口必须可从公网访问。

5. 启用正式 HTTPS 配置和应用：

```bash
python3 scripts/backup_database.py
sudo install -m 644 .deployment/nginx.conf /etc/nginx/sites-available/cppaiservice.conf
sudo nginx -t
sudo install -m 644 .deployment/cppaiservice.service /etc/systemd/system/
sudo install -m 644 .deployment/cppaiservice-backup.service /etc/systemd/system/
sudo install -m 644 .deployment/cppaiservice-backup.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now cppaiservice.service cppaiservice-backup.timer
sudo systemctl reload nginx
```

Nginx 会把 HTTP 跳转到 HTTPS，关闭生成响应的缓冲，让答案逐段显示，并限制请求大小、连接数和等待时间。应用异常退出后由 systemd 自动重启，开机自动启动。以上命令会安装并启用服务；只生成文件并不等于已上线。

6. 为续签证书添加重载动作，然后验证：

```bash
sudo install -d /etc/letsencrypt/renewal-hooks/deploy
printf '#!/bin/sh\n/usr/sbin/nginx -t && /usr/bin/systemctl reload nginx\n' | sudo tee /etc/letsencrypt/renewal-hooks/deploy/cppaiservice-nginx >/dev/null
sudo chmod 755 /etc/letsencrypt/renewal-hooks/deploy/cppaiservice-nginx
sudo certbot renew --dry-run
sudo systemctl status cppaiservice.service --no-pager
sudo journalctl -u cppaiservice.service -n 50 --no-pager
```

用手机关闭 Wi-Fi 后访问 `https://你的域名`，完成注册、登录、聊天和退出，这才是外网可访问的验收。把这个 HTTPS 地址发给别人，不是 GitHub 仓库地址。

## 数据备份与恢复

`python3 scripts/backup_database.py` 在忽略的 `.backups/` 下生成权限为 `600` 的压缩 SQL 文件。定时服务将备份写入 `/var/lib/cppaiservice/backups/`，每天服务器当地时间 03:30 左右执行，默认保留 14 天。备份包含账号哈希、聊天记录和额度；API Key 与 `.env` 不包含在 SQL 备份中，应另外保管配置。数据库使用事务快照，失败不会覆盖之前的备份。

```bash
sudo systemctl start cppaiservice-backup.service
sudo systemctl list-timers cppaiservice-backup.timer
sudo journalctl -u cppaiservice-backup.service -n 20 --no-pager
```

首次上线应把备份复制到另一处私有存储。只有同一块磁盘上的副本不能应对整台服务器损坏。

恢复时先停止应用，将选定 SQL 文件导入一个新的空数据库，检查 `users`、`chat_message`、`chat_sessions`、`usage_daily` 表和行数，再切换 `.env` 的数据库名并启动。不要直接覆盖仍在使用的数据库。登录 Session 保存在内存中，服务重启或恢复后用户需要重新登录；已经入库的聊天历史和当日额度会保留。RabbitMQ 消息异步落库，备份时间点之后及尚未消费的消息不在这份 SQL 快照中。

## 验证与范围

```bash
ctest --test-dir build-chat --output-on-failure
python3 tests/test_sessions.py build-chat/chat_session_fixture /usr/sbin/mysqld
python3 tests/test_deployment.py build-chat/chat_session_fixture /usr/sbin/mysqld /usr/sbin/nginx
# 浏览器回归：先安装 tests/package.json 的依赖
node tests/frontend.cjs
node tests/accounts_frontend.cjs
```

后两项使用临时数据库和本机模拟模型，不消耗真实模型额度。部署测试使用临时自签证书和真实 Nginx，验证 HTTPS Cookie、流式转发、代理地址覆盖以及备份导入恢复。

当前仍是单机应用，内存中缓存聊天历史和登录 Session；没有邮箱验证、找回密码、付费套餐、跨实例 Session 或验证码。每日注册限制和全站额度限制能控制普通试用成本，不代表阻止所有自动化滥用。访问量增大时再增加验证机制、分页/缓存、数据库容量规划和多实例方案。

实现参考：[OWASP 密码存储](https://cheatsheetseries.owasp.org/cheatsheets/Password_Storage_Cheat_Sheet.html)、[OpenSSL PBKDF2](https://docs.openssl.org/3.0/man3/PKCS5_PBKDF2_HMAC/)、[Nginx 反向代理参数](https://nginx.org/en/docs/http/ngx_http_proxy_module.html)。
