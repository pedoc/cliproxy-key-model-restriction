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

Requires Go 1.26+ with CGO enabled (the plugin uses cgo to export the host ABI).

## Install

Copy the compiled `.so` file to your CLIProxyAPI plugins directory:

```bash
cp key_model_restriction.so /path/to/CLIProxyAPI/plugins/
```

## Configuration

In your `config.yaml`:

```yaml
api-keys:
  - sk-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
  - sk-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb

plugins:
  enabled: true
  dir: ./plugins
  configs:
    key-model-restriction:
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

## How It Works

1. The host's `config-api-key` provider validates the API key.
2. The host hashes the key into `caller_scope` and puts it in request metadata.
3. This plugin's `InterceptRequestAfterAuth` hook reads `caller_scope`, looks up the key's allowed models, and terminates the request with 403 if the model is not allowed.

## License

MIT
