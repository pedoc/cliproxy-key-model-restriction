# dev.md — 开发经验与避坑指南

本插件（`cliproxy-key-model-restriction`）开发过程中踩过的坑与流程教训。改动前请先扫一遍，
尤其是第 1、5、6、7、8、9、11、12 节。

---

## 0. 最容易踩的 8 个坑（速查）

| # | 坑 | 后果 | 正确做法 |
|---|---|---|---|
| 1 | `pluginapi` 结构体**没有 JSON tag**，却按 snake_case 发字段 | 静默失效：`StatusCode` 变 0（被兜底成 403，看起来"正常"）、`ResponseBody` 永远为空 | 拦截响应必须发 **Go 字段名**：`Terminate` / `StatusCode` / `ResponseBody` |
| 2 | 想用 Go 写插件（`-buildmode=plugin`） | 与该服务运行时冲突，**服务启动卡死**（进程在、端口不监听、无报错） | 用 **C** 写，导出 C ABI |
| 3 | SHA256 前缀含 `\x00`，用 `strlen` 取长度 | 哈希输入少一个字节 → `caller_scope` 永不匹配 | 用 `sizeof(prefix) - 1` |
| 4 | `config_yaml` / `[]byte` 字段当普通字符串处理 | 配置/响应体解析失败 | Go 的 `[]byte` 在 JSON 里是 **base64**，要解码/编码 |
| 5 | 只匹配 `Model`（解析后的上游名） | 用户把客户端模型名写进白名单会静默失效 | 允许列表：任一字段命中即可；拒绝列表：任一字段命中就拒绝 |
| 6 | 把全局规则和 Key 规则混合计算 | 例外 Key 仍被全局规则误伤，配置意图无法解释 | Key 有自己的规则时**完整覆盖** `default`；没有专属规则才继承 `default` |
| 7 | 把业务分类当成模型匹配规则 | `codex-auto-review` 虽被业务归类为 GPT，插件不会自动识别分类 | 插件只做**精确字符串匹配**；是否允许由 `default` 或 Key 白名单决定 |
| 8 | 只看 HTTP 状态码判断插件是否生效 | 上游 400、宿主兜底 403、插件 403 容易混淆 | 同时检查完整 body、插件 `host.log` 的 `decision/reason/rule_source` 和服务启动日志 |

---

## 1. 最重要：`pluginapi` 结构体没有 JSON tag

### 现象
插件明明返回了 `terminate: true` + `status_code: 403`，服务也返回 403，
但响应体是空的（`Content-Length: 0`），而且你把状态码改成 400 也还是 403。

### 根因
`sdk/pluginapi/types.go` 里的结构体**完全没有 tag**：

```go
type RequestInterceptResponse struct {
	Headers         http.Header
	Body            []byte
	ClearHeaders    []string
	Terminate       bool
	StatusCode      int
	ResponseHeaders http.Header
	ResponseBody    []byte
}
```

没有 tag 时，Go 的 `encoding/json` 只能按**字段名**做**大小写不敏感**匹配
（注意：不是下划线无关匹配）：

| 插件发送 | 匹配到的字段 | 结果 |
|---|---|---|
| `terminate` | `Terminate` | ✅ |
| `status_code` | — | ❌ 保持 0 |
| `response_body` | — | ❌ 保持空 |
| `response_headers` | — | ❌ 保持空 |
| `Terminate` | `Terminate` | ✅ |
| `StatusCode` | `StatusCode` | ✅ |
| `ResponseBody` | `ResponseBody` | ✅ |

### 最阴的地方
`StatusCode` 为 0 时，`normalizedTerminationStatus(0)` 会**兜底返回 403**：

```go
func normalizedTerminationStatus(statusCode int) int {
	if statusCode < http.StatusOK || statusCode > 599 {
		return http.StatusForbidden   // ← 0 落这里
	}
	return statusCode
}
```

所以你看到 403 就以为"成功了"，实际上：
- 状态码是**默认值**，不是你设的
- 响应体一直是空的

**教训：看到"符合预期"的结果时，要确认它来自你的输入，而不是系统的兜底默认值。**
验证方法：故意把状态码设成 400/401，如果返回的还是 403，说明字段根本没传进去。

### 正确写法
```c
"{\"ok\":true,\"result\":{\"Terminate\":true,\"StatusCode\":403,\"ResponseBody\":\"<base64>\"}}"
```

### 如何快速反查（超好用）
```bash
# 若二进制里搜不到该字符串，说明这个字段名没有 tag（不是 wire name）
strings <server-binary> | grep -o 'response_body\|response_headers' | sort | uniq -c
# 实测输出： response_headers 出现 1 次，response_body 出现 0 次 → ResponseBody 无 tag
```

### 对比：哪些地方**要**用 snake_case
`internal/pluginhost` 的内部 RPC 结构体**是有 tag 的**，所以下面这些用 snake_case 是对的：

| 结构体 | 字段 | 说明 |
|---|---|---|
| `rpcLifecycleRequest` | `config_yaml`, `schema_version` | 注册/重配请求 |
| `rpcCapabilities` | `request_interceptor` | 能力声明 |
| `rpc*Request` | `host_callback_id` | host 回调上下文 |

**一句话规则：**
- 发给宿主的 **拦截请求/响应**（`pluginapi.*`）→ 用 **Go 字段名**
- 插件**注册**相关的 RPC 信封（`pluginhost.rpc*`）→ 用 **snake_case**

---

## 2. 插件必须用 C 写，不要用 Go

### 现象
用 `go build -buildmode=plugin` 编出的 `.so`，服务启动后：
- 进程存在，但 `ss -tlnp | grep 8317` **端口不监听**
- `journalctl` 里只有版本号，没有 `API server started successfully`
- 没有明显报错，像"卡住"了

### 排查过的方向（全部无效，别重复走）
- ❌ 怀疑 Go 版本不匹配（试过 1.26.8 / 1.27.1 / 1.26.4，都不行）
- ❌ 怀疑字段名/大小写
- ✅ 真正原因：**Go plugin 的运行时与宿主冲突**

### 结论
- 官方 `examples/plugin/*/c/` 全部是 C，Go 示例仅用于特定能力；
  本插件的能力（RequestInterceptor）走 C ABI 最稳。
- 目录结构对齐官方：`c/src/plugin.c` + `c/CMakeLists.txt`，
  产物名即插件 ID（`key-model-restriction-c.so` → ID `key-model-restriction-c`）。

### 只导出 `cliproxy_plugin_init` 就够了（别去"补齐"其它符号）

`internal/pluginhost/loader_unix.go` 全程只做**一次** `dlsym`：

```
initSymbol := C.cliproxy_dlsym(handle, C.CString("cliproxy_plugin_init"))
```

`call` / `free_buffer` / `shutdown` **不走符号查找**，而是 init 通过函数表回填：

```
plugin->abi_version = ABI_VERSION;   // 必须等于 pluginabi.ABIVersion（当前 1）
plugin->call        = plugin_call;
plugin->free_buffer = plugin_free;
plugin->shutdown    = plugin_shutdown;
```

宿主只校验三件事：`rc == 0`、`abi_version == 1`、`call != nil && free_buffer != nil`。

**所以 `nm -D` 只看到 1 个导出符号是正确的，不是缺符号。**
（本次交付时我一度误判为"导出缺失"，实际是拿错了判据。）

> 校验产物别用"数导出符号个数"，用 `scripts/smoke-test.py`——
> 它完整复刻宿主流程：dlopen → dlsym init → 通过函数表调用 call，
> 并真实跑通"白名单放行 / 非白名单 403 / 未配置 key 放行"三种场景。

---

## 3. `caller_scope` 哈希：前缀含 `\x00`

宿主算法：

```go
sha256("cli-proxy-api:caller-scope:v1\x00" + apiKey) → hex
```

C 里**不能**用 `strlen` 量前缀长度，因为 `strlen` 会在 `\x00` 处停下，导致少一个字节：

```c
// ❌ 错：plen 会少 1，哈希全错
const char* prefix = "cli-proxy-api:caller-scope:v1\x00";
size_t plen = strlen(prefix);

// ✅ 对：sizeof 包含中间的 \x00（再减掉编译器补的结尾 \0）
const char prefix[] = "cli-proxy-api:caller-scope:v1\x00";
size_t plen = sizeof(prefix) - 1;   // 29
```

症状：插件能拿到 `caller_scope`，但和配置里算出来的永远对不上 → 所有请求都被放行。
排查技巧：把两边算出的哈希都打印出来对比，不要靠猜。

---

## 4. `[]byte` 字段在 JSON 里是 base64

Go 的 `encoding/json` 会把 `[]byte` 编成 base64 字符串，解码时也按 base64 还原。

| 字段 | 方向 | 处理 |
|---|---|---|
| `config_yaml`（注册请求外层字段） | 宿主 → 插件 | 插件收到后要 **base64 解码**，再解析 CPA UI 生成的新版 YAML（内容含 `keys`） |
| `ResponseBody`（拦截响应） | 插件 → 宿主 | 插件要 **base64 编码** |
| `Body` / `OriginalRequest` 等 | 双向 | 同上 |

判断依据：如果内容是 `eyJ...` 这种长相，就是 base64（`{"` 的 base64 开头就是 `eyJ`）。

---

## 5. 模型名匹配：`Model` vs `RequestedModel`

宿主在拦截请求里给两个模型字段：

| 字段 | 含义 | 例子 |
|---|---|---|
| `RequestedModel` | 客户端原始请求名 | `hy3-201` |
| `Model` | 解析/改写后的上游模型名 | `hy3` |

**只匹配 `Model` 的坑：** 用户把 `hy3-201`（客户端名）写进白名单，永远匹配不上，
但用户看到的是自己请求的 `hy3-201`，会以为配置无效，极难排查。

**当前实现：**允许列表对 `Model` / `RequestedModel` 任一命中即可放行；拒绝列表对任一字段命中即拒绝。403 信息里显示**客户端请求名**（用户更直观）。

---

## 6. 全局规则与 Key 专属规则

### 推荐配置模型

```yaml
keys:
  <key-a>:
    allowed_models:
      - gpt-5.6-luna
      - gpt-5.5
  <key-b>:
    unrestricted: true

default:
  denied_models:
    - hy3
    - hy3-201
```

CPA Management Center 的 `ConfigField` 是 `object`，UI 编辑框实际填写的是 JSON；插件收到后，宿主通过注册 RPC 的 `config_yaml` 字段以 base64 传递 YAML。因此要区分两层：

- UI 的 `default` 编辑框：填写 `{ "denied_models": ["hy3-201"] }`；
- UI 的 `keys` 编辑框：填写 `{ "<key>": { "unrestricted": true } }`；
- 只有填写整个插件对象时，才使用 `{ "default": {...}, "keys": {...} }`；
- 不要把宿主外层的 `plugins.configs`、`enabled`、`priority` 粘贴进单独的 `default` 或 `keys` 编辑框。

### 规则优先级

1. Key 有 `unrestricted: true` → 该 Key 全部放行；
2. Key 有自己的 `allowed_models` 或 `denied_models` → 完整覆盖 `default`，不与全局列表合并；
3. Key 未配置专属规则 → 继承 `default`；
4. `default` 缺失或没有有效限制 → 保持旧行为，默认放行；
5. 同一条规则的 `denied_models` 优先于 `allowed_models`；
6. 旧格式 `allowed_models: []` 继续表示该 Key 不限制，不能改解释为继承全局。

这个设计支持“全局禁止少量模型 + 单个 Key 全部放行”，也支持“某个 Key 只允许两个模型”。关键是不要把全局和 Key 列表混合计算，否则配置意图会变得不可读。

### 配置顺序和 UI 保存注意事项

`keys` 和 `default` 是两个顶层 section，解析时顺序不重要；用户配置中 `keys` 写在 `default` 前面也应得到相同结果。修改 UI 中的单个 object 时，要注意管理接口通常按顶层字段保存：

- 保存 `default` 时不要意外覆盖 `keys`；
- 保存 `keys` 时不要意外删除 `default`；
- 修改后用日志里的 `rule_source` 验证实际采用的规则；
- 若 UI 只提供整个插件对象编辑器，才一次性提交 `default` 和 `keys`。

### 为什么用户的配置里 `codex-auto-review` 会有两种结果

`codex-auto-review` 没有出现在全局 `default.denied_models`，所以未配置 Key 使用全局规则时会放行。可是如果某个 Key 自己配置了：

```yaml
allowed_models:
  - gpt-5.6-luna
  - gpt-5.5
```

那么它只允许这两个精确名称，`codex-auto-review` 即使按业务分类属于 GPT 系列，也会被这个 Key 的专属白名单拒绝。分类是人的约定，插件只做字符串精确匹配。

### 最终验收矩阵

以“全局禁用部分模型 + 一个 Key 完全放行 + 一个 Key 只允许两个模型”为例，验收至少覆盖：

| Key 类型 | 请求模型 | 预期结果 | 规则来源 |
|---|---|---:|---|
| 未配置 Key | 不在 `default.denied_models` | 放行 | `default` |
| 未配置 Key | 在 `default.denied_models` | 403 | `default` |
| Key 白名单 | 白名单中的模型 | 放行 | `key` |
| Key 白名单 | 不在白名单中的模型（包括 `codex-auto-review`） | 403 | `key` |
| `unrestricted: true` | 任意模型 | 放行 | `key` |

测试结果不能只看“插件是否收到请求”，还要核对 `host.log` 中的 `decision`、`reason`、`rule_source`，以及最终 HTTP body。

---

## 7. 调试方法论（这次绕弯的根本原因）

### ✅ 应该这样做
1. **先隔离，再端到端。** 用 Python `ctypes` 直接 `dlopen` 插件 `.so`，喂假请求、看原始输出。
   这一步能在几秒内区分"插件问题"还是"宿主问题"，比改配置+重启+curl 快一个数量级。
2. **要证据，不要猜测。** 每个结论都要有可观测证据（原始 JSON、哈希对比、二进制 strings）。
3. **一次只动一个变量**，改完立刻验证。
4. **确认"成功"的来源。** 见第 1 节的兜底默认值陷阱。
5. **先用 `/v1/models` 获取实时模型名。** 不要只扫描静态配置或凭模型前缀猜测；最终允许/拒绝仍以插件配置中的精确字符串为准。
6. **把别名作为独立测试维度。** 至少分别测试 `Model` 命中、`RequestedModel` 命中和两者不同的情况。

### ❌ 不要这样做
- 在没定位根因前反复改代码 + 重启，指望"这次也许好了"
- 同时改多个地方，导致无法判断是哪个改动生效
- 只看 HTTP 状态码就下结论（要看完整 body + headers）
- 把 HTTP 400 直接等同于“插件放行”：400 可能来自上游模型解析失败；必须结合插件日志确认是否出现 `blocked request`。
- 把“请求通过插件”误认为“请求最终成功”：插件放行后仍可能被上游拒绝，放行证据应看插件日志，最终结果另看 HTTP 响应。
- 把真实 API Key 写进测试脚本、命令历史或仓库：本地隔离测试必须使用占位 Key；线上测试只在远程进程内使用，并输出掩码值。

### 隔离测试脚本模板
```python
import ctypes, base64, json
class Buf(ctypes.Structure):
    _fields_ = [("ptr", ctypes.c_void_p), ("len", ctypes.c_size_t)]
lib = ctypes.CDLL("./key-model-restriction-c.so")
# ... 定义 HostAPI / PluginAPI 结构体后：
# lib.cliproxy_plugin_init(byref(host), byref(plugin))
# plugin.call(b"plugin.register", ...)     # 注入新版 keys YAML（外层字段 base64）
# plugin.call(b"request.intercept_after", ...)  # 再喂拦截请求，打印原始输出
```

### 配置回归必须覆盖的最小集合

对当前规则模型，至少用占位 Key 覆盖以下 8 类：

1. 未配置 Key + 普通允许模型 → 放行；
2. 未配置 Key + `default.denied_models` 中的模型 → 拒绝；
3. Key 专属 `allowed_models` 中的模型 → 放行；
4. Key 专属白名单之外的模型 → 拒绝；
5. Key 专属 `unrestricted: true` + 全局拒绝模型 → 放行；
6. 旧格式 `allowed_models: []` + 全局拒绝模型 → 放行；
7. `Model` 与 `RequestedModel` 分别命中拒绝列表 → 都拒绝；
8. 重配置后检查旧规则没有残留。

测试断言应同时检查：`Terminate`、`StatusCode`、`ResponseBody`、`host.log` 的 `decision` / `reason` / `rule_source`。

---

## 8. CPA 决策日志

C ABI 宿主提供 `host.log` 回调，插件通过 `cliproxy_host_api.call` 调用它：

```json
{
  "level": "warn",
  "message": "key-model-restriction blocked request",
  "fields": {
    "plugin": "key-model-restriction",
    "key_fingerprint": "<caller_scope 前 12 位>",
    "api_key_masked": "sk-example...1234",
    "rule_source": "default",
    "model": "hy3-201",
    "requested_model": "hy3-201",
    "decision": "block",
    "reason": "model_not_allowed",
    "allowed_models": "gpt-5.6-luna, gpt-5.5",
    "request_id": "...",
    "trace_id": "..."
  }
}
```

日志查看：

```bash
# 服务启动、插件加载和注册信息
journalctl --user -u cliproxyapi --no-pager -n 30

# 插件决策日志（CPA 默认写入文件）
grep 'key-model-restriction' /home/dev01/.cli-proxy-api/logs/main.log | tail -20
```

本次实测中，`journalctl` 主要显示服务 stdout 和启动信息，插件决策日志实际出现在
`/home/dev01/.cli-proxy-api/logs/main.log`。不要只查错日志位置。
CPA 默认日志格式会显示消息文本中的 `api_key_masked=... key_fingerprint=<12 位指纹>`；自定义 `fields` 可能不会全部展示。
当前版本记录配置 API Key 的前 8 位和后 4 位作为 `api_key_masked`，不会记录完整 API Key。
`rule_source` 为 `default`、`key` 或 `none`，用于确认本次请求采用了全局规则、Key 专属覆盖，还是缺少 caller scope。
全局规则中 `denied_models` 优先于 `allowed_models`；Key 一旦配置自己的规则，就不再和全局列表合并。

### 日志字段不是最终结果

一次请求至少有三层结果，必须分开记录：

1. **插件决策**：`host.log` 的 `decision`、`reason`、`rule_source`；
2. **宿主处理**：是否把插件返回的 `Terminate`、`StatusCode`、`ResponseBody` 正确合并到 HTTP 响应；
3. **上游结果**：请求放行后，上游是否接受模型、认证和请求参数。

因此：

- 插件日志 `blocked request` + HTTP 403 → 插件拦截成功；
- 插件日志 `allowed request` + HTTP 200 → 插件放行且上游成功；
- 插件日志 `allowed request` + HTTP 400/401/5xx → 插件放行，但上游或宿主后续失败；
- HTTP 400 本身不能证明插件放行，必须先看是否有对应的 `blocked request` 日志。

本次线上验收中，`hy3` 请求出现 HTTP 400，但插件已经记录 `blocked request`，所以它不是绕过限制，而是拦截后链路仍返回了不同状态码。遇到这种情况要用请求时间、trace/request ID 和日志内容关联，而不能只看状态码。

### API Key 测试安全

线上配置中的 Key 可以用于远程验收，但不要复制到：

- 测试脚本并提交到 Git；
- shell 历史或命令参数；
- 普通日志、错误信息或截图。

本地 `ctypes` 测试必须使用占位 Key；线上批量测试只输出掩码 Key（前 8 位 + 后 4 位），并在远程进程内完成请求。测试结束后删除临时脚本、动态库和缓存。

---

## 9. 流程纪律

1. **绝不修改用户的 `config.yaml`。** 用户手动改；插件配置由 config watcher 触发
   `plugin.reconfigure` **热加载**，改完不需要重启服务（实测有效）。
2. **改代码前必须已有根因。** 没有根因就先加可观测性（日志/隔离测试），不要盲改。
3. **改动要小而准。** 定位到一行就改一行，别顺手重构。
4. **每次修复都要回归验证**：允许的模型、被拦截的模型、未配置的 key 三种场景都要过。
5. **交付要说清楚"证据"**：原始返回值、命令、结果，而不是"应该没问题"。

---

## 10. 常用命令速查

```bash
# 构建
gcc -shared -fPIC -O2 -o key-model-restriction-c.so c/src/plugin.c

# 部署前先记录本地产物哈希，并在远程保留备份
sha256sum key-model-restriction-c.so
ssh dev01@192.168.110.201 'sha256sum /home/dev01/cliproxyapi/config.yaml'
ssh dev01@192.168.110.201 'mkdir -p /home/dev01/cliproxyapi/plugin-backups && cp -p /home/dev01/cliproxyapi/plugins/key-model-restriction-c.so /home/dev01/cliproxyapi/plugin-backups/key-model-restriction-c.so.<timestamp>'
scp key-model-restriction-c.so dev01@192.168.110.201:/tmp/key-model-restriction-c.so
ssh dev01@192.168.110.201 'install -m 0755 /tmp/key-model-restriction-c.so /home/dev01/cliproxyapi/plugins/key-model-restriction-c.so.new && mv -f /home/dev01/cliproxyapi/plugins/key-model-restriction-c.so.new /home/dev01/cliproxyapi/plugins/key-model-restriction-c.so && rm -f /tmp/key-model-restriction-c.so'

# 重启 + 看日志（用户级 systemd）
ssh dev01@192.168.110.201 'systemctl --user restart cliproxyapi'
ssh dev01@192.168.110.201 'journalctl --user -u cliproxyapi --no-pager -n 20'
ssh dev01@192.168.110.201 'sha256sum /home/dev01/cliproxyapi/config.yaml /home/dev01/cliproxyapi/plugins/key-model-restriction-c.so; ss -tlnp | grep 8317'

# 获取当前实际可用模型（线上验收优先于扫描静态配置）
curl -sS -H "Authorization: Bearer <KEY>" http://192.168.110.201:8317/v1/models

# 端到端测试（必须看完整 body 和状态码）
curl -sS -w "\nHTTP %{http_code}\n" \
  -H "Authorization: Bearer <KEY>" -H 'Content-Type: application/json' \
  -d '{"model":"<MODEL>","messages":[{"role":"user","content":"hi"}]}' \
  http://192.168.110.201:8317/v1/chat/completions

# 回滚：停止服务、恢复带时间戳的备份、再启动服务
ssh dev01@192.168.110.201 'systemctl --user stop cliproxyapi; cp -p /home/dev01/cliproxyapi/plugin-backups/key-model-restriction-c.so.<timestamp> /home/dev01/cliproxyapi/plugins/key-model-restriction-c.so; systemctl --user start cliproxyapi'

# 反查某个 JSON 字段名在宿主二进制里有没有 tag
strings /home/dev01/cliproxyapi/cli-proxy-api | grep -o '<field>' | wc -l
```

---

## 11. 发布到插件商店（远程安装，而不是手工拷 `.so`）

宿主支持 "registry 索引 + GitHub Releases" 的安装方式，入口在管理 API：

| 接口 | 作用 |
|---|---|
| `GET /v0/management/plugin-store` | 列出商店插件（含 `installed` / `update_available`） |
| `POST /v0/management/plugin-store/:id/install` | 安装（`?source=` 选源，`?version=` 选版本） |
| `DELETE /v0/management/plugins/:id` | 卸载（删文件 + 删配置） |

### 产物命名是硬约定（`internal/pluginstore`）

按**精确名称**找资产，写错直接 `release asset ... not found`：

- 压缩包：`<id>_<version>_<goos>_<goarch>.zip`
- 校验文件：`checksums.txt`，格式 `<sha256>  <文件名>`（两列）
- tag：`v<version>`，宿主会去掉前导 `v` 并要求结果合法
- **zip 根目录**放 `<id>.so`（或 `<id>-v<version>.so`），且只能有一个动态库
- 落地路径：`plugins/<goos>/<goarch>/<id>-v<version>.so`（不再是平铺）

Windows 上覆盖已加载的插件会被拒绝（`ErrLoadedPluginLocked` → 409 + `restart_required`）。

### 本仓库的做法

- `scripts/build-release.sh`：`zig cc` 单工具链交叉编译 6 个目标平台并打包
- `.github/workflows/release.yml`：推 `v*` tag → 编译 → 断言布局 → 冒烟测试 → `gh release create`
- `registry.example.json`：自建 registry 索引的样板
- 发布前先在本地运行 `scripts/smoke-test.py`，再校验 6 个 zip 和 `checksums.txt`；不要把“CI 已触发”当成“Release 已发布”。
- 发布后通过 `gh release view v<version>` 确认 6 个平台资产和 `checksums.txt` 都已上传，再让商店安装。
- 同一个版本不要重复复用 tag；发现产物或配置错误时递增 patch 版本（例如 `1.4.0` → `1.4.1`），避免远端 Release 和本地代码不一致。

**坑：本机 binutils 解析不了 Mach-O / ARM64 PE**（`file format not recognized`），
所以校验 darwin/windows 产物时要自己解析导出表（见 `smoke-test.py` 思路），
不能靠 `nm` / `objdump` 的输出来判断符号有无。

### ⚠️ 最容易翻车：插件 ID 就是配置键

| 安装方式 | 文件名 | 插件 ID / 配置键 |
|---|---|---|
| 手工拷贝 | `key-model-restriction-c.so` | `key-model-restriction-c` |
| 商店安装 | `key-model-restriction-v1.2.0.so` | `key-model-restriction` |

两条路径的配置键**不一样**。键写错 → 插件加载成功、注册成功，
但插件配置不会按预期下发 → **静默不拦截**，没有任何报错。

---

## 12. 上线前 checklist

- [ ] `gcc -Wall -Wextra -Werror` 编译无 warning
- [ ] 本地 `ctypes` 隔离测试通过（使用占位 Key，不使用线上真实 Key）
- [ ] 配置矩阵覆盖：普通允许、全局拒绝、Key 白名单覆盖、`unrestricted: true`、旧 `allowed_models: []`
- [ ] 别名矩阵覆盖：`Model` 命中拒绝、`RequestedModel` 命中拒绝、两者不同；拒绝列表任一命中即拒绝
- [ ] `codex-auto-review` 等特殊命名模型按精确配置验证，不按业务分类猜测
- [ ] 403 body 里含 `code` 字段，便于客户端识别
- [ ] 同时核对插件 `host.log` 的 `decision` / `reason` / `rule_source` 与最终 HTTP body；HTTP 400 不单独作为放行证据
- [ ] 远程更新前备份 `.so`，记录插件与 `config.yaml` 哈希；更新后确认插件版本、PID、8317 端口和配置哈希
- [ ] 发布版：6 个平台 zip 齐全、`checksums.txt` 行数 = zip 个数、zip 根目录只有 `<id>.<ext>`
- [ ] 若走商店安装：确认 `plugins.configs` 的键 = 插件 ID（不是 `.so` 旧文件名）
- [ ] 改动已 commit，并写清"根因 + 验证方式"
