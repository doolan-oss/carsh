//
// btstack_config.h — carsh device (bare-metal Pi 4 / Circle) BTstack build.
//
// Tuned for a Classic-only A2DP **sink** + AVRCP CT/TG bridge over a USB BT
// dongle (stage 1 of device/README "Bluetooth": USB HCI transport via Circle's
// CUSBBluetoothDevice; the onboard CYW43455 is stage 2). No BLE / mesh / LE-audio
// — keeps the memory pools and code size small. Used only when the device image
// is built with CARSH_HAVE_BTSTACK=1; the default build never sees BTstack.
//
// Sizing follows BTstack's port/raspi + example/a2dp_sink_demo configs. Revisit
// the MAX_NR_* pool counts at the bench if HCI runs out of buffers (R12).
#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// --- Port capabilities -----------------------------------------------------
#define HAVE_ASSERT
#define HAVE_MALLOC                 // newlib malloc is available under Circle
#define HAVE_EMBEDDED_TIME_MS       // run loop uses hal_time_ms() (device/hal_*)
// Realtek chipset firmware loading uses fopen/fread guarded by HAVE_POSIX_FILE_IO.
// circle-stdlib's newlib maps fopen() onto the mounted FatFs volume (kernel mounts
// "SD:"), so the rtlbt firmware downloads from SD:/firmware/rtlbt. (R3 / H6.)
#define HAVE_POSIX_FILE_IO

// --- Enabled BTstack features ----------------------------------------------
#define ENABLE_CLASSIC              // BR/EDR only (A2DP + AVRCP live here)
#define ENABLE_LOG_ERROR
#define ENABLE_LOG_INFO
#define ENABLE_PRINTF_HEXDUMP
#define ENABLE_SOFTWARE_AES128      // no LE-SC HW; classic pairing crypto path

// --- Buffers / sizes -------------------------------------------------------
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_INCOMING_PRE_BUFFER_SIZE 14

#define NVM_NUM_DEVICE_DB_ENTRIES 16
#define NVM_NUM_LINK_KEYS         16

// Static memory pool counts (used by btstack_memory when not pure-malloc). One
// phone at a time: a couple of L2CAP channels (signaling + AVDTP media + AVCTP)
// and the AVDTP/AVRCP connection objects.
#define MAX_NR_HCI_CONNECTIONS              2
#define MAX_NR_L2CAP_SERVICES               4
#define MAX_NR_L2CAP_CHANNELS               6
#define MAX_NR_SERVICE_RECORD_ITEMS         4
#define MAX_NR_AVDTP_CONNECTIONS            2
#define MAX_NR_AVDTP_STREAM_ENDPOINTS       2
#define MAX_NR_AVRCP_CONNECTIONS            2

#endif // BTSTACK_CONFIG_H
