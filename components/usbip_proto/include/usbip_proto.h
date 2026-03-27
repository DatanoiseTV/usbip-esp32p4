/*
 * USB/IP Protocol Definitions
 * Wire protocol structures and constants per USB/IP specification
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* USB/IP protocol versions */
#define USBIP_VERSION_V100  0x0100
#define USBIP_VERSION_V111  0x0111
#define USBIP_VERSION       USBIP_VERSION_V111

/* Operation codes */
#define OP_REQ_DEVLIST  0x8005
#define OP_REP_DEVLIST  0x0005
#define OP_REQ_IMPORT   0x8003
#define OP_REP_IMPORT   0x0003

/* URB commands */
#define USBIP_CMD_SUBMIT    0x00000001
#define USBIP_RET_SUBMIT    0x00000003
#define USBIP_CMD_UNLINK    0x00000002
#define USBIP_RET_UNLINK    0x00000004

/* Direction */
#define USBIP_DIR_OUT   0x00
#define USBIP_DIR_IN    0x01

/* Status codes */
#define ST_OK   0x00
#define ST_NA   0x01

/* Sizes */
#define USBIP_DEV_PATH_MAX  256
#define USBIP_BUSID_MAX     32

/*
 * Packet structures - all packed for wire format
 */

/* Common header for all operations (8 bytes) */
typedef struct __attribute__((packed)) {
    uint16_t version;
    uint16_t code;
    uint32_t status;
} usbip_op_common_t;

/* USB device descriptor on the wire (312 bytes) */
typedef struct __attribute__((packed)) {
    char     path[USBIP_DEV_PATH_MAX];  /* 256 */
    char     busid[USBIP_BUSID_MAX];    /* 32 */
    uint32_t busnum;
    uint32_t devnum;
    uint32_t speed;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bConfigurationValue;
    uint8_t  bNumConfigurations;
    uint8_t  bNumInterfaces;
} usbip_usb_device_t;

/* USB interface descriptor on the wire (4 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t padding;
} usbip_usb_interface_t;

/* Device list reply body (4 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t ndev;
} usbip_op_devlist_reply_t;

/* Import request body (32 bytes) */
typedef struct __attribute__((packed)) {
    char busid[USBIP_BUSID_MAX];
} usbip_op_import_request_t;

/* URB header basic (20 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t command;
    uint32_t seqnum;
    uint32_t devid;
    uint32_t direction;
    uint32_t ep;
} usbip_header_basic_t;

/* CMD_SUBMIT payload (28 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t transfer_flags;
    int32_t  transfer_buffer_length;
    int32_t  start_frame;
    int32_t  number_of_packets;
    int32_t  interval;
    uint8_t  setup[8];
} usbip_header_cmd_submit_t;

/* RET_SUBMIT payload (28 bytes) */
typedef struct __attribute__((packed)) {
    int32_t  status;
    int32_t  actual_length;
    int32_t  start_frame;
    int32_t  number_of_packets;
    int32_t  error_count;
    uint8_t  padding[8];
} usbip_header_ret_submit_t;

/* CMD_UNLINK payload (28 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t seqnum;
    uint8_t  padding[24];
} usbip_header_cmd_unlink_t;

/* RET_UNLINK payload (28 bytes) */
typedef struct __attribute__((packed)) {
    int32_t  status;
    uint8_t  padding[24];
} usbip_header_ret_unlink_t;

/* Full USB/IP header (48 bytes) */
typedef struct __attribute__((packed)) {
    usbip_header_basic_t base;
    union {
        usbip_header_cmd_submit_t cmd_submit;
        usbip_header_ret_submit_t ret_submit;
        usbip_header_cmd_unlink_t cmd_unlink;
        usbip_header_ret_unlink_t ret_unlink;
    } u;
} usbip_header_t;

/* Isochronous packet descriptor (16 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t offset;
    uint32_t length;
    uint32_t actual_length;
    uint32_t status;
} usbip_iso_packet_descriptor_t;

/* Static size checks */
_Static_assert(sizeof(usbip_op_common_t) == 8, "op_common must be 8 bytes");
_Static_assert(sizeof(usbip_usb_device_t) == 312, "usb_device must be 312 bytes");
_Static_assert(sizeof(usbip_usb_interface_t) == 4, "usb_interface must be 4 bytes");
_Static_assert(sizeof(usbip_op_devlist_reply_t) == 4, "devlist_reply must be 4 bytes");
_Static_assert(sizeof(usbip_op_import_request_t) == 32, "import_request must be 32 bytes");
_Static_assert(sizeof(usbip_header_t) == 48, "header must be 48 bytes");
_Static_assert(sizeof(usbip_iso_packet_descriptor_t) == 16, "iso_desc must be 16 bytes");

/*
 * Pack/Unpack functions
 * pack=true: host byte order -> network byte order
 * pack=false: network byte order -> host byte order
 */
void usbip_pack_op_common(usbip_op_common_t *op, bool pack);
void usbip_pack_usb_device(usbip_usb_device_t *dev, bool pack);
void usbip_pack_header(usbip_header_t *hdr, bool pack);
void usbip_pack_iso_descriptor(usbip_iso_packet_descriptor_t *iso, bool pack);
void usbip_pack_devlist_reply(usbip_op_devlist_reply_t *rep, bool pack);

/* Version check */
bool usbip_version_supported(uint16_t version);

/* Network I/O helpers */
int usbip_net_send(int sockfd, const void *buf, size_t len);
int usbip_net_recv(int sockfd, void *buf, size_t len);
void usbip_net_configure_socket(int sockfd);

#ifdef __cplusplus
}
#endif
