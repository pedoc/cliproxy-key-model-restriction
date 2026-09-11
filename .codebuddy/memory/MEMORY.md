# MEMORY

长期稳定的跨会话事实（本项目记忆从 CLIProxyAPI 仓库迁来）。

## 项目
- `cliproxy-key-model-restriction`：CLIProxyAPI 的 C ABI 插件，按 API Key 限制可使用的模型。
- 不配置的 key / `allowed_models` 为空 → 不限制。
- 目录结构遵循官方示例：`c/src/plugin.c` + `c/CMakeLists.txt`。

## 约定与偏好
- **不要修改用户的 `config.yaml`**，用户手动改；插件配置改动后由 config watcher 触发 `plugin.reconfigure` 热加载。
- 远程部署：`dev01@192.168.110.201:/home/dev01/cliproxyapi/plugins/key-model-restriction-c.so`
  - 重启：`systemctl --user restart cliproxyapi`（用户 systemd unit）
  - 日志：`journalctl --user -u cliproxyapi --no-pager -n 20`
- 禁止在代码里留非英文注释；注释用英文。
- 端到端测试要展示原始返回值（HTTP code + 完整 body）。

## 构建与验证
- 构建：`gcc -shared -fPIC -O2 -o key-model-restriction-c.so c/src/plugin.c`
- 插件 ID 由文件名决定：`key-model-restriction-c`（配置文件 `plugins.configs.key-model-restriction-c`）。
- 本地快速验证：用 Python `ctypes` 直接 `dlopen` 该 `.so`，调 `cliproxy_plugin_init` 再调
  `cliproxy_plugin_call`，比端到端调试快很多；记得先调 `plugin.register` 注入 `config_yaml`。

## 关键技术约束
- **插件 ABI 用 C**：Go 插件（`-buildmode=plugin`）与该服务有运行时冲突，会导致服务启动卡死。
- `sdk/pluginapi` 的结构体（`RequestInterceptRequest/Response`、`Metadata` 等）**没有 JSON tag**，
  必须按 Go 字段名发送：`Terminate` / `StatusCode` / `ResponseBody`；`[]byte` 字段用 base64。
  写成 `status_code` / `response_body` 会静默失效（403 是兜底默认值，body 永远为空）。
- `caller_scope` = `sha256("cli-proxy-api:caller-scope:v1\x00" + apiKey)` 的 hex；
  前缀含 null 字节，C 里算长度必须用 `sizeof` 而不是 `strlen`。
- 模型匹配：`Model`（解析后的上游名）与 `RequestedModel`（客户端请求名）可能不同，
  两者任一命中 `allowed_models` 即放行。
