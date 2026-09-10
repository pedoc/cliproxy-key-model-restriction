package main

/*
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    void* ptr;
    size_t len;
} cliproxy_buffer;

typedef int (*cliproxy_host_call_fn)(void*, const char*, const uint8_t*, size_t, cliproxy_buffer*);
typedef void (*cliproxy_host_free_fn)(void*, size_t);

typedef struct {
    uint32_t abi_version;
    void* host_ctx;
    cliproxy_host_call_fn call;
    cliproxy_host_free_fn free_buffer;
} cliproxy_host_api;

typedef int (*cliproxy_plugin_call_fn)(const char*, const uint8_t*, size_t, cliproxy_buffer*);
typedef void (*cliproxy_plugin_free_fn)(void*, size_t);
typedef void (*cliproxy_plugin_shutdown_fn)(void);

typedef struct {
    uint32_t abi_version;
    cliproxy_plugin_call_fn call;
    cliproxy_plugin_free_fn free_buffer;
    cliproxy_plugin_shutdown_fn shutdown;
} cliproxy_plugin_api;

extern int cliproxy_plugin_init(cliproxy_host_api* host, cliproxy_plugin_api* plugin);
extern int cliproxy_plugin_call(char* method, uint8_t* request, size_t request_len, cliproxy_buffer* response);
extern void cliproxy_plugin_free(void* ptr, size_t len);
extern void cliproxy_plugin_shutdown();

*/
import "C"

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"sync"
	"unsafe"
)

// ============================================================================
// Configuration
// ============================================================================

// KeyModelConfig describes model restrictions for a single API key.
type KeyModelConfig struct {
	// AllowedModels is a list of model names the key may use.
	// Empty or nil means no restriction (all models allowed).
	AllowedModels []string `json:"allowed_models"`
}

// PluginConfig is the top-level plugin configuration.
type PluginConfig struct {
	// Keys maps raw API keys to their model restrictions.
	Keys map[string]KeyModelConfig `json:"keys"`
}

// ============================================================================
// Plugin state
// ============================================================================

var (
	config   PluginConfig
	configMu sync.RWMutex
)

// ============================================================================
// Exported plugin functions (called by the host via C ABI)
// ============================================================================

//export cliproxy_plugin_init
func cliproxy_plugin_init(host *C.cliproxy_host_api, plugin *C.cliproxy_plugin_api) C.int {
	if host == nil || plugin == nil {
		return -1
	}

	// Fill in the plugin function table
	plugin.abi_version = C.uint32_t(1) // ABIVersion = 1
	plugin.call = C.cliproxy_plugin_call_fn(C.cliproxy_plugin_call)
	plugin.free_buffer = C.cliproxy_plugin_free_fn(C.cliproxy_plugin_free)
	plugin.shutdown = C.cliproxy_plugin_shutdown_fn(C.cliproxy_plugin_shutdown)

	return 0
}

//export cliproxy_plugin_call
func cliproxy_plugin_call(method *C.char, request *C.uint8_t, request_len C.size_t, response *C.cliproxy_buffer) C.int {
	if method == nil || response == nil {
		return -1
	}

	m := C.GoString(method)
	req := C.GoBytes(unsafe.Pointer(request), C.int(request_len))

	var result []byte
	switch m {
	case "plugin.register", "plugin.reconfigure":
		result = handleRegister(req)
	case "request.intercept_before":
		result = handleInterceptBeforeAuth(req)
	case "request.intercept_after":
		result = handleInterceptAfterAuth(req)
	default:
		result = marshalError("unknown_method", "unsupported method: "+m)
	}

	// Allocate response buffer using C malloc (host will free it via cliproxy_plugin_free)
	if len(result) > 0 {
		cstr := C.CString(string(result))
		response.ptr = unsafe.Pointer(cstr)
		response.len = C.size_t(len(result))
	}
	return 0
}

//export cliproxy_plugin_free
func cliproxy_plugin_free(ptr unsafe.Pointer, len C.size_t) {
	if ptr != nil {
		C.free(ptr)
	}
}

//export cliproxy_plugin_shutdown
func cliproxy_plugin_shutdown() {
	// Clean up resources
}

// ============================================================================
// RPC handlers
// ============================================================================

func handleRegister(req []byte) []byte {
	var regReq struct {
		ConfigYAML    []byte `json:"config_yaml"`
		SchemaVersion uint32 `json:"schema_version"`
	}
	if err := json.Unmarshal(req, &regReq); err != nil {
		return marshalError("invalid_request", err.Error())
	}

	// Parse config from YAML/JSON
	if len(regReq.ConfigYAML) > 0 {
		var cfg PluginConfig
		if err := json.Unmarshal(regReq.ConfigYAML, &cfg); err != nil {
			return marshalError("invalid_config", fmt.Sprintf("parse config: %v", err))
		}
		configMu.Lock()
		config = cfg
		configMu.Unlock()
	}

	resp := struct {
		SchemaVersion uint32         `json:"schema_version"`
		Metadata      map[string]any `json:"metadata"`
		Capabilities  map[string]any `json:"capabilities"`
	}{
		SchemaVersion: 6,
		Metadata: map[string]any{
			"name":               "key-model-restriction",
			"version":            "1.0.0",
			"author":             "cliproxy-plugin",
			"github_repository":  "https://github.com/router-for-me/cliproxy-key-model-restriction",
		},
		Capabilities: map[string]any{
			"request_interceptor": true,
		},
	}
	return marshalResult(resp)
}

func handleInterceptBeforeAuth(req []byte) []byte {
	// No-op: we only intercept after auth to know the caller scope
	return marshalResult(map[string]any{})
}

func handleInterceptAfterAuth(req []byte) []byte {
	var interceptReq struct {
		RequestID      string         `json:"request_id"`
		TraceID        string         `json:"trace_id"`
		SourceFormat   string         `json:"source_format"`
		ToFormat       string         `json:"to_format"`
		Model          string         `json:"model"`
		RequestedModel string         `json:"requested_model"`
		Stream         bool           `json:"stream"`
		Headers        http.Header    `json:"headers"`
		Body           []byte         `json:"body"`
		Metadata       map[string]any `json:"metadata"`
	}
	if err := json.Unmarshal(req, &interceptReq); err != nil {
		return marshalError("invalid_request", err.Error())
	}

	// Get caller_scope from metadata (set by the host from userApiKey)
	rawScope, _ := interceptReq.Metadata["caller_scope"].(string)
	if rawScope == "" {
		// No caller scope means no downstream API key auth; let it pass
		return marshalResult(map[string]any{})
	}

	// Build scope index and look up this key's config
	configMu.RLock()
	index := buildScopeIndex(config)
	kc, found := index[rawScope]
	configMu.RUnlock()

	if !found {
		// This key is not managed by this plugin; let it pass
		return marshalResult(map[string]any{})
	}

	// Determine the model being requested
	model := interceptReq.Model
	if model == "" {
		model = interceptReq.RequestedModel
	}

	// Check if the model is allowed
	if !isModelAllowed(kc, model) {
		body := fmt.Sprintf(
			`{"error":{"type":"forbidden","message":"API key is not allowed to use model: %s"}}`,
			model,
		)
		return marshalResult(map[string]any{
			"terminate":  true,
			"status_code": http.StatusForbidden,
			"response_body": []byte(body),
		})
	}

	// Allowed; let it pass
	return marshalResult(map[string]any{})
}

// ============================================================================
// Core logic
// ============================================================================

// hashKey produces the same hash as the host's CallerScope function.
func hashKey(key string) string {
	sum := sha256.Sum256([]byte("cli-proxy-api:caller-scope:v1\x00" + key))
	return hex.EncodeToString(sum[:])
}

// buildScopeIndex builds a map from hashed scope to key config.
func buildScopeIndex(cfg PluginConfig) map[string]KeyModelConfig {
	index := make(map[string]KeyModelConfig, len(cfg.Keys))
	for rawKey, kc := range cfg.Keys {
		scope := hashKey(strings.TrimSpace(rawKey))
		if scope == "" {
			continue
		}
		index[scope] = kc
	}
	return index
}

// isModelAllowed reports whether the given model is allowed for this key config.
func isModelAllowed(kc KeyModelConfig, model string) bool {
	// No allowed_models configured = no restriction
	if len(kc.AllowedModels) == 0 {
		return true
	}
	for _, allowed := range kc.AllowedModels {
		if strings.EqualFold(strings.TrimSpace(allowed), strings.TrimSpace(model)) {
			return true
		}
	}
	return false
}

// ============================================================================
// Helpers
// ============================================================================

func marshalResult(v any) []byte {
	env := map[string]any{"ok": true}
	raw, err := json.Marshal(v)
	if err != nil {
		return marshalError("marshal_error", err.Error())
	}
	env["result"] = json.RawMessage(raw)
	out, _ := json.Marshal(env)
	return out
}

func marshalError(code, message string) []byte {
	env := map[string]any{
		"ok": false,
		"error": map[string]any{
			"code":    code,
			"message": message,
		},
	}
	out, _ := json.Marshal(env)
	return out
}

// ============================================================================
// Main (required for plugin build mode, but never called)
// ============================================================================

func main() {
	// This is never executed in plugin mode.
	// The host loads the .so and calls cliproxy_plugin_init directly.
}
