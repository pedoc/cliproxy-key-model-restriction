# Key Model Restriction Plugin

CLIProxyAPI plugin that restricts which models each API key can use.

## Behavior

The plugin has one global default rule and optional per-key overrides:

1. A key with `unrestricted: true` is always allowed.
2. A key with its own `allowed_models` or `denied_models` uses that rule instead of the global default.
3. A key not listed under `keys` inherits `default`.
4. If both lists match, `denied_models` wins.
5. No `default` rule means unrestricted, preserving the old behavior.
6. Legacy `allowed_models: []` remains an explicit unrestricted rule.

Unauthorized models receive HTTP 403.

## Build

```bash
chmod +x build.sh
./build.sh
```

Or manually:
```bash
gcc -shared -fPIC -O2 -o key-model-restriction-c.so c/src/plugin.c
```

## Install

Copy the compiled `.so` file to your CLIProxyAPI plugins directory:

```bash
cp key-model-restriction-c.so /path/to/CLIProxyAPI/plugins/
```

## Install from the plugin store

CLIProxyAPI can install plugins from a registry index instead of copying files by
hand. This repository ships everything needed for that flow:

| File | Purpose |
|---|---|
| `scripts/build-release.sh` | cross-compiles every supported platform into the exact release layout the store expects |
| `.github/workflows/release.yml` | runs that script on a `v*` tag and publishes the GitHub release |
| `scripts/smoke-test.py` | loads a built library through the server's own `dlopen` path and exercises the interceptor |
| `registry.example.json` | a registry index you can host yourself |

Publish a version by pushing a tag:

```bash
git tag v1.0.0
git push origin v1.0.0
```

The workflow then publishes a release whose assets are:

```
key-model-restriction_1.0.0_linux_amd64.zip
key-model-restriction_1.0.0_linux_arm64.zip
key-model-restriction_1.0.0_darwin_amd64.zip
key-model-restriction_1.0.0_darwin_arm64.zip
key-model-restriction_1.0.0_windows_amd64.zip
key-model-restriction_1.0.0_windows_arm64.zip
checksums.txt
```

Each zip holds `key-model-restriction.<ext>` at its root, and `checksums.txt`
lists the SHA256 of every archive — both are required by the store.

Install it through the management API:

```bash
curl -X POST \
  "http://127.0.0.1:8317/v0/management/plugin-store/key-model-restriction/install" \
  -H "Authorization: Bearer <management-secret-key>"
```

> **The plugin id becomes the config key.** A store install writes
> `plugins/linux/amd64/key-model-restriction-v1.0.0.so` and enables
> `plugins.configs.key-model-restriction`, so `config_yaml` must live under the
> `key-model-restriction` key rather than `key-model-restriction-c`. A mismatched
> key means the plugin loads but silently applies no restriction.

Private repositories need credentials. Only the environment variable *name* goes
into the config:

```yaml
plugins:
  store-sources:
    - https://raw.githubusercontent.com/<you>/<your-store>/main/registry.json
  store-auth:
    - match: https://github.com/<you>/
      apply-to: [metadata, artifact]
      type: github-token
      token-env: GITHUB_PLUGIN_TOKEN
```

## Configuration

In your `config.yaml`, the plugin configuration uses a global `default` rule plus the direct `keys` object. Both are exposed as separate object editors in the CPA Management Center:

```yaml
plugins:
  enabled: true
  dir: "plugins"
  configs:
    key-model-restriction-c:
      enabled: true
      priority: 100
      default:
        denied_models:
          - hy3-201
          - gpt-4-turbo
      keys:
        sk-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:
          allowed_models:
            - gpt-4o
            - gpt-4o-mini
            - claude-3-5-sonnet
        sk-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb:
          unrestricted: true
```

The plugin declares both `default` and `keys` as CPA `ConfigField` values of type `object`. Therefore the official
Management Center automatically shows separate object/JSON editors for them. Use `default` for the
common rule and `keys` only for exceptions or explicit per-key policies:

1. Open the CPA Management Center and go to **Plugins**.
2. Find **key-model-restriction-c** (or the store-installed ID **key-model-restriction**).
3. Click its configuration button.
4. Edit the `default` object for the global rule and the `keys` object for per-key overrides, then save.
5. CPA writes the configuration and triggers `plugin.reconfigure`; no service restart is needed.

The UI editor uses JSON for object fields, so this is also valid in the `keys` field:

```json
{
  "sk-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa": {
    "allowed_models": ["gpt-4o", "gpt-4o-mini", "claude-3-5-sonnet"]
  },
  "sk-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb": {
    "allowed_models": []
  }
}
```

> **Note:** the key under `configs:` must match the plugin's **file name** (without the `.so`
> suffix), not the display name in the plugin metadata. Since `build.sh` produces
> `key-model-restriction-c.so`, the manual-install config key is `key-model-restriction-c`. If
> you rename the `.so` when deploying, rename the config key to match.
>
> A store installation uses the registry ID `key-model-restriction`, so its config key is
> `key-model-restriction` and its versioned file is `key-model-restriction-v1.2.0.so`.

| Config state | Effect |
|---|---|
| `default.denied_models: ["hy3-201"]`, key absent | `hy3-201` denied for this key |
| key has `allowed_models: ["gpt-4o"]` | only `gpt-4o` allowed; global rule ignored |
| key has `unrestricted: true` | all models allowed; global rule ignored |
| legacy key `allowed_models: []` | all models allowed; global rule ignored |
| no `default` and key absent | no restriction |

Matching is case-insensitive. A request is allowed if **either** the requested model name or the
resolved upstream model name passes the effective rule. For `denied_models`, a match in **either**
field denies the request, so aliases cannot bypass the deny list.

## Decision Logs

The plugin sends one decision record per `request.intercept_after` call through CPA's `host.log` callback.
The records are visible in the CPA service log, for example:

```text
journalctl --user -u cliproxyapi --no-pager | grep key-model-restriction
```

Each record includes:

- the visible message suffix `api_key_masked=sk-example...1234 key_fingerprint=<12-character fingerprint>` (CPA's default formatter displays the message even when it omits custom fields)
- `api_key_masked`: the configured API Key with only its first 8 and last 4 characters visible
- `decision`: `allow` or `block`
- `reason`: `model_allowed`, `model_not_allowed`, or `missing_caller_scope`
- `rule_source`: `default`, `key`, or `none`
- `model` and `requested_model`
- `allowed_models`
- `request_id` and `trace_id` when supplied by CPA
- `key_fingerprint`: the first 12 characters of the host-generated `caller_scope`

The plugin writes only a masked form of the configured API Key to logs (first 8 and last 4
characters). It does not write the complete API Key.

## How It Works

1. The host's `config-api-key` provider validates the API key.
2. The host hashes the key into `caller_scope` and puts it in request metadata.
3. This plugin's `request.intercept_after` hook reads `caller_scope`, resolves the per-key rule or global `default`, and terminates the request with 403 if the model is not allowed.
4. The plugin sends the decision and `rule_source` (`key`, `default`, or `none`) to CPA through `host.log`.

## Project Structure

```
cliproxy-key-model-restriction/
├── .github/workflows/release.yml   # tag -> cross-compile -> publish release
├── c/
│   ├── CMakeLists.txt
│   └── src/
│       └── plugin.c                # the plugin implementation
├── scripts/
│   ├── build-release.sh            # builds plugin store release assets
│   └── smoke-test.py               # loads a build and exercises the ABI
├── build.sh                        # local single-platform build
├── config.example.json
├── registry.example.json
├── dev.md                          # lessons learned / gotchas
└── README.md
```

## License

MIT
