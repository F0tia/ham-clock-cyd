// Build-time defaults. Callsign, location, time zone and clock format are
// set on the device's setup page and saved on the board; these are only
// used until then.
#pragma once

#define DEFAULT_CALLSIGN  "N0CALL"
#define DEFAULT_LOCATION  ""   // grid square or "lat,lon"; empty = not set
#define DEFAULT_TIMEZONE  "UTC0"

// N0NBH solar data updates every few hours; be polite.
#define SOLAR_EVERY_MS   (30UL * 60 * 1000)
#define SOLAR_RETRY_MS   (2UL * 60 * 1000)

#define SETUP_AP_NAME    "HamClock-Setup"
#define USER_AGENT       "HamClockCYD/1.0.1 (+https://github.com/KI6UIT/ham-clock-cyd)"
