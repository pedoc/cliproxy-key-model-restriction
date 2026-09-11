#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define CLIPROXY_EXPORT __declspec(dllexport)
#else
#define CLIPROXY_EXPORT __attribute__((visibility("default")))
#endif

#define ABI_VERSION 1

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

// ============================================================================
// SHA256 (minimal implementation)
// ============================================================================

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define RR(x,n) ((x)>>(n)|((x)<<(32-(n))))
#define CH(x,y,z) (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x) (RR(x,2)^RR(x,13)^RR(x,22))
#define EP1(x) (RR(x,6)^RR(x,11)^RR(x,25))
#define SIG0(x) (RR(x,7)^RR(x,18)^((x)>>3))
#define SIG1(x) (RR(x,17)^RR(x,19)^((x)>>10))

static void sha256_raw(const uint8_t* data, size_t len, uint8_t out[32]) {
    uint32_t h0=0x6a09e667, h1=0xbb67ae85, h2=0x3c6ef372, h3=0xa54ff53a;
    uint32_t h4=0x510e527f, h5=0x9b05688c, h6=0x1f83d9ab, h7=0x5be0cd19;
    size_t newLen = len + 1;
    while (newLen % 64 != 56) newLen++;
    uint8_t* msg = (uint8_t*)calloc(newLen + 8, 1);
    memcpy(msg, data, len);
    msg[len] = 0x80;
    uint64_t bitLen = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) msg[newLen + i] = (bitLen >> (56 - i * 8)) & 0xff;
    size_t chunks = (newLen + 8) / 64;
    for (size_t c = 0; c < chunks; c++) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)msg[c*64+i*4]<<24)|((uint32_t)msg[c*64+i*4+1]<<16)|
                   ((uint32_t)msg[c*64+i*4+2]<<8)|((uint32_t)msg[c*64+i*4+3]);
        }
        for (int i = 16; i < 64; i++)
            w[i] = SIG1(w[i-2])+w[i-7]+SIG0(w[i-15])+w[i-16];
        uint32_t a=h0,b=h1,c2=h2,d=h3,e=h4,f=h5,g=h6,h=h7;
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = h+EP1(e)+CH(e,f,g)+K[i]+w[i];
            uint32_t t2 = EP0(a)+MAJ(a,b,c2);
            h=g;g=f;f=e;e=d+t1;d=c2;c2=b;b=a;a=t1+t2;
        }
        h0+=a;h1+=b;h2+=c2;h3+=d;h4+=e;h5+=f;h6+=g;h7+=h;
    }
    free(msg);
    uint32_t hs[8] = {h0,h1,h2,h3,h4,h5,h6,h7};
    for (int i = 0; i < 8; i++) {
        out[i*4]=(hs[i]>>24)&0xff;out[i*4+1]=(hs[i]>>16)&0xff;
        out[i*4+2]=(hs[i]>>8)&0xff;out[i*4+3]=hs[i]&0xff;
    }
}

static char* hash_key(const char* key) {
    // Prefix includes a null byte - use sizeof to get the correct length
    // because strlen would stop at the null byte
    const char prefix[] = "cli-proxy-api:caller-scope:v1\x00";
    size_t plen = sizeof(prefix) - 1;  // 29 bytes (excluding trailing null terminator from sizeof)
    size_t klen = strlen(key);
    uint8_t hash[32];
    // Hash prefix + key as a single input (prefix includes the null byte)
    uint8_t* combined = (uint8_t*)malloc(plen + klen);
    memcpy(combined, prefix, plen);
    memcpy(combined + plen, key, klen);
    sha256_raw(combined, plen + klen, hash);
    free(combined);
    char* hex_out = (char*)malloc(65);
    for (int i = 0; i < 32; i++) sprintf(hex_out + i*2, "%02x", hash[i]);
    hex_out[64] = '\0';
    return hex_out;
}

// ============================================================================
// JSON helpers
// ============================================================================

static int ispace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static const char BASE64_TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char* base64_encode_str(const char* data, size_t len) {
    size_t out_len = ((len + 2) / 3) * 4;
    char* out = (char*)malloc(out_len + 1);
    if (!out) return NULL;
    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t octet_a = i < len ? (unsigned char)data[i++] : 0;
        uint32_t octet_b = i < len ? (unsigned char)data[i++] : 0;
        uint32_t octet_c = i < len ? (unsigned char)data[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        out[j++] = BASE64_TABLE[(triple >> 18) & 0x3F];
        out[j++] = BASE64_TABLE[(triple >> 12) & 0x3F];
        out[j++] = BASE64_TABLE[(triple >> 6) & 0x3F];
        out[j++] = BASE64_TABLE[triple & 0x3F];
    }
    if (len % 3 == 1) { out[out_len - 2] = '='; out[out_len - 1] = '='; }
    else if (len % 3 == 2) { out[out_len - 1] = '='; }
    out[out_len] = '\0';
    return out;
}

static char* json_escape(const char* value) {
    if (value == NULL) return strdup("");
    size_t len = strlen(value);
    char* out = (char*)malloc(len * 6 + 1);
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c == '"') { out[pos++] = '\\'; out[pos++] = '"'; }
        else if (c == '\\') { out[pos++] = '\\'; out[pos++] = '\\'; }
        else if (c == '\n') { out[pos++] = '\\'; out[pos++] = 'n'; }
        else if (c == '\r') { out[pos++] = '\\'; out[pos++] = 'r'; }
        else if (c == '\t') { out[pos++] = '\\'; out[pos++] = 't'; }
        else if (c < 0x20) { pos += sprintf(out + pos, "\\u%04x", c); }
        else { out[pos++] = (char)c; }
    }
    out[pos] = '\0';
    return out;
}

static int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static char* base64_decode(const char* input, size_t* out_len) {
    size_t len = input == NULL ? 0 : strlen(input);
    char* out = (char*)malloc(((len * 3) / 4) + 4);
    if (out == NULL) return NULL;
    int value = 0, bits = -8;
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        if (input[i] == '=') break;
        int digit = base64_value(input[i]);
        if (digit < 0) continue;
        value = (value << 6) | digit;
        bits += 6;
        if (bits >= 0) {
            out[pos++] = (char)((value >> bits) & 0xFF);
            bits -= 8;
        }
    }
    out[pos] = '\0';
    *out_len = pos;
    return out;
}

static char* extract_json_string(const char* json, const char* key) {
    if (json == NULL || key == NULL) return NULL;
    char* pattern = (char*)malloc(strlen(key) + 3);
    sprintf(pattern, "\"%s\"", key);
    const char* pos = json;
    size_t plen = strlen(pattern);
    while ((pos = strstr(pos, pattern)) != NULL) {
        const char* p = pos + plen;
        while (*p && ispace((unsigned char)*p)) p++;
        if (*p++ != ':') { pos += plen; continue; }
        while (*p && ispace((unsigned char)*p)) p++;
        if (*p != '"') { pos += plen; continue; }
        p++;
        char* out = (char*)malloc(strlen(p) + 1);
        size_t opos = 0;
        while (*p) {
            if (*p == '"') { out[opos] = '\0'; free(pattern); return out; }
            if (*p == '\\' && p[1]) {
                p++;
                if (*p == 'n') out[opos++] = '\n';
                else if (*p == 'r') out[opos++] = '\r';
                else if (*p == 't') out[opos++] = '\t';
                else out[opos++] = *p;
            } else out[opos++] = *p;
            p++;
        }
        free(out);
        pos += plen;
    }
    free(pattern);
    return NULL;
}

// ============================================================================
// Config storage
// ============================================================================

#define MAX_KEYS 64
#define MAX_MODELS 64

typedef struct {
    char scope[65];  // hashed key
    char* models[MAX_MODELS];
    int model_count;
} KeyConfig;

static KeyConfig g_keys[MAX_KEYS];
static int g_key_count = 0;

static void clear_config(void) {
    for (int i = 0; i < g_key_count; i++) {
        for (int j = 0; j < g_keys[i].model_count; j++)
            free(g_keys[i].models[j]);
    }
    g_key_count = 0;
}

static const KeyConfig* find_key_config(const char* scope) {
    for (int i = 0; i < g_key_count; i++) {
        if (strcmp(g_keys[i].scope, scope) == 0) return &g_keys[i];
    }
    return NULL;
}

static int model_allowed(const KeyConfig* kc, const char* model) {
    if (kc->model_count == 0) return 1;  // empty = no restriction
    for (int i = 0; i < kc->model_count; i++) {
        if (strcasecmp(kc->models[i], model) == 0) return 1;
    }
    return 0;
}

// ============================================================================
// Config parser (simple JSON)
// ============================================================================

static char* extract_json_object(const char* json, const char* key) {
    if (!json || !key) return NULL;
    char* pattern = (char*)malloc(strlen(key) + 3);
    sprintf(pattern, "\"%s\"", key);
    const char* pos = strstr(json, pattern);
    free(pattern);
    if (!pos) return NULL;
    pos = strchr(pos, ':');
    if (!pos) return NULL;
    pos++;
    while (*pos && ispace(*pos)) pos++;
    if (*pos != '{') return NULL;
    int depth = 1;
    const char* start = pos + 1;
    pos++;
    while (*pos && depth > 0) {
        if (*pos == '{') depth++;
        else if (*pos == '}') depth--;
        if (depth > 0) pos++;
    }
    if (depth != 0) return NULL;
    size_t len = (size_t)(pos - start);
    char* out = (char*)malloc(len + 1);
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static char* extract_json_array(const char* json, const char* key) {
    if (!json || !key) return NULL;
    char* pattern = (char*)malloc(strlen(key) + 3);
    sprintf(pattern, "\"%s\"", key);
    const char* pos = strstr(json, pattern);
    free(pattern);
    if (!pos) return NULL;
    pos = strchr(pos, ':');
    if (!pos) return NULL;
    pos++;
    while (*pos && ispace(*pos)) pos++;
    if (*pos != '[') return NULL;
    int depth = 1;
    const char* start = pos + 1;
    pos++;
    while (*pos && depth > 0) {
        if (*pos == '[') depth++;
        else if (*pos == ']') depth--;
        if (depth > 0) pos++;
    }
    if (depth != 0) return NULL;
    size_t len = (size_t)(pos - start);
    char* out = (char*)malloc(len + 1);
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static int parse_config(const char* json) {
    clear_config();
    if (!json) return 0;
    char* keys_obj = extract_json_object(json, "keys");
    if (!keys_obj) { clear_config(); return 0; }
    // Parse each key in the keys object
    const char* pos = keys_obj;
    while (*pos) {
        while (*pos && *pos != '"') pos++;
        if (!*pos) break;
        pos++;
        const char* key_start = pos;
        while (*pos && *pos != '"') pos++;
        if (!*pos) break;
        size_t key_len = (size_t)(pos - key_start);
        char* raw_key = (char*)malloc(key_len + 1);
        memcpy(raw_key, key_start, key_len);
        raw_key[key_len] = '\0';
        pos++; // skip closing quote
        while (*pos && ispace(*pos)) pos++;
        if (*pos == ':') pos++;
        while (*pos && ispace(*pos)) pos++;
        if (*pos == '{') {
            int depth = 1;
            const char* obj_start = pos + 1;
            pos++;
            while (*pos && depth > 0) {
                if (*pos == '{') depth++;
                else if (*pos == '}') depth--;
                if (depth > 0) pos++;
            }
            size_t obj_len = (size_t)(pos - obj_start);
            char* obj = (char*)malloc(obj_len + 1);
            memcpy(obj, obj_start, obj_len);
            obj[obj_len] = '\0';
            // Hash the key
            char* scope = hash_key(raw_key);
            // Parse allowed_models from obj
            char* arr = extract_json_array(obj, "allowed_models");
            if (g_key_count < MAX_KEYS) {
                KeyConfig* kc = &g_keys[g_key_count++];
                strncpy(kc->scope, scope, 64);
                kc->scope[64] = '\0';
                kc->model_count = 0;
                if (arr) {
                    const char* ap = arr;
                    while (*ap) {
                        while (*ap && *ap != '"') ap++;
                        if (!*ap) break;
                        ap++;
                        const char* val_start = ap;
                        while (*ap && *ap != '"') ap++;
                        if (!*ap) break;
                        size_t vlen = (size_t)(ap - val_start);
                        if (kc->model_count < MAX_MODELS) {
                            kc->models[kc->model_count] = (char*)malloc(vlen + 1);
                            memcpy(kc->models[kc->model_count], val_start, vlen);
                            kc->models[kc->model_count][vlen] = '\0';
                            kc->model_count++;
                        }
                        ap++;
                    }
                }
                free(arr);
            }
            free(scope);
            free(obj);
        }
        free(raw_key);
        pos++;
    }
    free(keys_obj);
    return 0;
}

// ============================================================================
// Response helpers
// ============================================================================

static char* format_string(const char* fmt, ...) {
    va_list args, copy;
    va_start(args, fmt);
    va_copy(copy, args);
    int len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (len < 0) { va_end(copy); return NULL; }
    char* out = (char*)malloc((size_t)len + 1);
    vsnprintf(out, (size_t)len + 1, fmt, copy);
    va_end(copy);
    return out;
}

static void write_response(cliproxy_buffer* resp, const char* text) {
    if (!resp || !text) return;
    size_t len = strlen(text);
    void* ptr = malloc(len);
    if (!ptr) { resp->ptr = NULL; resp->len = 0; return; }
    memcpy(ptr, text, len);
    resp->ptr = ptr;
    resp->len = len;
}

// ============================================================================
// Plugin call handler
// ============================================================================

static int plugin_call(const char* method, const uint8_t* request, size_t request_len, cliproxy_buffer* response) {
    if (response) { response->ptr = NULL; response->len = 0; }
    if (!method) {
        write_response(response, "{\"ok\":false,\"error\":{\"code\":\"invalid_method\",\"message\":\"method is required\"}}");
        return 1;
    }
    if (strcmp(method, "plugin.register") == 0 || strcmp(method, "plugin.reconfigure") == 0) {
        // Parse config from request
        char* json = NULL;
        if (request && request_len > 0) {
            json = (char*)malloc(request_len + 1);
            memcpy(json, request, request_len);
            json[request_len] = '\0';
        }
        if (json) {
            // Extract config_yaml from the request envelope (base64-encoded by host)
            char* config_yaml_b64 = extract_json_string(json, "config_yaml");
            if (config_yaml_b64) {
                size_t decoded_len = 0;
                char* config_yaml = base64_decode(config_yaml_b64, &decoded_len);
                if (config_yaml) {
                    parse_config(config_yaml);
                    free(config_yaml);
                }
                free(config_yaml_b64);
            }
            free(json);
        }
        // Return registration
        write_response(response,
            "{\"ok\":true,\"result\":{\"schema_version\":6,"
            "\"metadata\":{\"Name\":\"key-model-restriction\",\"Version\":\"1.0.0\","
            "\"Author\":\"cliproxy-plugin\","
            "\"GitHubRepository\":\"https://github.com/router-for-me/cliproxy-key-model-restriction\"},"
            "\"capabilities\":{\"request_interceptor\":true}}}");
        return 0;
    }
    if (strcmp(method, "request.intercept_before") == 0) {
        // No-op: pass through
        write_response(response, "{\"ok\":true,\"result\":{}}");
        return 0;
    }
    if (strcmp(method, "request.intercept_after") == 0) {
        // Parse the request to get model and caller_scope
        char* json = NULL;
        if (request && request_len > 0) {
            json = (char*)malloc(request_len + 1);
            memcpy(json, request, request_len);
            json[request_len] = '\0';
        }
        if (!json) {
            write_response(response, "{\"ok\":true,\"result\":{}}");
            return 0;
        }
        // Extract models (JSON fields are PascalCase: "Model" = resolved upstream model,
        // "RequestedModel" = the model the client asked for before alias/pool rewriting).
        char* model = extract_json_string(json, "Model");
        char* requested_model = extract_json_string(json, "RequestedModel");
        // Extract caller_scope from metadata
        char* metadata = extract_json_object(json, "Metadata");
        char* caller_scope = NULL;
        if (metadata) {
            caller_scope = extract_json_string(metadata, "caller_scope");
        }
        if (!caller_scope || strlen(caller_scope) == 0) {
            free(model); free(requested_model); free(caller_scope); free(metadata); free(json);
            write_response(response, "{\"ok\":true,\"result\":{}}");
            return 0;
        }
        const KeyConfig* kc = find_key_config(caller_scope);
        if (!kc) {
            free(model); free(requested_model); free(caller_scope); free(metadata); free(json);
            write_response(response, "{\"ok\":true,\"result\":{}}");
            return 0;
        }
        // Allow when either the requested or the resolved model matches the allow-list.
        int allowed = 0;
        if (model && strlen(model) > 0 && model_allowed(kc, model)) {
            allowed = 1;
        }
        if (!allowed && requested_model && strlen(requested_model) > 0 && model_allowed(kc, requested_model)) {
            allowed = 1;
        }
        if (!allowed) {
            // Report the model the client asked for when available.
            const char* display_model = (requested_model && strlen(requested_model) > 0) ? requested_model : model;
            // Build the allowed-model list for the error message.
            char allowed_list[512] = "";
            for (int i = 0; i < kc->model_count; i++) {
                if (i > 0) {
                    strncat(allowed_list, ", ", sizeof(allowed_list) - strlen(allowed_list) - 1);
                }
                strncat(allowed_list, kc->models[i], sizeof(allowed_list) - strlen(allowed_list) - 1);
            }
            char* model_esc = json_escape(display_model ? display_model : "");
            char* allowed_esc = json_escape(allowed_list);
            char* body = format_string(
                "{\"error\":{\"type\":\"model_restriction\",\"code\":\"key_model_restriction\","
                "\"message\":\"Blocked by key-model-restriction plugin: API key is not allowed to use model '%s'. Allowed models: [%s]\"}}",
                model_esc ? model_esc : "",
                allowed_esc ? allowed_esc : "");
            // Response fields must use Go field names (Status_code/response_body are NOT matched
            // because pluginapi.RequestInterceptResponse has no JSON tags). []byte fields are base64.
            char* body_b64 = base64_encode_str(body, strlen(body));
            char* result = format_string(
                "{\"ok\":true,\"result\":{\"Terminate\":true,\"StatusCode\":403,\"ResponseBody\":\"%s\"}}",
                body_b64 ? body_b64 : "");
            write_response(response, result);
            free(model_esc); free(allowed_esc); free(body); free(body_b64); free(result);
            free(model); free(requested_model); free(caller_scope); free(metadata); free(json);
            return 0;
        }
        free(model); free(requested_model); free(caller_scope); free(metadata); free(json);
        write_response(response, "{\"ok\":true,\"result\":{}}");
        return 0;
    }
    write_response(response, "{\"ok\":false,\"error\":{\"code\":\"unknown_method\",\"message\":\"unknown method\"}}");
    return 0;
}

static void plugin_free(void* ptr, size_t len) {
    (void)len;
    free(ptr);
}

static void plugin_shutdown(void) {
    clear_config();
}

// ============================================================================
// Exported functions
// ============================================================================

CLIPROXY_EXPORT int cliproxy_plugin_init(const cliproxy_host_api* host, cliproxy_plugin_api* plugin) {
    if (!plugin) return 1;
    (void)host;
    plugin->abi_version = ABI_VERSION;
    plugin->call = plugin_call;
    plugin->free_buffer = plugin_free;
    plugin->shutdown = plugin_shutdown;
    return 0;
}
