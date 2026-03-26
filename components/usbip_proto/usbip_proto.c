/*
 * USB/IP Protocol Layer
 * Serialization, version handling, and network I/O helpers
 */

#include "usbip_proto.h"
#include "esp_log.h"

#include <string.h>
#include <errno.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>

static const char *TAG = "usbip_proto";

/*
 * Byte-order swap helpers using lwIP's htonl/ntohl/htons/ntohs.
 * pack=true  -> host to network (hton*)
 * pack=false -> network to host (ntoh*)
 * For symmetric functions like htonl/ntohl they are identical,
 * but we use the correct name for clarity.
 */

static inline uint32_t swap32(uint32_t val, bool pack)
{
    return pack ? htonl(val) : ntohl(val);
}

static inline uint16_t swap16(uint16_t val, bool pack)
{
    return pack ? htons(val) : ntohs(val);
}

static inline int32_t swap32s(int32_t val, bool pack)
{
    uint32_t u;
    memcpy(&u, &val, sizeof(u));
    u = swap32(u, pack);
    int32_t r;
    memcpy(&r, &u, sizeof(r));
    return r;
}

/* --- Pack/Unpack functions --- */

void usbip_pack_op_common(usbip_op_common_t *op, bool pack)
{
    op->version = swap16(op->version, pack);
    op->code    = swap16(op->code, pack);
    op->status  = swap32(op->status, pack);
}

void usbip_pack_usb_device(usbip_usb_device_t *dev, bool pack)
{
    dev->busnum    = swap32(dev->busnum, pack);
    dev->devnum    = swap32(dev->devnum, pack);
    dev->speed     = swap32(dev->speed, pack);
    dev->idVendor  = swap16(dev->idVendor, pack);
    dev->idProduct = swap16(dev->idProduct, pack);
    dev->bcdDevice = swap16(dev->bcdDevice, pack);
}

void usbip_pack_header(usbip_header_t *hdr, bool pack)
{
    /* Swap all 5 base fields */
    hdr->base.command   = swap32(hdr->base.command, pack);
    hdr->base.seqnum    = swap32(hdr->base.seqnum, pack);
    hdr->base.devid     = swap32(hdr->base.devid, pack);
    hdr->base.direction = swap32(hdr->base.direction, pack);
    hdr->base.ep        = swap32(hdr->base.ep, pack);

    /*
     * Swap the first 5 uint32_t words (20 bytes) of the union.
     * The remaining 8 bytes (setup packet or padding) are byte arrays
     * and must NOT be swapped.
     */
    uint32_t *words = (uint32_t *)&hdr->u;
    for (int i = 0; i < 5; i++) {
        words[i] = swap32(words[i], pack);
    }
}

void usbip_pack_iso_descriptor(usbip_iso_packet_descriptor_t *iso, bool pack)
{
    iso->offset        = swap32(iso->offset, pack);
    iso->length        = swap32(iso->length, pack);
    iso->actual_length = swap32(iso->actual_length, pack);
    iso->status        = swap32(iso->status, pack);
}

void usbip_pack_devlist_reply(usbip_op_devlist_reply_t *rep, bool pack)
{
    rep->ndev = swap32(rep->ndev, pack);
}

/* --- Version handling --- */

bool usbip_version_supported(uint16_t version)
{
    return (version == USBIP_VERSION_V100) || (version == USBIP_VERSION_V111);
}

/* --- Network I/O helpers --- */

int usbip_net_send(int sockfd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = send(sockfd, p, remaining, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ESP_LOGE(TAG, "send error: errno=%d", errno);
            return -1;
        }
        if (n == 0) {
            ESP_LOGE(TAG, "send returned 0");
            return -1;
        }
        p += n;
        remaining -= (size_t)n;
    }

    return 0;
}

int usbip_net_recv(int sockfd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = recv(sockfd, p, remaining, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ESP_LOGE(TAG, "recv error: errno=%d", errno);
            return -1;
        }
        if (n == 0) {
            ESP_LOGE(TAG, "recv: connection closed");
            return -1;
        }
        p += n;
        remaining -= (size_t)n;
    }

    return 0;
}

void usbip_net_configure_socket(int sockfd)
{
    int opt = 1;

    /* TCP_NODELAY - disable Nagle's algorithm */
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        ESP_LOGW(TAG, "Failed to set TCP_NODELAY: errno=%d", errno);
    }

    /* SO_KEEPALIVE */
    if (setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) < 0) {
        ESP_LOGW(TAG, "Failed to set SO_KEEPALIVE: errno=%d", errno);
    }

#ifdef TCP_KEEPIDLE
    opt = 10;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &opt, sizeof(opt)) < 0) {
        ESP_LOGW(TAG, "Failed to set TCP_KEEPIDLE: errno=%d", errno);
    }
#endif

#ifdef TCP_KEEPINTVL
    opt = 5;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &opt, sizeof(opt)) < 0) {
        ESP_LOGW(TAG, "Failed to set TCP_KEEPINTVL: errno=%d", errno);
    }
#endif

#ifdef TCP_KEEPCNT
    opt = 3;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &opt, sizeof(opt)) < 0) {
        ESP_LOGW(TAG, "Failed to set TCP_KEEPCNT: errno=%d", errno);
    }
#endif
}
