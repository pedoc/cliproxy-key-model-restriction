# dev.md — 开发经验与避坑指南

本插件（`cliproxy-key-model-restriction`）开发过程中踩过的坑与流程教训。改动前请先扫一遍，
尤其是第 1、7、8 节。

---

## 0. 最容易踩的 5 个坑（速查）

| # | 坑 | 后果 | 正确做法 |
|---|---|---|---|
| 1 | `pluginapi` 结构体**没有 JSON tag**，却按 snake_case 发字段 | 静默失效：`StatusCode` 变 0（被兜底成 403，看起来"正常"）、`ResponseBody` 永远为空 | 拦截响应必须发 **Go 字段名**：`Terminate` / `StatusCode` / `ResponseBody` |
| 2 | 想用 Go 写插件（`-buildmode=plugin`） | 与该服务运行时冲突，**服务启动卡死**（进程在、端口不监听、无报错） | 用 **C** 写，导出 C ABI |
| 3 | SHA256 前缀含 `\x00`，用 `strlen` 取长度 | 哈希输入少一个字节 → `caller_scope` 永不匹配 | 用 `sizeof(prefix) - 1` |
| 4 | `config_yaml` / `[]byte` 字段当普通字符串处理 | 配置/响应体解析失败 | Go 的 `[]byte` 在 JSON 里是 **base64**，要解码/编码 |
| 5 | 只匹配 `Model`（解析后的上游名） | 用户把客户端模型名写进白名单会静默失效 | `Model` 与 `RequestedModel` **任一命中**即放行 |

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
| `config_yaml`（注册请求） | 宿主 → 插件 | 插件收到后要 **base64 解码** 再解析 JSON |
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

**当前实现：任一命中即放行**，且 403 信息里显示**客户端请求名**（用户更直观）。

---

## 6. 调试方法论（这次绕弯的根本原因）

### ✅ 应该这样做
1. **先隔离，再端到端。** 用 Python `ctypes` 直接 `dlopen` 插件 `.so`，喂假请求、看原始输出。
   这一步能在几秒内区分"插件问题"还是"宿主问题"，比改配置+重启+curl 快一个数量级。
2. **要证据，不要猜测。** 每个结论都要有可观测证据（原始 JSON、哈希对比、二进制 strings）。
3. **一次只动一个变量**，改完立刻验证。
4. **确认"成功"的来源。** 见第 1 节的兜底默认值陷阱。

### ❌ 不要这样做
- 在没定位根因前反复改代码 + 重启，指望"这次也许好了"
- 同时改多个地方，导致无法判断是哪个改动生效
- 只看 HTTP 状态码就下结论（要看完整 body + headers）

### 隔离测试脚本模板
```python
import ctypes, base64, json
class Buf(ctypes.Structure):
    _fields_ = [("ptr", ctypes.c_void_p), ("len", ctypes.c_size_t)]
lib = ctypes.CDLL("./key-model-restriction-c.so")
# ... 定义 HostAPI / PluginAPI 结构体后：
# lib.cliproxy_plugin_init(byref(host), byref(plugin))
# plugin.call(b"plugin.register", ...)     # 先注入 config_yaml(base64)
# plugin.call(b"request.intercept_after", ...)  # 再喂拦截请求，打印原始输出
```

---

## 7. 流程纪律

1. **绝不修改用户的 `config.yaml`。** 用户手动改；插件配置由 config watcher 触发
   `plugin.reconfigure` **热加载**，改完不需要重启服务（实测有效）。
2. **改代码前必须已有根因。** 没有根因就先加可观测性（日志/隔离测试），不要盲改。
3. **改动要小而准。** 定位到一行就改一行，别顺手重构。
4. **每次修复都要回归验证**：允许的模型、被拦截的模型、未配置的 key 三种场景都要过。
5. **交付要说清楚"证据"**：原始返回值、命令、结果，而不是"应该没问题"。

---

## 8. 常用命令速查

```bash
# 构建
gcc -shared -fPIC -O2 -o key-model-restriction-c.so c/src/plugin.c

# 部署
scp key-model-restriction-c.so dev01@192.168.110.201:/home/dev01/cliproxyapi/plugins/

# 重启 + 看日志（用户级 systemd）
systemctl --user restart cliproxyapi
journalctl --user -u cliproxyapi --no-pager -n 20

# 端到端测试（三种场景）
curl -s -w "\nHTTP %{http_code}\n" \
  -H "Authorization: Bearer <KEY>" -H 'Content-Type: application/json' \
  -d '{"model":"<MODEL>","messages":[{"role":"user","content":"hi"}]}' \
  http://192.168.110.201:8317/v1/chat/completions

# 反查某个 JSON 字段名在宿主二进制里有没有 tag
strings /home/dev01/cliproxyapi/cli-proxy-api | grep -o '<field>' | wc -l
```

---

## 9. 上线前 checklist

- [ ] `gcc` 编译无 warning
- [ ] 本地 ctypes 隔离测试通过（含边界：空 `allowed_models`、别名、不存在的 scoped key）
- [ ] 远程三种场景回归：允许 → 非 403；不允许 → 403 **且 body 有内容**；未配置 key → 放行
- [ ] 403 body 里含 `code` 字段，便于客户端识别
- [ ] 改动已 commit，并写清"根因 + 验证方式"
