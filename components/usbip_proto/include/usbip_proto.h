/*
 * USB/IP Protocol Definitions
 * Wire protocol structures and constants per USB/IP specification
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* USB/IP protocol version */
#define USBIP_VERSION  0x0111

/* USB/IP command codes */
#define USBIP_CMD_SUBMIT    0x00000001
#define USBIP_CMD_UNLINK    0x00000002
#define USBIP_RET_SUBMIT    0x00000003
#define USBIP_RET_UNLINK    0x00000004

/* USB/IP OP codes (device list, import) */
#define USBIP_OP_REQ_DEVLIST  0x8005
#define USBIP_OP_REP_DEVLIST  0x0005
#define USBIP_OP_REQ_IMPORT   0x8003
#define USBIP_OP_REP_IMPORT   0x0003

/* Status codes */
#define USBIP_ST_OK     0x00000000
#define USBIP_ST_NA     0x00000001

/**
 * @brief Initialize USB/IP protocol layer (stub)
 */
void usbip_proto_init(void);

#ifdef __cplusplus
}
#endif
