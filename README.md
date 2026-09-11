# Key Model Restriction Plugin

CLIProxyAPI plugin that restricts which models each API key can use.

## Behavior

- If an API key is **not** in the plugin config → no restriction (all models allowed).
- If an API key is in the config with `allowed_models: []` → no restriction.
- If an API key is in the config with `allowed_models: ["gpt-4o", ...]` → only those models are allowed.
- Requests to unauthorized models receive HTTP 403.

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

In your `config.yaml`, the plugin configuration uses the direct `keys` object:

```yaml
plugins:
  enabled: true
  dir: "plugins"
  configs:
    key-model-restriction-c:
      enabled: true
      priority: 100
      keys:
        sk-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:
          allowed_models:
            - gpt-4o
            - gpt-4o-mini
            - claude-3-5-sonnet
        sk-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb:
          allowed_models: []
```

The plugin declares `keys` as a CPA `ConfigField` of type `object`. Therefore the official
Management Center automatically shows an object/JSON editor for it:

1. Open the CPA Management Center and go to **Plugins**.
2. Find **key-model-restriction-c** (or the store-installed ID **key-model-restriction**).
3. Click its configuration button.
4. Edit the `keys` object as JSON and save.
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
| key absent from `keys` | no restriction |
| key present, `allowed_models: []` | no restriction |
| key present, `allowed_models: ["gpt-4o"]` | only `gpt-4o` allowed |

Matching is case-insensitive. A request is allowed if **either** the requested model name or the
resolved upstream model name is in the allow-list.

## Decision Logs

The plugin sends one decision record per `request.intercept_after` call through CPA's `host.log` callback.
The records are visible in the CPA service log, for example:

```text
journalctl --user -u cliproxyapi --no-pager | grep key-model-restriction
```

Each record includes:

- the visible message suffix `key_fingerprint=<12-character fingerprint>` (CPA's default formatter displays the message even when it omits custom fields)
- `decision`: `allow` or `block`
- `reason`: `model_allowed`, `model_not_allowed`, `key_not_configured`, or `missing_caller_scope`
- `model` and `requested_model`
- `allowed_models`
- `request_id` and `trace_id` when supplied by CPA
- `key_fingerprint`: the first 12 characters of the host-generated `caller_scope`

The plugin deliberately does **not** write the raw API Key to logs. This keeps the log useful for
correlation without turning `journalctl` into a credential store. If you need to correlate a
fingerprint with a configured key, calculate the same caller scope locally; do not put the raw key
in a log query or shell history.

## How It Works

1. The host's `config-api-key` provider validates the API key.
2. The host hashes the key into `caller_scope` and puts it in request metadata.
3. This plugin's `request.intercept_after` hook reads `caller_scope`, looks up the key's allowed models, and terminates the request with 403 if the model is not allowed.
4. The plugin sends the decision to CPA through `host.log`.

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
