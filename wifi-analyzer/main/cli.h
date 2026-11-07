#pragma once

/* Transport-agnostic command-line for the analyzer. The SSH server feeds it one
   line at a time and streams the text back to the client via the out() callback.
   The same engine could back a serial or telnet console. */

typedef void (*cli_out_fn)(const char *text, void *ctx);

/* Runs one command line, streaming output through out(). Long operations (Wi-Fi
   scan, LAN discovery, port scan) block until finished, printing progress. */
void cli_execute(const char *line, cli_out_fn out, void *ctx);

const char *cli_banner(void); /* shown once on connect */
const char *cli_prompt(void); /* e.g. "esp32> " */
