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

typedef int (*cliproxy_plugin_call_fn)(char*, uint8_t*, size_t, cliproxy_buffer*);
typedef void (*cliproxy_plugin_free_fn)(void*, size_t);
typedef void (*cliproxy_plugin_shutdown_fn)(void);

typedef struct {
	uint32_t abi_version;
	cliproxy_plugin_call_fn call;
	cliproxy_plugin_free_fn free_buffer;
	cliproxy_plugin_shutdown_fn shutdown;
} cliproxy_plugin_api;

extern int cliproxyPluginCall(char*, uint8_t*, size_t, cliproxy_buffer*);
extern void cliproxyPluginFree(void*, size_t);
extern void cliproxyPluginShutdown(void);
*/
import "C"

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"unsafe"

	"github.com/router-for-me/CLIProxyAPI/v7/sdk/pluginabi"
	"github.com/router-for-me/CLIProxyAPI/v7/sdk/pluginapi"
)

// ============================================================================
// Configuration
// ============================================================================

// KeyModelConfig describes model restrictions for a single API key.
type KeyModelConfig struct {
	AllowedModels []string `json:"allowed_models"`
}

// PluginConfig is the top-level plugin configuration.
type PluginConfig struct {
	Keys map[string]KeyModelConfig `json:"keys"`
}

// ============================================================================
// Plugin state
// ============================================================================

var (
	config PluginConfig
)

// ============================================================================
// Exported plugin functions
// ============================================================================

//export cliproxy_plugin_init
func cliproxy_plugin_init(host *C.cliproxy_host_api, plugin *C.cliproxy_plugin_api) C.int {
	if plugin == nil {
		return 1
	}
	plugin.abi_version = C.uint32_t(pluginabi.ABIVersion)
	plugin.call = C.cliproxy_plugin_call_fn(C.cliproxyPluginCall)
	plugin.free_buffer = C.cliproxy_plugin_free_fn(C.cliproxyPluginFree)
	plugin.shutdown = C.cliproxy_plugin_shutdown_fn(C.cliproxyPluginShutdown)
	return 0
}

//export cliproxyPluginCall
func cliproxyPluginCall(method *C.char, request *C.uint8_t, requestLen C.size_t, response *C.cliproxy_buffer) C.int {
	if response != nil {
		response.ptr = nil
		response.len = 0
	}
	if method == nil {
		writeResponse(response, errorEnvelope("invalid_method", "method is required"))
		return 1
	}
	var requestBytes []byte
	if request != nil && requestLen > 0 {
		requestBytes = C.GoBytes(unsafe.Pointer(request), C.int(requestLen))
	}
	raw, errHandle := handleMethod(C.GoString(method), requestBytes)
	if errHandle != nil {
		writeResponse(response, errorEnvelope("plugin_error", errHandle.Error()))
		return 1
	}
	writeResponse(response, raw)
	return 0
}

//export cliproxyPluginFree
func cliproxyPluginFree(ptr unsafe.Pointer, len C.size_t) {
	if ptr != nil {
		C.free(ptr)
	}
}

//export cliproxyPluginShutdown
func cliproxyPluginShutdown() {}

// ============================================================================
// RPC handlers
// ============================================================================

func handleMethod(method string, request []byte) ([]byte, error) {
	switch method {
	case pluginabi.MethodPluginRegister, pluginabi.MethodPluginReconfigure:
		return handleRegister(request)
	case pluginabi.MethodRequestInterceptBefore:
		return handleInterceptBefore(request)
	case pluginabi.MethodRequestInterceptAfter:
		return handleInterceptAfter(request)
	default:
		return errorEnvelope("unknown_method", "unknown method: "+method), nil
	}
}

func handleRegister(request []byte) ([]byte, error) {
	// Parse config from request
	var lifecycleReq struct {
		ConfigYAML []byte `json:"config_yaml"`
	}
	if len(request) > 0 {
		if err := json.Unmarshal(request, &lifecycleReq); err != nil {
			return errorEnvelope("config_error", fmt.Sprintf("failed to parse request: %v", err)), nil
		}
		if len(lifecycleReq.ConfigYAML) > 0 {
			var cfg PluginConfig
			if err := json.Unmarshal(lifecycleReq.ConfigYAML, &cfg); err != nil {
				return errorEnvelope("config_error", fmt.Sprintf("failed to parse config_yaml JSON: %v (raw: %s)", err, string(lifecycleReq.ConfigYAML))), nil
			}
			config = cfg
		}
	}

	// Return registration with request_interceptor capability
	// Use a custom struct (like the official example) because pluginapi.Capabilities
	// contains interface types that don't marshal to JSON booleans correctly.
	type caps struct {
		RequestInterceptor bool `json:"request_interceptor"`
	}
	type regResponse struct {
		SchemaVersion uint32             `json:"schema_version"`
		Metadata      pluginapi.Metadata `json:"metadata"`
		Capabilities  caps               `json:"capabilities"`
	}
	return okEnvelope(regResponse{
		SchemaVersion: pluginabi.SchemaVersion,
		Metadata: pluginapi.Metadata{
			Name:             "key-model-restriction",
			Version:          "1.0.0",
			Author:           "cliproxy-plugin",
			GitHubRepository: "https://github.com/router-for-me/cliproxy-key-model-restriction",
		},
		Capabilities: caps{
			RequestInterceptor: true,
		},
	})
}

func handleInterceptBefore(request []byte) ([]byte, error) {
	// No-op: we only intercept after auth to know the caller scope
	return okEnvelope(pluginapi.RequestInterceptResponse{})
}

func handleInterceptAfter(request []byte) ([]byte, error) {
	var req pluginapi.RequestInterceptRequest
	if len(request) > 0 {
		if err := json.Unmarshal(request, &req); err != nil {
			return errorEnvelope("invalid_request", err.Error()), nil
		}
	}

	// Get caller_scope from metadata
	rawScope, _ := req.Metadata["caller_scope"].(string)
	if rawScope == "" {
		// No caller scope, pass through
		return okEnvelope(pluginapi.RequestInterceptResponse{})
	}

	// Build scope index and look up config
	index := buildScopeIndex(config)
	kc, found := index[rawScope]
	if !found {
		// Key not managed by this plugin, pass through
		return okEnvelope(pluginapi.RequestInterceptResponse{})
	}

	// Determine the model
	model := req.Model
	if model == "" {
		model = req.RequestedModel
	}

	// Check if model is allowed
	if !isModelAllowed(kc, model) {
		body := []byte(`{"error":{"type":"forbidden","message":"API key is not allowed to use model: ` + model + `"}}`)
		return okEnvelope(pluginapi.RequestInterceptResponse{
			Terminate:    true,
			StatusCode:   http.StatusForbidden,
			ResponseBody: body,
		})
	}

	return okEnvelope(pluginapi.RequestInterceptResponse{})
}

// ============================================================================
// Core logic
// ============================================================================

func hashKey(key string) string {
	sum := sha256.Sum256([]byte("cli-proxy-api:caller-scope:v1\x00" + key))
	return hex.EncodeToString(sum[:])
}

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

func isModelAllowed(kc KeyModelConfig, model string) bool {
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

func okEnvelope(v any) ([]byte, error) {
	raw, err := json.Marshal(v)
	if err != nil {
		return nil, err
	}
	env := map[string]any{"ok": true, "result": json.RawMessage(raw)}
	return json.Marshal(env)
}

func errorEnvelope(code, message string) []byte {
	raw, _ := json.Marshal(map[string]any{
		"ok":    false,
		"error": map[string]string{"code": code, "message": message},
	})
	return raw
}

func writeResponse(response *C.cliproxy_buffer, raw []byte) {
	if response == nil || len(raw) == 0 {
		return
	}
	ptr := C.CBytes(raw)
	if ptr == nil {
		return
	}
	response.ptr = ptr
	response.len = C.size_t(len(raw))
}

func main() {}
