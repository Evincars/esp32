#pragma once

#include <cstddef>
#include <cstdint>

/* SSH server (wolfSSH) exposing the CLI. Connect with a normal client:
     ssh <user>@<board-ip>
   Config (enable, port, user, password) and the generated ECC host key are
   stored in NVS. */

#define SSH_USER_MAX 24
#define SSH_PASS_MAX 32

void ssh_server_init(void); /* loads config + host key; starts the server if enabled */

bool ssh_server_running(void);
bool ssh_server_enabled(void);
uint16_t ssh_server_port(void);
int ssh_server_clients(void);        /* 1 while a session is connected, else 0 */
bool ssh_server_has_hostkey(void);   /* a host key has been provisioned */
void ssh_server_user(char *buf, size_t n);

bool ssh_server_start(void);
void ssh_server_stop(void);

/* Persists config and (re)starts/stops accordingly. pass==nullptr keeps the
   current password; a non-null empty string is rejected by the caller. */
void ssh_server_configure(bool enabled, uint16_t port, const char *user, const char *pass);

/* Regenerates the ECC host key (clients will warn about a changed fingerprint). */
bool ssh_server_regen_hostkey(void);
