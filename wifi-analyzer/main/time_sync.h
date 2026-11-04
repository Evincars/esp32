#pragma once

/* Wall-clock time from SNTP (pool.ntp.org) plus a small table of time zones. */

void time_sync_init(void);  /* applies the configured time zone */
void time_sync_start(void); /* starts SNTP; safe to call on every new IP */
bool time_sync_valid(void); /* true once the clock has been set */

int time_sync_zone_count(void);
const char *time_sync_zone_name(int idx);
void time_sync_set_zone(int idx);
