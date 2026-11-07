#define WOLFSSL_USER_SETTINGS
#include "ssh_server.h"

#include <cstdlib>
#include <cstring>

#include "cli.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"

#include <wolfssh/ssh.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/random.h>

static const char *TAG = "ssh";
#define NVS_NS "sshcfg"
#define SSH_TASK_STACK (18 * 1024)

/* ---------------- config / state ---------------- */

static bool s_enabled;
static uint16_t s_port = 22;
static char s_user[SSH_USER_MAX] = "root";
static char s_pass[SSH_PASS_MAX] = "esp32";
static uint8_t *s_hostkey;
static uint32_t s_hostkey_sz;

static volatile bool s_running, s_stop, s_client;
static TaskHandle_t s_task;
static StackType_t *s_stack; /* PSRAM stack for the SSH task */

/* ---------------- NVS ---------------- */

static void cfg_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "en", s_enabled ? 1 : 0);
    nvs_set_u16(h, "port", s_port);
    nvs_set_str(h, "user", s_user);
    nvs_set_str(h, "pass", s_pass);
    nvs_commit(h);
    nvs_close(h);
}

static void cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t en = 0;
    if (nvs_get_u8(h, "en", &en) == ESP_OK) s_enabled = en;
    nvs_get_u16(h, "port", &s_port);
    size_t n = sizeof(s_user);
    nvs_get_str(h, "user", s_user, &n);
    n = sizeof(s_pass);
    nvs_get_str(h, "pass", s_pass, &n);
    nvs_close(h);
}

static bool hostkey_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = 0;
    bool ok = false;
    if (nvs_get_blob(h, "hk", nullptr, &sz) == ESP_OK && sz > 0 && sz < 1024) {
        uint8_t *buf = (uint8_t *)malloc(sz);
        if (buf && nvs_get_blob(h, "hk", buf, &sz) == ESP_OK) {
            free(s_hostkey);
            s_hostkey = buf;
            s_hostkey_sz = sz;
            ok = true;
        } else {
            free(buf);
        }
    }
    nvs_close(h);
    return ok;
}

static void hostkey_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "hk", s_hostkey, s_hostkey_sz);
    nvs_commit(h);
    nvs_close(h);
}

/* Generates a fresh ECC P-256 host key (DER) into s_hostkey. */
static bool hostkey_generate(void)
{
    WC_RNG rng;
    ecc_key key;
    if (wc_InitRng(&rng) != 0) return false;
    if (wc_ecc_init(&key) != 0) {
        wc_FreeRng(&rng);
        return false;
    }
    bool ok = false;
    if (wc_ecc_make_key(&rng, 32, &key) == 0) {
        uint8_t der[256];
        int sz = wc_EccKeyToDer(&key, der, sizeof(der));
        if (sz > 0) {
            free(s_hostkey);
            s_hostkey = (uint8_t *)malloc(sz);
            if (s_hostkey) {
                memcpy(s_hostkey, der, sz);
                s_hostkey_sz = sz;
                ok = true;
            }
        }
    }
    wc_ecc_free(&key);
    wc_FreeRng(&rng);
    if (ok) ESP_LOGI(TAG, "generated ECC host key (%lu B)", (unsigned long)s_hostkey_sz);
    return ok;
}

/* ---------------- SSH session ---------------- */

static int auth_cb(byte authType, WS_UserAuthData *ad, void *)
{
    if (authType != WOLFSSH_USERAUTH_PASSWORD) return WOLFSSH_USERAUTH_FAILURE;
    size_t ul = strlen(s_user), pl = strlen(s_pass);
    if (ad->usernameSz == ul && memcmp(ad->username, s_user, ul) == 0 && ad->sf.password.passwordSz == pl &&
        memcmp(ad->sf.password.password, s_pass, pl) == 0)
        return WOLFSSH_USERAUTH_SUCCESS;
    return WOLFSSH_USERAUTH_INVALID_PASSWORD;
}

static void ssh_write(WOLFSSH *ssh, const char *s)
{
    wolfSSH_stream_send(ssh, (byte *)s, strlen(s));
}

static void ssh_out(const char *text, void *ctx) { ssh_write((WOLFSSH *)ctx, text); }

/* Minimal line editor over the shell channel, feeding whole lines to the CLI. */
static void run_shell(WOLFSSH *ssh)
{
    ssh_write(ssh, cli_banner());
    ssh_write(ssh, cli_prompt());

    char line[160];
    int len = 0;
    byte buf[96];
    for (;;) {
        int n = wolfSSH_stream_read(ssh, buf, sizeof(buf));
        if (n == WS_WANT_READ || n == WS_WANT_WRITE) continue;
        if (n <= 0) break; /* EOF, disconnect, or error */
        for (int i = 0; i < n; i++) {
            byte c = buf[i];
            if (c == '\r' || c == '\n') {
                ssh_write(ssh, "\r\n");
                if (len > 0) {
                    line[len] = 0;
                    if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) return;
                    cli_execute(line, ssh_out, ssh);
                    len = 0;
                }
                ssh_write(ssh, cli_prompt());
            } else if (c == 0x7f || c == 0x08) {
                if (len > 0) {
                    len--;
                    ssh_write(ssh, "\b \b");
                }
            } else if (c >= 0x20 && c < 0x7f && len < (int)sizeof(line) - 1) {
                line[len++] = c;
                char e[2] = {(char)c, 0};
                ssh_write(ssh, e); /* echo */
            }
        }
    }
}

static void server_task(void *)
{
    wolfSSH_Init();
#ifdef DEBUG_WOLFSSH
    wolfSSH_Debugging_ON(); /* prints the handshake / algorithm negotiation to serial */
#endif
    WOLFSSH_CTX *ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, nullptr);
    if (!ctx) {
        ESP_LOGE(TAG, "CTX_new failed");
        s_running = false;
        s_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    wolfSSH_SetUserAuth(ctx, auth_cb);
    if (wolfSSH_CTX_UsePrivateKey_buffer(ctx, s_hostkey, s_hostkey_sz, WOLFSSH_FORMAT_ASN1) < 0) {
        ESP_LOGE(TAG, "host key load failed");
        wolfSSH_CTX_free(ctx);
        s_running = false;
        s_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct timeval tv = {1, 0};
    setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); /* so accept() returns to check s_stop */

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(s_port);
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 1) != 0) {
        ESP_LOGE(TAG, "bind/listen on port %u failed", s_port);
        close(ls);
        wolfSSH_CTX_free(ctx);
        s_running = false;
        s_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "SSH server listening on port %u", s_port);

    while (!s_stop) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        int cs = accept(ls, (struct sockaddr *)&cli, &cl);
        if (cs < 0) continue; /* timeout: loop and re-check s_stop */

        /* The accepted socket inherits the listener's 1 s recv timeout, which
           breaks the multi-round-trip SSH handshake (a blocking read stalls past
           1 s and wolfSSH treats it as an error). Clear it so reads block. */
        struct timeval none = {0, 0};
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));

        s_client = true;
        WOLFSSH *ssh = wolfSSH_new(ctx);
        if (ssh) {
            wolfSSH_set_fd(ssh, cs);
            int r = wolfSSH_accept(ssh);
            if (r == WS_SUCCESS) {
                run_shell(ssh);
            } else {
                int e = wolfSSH_get_error(ssh);
                ESP_LOGW(TAG, "handshake failed (accept=%d err=%d %s)", r, e, wolfSSH_ErrorToName(e));
            }
            wolfSSH_stream_exit(ssh, 0);
            wolfSSH_free(ssh);
        }
        close(cs);
        s_client = false;
    }

    close(ls);
    wolfSSH_CTX_free(ctx);
    wolfSSH_Cleanup();
    s_running = false;
    s_task = nullptr;
    vTaskDelete(nullptr);
}

/* ---------------- public API ---------------- */

bool ssh_server_running(void) { return s_running; }
bool ssh_server_enabled(void) { return s_enabled; }
uint16_t ssh_server_port(void) { return s_port; }
int ssh_server_clients(void) { return s_client ? 1 : 0; }
bool ssh_server_has_hostkey(void) { return s_hostkey && s_hostkey_sz; }
void ssh_server_user(char *buf, size_t n) { snprintf(buf, n, "%s", s_user); }

bool ssh_server_start(void)
{
    if (s_running) return true;
    if (!ssh_server_has_hostkey() && !hostkey_generate()) {
        ESP_LOGE(TAG, "no host key");
        return false;
    }
    if (!s_hostkey_sz) return false;
    hostkey_save();
    s_stop = false;
    s_running = true;

    /* Put the big SSH/crypto stack in PSRAM so it doesn't eat scarce internal
       DRAM. The task itself never touches flash (NVS writes happen in the caller),
       so a PSRAM stack is safe here. */
    static StaticTask_t s_tcb;
    if (!s_stack) s_stack = (StackType_t *)heap_caps_malloc(SSH_TASK_STACK, MALLOC_CAP_SPIRAM);
    if (s_stack) {
        s_task = xTaskCreateStaticPinnedToCore(server_task, "sshd", SSH_TASK_STACK, nullptr, 4, s_stack, &s_tcb, 0);
    } else if (xTaskCreatePinnedToCore(server_task, "sshd", SSH_TASK_STACK, nullptr, 4, &s_task, 0) != pdPASS) {
        s_task = nullptr;
    }
    if (!s_task) {
        s_running = false;
        return false;
    }
    return true;
}

void ssh_server_stop(void)
{
    if (!s_running) return;
    s_stop = true;
    for (int i = 0; i < 60 && s_running; i++) vTaskDelay(pdMS_TO_TICKS(50));
}

bool ssh_server_regen_hostkey(void)
{
    bool was = s_running;
    if (was) ssh_server_stop();
    bool ok = hostkey_generate();
    if (ok) hostkey_save();
    if (was && ok) ssh_server_start();
    return ok;
}

void ssh_server_configure(bool enabled, uint16_t port, const char *user, const char *pass)
{
    bool was_running = s_running;
    if (was_running) ssh_server_stop();

    s_enabled = enabled;
    if (port) s_port = port;
    if (user && user[0]) snprintf(s_user, sizeof(s_user), "%s", user);
    if (pass && pass[0]) snprintf(s_pass, sizeof(s_pass), "%s", pass);
    cfg_save();

    if (s_enabled) ssh_server_start();
}

void ssh_server_init(void)
{
    cfg_load();
    hostkey_load();
    if (s_enabled) ssh_server_start();
}
