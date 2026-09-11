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
- 本地构建：`gcc -shared -fPIC -O2 -o key-model-restriction-c.so c/src/plugin.c`
- 发布构建：`VERSION=1.0.0 ./scripts/build-release.sh`（zig cc 交叉编译 6 平台，产出商店 zip + checksums.txt）
- **ABI 只导出 `cliproxy_plugin_init`**：宿主 `loader_unix.go` 只 dlsym 这一个符号，
  `call`/`free_buffer`/`shutdown` 由 init 回填函数表。`nm -D` 只有 1 个符号是**正常的**，别去"补齐"。
- 插件 ID 由文件名决定（剥掉 `-v<version>`）：`key-model-restriction-c.so` → 配置键 `key-model-restriction-c`。
- 验证产物用 `scripts/smoke-test.py <zip|.so>`：完整复刻 dlopen → init → 函数表调用，
  比端到端调试快；注意请求字段是 **PascalCase**（`Model`/`RequestedModel`/`Metadata`），
  元数据里的 key 才是 `caller_scope`。
- 本机 binutils 解析不了 Mach-O / ARM64 PE，校验 darwin/windows 产物要自己解析二进制。

## 发布到插件商店
- 产物约定（`internal/pluginstore` 精确匹配，写错即 `release asset ... not found`）：
  `<id>_<version>_<goos>_<goarch>.zip`、`checksums.txt`（`<sha256>  <文件名>` 两列）、
  tag `v<version>`、zip **根目录**放 `<id>.<ext>` 且只能有一个动态库。
- 落地路径 `plugins/<goos>/<goarch>/<id>-v<version>.so`（不再平铺）；插件 ID = 配置键。
- CI：推 `v*` tag → `.github/workflows/release.yml` → 编译 / 断言布局 / 冒烟 / `gh release create`。
- 管理接口：`GET /v0/management/plugin-store`、`POST /v0/management/plugin-store/:id/install`、
  `DELETE /v0/management/plugins/:id`。
- 商店安装会写 `plugins.configs.<id>.store`（source-id/source-url/version/release-tag），
  这是更新检测与来源锁定的依据。
- 私有仓库需 `plugins.store-auth`（`type: github-token` + `token-env`，只写变量名不写 token）。

## 关键技术约束
- **插件 ABI 用 C**：Go 插件（`-buildmode=plugin`）与该服务有运行时冲突，会导致服务启动卡死。
- `sdk/pluginapi` 的结构体（`RequestInterceptRequest/Response`、`Metadata` 等）**没有 JSON tag**，
  必须按 Go 字段名发送：`Terminate` / `StatusCode` / `ResponseBody`；`[]byte` 字段用 base64。
  写成 `status_code` / `response_body` 会静默失效（403 是兜底默认值，body 永远为空）。
- `caller_scope` = `sha256("cli-proxy-api:caller-scope:v1\x00" + apiKey)` 的 hex；
  前缀含 null 字节，C 里算长度必须用 `sizeof` 而不是 `strlen`。
- 模型匹配：`Model`（解析后的上游名）与 `RequestedModel`（客户端请求名）可能不同，
  两者任一命中 `allowed_models` 即放行。
