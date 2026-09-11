# CODEBUDDY.md

This file provides guidance to CodeBuddy Code when working with code in this repository.

## 仓库定位

这是 CLIProxyAPI 的原生 C 插件仓库，不是 CLIProxyAPI 服务本体。插件通过 C ABI 被宿主动态加载，按 API Key 限制可使用的模型；HTTP server、路由、API Key provider 和宿主 plugin loader 位于仓库外。

核心实现集中在 `c/src/plugin.c`，配置解析、规则匹配、拦截响应、日志和 ABI 适配都在该文件中完成。不要使用 Go `-buildmode=plugin`：该插件必须使用 C ABI。

修改插件逻辑前，重点阅读：

- `c/src/plugin.c`：实际实现和 ABI 数据流。
- `README.md`：安装、配置、插件商店和用户可见行为。
- `dev.md`：历史踩坑、调试方法和验收矩阵。

## 目录与职责

```text
c/src/plugin.c                    # C ABI 插件实现
c/CMakeLists.txt                  # CMake 构建描述
build.sh                          # GCC 本地单平台构建
scripts/smoke-test.py             # ctypes ABI 冒烟测试
scripts/build-release.sh          # Zig 六平台交叉构建与打包
.github/workflows/release.yml     # CI 构建、布局校验、冒烟测试和发布
config.example.json               # 配置格式示例
registry.example.json             # 插件商店 registry 示例
README.md                         # 用户说明
dev.md                            # 开发避坑与验证记录
```

本仓库没有 Go 源码、`go.mod`、`Makefile`、Go 单元测试或独立 lint/格式化配置。不存在单独的服务启动入口；构建 `.so` 后由 CLIProxyAPI 宿主加载。

## 常用命令

以下命令默认在仓库根目录执行。

### 本地构建

推荐使用仓库脚本：

```bash
chmod +x build.sh
./build.sh
```

默认生成 `key-model-restriction-c.so`。也可以直接构建：

```bash
gcc -shared -fPIC -O2 \
  -o key-model-restriction-c.so \
  c/src/plugin.c
```

构建前的严格警告检查：

```bash
gcc -Wall -Wextra -Werror -shared -fPIC -O2 \
  -o key-model-restriction-c.so \
  c/src/plugin.c
```

这条严格 GCC 命令是当前最接近 lint 的检查；仓库没有单独的 `clang-format`、`clang-tidy` 或其他 lint 流程。

`c/CMakeLists.txt` 也支持标准 CMake 构建，但正式文档和 CI 主要使用 GCC/脚本：

```bash
cmake -S c -B build
cmake --build build
```

### 本地测试

构建后的动态库使用 Python `ctypes` 冒烟测试。它会模拟宿主的 `dlopen`、`cliproxy_plugin_init`、插件函数表和拦截调用：

```bash
python3 scripts/smoke-test.py key-model-restriction-c.so
```

也可以直接测试 Linux amd64 发布包：

```bash
python3 scripts/smoke-test.py \
  dist/key-model-restriction_1.4.1_linux_amd64.zip
```

冒烟脚本一次运行全部内置场景，包括注册、全局规则、Key 白名单、Key 例外、别名模型、缺少 `caller_scope`、host 日志和 API Key 脱敏。脚本没有按测试名或场景筛选的参数，因此不存在更小的“单个测试”命令；最小测试单位是把某个 `.so` 或发布 `.zip` 作为脚本输入。

### 发布构建

```bash
VERSION=1.4.1 ./scripts/build-release.sh
```

脚本需要 `zig`、`zip` 和 `sha256sum`，默认交叉编译：

```text
linux/amd64   linux/arm64
darwin/amd64   darwin/arm64
windows/amd64 windows/arm64
```

构建后校验发布包：

```bash
(cd dist && sha256sum -c checksums.txt)
```

发布资产命名为 `key-model-restriction_<version>_<goos>_<goarch>.zip` 和 `checksums.txt`。每个 zip 的根目录必须只包含一个动态库：Linux 为 `.so`、macOS 为 `.dylib`、Windows 为 `.dll`。

## 高层架构与调用链

宿主和插件的边界如下：

```text
CLIProxyAPI 宿主
  API Key provider 校验 Key，并生成 Metadata.caller_scope
        ↓ C ABI
cliproxy_plugin_init
        ↓ 回填 plugin_api 函数表
plugin.register / plugin.reconfigure
        ↓ config_yaml(base64) 解码并解析
  default / keys → 进程内规则
        ↓
request.intercept_before → 透传
request.intercept_after
  读取 Model、RequestedModel、Metadata.caller_scope
  查找 Key 规则并选择有效规则
  模型匹配 → 放行或 Terminate + 403
  host.log 记录每次决策
```

`c/src/plugin.c` 的 `plugin_call` 按方法分发注册/重配置、before 拦截和 after 拦截。配置重载走 `plugin.reconfigure`，解析后会清理并重建现有规则；不要把逻辑假设为必须重启服务。

## 配置与规则语义

插件配置包含两个顶层对象：`default` 是全局默认规则，`keys` 是按原始 API Key 配置的规则。宿主层的 `enabled`、`priority` 和 `plugins.configs.<id>` 不属于单独的 `default` 或 `keys` object 编辑器内容。

规则选择顺序：

1. Key 有 `unrestricted: true` 时，该 Key 全部放行。
2. Key 有自己的 `allowed_models` 或 `denied_models` 时，完整覆盖 `default`，不与全局列表合并。
3. Key 没有专属规则时，继承 `default`。
4. 没有有效限制时默认放行。
5. 同一规则同时命中时，`denied_models` 优先。
6. 兼容旧格式：`allowed_models: []` 表示显式无限制，不表示继承全局规则。

模型名匹配不区分大小写，并且只做精确字符串匹配，不按“GPT 系列”等业务分类推断。请求包含两个需要同时考虑的字段：

- `RequestedModel`：客户端原始请求模型名。
- `Model`：宿主解析或改写后的上游模型名。

允许列表中任一字段命中即可放行；拒绝列表中任一字段命中即拒绝，避免模型别名绕过限制。拒绝响应使用 HTTP 403，并返回包含 `key_model_restriction` code 的错误体。

## 插件 ID、文件名与配置键

插件配置键由实际动态库文件名或插件商店 ID 决定，二者不要混淆：

| 安装方式 | 动态库文件名 | `plugins.configs` 配置键 |
|---|---|---|
| 手工构建/复制 | `key-model-restriction-c.so` | `key-model-restriction-c` |
| 插件商店安装 | `key-model-restriction-v<version>.so` | `key-model-restriction` |

手工安装时，默认 `build.sh` 的输出名去掉 `.so` 后就是配置键；如果部署时重命名 `.so`，配置键也必须随之变化。商店安装使用 registry ID `key-model-restriction`。键写错时，插件可能仍然加载并注册，但收不到预期配置，从而静默不生效。

## C ABI 关键约束

### 导出入口与函数表

- 只导出 `cliproxy_plugin_init`，ABI 版本为 `1`。
- `call`、`free_buffer`、`shutdown` 由初始化函数写入 `cliproxy_plugin_api`，不需要作为额外动态符号导出。
- 不要用 `nm -D` 的导出符号数量判断插件完整性；使用 `scripts/smoke-test.py` 验证完整宿主调用路径。

### JSON 字段命名

注册 RPC 的外层字段遵循协议，例如 `config_yaml`；但拦截响应对应的宿主 Go 结构体没有 JSON tags，因此必须使用 Go 字段名：

```json
{"ok":true,"result":{"Terminate":true,"StatusCode":403,"ResponseBody":"<base64>"}}
```

不要把拦截响应写成 `terminate`、`status_code`、`response_body`。后者可能导致字段保持零值，宿主再用默认值处理，造成误判。

### base64

Go 的 `[]byte` 在 JSON 中使用 base64：

- 宿主发送的 `config_yaml` 必须先 base64 解码，再解析 YAML。
- 插件返回的 `ResponseBody` 必须 base64 编码。
- 其他 `[]byte` 字段也遵循同一规则。

### `caller_scope` 哈希

宿主使用以下输入计算 Key 的 scope：

```text
sha256("cli-proxy-api:caller-scope:v1\\x00" + apiKey)
```

C 实现的前缀包含中间 NUL 字节，长度必须这样计算：

```c
const char prefix[] = "cli-proxy-api:caller-scope:v1\x00";
size_t plen = sizeof(prefix) - 1;
```

不能使用 `strlen(prefix)`，否则会少算 NUL 字节，导致配置 Key 永远匹配不上。

## 验证与调试重点

- 优先使用 `scripts/smoke-test.py` 隔离验证 `.so`，再做宿主端到端验证。
- 不要只看 HTTP 状态码；同时核对拦截响应中的 `Terminate`、`StatusCode`、`ResponseBody`，以及 `host.log` 的 `decision`、`reason`、`rule_source`。
- HTTP 400 不能单独证明插件放行；它可能来自上游或宿主后续处理。
- 本地测试使用占位 API Key。不要把真实 API Key 写入脚本、命令历史、日志或截图。
- 日志只应包含掩码 Key（前 8 位和后 4 位）及 caller scope 前 12 位指纹，不应包含完整 API Key。
- 规则回归至少覆盖：默认规则允许/拒绝、Key 白名单覆盖、`unrestricted: true`、`allowed_models: []`、`Model`/`RequestedModel` 别名拒绝，以及重配置后旧规则不残留。

## CI 行为

`.github/workflows/release.yml` 在 `master` 推送、Pull Request、手动触发和 `v*` tag 上运行。CI 会：

1. 安装固定版本的 Zig 和 `zip`。
2. 执行 `scripts/build-release.sh` 构建六平台资产。
3. 校验六个 zip 的精确命名、根目录布局和 `checksums.txt`。
4. 对 Linux amd64 zip 运行 `scripts/smoke-test.py`。
5. 上传构建资产；`v*` tag 额外创建 GitHub Release。

修改发布脚本时，必须同时考虑插件商店对资产名称、扩展名、zip 根目录和 checksum 格式的严格约定。
