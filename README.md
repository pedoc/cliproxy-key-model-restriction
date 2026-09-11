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

## Configuration

In your `config.yaml`:

```yaml
plugins:
  enabled: true
  dir: "plugins"
  configs:
    key-model-restriction-c:
      enabled: true
      priority: 100
      config_yaml: |
        {
          "keys": {
            "sk-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa": {
              "allowed_models": ["gpt-4o", "gpt-4o-mini", "claude-3-5-sonnet"]
            },
            "sk-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb": {
              "allowed_models": []
            }
          }
        }
```

> **Note:** the key under `configs:` must match the plugin's **file name** (without the `.so`
> suffix), not the display name in the plugin metadata. Since `build.sh` produces
> `key-model-restriction-c.so`, the config key is `key-model-restriction-c`. If you rename the
> `.so` when deploying, rename the config key to match — otherwise your `config_yaml` is never
> delivered and the plugin silently applies no restriction.

| Config state | Effect |
|---|---|
| key absent from `keys` | no restriction |
| key present, `allowed_models: []` | no restriction |
| key present, `allowed_models: ["gpt-4o"]` | only `gpt-4o` allowed |

Matching is case-insensitive. A request is allowed if **either** the requested model name or the
resolved upstream model name is in the allow-list.

## How It Works

1. The host's `config-api-key` provider validates the API key.
2. The host hashes the key into `caller_scope` and puts it in request metadata.
3. This plugin's `request.intercept_after` hook reads `caller_scope`, looks up the key's allowed models, and terminates the request with 403 if the model is not allowed.

## Project Structure

```
cliproxy-key-model-restriction/
├── c/
│   ├── CMakeLists.txt
│   └── src/
│       └── plugin.c
├── build.sh
├── config.example.json
├── dev.md
└── README.md
```

## License

MIT
