#pragma once

#include <cstddef>
#include <cstdint>

/* DIAL (Discovery And Launch) control for smart TVs / cast targets such as a
   Samsung TV (the DIAL REST server usually listens on port 8008). Discovers
   devices over SSDP and launches/stops apps (YouTube, Netflix...) over HTTP. */

#define TV_MAX_DEVICES 6

typedef struct {
    char name[40];    /* friendlyName, or the host if unknown */
    uint32_t ip;      /* IPv4, network byte order */
    uint16_t port;    /* DIAL REST port (commonly 8008) */
    char app_url[96]; /* DIAL REST base, from the Application-URL header */
} tv_device_t;

/* Blocking SSDP discovery for ~timeout_ms. Fills out[0..ret). Safe to call from
   a task with a few KB of stack. */
int tvcast_discover(tv_device_t *out, int max, int timeout_ms);

/* Launch an app by DIAL name (e.g. "YouTube", "Netflix"). `body` is an optional
   application/x-www-form-urlencoded payload (may be nullptr). Returns the HTTP
   status (201/200 on success) or a negative value on a transport error. */
int tvcast_launch(const tv_device_t *d, const char *app, const char *body);
int tvcast_stop(const tv_device_t *d, const char *app);

/* GETs the app resource and copies a short state word ("running"/"stopped"/...)
   into `state`. Returns the HTTP status or negative on error. */
int tvcast_app_state(const tv_device_t *d, const char *app, char *state, size_t nstate);

/* Launch the YouTube app; if video_id is non-empty, start playing that video. */
int tvcast_youtube(const tv_device_t *d, const char *video_id);
