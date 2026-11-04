#include "pkt_decode.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "lwip/sockets.h"
#include "oui.h"
#include "portscan.h"

/* One pass over the frame. With no outputs attached it only collects class bits,
   which keeps the capture path cheap; every formatting call is skipped then. */
namespace {

struct Dis {
    const uint8_t *d;
    int n;        /* captured bytes */
    int orig_len; /* length on the wire */
    const sniff_pkt_t *pkt; /* null when only classifying */
    pkt_summary_t *sum;
    pkt_layer_t *L;
    int maxl, nl;
    uint32_t cls;
    pkt_layer_t *cur;

    bool has(int off, int len) const { return off >= 0 && len >= 0 && off + len <= n; }
    uint8_t u8(int off) const { return d[off]; }
    uint16_t u16(int off) const { return (uint16_t)(d[off] << 8 | d[off + 1]); }
    uint32_t u32(int off) const { return (uint32_t)d[off] << 24 | (uint32_t)d[off + 1] << 16 | d[off + 2] << 8 | d[off + 3]; }

    void layer(int osi, const char *name, int off, int len)
    {
        if (sum && osi >= sum->top_osi) sum->top_osi = osi;
        cur = nullptr;
        if (!L || nl >= maxl) return;
        cur = &L[nl++];
        memset(cur, 0, sizeof(*cur));
        cur->osi = osi;
        strlcpy(cur->name, name, sizeof(cur->name));
        cur->off = off;
        cur->len = std::max(0, std::min(len, n - off));
    }

    void field(const char *k, const char *fmt, ...) __attribute__((format(printf, 3, 4)))
    {
        if (!cur || cur->nfields >= PKT_MAX_FIELDS) return;
        pkt_field_t &f = cur->f[cur->nfields++];
        strlcpy(f.k, k, sizeof(f.k));
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(f.v, sizeof(f.v), fmt, ap);
        va_end(ap);
    }

    void proto(const char *p)
    {
        if (sum) strlcpy(sum->proto, p, sizeof(sum->proto));
    }

    void info(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        if (!sum) return;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(sum->info, sizeof(sum->info), fmt, ap);
        va_end(ap);
    }

    bool detail() const { return cur != nullptr; }
};

/* ---------------- formatting helpers ---------------- */

void fmt_mac(char *out, size_t len, const uint8_t *m)
{
    if (memcmp(m, "\xff\xff\xff\xff\xff\xff", 6) == 0) {
        strlcpy(out, "Broadcast", len);
        return;
    }
    snprintf(out, len, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

void fmt_ip4(char *out, size_t len, const uint8_t *a)
{
    snprintf(out, len, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
}

void fmt_ip6(char *out, size_t len, const uint8_t *a)
{
    if (!inet_ntop(AF_INET6, a, out, len)) strlcpy(out, "?", len);
}

void field_mac(Dis &x, const char *k, const uint8_t *m)
{
    if (!x.detail()) return;
    char buf[20];
    fmt_mac(buf, sizeof(buf), m);
    const char *v = (m[0] & 1) ? nullptr : oui_vendor(m);
    if (v) x.field(k, "%s (%s)", buf, v);
    else x.field(k, "%s", buf);
}

void set_addrs_mac(Dis &x, const uint8_t *src, const uint8_t *dst)
{
    if (!x.sum) return;
    fmt_mac(x.sum->src, sizeof(x.sum->src), src);
    fmt_mac(x.sum->dst, sizeof(x.sum->dst), dst);
}

/* Printable copy of a byte run, stopping at CR/LF. */
void text_line(const uint8_t *p, int n, char *out, size_t len)
{
    size_t k = 0;
    for (int i = 0; i < n && k + 1 < len; i++) {
        if (p[i] == '\r' || p[i] == '\n') break;
        out[k++] = isprint(p[i]) ? (char)p[i] : '.';
    }
    out[k] = 0;
}

/* Finds "Name: value" in an HTTP-style header block. */
bool header_value(const uint8_t *p, int n, const char *name, char *out, size_t len)
{
    size_t nl = strlen(name);
    for (int i = 0; i + (int)nl + 1 < n; i++) {
        if ((i == 0 || p[i - 1] == '\n') && strncasecmp((const char *)p + i, name, nl) == 0 && p[i + nl] == ':') {
            int j = i + nl + 1;
            while (j < n && p[j] == ' ') j++;
            text_line(p + j, n - j, out, len);
            return true;
        }
    }
    return false;
}

const char *ip_proto_name(int p)
{
    switch (p) {
    case 1: return "ICMP";
    case 2: return "IGMP";
    case 6: return "TCP";
    case 17: return "UDP";
    case 58: return "ICMPv6";
    default: return "?";
    }
}

/* ---------------- forward declarations ---------------- */

void ethertype(Dis &x, uint16_t type, int off);

/* ---------------- application layer ---------------- */

const char *dns_type_name(uint16_t t)
{
    switch (t) {
    case 1: return "A";
    case 2: return "NS";
    case 5: return "CNAME";
    case 6: return "SOA";
    case 12: return "PTR";
    case 15: return "MX";
    case 16: return "TXT";
    case 28: return "AAAA";
    case 32: return "NB";
    case 33: return "SRV";
    case 47: return "NSEC";
    case 65: return "HTTPS";
    case 255: return "ANY";
    default: return "?";
    }
}

/* Reads a (possibly compressed) DNS name; returns the offset after it, or -1.
   NetBIOS names (32 letters 'A'..'P') are decoded to their 15-character form. */
int dns_name(Dis &x, int base, int off, int end, char *out, size_t len, bool nbns)
{
    size_t k = 0;
    int ret = -1, jumps = 0;
    out[0] = 0;
    while (off < end && x.has(off, 1)) {
        uint8_t l = x.u8(off);
        if (l == 0) {
            if (ret < 0) ret = off + 1;
            break;
        }
        if ((l & 0xC0) == 0xC0) {
            if (!x.has(off, 2) || ++jumps > 16) return -1;
            if (ret < 0) ret = off + 2;
            off = base + ((l & 0x3F) << 8 | x.u8(off + 1));
            continue;
        }
        if (!x.has(off + 1, l)) return -1;
        if (k && k + 1 < len) out[k++] = '.';
        if (nbns && l == 32) {
            char name[17];
            for (int i = 0; i < 16; i++) name[i] = (char)(((x.u8(off + 1 + 2 * i) - 'A') << 4) | (x.u8(off + 2 + 2 * i) - 'A'));
            int nlen = 15;
            while (nlen > 0 && name[nlen - 1] == ' ') nlen--;
            for (int i = 0; i < nlen && k + 1 < len; i++) out[k++] = isprint((unsigned char)name[i]) ? name[i] : '.';
        } else {
            for (int i = 0; i < l && k + 1 < len; i++) {
                uint8_t c = x.u8(off + 1 + i);
                out[k++] = isprint(c) ? (char)c : '.';
            }
        }
        off += 1 + l;
    }
    out[std::min(k, len - 1)] = 0;
    if (!out[0]) strlcpy(out, "<root>", len);
    return ret;
}

void dns(Dis &x, int off, int end, const char *name, bool nbns)
{
    x.cls |= PKT_C_DNS;
    x.proto(name);
    if (!x.has(off, 12)) return;
    x.layer(7, name, off, end - off);
    uint16_t id = x.u16(off), flags = x.u16(off + 2);
    int qd = x.u16(off + 4), an = x.u16(off + 6);
    bool resp = flags & 0x8000;
    int rcode = flags & 0xF;
    x.field("Transaction ID", "0x%04x", id);
    x.field("Flags", "0x%04x %s%s", flags, resp ? "response" : "query", rcode ? ", error" : "");
    x.field("Questions", "%d", qd);
    x.field("Answers", "%d", an);
    if (rcode) x.field("Reply code", "%d%s", rcode, rcode == 3 ? " (no such name)" : "");

    char qname[64] = "", ans[64] = "";
    uint16_t qtype = 0;
    int p = off + 12;
    if (qd > 0) {
        p = dns_name(x, off, p, end, qname, sizeof(qname), nbns);
        if (p > 0 && x.has(p, 4)) {
            qtype = x.u16(p);
            x.field("Query", "%s %s", qname, dns_type_name(qtype));
            p += 4;
        } else {
            p = -1;
        }
    }
    for (int i = 1; i < qd && p > 0; i++) { /* skip the remaining questions */
        char tmp[8];
        p = dns_name(x, off, p, end, tmp, sizeof(tmp), nbns);
        if (p > 0) p += 4;
    }
    if (an > 0 && p > 0) {
        char aname[64];
        p = dns_name(x, off, p, end, aname, sizeof(aname), nbns);
        if (p > 0 && x.has(p, 10)) {
            uint16_t atype = x.u16(p), rdlen = x.u16(p + 8);
            uint32_t ttl = x.u32(p + 4);
            int rd = p + 10;
            if (atype == 1 && rdlen == 4 && x.has(rd, 4)) {
                fmt_ip4(ans, sizeof(ans), x.d + rd);
            } else if (atype == 28 && rdlen == 16 && x.has(rd, 16)) {
                fmt_ip6(ans, sizeof(ans), x.d + rd);
            } else if (atype == 12 || atype == 5 || atype == 2) {
                dns_name(x, off, rd, end, ans, sizeof(ans), nbns);
            } else if (atype == 32 && rdlen >= 6 && x.has(rd + 2, 4)) {
                fmt_ip4(ans, sizeof(ans), x.d + rd + 2);
            }
            if (!qname[0]) strlcpy(qname, aname, sizeof(qname));
            if (!qtype) qtype = atype;
            x.field("Answer", "%s %s", dns_type_name(atype), ans[0] ? ans : "...");
            x.field("TTL", "%lu s", (unsigned long)ttl);
        }
    }
    if (x.sum) {
        if (resp) x.info("Response %s %s%s%s", dns_type_name(qtype), qname, ans[0] ? " -> " : "", ans);
        else x.info("Query %s %s", dns_type_name(qtype), qname);
    }
}

void dhcp(Dis &x, int off, int end)
{
    x.cls |= PKT_C_DHCP;
    x.proto("DHCP");
    if (!x.has(off, 240) || x.u32(off + 236) != 0x63825363) return;
    x.layer(7, "DHCP", off, end - off);
    static const char *TYPES[] = {"?", "Discover", "Offer", "Request", "Decline", "ACK", "NAK", "Release", "Inform"};
    uint32_t xid = x.u32(off + 4);
    int type = 0;
    char host[33] = "", buf[20];
    x.field("Message", "%s", x.u8(off) == 1 ? "Boot request" : "Boot reply");
    x.field("Transaction ID", "0x%08lx", (unsigned long)xid);
    if (x.detail()) {
        fmt_ip4(buf, sizeof(buf), x.d + off + 12);
        x.field("Client IP", "%s", buf);
        fmt_ip4(buf, sizeof(buf), x.d + off + 16);
        x.field("Your IP", "%s", buf);
    }
    field_mac(x, "Client MAC", x.d + off + 28);

    for (int p = off + 240; x.has(p, 1);) {
        uint8_t code = x.u8(p);
        if (code == 255) break;
        if (code == 0) {
            p++;
            continue;
        }
        if (!x.has(p, 2)) break;
        int l = x.u8(p + 1), v = p + 2;
        if (!x.has(v, l)) break;
        if (code == 53 && l == 1) type = x.u8(v);
        else if (code == 12) text_line(x.d + v, l, host, std::min<size_t>(sizeof(host), l + 1));
        else if (x.detail() && (code == 50 || code == 54 || code == 3 || code == 1 || code == 6) && l >= 4) {
            fmt_ip4(buf, sizeof(buf), x.d + v);
            x.field(code == 50 ? "Requested IP" : code == 54 ? "Server ID" : code == 3 ? "Router" : code == 1 ? "Subnet mask" : "DNS", "%s", buf);
        } else if (code == 51 && l == 4) {
            x.field("Lease time", "%lu s", (unsigned long)x.u32(v));
        } else if (code == 60 && x.detail()) {
            char vc[40];
            text_line(x.d + v, l, vc, std::min<size_t>(sizeof(vc), l + 1));
            x.field("Vendor class", "%s", vc);
        }
        p = v + l;
    }
    const char *tname = type > 0 && type <= 8 ? TYPES[type] : "?";
    x.field("Type", "%s", tname);
    if (host[0]) x.field("Hostname", "%s", host);
    x.info("DHCP %s  xid 0x%08lx%s%s", tname, (unsigned long)xid, host[0] ? "  " : "", host);
}

void ntp(Dis &x, int off, int end)
{
    x.proto("NTP");
    if (!x.has(off, 48)) return;
    x.layer(7, "NTP", off, end - off);
    uint8_t b = x.u8(off);
    int vn = (b >> 3) & 7, mode = b & 7;
    static const char *MODES[] = {"reserved", "sym. active", "sym. passive", "client", "server", "broadcast", "control", "private"};
    x.field("Version", "%d", vn);
    x.field("Mode", "%s", MODES[mode]);
    x.field("Stratum", "%d", x.u8(off + 1));
    x.field("Poll", "%d", (int8_t)x.u8(off + 2));
    x.info("NTP v%d, %s, stratum %d", vn, MODES[mode], x.u8(off + 1));
}

void ssdp_like(Dis &x, int off, int end, const char *name)
{
    x.proto(name);
    int len = std::min(end, x.n) - off;
    if (len <= 0) return;
    x.layer(7, name, off, end - off);
    char line[64], v[64];
    text_line(x.d + off, len, line, sizeof(line));
    x.field("Start line", "%s", line);
    const char *keys[] = {"Host", "ST", "NT", "NTS", "USN", "Server", "User-Agent", "Location", "Content-Type"};
    for (const char *k : keys) {
        if (x.detail() && header_value(x.d + off, len, k, v, sizeof(v))) x.field(k, "%s", v);
    }
    if (x.sum) {
        if (header_value(x.d + off, len, "ST", v, sizeof(v)) || header_value(x.d + off, len, "NT", v, sizeof(v))) {
            char *sp = strstr(line, " HTTP/");
            if (sp) *sp = 0;
            x.info("%s  %s", line, v);
        } else {
            x.info("%s", line);
        }
    }
}

bool looks_http(Dis &x, int off)
{
    static const char *M[] = {"GET ", "POST ", "HEAD ", "PUT ", "DELETE ", "OPTIONS ", "PATCH ", "HTTP/1.", "NOTIFY ", "M-SEARCH "};
    for (const char *m : M) {
        int l = strlen(m);
        if (x.has(off, l) && memcmp(x.d + off, m, l) == 0) return true;
    }
    return false;
}

void tls(Dis &x, int off, int end, uint16_t port)
{
    x.cls |= PKT_C_WEB;
    x.proto("TLS");
    if (!x.has(off, 5)) return;
    uint8_t ct = x.u8(off);
    uint16_t ver = x.u16(off + 1), rlen = x.u16(off + 3);
    const char *ct_name = ct == 20 ? "Change Cipher Spec" : ct == 21 ? "Alert" : ct == 22 ? "Handshake" : ct == 23 ? "Application Data" : "?";
    x.layer(6, "TLS record", off, 5 + rlen);
    x.field("Content type", "%s (%d)", ct_name, ct);
    x.field("Version", "0x%04x", ver);
    x.field("Length", "%d", rlen);

    char sni[64] = "", alpn[16] = "";
    const char *hs_name = nullptr;
    bool tls13 = false;
    int h = off + 5;
    if (ct == 22 && x.has(h, 4)) {
        uint8_t ht = x.u8(h);
        hs_name = ht == 1 ? "Client Hello" : ht == 2 ? "Server Hello" : ht == 4 ? "New Session Ticket" : ht == 11 ? "Certificate"
                  : ht == 12 ? "Server Key Exchange" : ht == 14 ? "Server Hello Done" : ht == 16 ? "Client Key Exchange" : "Handshake";
        x.field("Handshake", "%s", hs_name);
        if (ht == 1 || ht == 2) {
            int p = h + 4 + 2 + 32; /* version + random */
            if (x.has(p, 1)) p += 1 + x.u8(p); /* session id */
            if (ht == 1) {
                if (x.has(p, 2)) p += 2 + x.u16(p); /* cipher suites */
                if (x.has(p, 1)) p += 1 + x.u8(p);  /* compression */
            } else {
                if (x.has(p, 2)) x.field("Cipher suite", "0x%04x", x.u16(p));
                p += 3;
            }
            if (x.has(p, 2)) {
                int ext_end = p + 2 + x.u16(p);
                for (p += 2; x.has(p, 4) && p < ext_end;) {
                    uint16_t et = x.u16(p), el = x.u16(p + 2);
                    int v = p + 4;
                    if (et == 0 && x.has(v + 3, 2)) {
                        int nl = x.u16(v + 3);
                        text_line(x.d + v + 5, std::min(nl, x.n - v - 5), sni, std::min<size_t>(sizeof(sni), nl + 1));
                    } else if (et == 16 && x.has(v + 2, 1)) {
                        int pl = x.u8(v + 2);
                        text_line(x.d + v + 3, std::min(pl, x.n - v - 3), alpn, std::min<size_t>(sizeof(alpn), pl + 1));
                    } else if (et == 43) {
                        tls13 = true;
                    }
                    p = v + el;
                }
            }
            if (sni[0]) x.field("Server name", "%s", sni);
            if (alpn[0]) x.field("ALPN", "%s", alpn);
            if (tls13) x.field("TLS 1.3", "supported_versions present");
        }
    }

    const char *app = alpn[0] ? alpn : portscan_service(port);
    x.layer(7, "Encrypted application", h, end - h);
    x.field("Protocol", "%s", app[0] ? app : "unknown");
    x.field("Payload", "%d bytes, encrypted", std::max(0, end - off));
    if (hs_name) x.info("%s%s%s", hs_name, sni[0] ? "  SNI=" : "", sni);
    else x.info("%s, %d bytes", ct_name, rlen);
}

void http(Dis &x, int off, int end)
{
    x.cls |= PKT_C_WEB;
    x.proto("HTTP");
    int len = std::min(end, x.n) - off;
    if (len <= 0) return;
    x.layer(7, "HTTP", off, end - off);
    char line[80], v[64];
    text_line(x.d + off, len, line, sizeof(line));
    x.field("Start line", "%s", line);
    const char *keys[] = {"Host", "User-Agent", "Server", "Content-Type", "Content-Length", "Location"};
    for (const char *k : keys) {
        if (x.detail() && header_value(x.d + off, len, k, v, sizeof(v))) x.field(k, "%s", v);
    }
    x.info("%s", line);
}

void ssh(Dis &x, int off, int end)
{
    x.proto("SSH");
    x.layer(7, "SSH", off, end - off);
    char line[64];
    if (x.has(off, 4) && memcmp(x.d + off, "SSH-", 4) == 0) {
        text_line(x.d + off, x.n - off, line, sizeof(line));
        x.field("Banner", "%s", line);
        x.info("%s", line);
    } else {
        x.field("Payload", "%d bytes, encrypted", end - off);
        x.info("Encrypted packet, %d bytes", end - off);
    }
}

void mqtt(Dis &x, int off, int end)
{
    x.proto("MQTT");
    if (!x.has(off, 2)) return;
    static const char *T[] = {"?", "CONNECT", "CONNACK", "PUBLISH", "PUBACK", "PUBREC", "PUBREL", "PUBCOMP",
                              "SUBSCRIBE", "SUBACK", "UNSUBSCRIBE", "UNSUBACK", "PINGREQ", "PINGRESP", "DISCONNECT", "AUTH"};
    int type = x.u8(off) >> 4;
    x.layer(7, "MQTT", off, end - off);
    x.field("Type", "%s", T[type]);
    int p = off + 1, rem = 0, mul = 1;
    while (x.has(p, 1) && mul <= 128 * 128 * 128) {
        rem += (x.u8(p) & 0x7F) * mul;
        mul *= 128;
        if (!(x.u8(p++) & 0x80)) break;
    }
    x.field("Remaining length", "%d", rem);
    char topic[48] = "";
    if (type == 3 && x.has(p, 2)) {
        int tl = x.u16(p);
        text_line(x.d + p + 2, std::min(tl, x.n - p - 2), topic, std::min<size_t>(sizeof(topic), tl + 1));
        x.field("Topic", "%s", topic);
    }
    x.info("%s%s%s", T[type], topic[0] ? "  " : "", topic);
}

void quic(Dis &x, int off, int end)
{
    x.cls |= PKT_C_WEB;
    x.proto("QUIC");
    if (!x.has(off, 1)) return;
    x.layer(7, "QUIC", off, end - off);
    uint8_t b = x.u8(off);
    if ((b & 0x80) && x.has(off, 5)) {
        static const char *T[] = {"Initial", "0-RTT", "Handshake", "Retry"};
        x.field("Header", "long, %s", T[(b >> 4) & 3]);
        x.field("Version", "0x%08lx", (unsigned long)x.u32(off + 1));
        x.info("%s, %d bytes", T[(b >> 4) & 3], end - off);
    } else {
        x.field("Header", "short (1-RTT, protected)");
        x.info("Protected payload, %d bytes", end - off);
    }
}

void raw_data(Dis &x, int off, int end)
{
    if (end - off <= 0) return;
    x.layer(7, "Data", off, end - off);
    x.field("Payload", "%d bytes", end - off);
}

bool port_in(uint16_t a, uint16_t b, std::initializer_list<uint16_t> ports)
{
    for (uint16_t p : ports) {
        if (a == p || b == p) return true;
    }
    return false;
}

void app_udp(Dis &x, uint16_t sp, uint16_t dp, int off, int end)
{
    if (port_in(sp, dp, {5353})) dns(x, off, end, "mDNS", false);
    else if (port_in(sp, dp, {5355})) dns(x, off, end, "LLMNR", false);
    else if (port_in(sp, dp, {137})) dns(x, off, end, "NBNS", true);
    else if (port_in(sp, dp, {53})) dns(x, off, end, "DNS", false);
    else if (port_in(sp, dp, {67, 68})) dhcp(x, off, end);
    else if (port_in(sp, dp, {546, 547})) {
        x.cls |= PKT_C_DHCP;
        x.proto("DHCPv6");
        static const char *T[] = {"?", "Solicit", "Advertise", "Request", "Confirm", "Renew", "Rebind", "Reply", "Release", "Decline", "Reconfigure", "Info-Request"};
        int t = x.has(off, 1) ? x.u8(off) : 0;
        x.layer(7, "DHCPv6", off, end - off);
        x.field("Message", "%s", t < 12 ? T[t] : "?");
        x.info("DHCPv6 %s", t < 12 ? T[t] : "?");
    } else if (port_in(sp, dp, {123})) ntp(x, off, end);
    else if (port_in(sp, dp, {1900})) ssdp_like(x, off, end, "SSDP");
    else if (port_in(sp, dp, {3702})) ssdp_like(x, off, end, "WS-Disc");
    else if (port_in(sp, dp, {443})) quic(x, off, end);
    else raw_data(x, off, end);
}

void app_tcp(Dis &x, uint16_t sp, uint16_t dp, int off, int end)
{
    if (end - off <= 0) return;
    bool tls_record = x.has(off, 3) && x.u8(off) >= 20 && x.u8(off) <= 23 && x.u8(off + 1) == 3;
    if (tls_record) tls(x, off, end, std::min(sp, dp));
    else if (looks_http(x, off)) http(x, off, end);
    else if (port_in(sp, dp, {22})) ssh(x, off, end);
    else if (port_in(sp, dp, {1883})) mqtt(x, off, end);
    else if (port_in(sp, dp, {53})) dns(x, off + 2, end, "DNS", false);
    else if (port_in(sp, dp, {443, 8443, 993, 995, 465, 8883})) tls(x, off, end, std::min(sp, dp));
    else raw_data(x, off, end);
}

/* ---------------- transport layer ---------------- */

void field_port(Dis &x, const char *k, uint16_t port)
{
    const char *svc = portscan_service(port);
    if (svc[0]) x.field(k, "%u (%s)", port, svc);
    else x.field(k, "%u", port);
}

void tcp(Dis &x, int off, int seglen)
{
    x.cls |= PKT_C_TCP;
    x.proto("TCP");
    if (!x.has(off, 20)) return;
    uint16_t sp = x.u16(off), dp = x.u16(off + 2);
    uint32_t seq = x.u32(off + 4), ack = x.u32(off + 8);
    int doff = (x.u8(off + 12) >> 4) * 4;
    uint8_t fl = x.u8(off + 13);
    uint16_t win = x.u16(off + 14);
    int plen = std::max(0, seglen - doff);

    char flags[40] = "";
    if (x.sum || x.L) {
        static const char *F[] = {"FIN", "SYN", "RST", "PSH", "ACK", "URG", "ECE", "CWR"};
        static const int ORDER[] = {1, 0, 2, 3, 4, 5, 6, 7};
        for (int i : ORDER) {
            if (!(fl & (1 << i))) continue;
            if (flags[0]) strlcat(flags, ", ", sizeof(flags));
            strlcat(flags, F[i], sizeof(flags));
        }
    }
    x.layer(4, "TCP", off, doff);
    field_port(x, "Source port", sp);
    field_port(x, "Dest port", dp);
    x.field("Sequence", "%lu", (unsigned long)seq);
    if (fl & 0x10) x.field("Acknowledgment", "%lu", (unsigned long)ack);
    x.field("Header length", "%d bytes", doff);
    x.field("Flags", "0x%02x [%s]", fl, flags);
    x.field("Window", "%u", win);
    x.field("Checksum", "0x%04x", x.u16(off + 16));
    x.field("Payload", "%d bytes", plen);
    x.info("%u -> %u [%s] Seq=%lu%s Win=%u Len=%d", sp, dp, flags, (unsigned long)seq, (fl & 0x10) ? " ACK" : "", win, plen);
    app_tcp(x, sp, dp, off + doff, off + doff + plen);
}

void udp(Dis &x, int off)
{
    x.cls |= PKT_C_UDP;
    x.proto("UDP");
    if (!x.has(off, 8)) return;
    uint16_t sp = x.u16(off), dp = x.u16(off + 2), len = x.u16(off + 4);
    x.layer(4, "UDP", off, 8);
    field_port(x, "Source port", sp);
    field_port(x, "Dest port", dp);
    x.field("Length", "%u", len);
    x.field("Checksum", "0x%04x", x.u16(off + 6));
    x.info("%u -> %u Len=%d", sp, dp, std::max(0, len - 8));
    app_udp(x, sp, dp, off + 8, off + std::max<int>(len, 8));
}

void icmp(Dis &x, int off, int len)
{
    x.cls |= PKT_C_ICMP;
    x.proto("ICMP");
    if (!x.has(off, 4)) return;
    int type = x.u8(off), code = x.u8(off + 1);
    const char *name = type == 0 ? "Echo reply" : type == 8 ? "Echo request" : type == 3 ? "Destination unreachable"
                       : type == 11 ? "Time exceeded" : type == 5 ? "Redirect" : "ICMP";
    x.layer(3, "ICMP", off, len);
    x.field("Type", "%d (%s)", type, name);
    x.field("Code", "%d", code);
    if ((type == 0 || type == 8) && x.has(off, 8)) {
        x.field("Identifier", "0x%04x", x.u16(off + 4));
        x.field("Sequence", "%u", x.u16(off + 6));
        x.info("%s  id=0x%04x seq=%u", name, x.u16(off + 4), x.u16(off + 6));
    } else if (type == 3) {
        const char *c = code == 0 ? "network" : code == 1 ? "host" : code == 3 ? "port" : "other";
        x.info("%s (%s unreachable)", name, c);
    } else {
        x.info("%s", name);
    }
}

void icmp6(Dis &x, int off, int len)
{
    x.cls |= PKT_C_ICMP;
    x.proto("ICMPv6");
    if (!x.has(off, 4)) return;
    int type = x.u8(off);
    const char *name = type == 128 ? "Echo request" : type == 129 ? "Echo reply" : type == 133 ? "Router solicitation"
                       : type == 134 ? "Router advertisement" : type == 135 ? "Neighbor solicitation"
                       : type == 136 ? "Neighbor advertisement" : type == 143 ? "MLDv2 report"
                       : type == 1 ? "Destination unreachable" : type == 130 ? "MLD query" : "ICMPv6";
    x.layer(3, "ICMPv6", off, len);
    x.field("Type", "%d (%s)", type, name);
    x.field("Code", "%d", x.u8(off + 1));
    if ((type == 135 || type == 136) && x.has(off + 8, 16)) {
        char t[48];
        fmt_ip6(t, sizeof(t), x.d + off + 8);
        x.field("Target", "%s", t);
        x.info("%s for %s", name, t);
    } else {
        x.info("%s", name);
    }
}

void igmp(Dis &x, int off, int len)
{
    x.proto("IGMP");
    if (!x.has(off, 8)) return;
    int type = x.u8(off);
    const char *name = type == 0x11 ? "Membership query" : type == 0x16 ? "v2 Membership report" : type == 0x17 ? "Leave group"
                       : type == 0x22 ? "v3 Membership report" : "IGMP";
    x.layer(3, "IGMP", off, len);
    x.field("Type", "0x%02x (%s)", type, name);
    char g[16];
    fmt_ip4(g, sizeof(g), x.d + off + 4);
    if (type != 0x22) x.field("Group", "%s", g);
    x.info("%s%s%s", name, type != 0x22 ? " " : "", type != 0x22 ? g : "");
}

/* ---------------- network layer ---------------- */

void arp(Dis &x, int off)
{
    x.cls |= PKT_C_ARP;
    x.proto("ARP");
    if (!x.has(off, 28)) return;
    uint16_t op = x.u16(off + 6);
    const uint8_t *sha = x.d + off + 8, *spa = x.d + off + 14, *tha = x.d + off + 18, *tpa = x.d + off + 24;
    char s[16], t[16];
    fmt_ip4(s, sizeof(s), spa);
    fmt_ip4(t, sizeof(t), tpa);
    x.layer(3, "ARP", off, 28);
    x.field("Opcode", "%u (%s)", op, op == 1 ? "request" : op == 2 ? "reply" : "?");
    field_mac(x, "Sender MAC", sha);
    x.field("Sender IP", "%s", s);
    field_mac(x, "Target MAC", tha);
    x.field("Target IP", "%s", t);
    if (op == 1 && memcmp(spa, tpa, 4) == 0) x.info("Gratuitous ARP for %s", s);
    else if (op == 1) x.info("Who has %s? Tell %s", t, s);
    else if (op == 2 && x.sum) {
        char m[20];
        fmt_mac(m, sizeof(m), sha);
        x.info("%s is at %s", s, m);
    }
}

void ipv4(Dis &x, int off)
{
    x.proto("IPv4");
    if (!x.has(off, 20)) return;
    int ihl = (x.u8(off) & 0x0F) * 4;
    int tot = x.u16(off + 2);
    uint16_t frag = x.u16(off + 6);
    int ttl = x.u8(off + 8), proto = x.u8(off + 9);
    if (x.sum) {
        fmt_ip4(x.sum->src, sizeof(x.sum->src), x.d + off + 12);
        fmt_ip4(x.sum->dst, sizeof(x.sum->dst), x.d + off + 16);
    }
    x.layer(3, "IPv4", off, ihl);
    if (x.detail()) {
        char a[16];
        x.field("Header length", "%d bytes", ihl);
        x.field("DSCP / ECN", "0x%02x", x.u8(off + 1));
        x.field("Total length", "%d", tot);
        x.field("Identification", "0x%04x", x.u16(off + 4));
        x.field("Flags", "%s", (frag & 0x6000) == 0x6000 ? "DF MF" : (frag & 0x4000) ? "DF" : (frag & 0x2000) ? "MF" : "none");
        x.field("Fragment offset", "%d", (frag & 0x1FFF) * 8);
        x.field("TTL", "%d", ttl);
        x.field("Protocol", "%d (%s)", proto, ip_proto_name(proto));
        x.field("Checksum", "0x%04x", x.u16(off + 10));
        fmt_ip4(a, sizeof(a), x.d + off + 12);
        x.field("Source", "%s", a);
        fmt_ip4(a, sizeof(a), x.d + off + 16);
        x.field("Destination", "%s", a);
    }
    x.info("IPv4 protocol %d", proto);
    if (frag & 0x1FFF) {
        x.info("Fragment, offset %d", (frag & 0x1FFF) * 8);
        return;
    }
    int l4 = off + ihl, l4len = tot - ihl;
    switch (proto) {
    case 1: icmp(x, l4, l4len); break;
    case 2: igmp(x, l4, l4len); break;
    case 6: tcp(x, l4, l4len); break;
    case 17: udp(x, l4); break;
    default: break;
    }
}

void ipv6(Dis &x, int off)
{
    x.proto("IPv6");
    if (!x.has(off, 40)) return;
    int plen = x.u16(off + 4), nh = x.u8(off + 6), hop = x.u8(off + 7);
    if (x.sum) {
        fmt_ip6(x.sum->src, sizeof(x.sum->src), x.d + off + 8);
        fmt_ip6(x.sum->dst, sizeof(x.sum->dst), x.d + off + 24);
    }
    x.layer(3, "IPv6", off, 40);
    if (x.detail()) {
        char a[48];
        uint32_t w = x.u32(off);
        x.field("Traffic class", "0x%02lx", (unsigned long)((w >> 20) & 0xFF));
        x.field("Flow label", "0x%05lx", (unsigned long)(w & 0xFFFFF));
        x.field("Payload length", "%d", plen);
        x.field("Next header", "%d (%s)", nh, nh == 0 ? "hop-by-hop" : ip_proto_name(nh));
        x.field("Hop limit", "%d", hop);
        fmt_ip6(a, sizeof(a), x.d + off + 8);
        x.field("Source", "%s", a);
        fmt_ip6(a, sizeof(a), x.d + off + 24);
        x.field("Destination", "%s", a);
    }
    int p = off + 40, end = off + 40 + plen;
    while ((nh == 0 || nh == 43 || nh == 60) && x.has(p, 2)) { /* extension headers */
        nh = x.u8(p);
        p += (x.u8(p + 1) + 1) * 8;
    }
    x.info("IPv6 next header %d", nh);
    switch (nh) {
    case 58: icmp6(x, p, end - p); break;
    case 6: tcp(x, p, end - p); break;
    case 17: udp(x, p); break;
    default: break;
    }
}

void eapol(Dis &x, int off)
{
    x.cls |= PKT_C_EAPOL;
    x.proto("EAPOL");
    if (!x.has(off, 4)) return;
    int type = x.u8(off + 1);
    x.layer(2, "802.1X (EAPOL)", off, 4 + x.u16(off + 2));
    x.field("Version", "%d", x.u8(off));
    x.field("Type", "%d (%s)", type, type == 3 ? "Key" : type == 0 ? "EAP packet" : type == 1 ? "Start" : "?");
    if (type == 3 && x.has(off + 4, 3)) {
        uint16_t ki = x.u16(off + 5);
        bool ack = ki & 0x0080, mic = ki & 0x0100, secure = ki & 0x0200, install = ki & 0x0040;
        int msg = ack && !mic ? 1 : !ack && mic && !secure ? 2 : ack && mic && install ? 3 : !ack && mic && secure ? 4 : 0;
        x.field("Key info", "0x%04x", ki);
        if (msg) x.field("Handshake", "message %d of 4", msg);
        if (msg) x.info("Key (message %d of 4)", msg);
        else x.info("Key");
    } else {
        x.info("EAPOL type %d", type);
    }
}

void ethertype(Dis &x, uint16_t type, int off)
{
    switch (type) {
    case 0x0806: arp(x, off); break;
    case 0x0800: ipv4(x, off); break;
    case 0x86DD: ipv6(x, off); break;
    case 0x888E: eapol(x, off); break;
    default:
        if (x.sum) snprintf(x.sum->proto, sizeof(x.sum->proto), "0x%04x", type);
        x.info("EtherType 0x%04x", type);
        break;
    }
}

/* ---------------- data link layer ---------------- */

void ethernet(Dis &x)
{
    if (!x.has(0, 14)) return;
    uint16_t type = x.u16(12);
    int off = 14;
    set_addrs_mac(x, x.d + 6, x.d + 0);
    x.layer(2, "Ethernet II", 0, 14);
    field_mac(x, "Destination", x.d + 0);
    field_mac(x, "Source", x.d + 6);
    if (type == 0x8100 && x.has(14, 4)) {
        x.field("VLAN", "%u", x.u16(14) & 0x0FFF);
        type = x.u16(16);
        off = 18;
    }
    x.field("Type", "0x%04x", type);
    x.proto("Ethernet");
    ethertype(x, type, off);
}

const char *mgmt_name(int sub)
{
    static const char *N[16] = {"Assoc Req",  "Assoc Resp", "Reassoc Req", "Reassoc Resp", "Probe Req", "Probe Resp",
                                "Timing Adv", "Mgmt",       "Beacon",      "ATIM",         "Disassoc",  "Auth",
                                "Deauth",     "Action",     "Action NoAck", "Mgmt"};
    return N[sub & 15];
}

const char *reason_name(int r)
{
    switch (r) {
    case 1: return "unspecified";
    case 2: return "auth no longer valid";
    case 3: return "station leaving";
    case 4: return "inactivity";
    case 6: return "class 2 from non-auth";
    case 7: return "class 3 from non-assoc";
    case 8: return "station leaving BSS";
    case 15: return "4-way handshake timeout";
    default: return "";
    }
}

void dot11_ies(Dis &x, int p, char *ssid, size_t ssid_len, int *channel, bool *rsn)
{
    while (x.has(p, 2)) {
        int id = x.u8(p), l = x.u8(p + 1), v = p + 2;
        if (!x.has(v, l)) break;
        if (id == 0) {
            if (l == 0 || x.u8(v) == 0) strlcpy(ssid, "", ssid_len);
            else text_line(x.d + v, l, ssid, std::min<size_t>(ssid_len, l + 1));
        } else if (id == 3 && l >= 1) {
            *channel = x.u8(v);
        } else if (id == 48) {
            *rsn = true;
        }
        p = v + l;
    }
}

void dot11(Dis &x)
{
    if (!x.has(0, 10)) return;
    uint8_t fc0 = x.u8(0), fc1 = x.u8(1);
    int type = (fc0 >> 2) & 3, sub = fc0 >> 4;
    if (type != 1 && !x.has(0, 24)) return; /* truncated management/data header */
    bool to_ds = fc1 & 1, from_ds = fc1 & 2, prot = fc1 & 0x40;
    const uint8_t *a1 = x.d + 4, *a2 = x.has(10, 6) ? x.d + 10 : nullptr, *a3 = x.has(16, 6) ? x.d + 16 : nullptr;

    char name[30];
    if (type == 0) snprintf(name, sizeof(name), "802.11 %s", mgmt_name(sub));
    else if (type == 1) snprintf(name, sizeof(name), "802.11 Control");
    else snprintf(name, sizeof(name), "802.11 %sData", (sub & 8) ? "QoS " : "");
    int hdr = type == 1 ? 10 : 24;
    if (type == 2 && to_ds && from_ds) hdr += 6;
    if (type == 2 && (sub & 8)) hdr += 2 + ((fc1 & 0x80) ? 4 : 0);

    x.layer(2, name, 0, hdr);
    x.field("Frame control", "type %d, subtype %d", type, sub);
    if (x.detail()) {
        char f[48] = "";
        if (to_ds) strlcat(f, "ToDS ", sizeof(f));
        if (from_ds) strlcat(f, "FromDS ", sizeof(f));
        if (fc1 & 0x08) strlcat(f, "Retry ", sizeof(f));
        if (fc1 & 0x10) strlcat(f, "PwrMgt ", sizeof(f));
        if (fc1 & 0x20) strlcat(f, "MoreData ", sizeof(f));
        if (prot) strlcat(f, "Protected ", sizeof(f));
        size_t fl = strlen(f);
        if (fl) f[fl - 1] = 0; /* trailing space */
        x.field("Flags", "%s", f[0] ? f : "none");
        x.field("Duration", "%u us", x.u16(2) & 0x7FFF);
    }

    /* who is who depends on the DS bits */
    const uint8_t *sa = a2, *da = a1, *bssid = a3;
    if (type == 2) {
        if (to_ds && !from_ds) { bssid = a1; sa = a2; da = a3; }
        else if (!to_ds && from_ds) { da = a1; bssid = a2; sa = a3; }
        else if (to_ds && from_ds) { da = a3; sa = x.has(24, 6) ? x.d + 24 : a2; bssid = nullptr; }
    }
    field_mac(x, type == 1 ? "Receiver" : "Destination", da);
    if (sa) field_mac(x, type == 1 ? "Transmitter" : "Source", sa);
    if (bssid && type != 1) field_mac(x, "BSSID", bssid);
    if (type != 1 && x.has(22, 2)) x.field("Sequence", "%u", (x.d[22] | x.d[23] << 8) >> 4); /* little endian */
    if (x.sum) {
        fmt_mac(x.sum->dst, sizeof(x.sum->dst), da);
        if (sa) fmt_mac(x.sum->src, sizeof(x.sum->src), sa);
    }

    if (type == 1) {
        static const char *C[16] = {"Ctrl", "Ctrl", "Trigger", "TACK", "BF Poll", "NDPA", "Ctrl Ext", "Wrapper",
                                    "BlockAck Req", "BlockAck", "PS-Poll", "RTS", "CTS", "ACK", "CF-End", "CF-End+Ack"};
        x.proto(C[sub]);
        x.info("%s", C[sub]);
        return;
    }

    if (type == 0) {
        x.cls |= PKT_C_MGMT;
        x.proto(mgmt_name(sub));
        int body = 24;
        char ssid[33] = "";
        int ch = 0;
        bool rsn = false;
        x.layer(2, "802.11 management body", body, x.n - body);
        if ((sub == 8 || sub == 5) && x.has(body, 12)) {
            uint16_t interval = x.d[body + 8] | x.d[body + 9] << 8;
            uint16_t cap = x.d[body + 10] | x.d[body + 11] << 8;
            dot11_ies(x, body + 12, ssid, sizeof(ssid), &ch, &rsn);
            x.field("SSID", "%s", ssid[0] ? ssid : "(hidden)");
            if (ch) x.field("Channel", "%d", ch);
            x.field("Interval", "%u TU", interval);
            x.field("Security", "%s", rsn ? "RSN (WPA2/3)" : (cap & 0x10) ? "WEP/WPA" : "Open");
            x.info("%s  SSID=%s", sub == 8 ? "Beacon" : "Probe response", ssid[0] ? ssid : "(hidden)");
            if (ch && x.sum) {
                size_t l = strlen(x.sum->info);
                snprintf(x.sum->info + l, sizeof(x.sum->info) - l, "  ch %d", ch);
            }
        } else if (sub == 4) {
            dot11_ies(x, body, ssid, sizeof(ssid), &ch, &rsn);
            x.field("SSID", "%s", ssid[0] ? ssid : "(wildcard)");
            x.info("Probe request  SSID=%s", ssid[0] ? ssid : "(wildcard)");
        } else if ((sub == 10 || sub == 12) && x.has(body, 2)) {
            int r = x.d[body] | x.d[body + 1] << 8;
            x.field("Reason", "%d %s", r, reason_name(r));
            x.info("%s, reason %d %s", sub == 12 ? "Deauthentication" : "Disassociation", r, reason_name(r));
        } else if (sub == 11 && x.has(body, 6)) {
            int alg = x.d[body] | x.d[body + 1] << 8, seq = x.d[body + 2] | x.d[body + 3] << 8;
            int st = x.d[body + 4] | x.d[body + 5] << 8;
            const char *an = alg == 0 ? "Open" : alg == 1 ? "Shared key" : alg == 3 ? "SAE" : "?";
            x.field("Algorithm", "%s", an);
            x.field("Sequence", "%d", seq);
            x.field("Status", "%d", st);
            x.info("Authentication %s, seq %d, status %d", an, seq, st);
        } else if ((sub == 0 || sub == 2) && x.has(body, 4)) {
            dot11_ies(x, body + (sub == 2 ? 10 : 4), ssid, sizeof(ssid), &ch, &rsn);
            x.field("SSID", "%s", ssid);
            x.info("%s  SSID=%s", sub == 0 ? "Association request" : "Reassociation request", ssid);
        } else if ((sub == 1 || sub == 3) && x.has(body, 6)) {
            int st = x.d[body + 2] | x.d[body + 3] << 8;
            x.field("Status", "%d", st);
            x.field("AID", "%d", (x.d[body + 4] | x.d[body + 5] << 8) & 0x3FFF);
            x.info("Association response, status %d", st);
        } else if (sub == 13 && x.has(body, 1)) {
            x.field("Category", "%d", x.u8(body));
            x.info("Action, category %d", x.u8(body));
        } else {
            x.info("%s", mgmt_name(sub));
        }
        return;
    }

    /* data */
    x.cls |= PKT_C_DATA;
    x.proto((sub & 8) ? "QoS Data" : "Data");
    if (sub == 4 || sub == 12) {
        x.proto("Null");
        x.info("Null function (power save %s)", (fc1 & 0x10) ? "on" : "off");
        return;
    }
    int plen = x.orig_len - hdr;
    if (prot) {
        x.field("Payload", "%d bytes, encrypted", plen);
        x.info("Protected data, %d bytes", plen);
        return;
    }
    static const uint8_t SNAP[6] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00};
    if (x.has(hdr, 8) && memcmp(x.d + hdr, SNAP, 6) == 0) {
        uint16_t et = x.u16(hdr + 6);
        x.layer(2, "LLC / SNAP", hdr, 8);
        x.field("DSAP / SSAP", "0xAA / 0xAA");
        x.field("Type", "0x%04x", et);
        ethertype(x, et, hdr + 8);
    } else {
        x.info("Data, %d bytes", plen);
    }
}

/* ---------------- physical layer ---------------- */

void physical(Dis &x)
{
    const sniff_pkt_t *p = x.pkt;
    if (p->src == SNIFF_SRC_AIR) {
        x.layer(1, "802.11 radio", 0, 0);
        x.field("Channel", "%d", p->channel);
        x.field("Signal", "%d dBm", p->rssi);
        if (p->noise) {
            x.field("Noise floor", "%d dBm", p->noise);
            x.field("SNR", "%d dB", p->rssi - p->noise);
        }
        if (p->sig_mode == 0) {
            static const float RATES[16] = {1, 2, 5.5f, 11, 0, 2, 5.5f, 11, 48, 24, 12, 6, 54, 36, 18, 9};
            float r = RATES[p->rate & 15];
            x.field("PHY", "%s", (p->rate & 15) < 8 ? "802.11b (DSSS)" : "802.11g (OFDM)");
            x.field("Rate", "%d.%d Mbit/s", (int)r, (int)(r * 10) % 10);
        } else {
            x.field("PHY", "%s", p->sig_mode == 1 ? "802.11n (HT)" : "802.11ac (VHT)");
            x.field("MCS", "%d", p->mcs);
        }
        x.field("Bandwidth", "%d MHz", p->cwb ? 40 : 20);
    } else {
        x.layer(1, "Wi-Fi station link", 0, 0);
        x.field("Interface", "STA (lwIP netif)");
        x.field("Direction", "%s", p->src == SNIFF_SRC_TX ? "sent" : "received");
    }
    x.field("Frame length", "%u bytes", p->len);
    x.field("Captured", "%u bytes", p->caplen);
}

void run(Dis &x, const sniff_pkt_t *p)
{
    x.d = p->data;
    x.n = p->caplen;
    x.orig_len = p->len;
    x.pkt = p;
    if (x.L) physical(x);
    if (p->src == SNIFF_SRC_AIR) dot11(x);
    else ethernet(x);
}

} // namespace

uint32_t pkt_classify(const uint8_t *data, int caplen, bool is_80211)
{
    Dis x = {};
    x.d = data;
    x.n = caplen;
    x.orig_len = caplen;
    if (is_80211) dot11(x);
    else ethernet(x);
    return x.cls;
}

void pkt_summarize(const sniff_pkt_t *p, pkt_summary_t *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->proto, "?", sizeof(out->proto));
    Dis x = {};
    x.sum = out;
    run(x, p);
    out->cls = x.cls;
}

int pkt_dissect(const sniff_pkt_t *p, pkt_layer_t *layers, int max)
{
    Dis x = {};
    x.L = layers;
    x.maxl = max;
    run(x, p);
    return x.nl;
}

const char *pkt_osi_name(int osi)
{
    static const char *N[] = {"", "Physical", "Data link", "Network", "Transport", "Session", "Presentation", "Application"};
    return osi >= 1 && osi <= 7 ? N[osi] : "";
}

void pkt_hexdump(const uint8_t *d, int len, int per_line, char *out, size_t out_len)
{
    size_t k = 0;
    out[0] = 0;
    for (int off = 0; off < len && k + 8 < out_len; off += per_line) {
        int w = snprintf(out + k, out_len - k, per_line > 8 ? "%04x " : "%03x ", off);
        if (w < 0 || (size_t)w >= out_len - k) break;
        k += w;
        for (int i = 0; i < per_line && k + 4 < out_len; i++) {
            if (off + i < len) k += snprintf(out + k, out_len - k, "%02x ", d[off + i]);
            else k += snprintf(out + k, out_len - k, "   ");
        }
        for (int i = 0; i < per_line && off + i < len && k + 2 < out_len; i++) {
            uint8_t c = d[off + i];
            out[k++] = isprint(c) ? (char)c : '.';
        }
        if (k + 1 < out_len) out[k++] = '\n';
        out[k] = 0;
    }
}
