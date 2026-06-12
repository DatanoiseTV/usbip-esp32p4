/*
 * Shared helpers for the Web UI component's JSON builders.
 */
#pragma once

/*
 * Escape a string for embedding in a JSON string literal.
 * Per RFC 8259, all control characters (U+0000-U+001F) must be escaped --
 * USB string descriptors converted from UTF-16LE can contain raw control
 * bytes, which would otherwise produce invalid JSON and break JSON.parse()
 * on the frontend.
 */
static inline void webui_json_escape(const char *src, char *dst, int dstlen)
{
    static const char hex[] = "0123456789abcdef";
    int j = 0;
    for (int i = 0; src[i] && j < dstlen - 2; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (j >= dstlen - 2) break;
            dst[j++] = '\\';
            dst[j++] = (char)c;
            continue;
        }
        if (c == '\n') { if (j >= dstlen - 2) break; dst[j++] = '\\'; dst[j++] = 'n'; continue; }
        if (c == '\r') { if (j >= dstlen - 2) break; dst[j++] = '\\'; dst[j++] = 'r'; continue; }
        if (c == '\t') { if (j >= dstlen - 2) break; dst[j++] = '\\'; dst[j++] = 't'; continue; }
        if (c < 0x20 || c == 0x7f) {
            /* All other control characters must be escaped per RFC 8259 */
            if (j >= dstlen - 6) break;
            dst[j++] = '\\'; dst[j++] = 'u'; dst[j++] = '0'; dst[j++] = '0';
            dst[j++] = hex[(c >> 4) & 0xf];
            dst[j++] = hex[c & 0xf];
            continue;
        }
        dst[j++] = (char)c;
    }
    dst[j] = '\0';
}
