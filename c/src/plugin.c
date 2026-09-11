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
#define PLUGIN_VERSION "1.2.1"

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

static const cliproxy_host_api* stored_host = NULL;

static void key_fingerprint(const char* caller_scope, char out[13]) {
    if (!out) return;
    if (!caller_scope || strlen(caller_scope) < 12) {
        strcpy(out, "unknown");
        return;
    }
    memcpy(out, caller_scope, 12);
    out[12] = '\0';
}

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
// Config parser (direct YAML from the CPA UI)
// ============================================================================

static char* yaml_scalar_copy(const char* value, size_t len) {
    while (len > 0 && ispace((unsigned char)value[0])) { value++; len--; }
    while (len > 0 && ispace((unsigned char)value[len - 1])) len--;
    if (len >= 2 && ((value[0] == '"' && value[len - 1] == '"') ||
                     (value[0] == '\'' && value[len - 1] == '\''))) {
        value++;
        len -= 2;
    }
    char* out = (char*)malloc(len + 1);
    if (!out) return NULL;
    size_t out_len = 0;
    for (size_t i = 0; i < len; i++) {
        if (value[i] == '\\' && i + 1 < len) {
            i++;
            if (value[i] == 'n') out[out_len++] = '\n';
            else if (value[i] == 'r') out[out_len++] = '\r';
            else if (value[i] == 't') out[out_len++] = '\t';
            else out[out_len++] = value[i];
        } else {
            out[out_len++] = value[i];
        }
    }
    out[out_len] = '\0';
    return out;
}

static KeyConfig* add_key_config(const char* raw_key) {
    if (!raw_key || !*raw_key || g_key_count >= MAX_KEYS) return NULL;
    KeyConfig* kc = &g_keys[g_key_count++];
    memset(kc, 0, sizeof(*kc));
    char* scope = hash_key(raw_key);
    if (!scope) {
        g_key_count--;
        return NULL;
    }
    strncpy(kc->scope, scope, 64);
    kc->scope[64] = '\0';
    free(scope);
    return kc;
}

static void add_model_config(KeyConfig* kc, const char* value, size_t len) {
    if (!kc || kc->model_count >= MAX_MODELS) return;
    char* model = yaml_scalar_copy(value, len);
    if (!model || !*model) {
        free(model);
        return;
    }
    kc->models[kc->model_count++] = model;
}

static char* extract_json_object(const char* json, const char* key) {
    if (!json || !key) return NULL;
    char* pattern = (char*)malloc(strlen(key) + 3);
    if (!pattern) return NULL;
    sprintf(pattern, "\"%s\"", key);
    const char* pos = strstr(json, pattern);
    free(pattern);
    if (!pos) return NULL;
    pos = strchr(pos, ':');
    if (!pos) return NULL;
    pos++;
    while (*pos && ispace((unsigned char)*pos)) pos++;
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
    if (!out) return NULL;
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static void parse_quoted_models(KeyConfig* kc, const char* array) {
    if (!kc || !array) return;
    const char* pos = array;
    while (*pos) {
        while (*pos && *pos != '"') pos++;
        if (!*pos) break;
        pos++;
        const char* start = pos;
        while (*pos && *pos != '"') {
            if (*pos == '\\' && pos[1]) pos++;
            pos++;
        }
        if (!*pos) break;
        add_model_config(kc, start, (size_t)(pos - start));
        pos++;
    }
}

static int yaml_line_indent(const char* line, const char* end) {
    int indent = 0;
    while (line < end) {
        if (*line == ' ') indent++;
        else if (*line == '\t') indent += 2;
        else break;
        line++;
    }
    return indent;
}

static int yaml_line_is_key(const char* content, size_t len, const char* wanted) {
    size_t wanted_len = strlen(wanted);
    if (len < wanted_len + 1 || strncmp(content, wanted, wanted_len) != 0 || content[wanted_len] != ':') return 0;
    return 1;
}

static char* yaml_mapping_key(const char* content, size_t len) {
    const char* colon = memchr(content, ':', len);
    if (!colon || colon == content) return NULL;
    const char* value = colon + 1;
    size_t value_len = len - (size_t)(value - content);
    while (value_len > 0 && ispace((unsigned char)value[0])) { value++; value_len--; }
    while (value_len > 0 && ispace((unsigned char)value[value_len - 1])) value_len--;
    if (value_len != 0) return NULL;
    return yaml_scalar_copy(content, (size_t)(colon - content));
}

static int parse_yaml_keys_config(const char* yaml) {
    const char* line = yaml;
    int keys_indent = -1;
    int key_indent = -1;
    int allowed_indent = -1;
    KeyConfig* current = NULL;
    int found = 0;

    while (line && *line) {
        const char* end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        const char* content = line;
        int indent = yaml_line_indent(line, end);
        content += indent;
        size_t len = (size_t)(end - content);
        while (len > 0 && (content[len - 1] == '\r' || ispace((unsigned char)content[len - 1]))) len--;

        if (len > 0 && content[0] != '#') {
            if (keys_indent < 0) {
                if (yaml_line_is_key(content, len, "keys")) {
                    keys_indent = indent;
                    found = 1;
                }
            } else if (indent <= keys_indent) {
                break;
            } else if (key_indent < 0 || indent == key_indent) {
                char* raw_key = yaml_mapping_key(content, len);
                if (raw_key) {
                    if (key_indent < 0) key_indent = indent;
                    current = add_key_config(raw_key);
                    free(raw_key);
                    allowed_indent = -1;
                }
            } else if (current && yaml_line_is_key(content, len, "allowed_models")) {
                const char* value = content + strlen("allowed_models") + 1;
                size_t value_len = len - (size_t)(value - content);
                while (value_len > 0 && ispace((unsigned char)value[0])) { value++; value_len--; }
                if (value_len > 0 && value[0] == '[') {
                    char* array = (char*)malloc(value_len + 1);
                    if (array) {
                        memcpy(array, value, value_len);
                        array[value_len] = '\0';
                        char* body = array + 1;
                        char* close = strrchr(body, ']');
                        if (close) *close = '\0';
                        parse_quoted_models(current, body);
                        free(array);
                    }
                    allowed_indent = -1;
                } else {
                    allowed_indent = indent;
                }
            } else if (current && allowed_indent >= 0 && indent > allowed_indent && content[0] == '-') {
                const char* value = content + 1;
                size_t value_len = len - 1;
                while (value_len > 0 && ispace((unsigned char)value[0])) { value++; value_len--; }
                add_model_config(current, value, value_len);
            }
        }
        if (!*end) break;
        line = end + 1;
    }
    return found;
}

static int parse_config(const char* text) {
    clear_config();
    if (!text) return 0;
    parse_yaml_keys_config(text);
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

static void call_host_log(const char* level, const char* message,
                          const char* caller_scope, const char* model,
                          const char* requested_model, const char* decision,
                          const char* reason, const char* allowed_models,
                          const char* request_id, const char* trace_id) {
    if (!stored_host || !stored_host->call || !level || !message) return;
    char key_id[13];
    key_fingerprint(caller_scope, key_id);
    char* level_esc = json_escape(level);
    char* message_esc = json_escape(message);
    char* key_id_esc = json_escape(key_id);
    char* model_esc = json_escape(model ? model : "");
    char* requested_esc = json_escape(requested_model ? requested_model : "");
    char* decision_esc = json_escape(decision ? decision : "");
    char* reason_esc = json_escape(reason ? reason : "");
    char* allowed_esc = json_escape(allowed_models ? allowed_models : "");
    char* request_id_esc = json_escape(request_id ? request_id : "");
    char* trace_id_esc = json_escape(trace_id ? trace_id : "");
    if (!level_esc || !message_esc || !key_id_esc || !model_esc ||
        !requested_esc || !decision_esc || !reason_esc || !allowed_esc ||
        !request_id_esc || !trace_id_esc) {
        free(level_esc); free(message_esc); free(key_id_esc);
        free(model_esc); free(requested_esc); free(decision_esc); free(reason_esc);
        free(allowed_esc); free(request_id_esc); free(trace_id_esc);
        return;
    }

    char* visible_message = format_string("%s key_fingerprint=%s", message, key_id);
    char* visible_message_esc = json_escape(visible_message ? visible_message : message);
    char* payload = format_string(
        "{\"level\":\"%s\",\"message\":\"%s\",\"fields\":{"
        "\"plugin\":\"key-model-restriction\","
        "\"key_fingerprint\":\"%s\",\"model\":\"%s\","
        "\"requested_model\":\"%s\",\"decision\":\"%s\","
        "\"reason\":\"%s\",\"allowed_models\":\"%s\","
        "\"request_id\":\"%s\",\"trace_id\":\"%s\"}}",
        level_esc, visible_message_esc ? visible_message_esc : message_esc,
        key_id_esc, model_esc, requested_esc, decision_esc, reason_esc,
        allowed_esc, request_id_esc, trace_id_esc);
    if (payload) {
        cliproxy_buffer response = {0};
        const uint8_t* request = (const uint8_t*)payload;
        size_t request_len = strlen(payload);
        int rc = stored_host->call(stored_host->host_ctx, "host.log", request, request_len, &response);
        if (response.ptr && stored_host->free_buffer) stored_host->free_buffer(response.ptr, response.len);
        (void)rc;
    }
    free(payload);
    free(visible_message); free(visible_message_esc);
    free(level_esc); free(message_esc); free(key_id_esc);
    free(model_esc); free(requested_esc); free(decision_esc); free(reason_esc);
    free(allowed_esc); free(request_id_esc); free(trace_id_esc);
}

static char* allowed_models_string(const KeyConfig* kc) {
    if (!kc || kc->model_count == 0) return strdup("");
    size_t capacity = 1;
    for (int i = 0; i < kc->model_count; i++) capacity += strlen(kc->models[i]) + 2;
    char* out = (char*)malloc(capacity);
    if (!out) return NULL;
    out[0] = '\0';
    for (int i = 0; i < kc->model_count; i++) {
        if (i > 0) strcat(out, ", ");
        strcat(out, kc->models[i]);
    }
    return out;
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
            "\"metadata\":{\"Name\":\"key-model-restriction\",\"Version\":\"" PLUGIN_VERSION "\","
            "\"Author\":\"pedoc\","
            "\"GitHubRepository\":\"https://github.com/pedoc/cliproxy-key-model-restriction\","
            "\"ConfigFields\":[{\"Name\":\"keys\",\"Type\":\"object\","
            "\"Description\":\"Map each API key to its allowed_models list. Empty or missing allowed_models means unrestricted.\"}]},"
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
        char* request_id = extract_json_string(json, "RequestID");
        char* trace_id = extract_json_string(json, "TraceID");
        char* model = extract_json_string(json, "Model");
        char* requested_model = extract_json_string(json, "RequestedModel");
        // Extract caller_scope from metadata
        char* metadata = extract_json_object(json, "Metadata");
        char* caller_scope = NULL;
        if (metadata) {
            caller_scope = extract_json_string(metadata, "caller_scope");
        }
        if (!caller_scope || strlen(caller_scope) == 0) {
            call_host_log("info", "key-model-restriction decision", NULL, model,
                          requested_model, "allow", "missing_caller_scope", "", request_id, trace_id);
            free(request_id); free(trace_id); free(model); free(requested_model);
            free(caller_scope); free(metadata); free(json);
            write_response(response, "{\"ok\":true,\"result\":{}}");
            return 0;
        }
        const KeyConfig* kc = find_key_config(caller_scope);
        if (!kc) {
            call_host_log("info", "key-model-restriction decision", caller_scope, model,
                          requested_model, "allow", "key_not_configured", "", request_id, trace_id);
            free(request_id); free(trace_id); free(model); free(requested_model);
            free(caller_scope); free(metadata); free(json);
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
        char* allowed_models = allowed_models_string(kc);
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
            call_host_log("warn", "key-model-restriction blocked request", caller_scope,
                          model, requested_model, "block", "model_not_allowed",
                          allowed_models, request_id, trace_id);
            write_response(response, result);
            free(model_esc); free(allowed_esc); free(body); free(body_b64); free(result);
            free(allowed_models); free(request_id); free(trace_id); free(model); free(requested_model);
            free(caller_scope); free(metadata); free(json);
            return 0;
        }
        call_host_log("info", "key-model-restriction allowed request", caller_scope,
                      model, requested_model, "allow", "model_allowed",
                      allowed_models, request_id, trace_id);
        free(allowed_models); free(request_id); free(trace_id); free(model); free(requested_model);
        free(caller_scope); free(metadata); free(json);
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
    stored_host = NULL;
}

// ============================================================================
// Exported functions
// ============================================================================

CLIPROXY_EXPORT int cliproxy_plugin_init(const cliproxy_host_api* host, cliproxy_plugin_api* plugin) {
    if (!plugin) return 1;
    stored_host = host;
    plugin->abi_version = ABI_VERSION;
    plugin->call = plugin_call;
    plugin->free_buffer = plugin_free;
    plugin->shutdown = plugin_shutdown;
    return 0;
}
