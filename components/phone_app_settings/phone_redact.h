/*
 * Masks for the log (serial port and the copy on the memory card): IMEI, IP addresses, SSIDs.
 * Each writes into `out` and returns it, for use as a printf argument.
 */
#pragma once

#include <stdio.h>
#include <string.h>

/** Only the last `keep` characters: "*** 1234" */
static inline const char *phone_redact_tail(const char *s, int keep, char *out, size_t len)
{
    const size_t n = s ? strlen(s) : 0;
    snprintf(out, len, "***%s", n > (size_t)keep ? s + n - keep : "");
    return out;
}

/** IPv4 text: only the last number, "*.*.*.45"; anything else (empty, IPv6) as "***" */
static inline const char *phone_redact_ip(const char *ip, char *out, size_t len)
{
    const char *dot = ip ? strrchr(ip, '.') : NULL;
    if (!ip || !*ip) {
        snprintf(out, len, "-");
    } else if (dot) {
        snprintf(out, len, "*.*.*%s", dot);
    } else {
        snprintf(out, len, "***");
    }
    return out;
}

/** SSID: the first 2 characters and the length, "Ho*** (9)" */
static inline const char *phone_redact_ssid(const char *ssid, char *out, size_t len)
{
    const size_t n = ssid ? strlen(ssid) : 0;
    snprintf(out, len, "%.2s*** (%u)", n > 2 ? ssid : "", (unsigned)n);
    return out;
}
