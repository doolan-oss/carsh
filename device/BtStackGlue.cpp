// BtStackGlue.cpp — the BTstack integration, entirely gated behind
// CARSH_HAVE_BTSTACK. Without the flag it provides trivial stubs so the default
// device image links without BTstack. With the flag (third_party/btstack + the
// rtlbt firmware on SD) this is the A2DP sink + AVRCP CT/TG bridge over a USB
// Realtek dongle.
//
// The profile setup mirrors BTstack v1.6.1 example/a2dp_sink_demo.c: the same
// init order, all three AVRCP packet handlers (avrcp + controller + target) and
// the A2DP sink / AVRCP CT / AVRCP TG / Device ID SDP records. What has been
// validated on the bench is tracked in STATUS.md and HARDWARE_TODO H6.
#include "BtStackGlue.h"

#include "CircleBluetooth.h"

#ifndef CARSH_HAVE_BTSTACK
// ---------------------------------------------------------------------------
// Stub build (default): no BTstack present.
namespace carsh::device {
bool btGlueInit(CircleBluetooth*) { return false; }
void btGlueProcess() {}
void btGlueSendAvrcp(int) {}
void btGlueStartScan() {}
void btGlueStopScan() {}
void btGlueConnect(const unsigned char*) {}
void btGlueDisconnect() {}
uint32_t btGlueA2dpSampleRate() { return 0; }
}  // namespace carsh::device

#else
// ---------------------------------------------------------------------------
// Live build: BTstack present (CARSH_HAVE_BTSTACK). Classic-only A2DP sink +
// AVRCP CT/TG over the Circle USB HCI transport (CircleHciTransport: HCI
// commands/events plus the ACL bulk path added by
// scripts/patches/usbbluetooth-acl.patch).
#include "CircleHciTransport.h"
#include "bt/LinkTracker.h"  // platform-independent connection bookkeeping
#include "bt/NowPlaying.h"   // platform-independent AVRCP metadata assembly

#include <circle/logger.h>  // bench bring-up diagnostics over the serial log
#include <cstddef>
#include <cstdio>           // fopen self-test: does newlib reach the SD card?
#include <cstdarg>          // route BTstack log_info/log_error to the serial log
#include <cstring>          // memset of the SDP record buffers

// BTstack is C; include its headers with C linkage. Specific headers only (NOT
// the umbrella btstack.h, which drags in BLE/mesh we don't build).
extern "C" {
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_embedded.h"
#include "btstack_event.h"
#include "btstack_util.h"
#include "bluetooth_company_id.h"
#include "hci.h"
#include "gap.h"
#include "l2cap.h"
#include "classic/sdp_server.h"
#include "classic/sdp_util.h"     // de_get_len(): SDP record size check
#include "classic/avdtp_util.h"   // avdtp_local_seid(); pulls in avdtp.h
#include "classic/a2dp_sink.h"
#include "classic/avrcp.h"
#include "classic/avrcp_controller.h"
#include "classic/avrcp_target.h"
#include "classic/device_id_server.h"
#include "classic/btstack_sbc.h"
#include "classic/btstack_sbc_bluedroid.h"
#include "btstack_chipset_realtek.h"
#include "hci_dump.h"
}

namespace carsh::device {
namespace {

const char FromBt[] = "btglue";  // serial-log tag for BT bring-up

// Route BTstack's log_info/log_error (incl. the Realtek chipset's firmware
// decisions) to the serial log, so bring-up is observable. INFO+ only.
void btDumpLogMessage(int level, const char* format, va_list ap) {
    // Errors only now that bring-up is solved (INFO floods during streaming). Bump
    // to HCI_DUMP_LOG_LEVEL_INFO temporarily to re-surface chipset/init diagnostics.
    if (level < HCI_DUMP_LOG_LEVEL_ERROR) return;
    char buf[200];
    vsnprintf(buf, sizeof(buf), format, ap);
    CLogger::Get()->Write(FromBt, level >= HCI_DUMP_LOG_LEVEL_ERROR ? LogError : LogNotice,
                          "bt: %s", buf);
}
// hci_dump.c calls log_packet for every packet WITHOUT a null check, so it must be
// non-null even though we only want messages — a no-op (packet logging is also
// disabled below).
void btDumpLogPacket(uint8_t, uint8_t, uint8_t*, uint16_t) {}
const hci_dump_t g_btDump = {nullptr, &btDumpLogPacket, &btDumpLogMessage};

CircleBluetooth* g_owner = nullptr;
btstack_packet_callback_registration_t g_hciEventCb;

// A2DP SBC sample rate (from the decoder), so the kernel clocks the PWM output at
// the stream's rate. Defaults to the most common A2DP rate until the first frame.
uint32_t g_btSampleRate = 44100;

// Which ACL link is the connected phone, its A2DP/AVRCP channel ids, and the
// one-shot report of a user connect's outcome. BTstack delivers these facts as
// separate HCI / A2DP / AVRCP events; the handlers below only extract fields and
// feed them here (host-tested in tests/bt/LinkTracker_test).
carsh::bt::LinkTracker g_link;

uint8_t g_localSeid = 0;

// Accumulates the per-attribute AVRCP now-playing subevents into one metadata
// (host-tested in tests/bt/BtParse_test).
carsh::bt::NowPlayingAssembler g_nowPlaying;

// SBC -> PCM decoder for the A2DP media payload (bluedroid).
const btstack_sbc_decoder_t* g_sbcDecoder = nullptr;
btstack_sbc_decoder_bluedroid_t g_sbcContext;

// Local SBC sink endpoint capabilities (full SBC range; 2..53 bitpool) — exactly
// the demo's media_sbc_codec_capabilities.
uint8_t g_sbcCapabilities[] = {0xFF, 0xFF, 2, 53};
uint8_t g_sbcConfiguration[4];

// SDP service records. Phones decide whether to open AVRCP to an A2DP sink (and
// which profiles to auto-connect) from these, so without them AVRCP never comes
// up. sdp_register_service() keeps a pointer into each buffer, so they must be
// static. Sizes are a2dp_sink_demo.c's; the carsh names below are no longer than
// the BTstack defaults those sizes were chosen for, and each record's encoded
// length is checked after it is built.
uint8_t g_sdpA2dpSink[150];
uint8_t g_sdpAvrcpController[200];
uint8_t g_sdpAvrcpTarget[150];
uint8_t g_sdpDeviceId[100];
const char kSdpProvider[] = "carsh";

// HU-initiated AVRCP. After A2DP signaling comes up, give the phone a moment to
// open AVRCP itself (it usually does once it sees our AVRCP records), and only
// connect from our side if it hasn't — avoiding an AVCTP connect collision in the
// common case. BTstack resolves a collision anyway; this just avoids the retry.
constexpr uint32_t kAvrcpConnectDelayMs = 1500;
btstack_timer_source_t g_avrcpConnectTimer;
bd_addr_t g_avrcpConnectAddr;

// Realtek firmware (RTL8761B etc.) lives on the SD boot partition; BTstack loads
// the matching rtlbt blob by USB product id during HCI bring-up (R3).
const char kRealtekFirmwareFolder[] = "SD:/firmware/rtlbt";

// Reusable float scratch for int16 PCM -> the engine's float ring.
float g_pcmFloat[4096];

carsh::bt::BdAddr toBdAddr(const uint8_t* a) {
    carsh::bt::BdAddr out{};
    for (int i = 0; i < 6; ++i) out[i] = a[i];
    return out;
}

// SBC decoder callback: interleaved int16 PCM -> float, forwarded to the engine
// ring via the owner. The jitter buffer + clock-drift SRC (R12) live downstream.
void sbcPcmHandler(int16_t* data, int numFrames, int numChannels, int sampleRate,
                   void* /*ctx*/) {
    g_btSampleRate = static_cast<uint32_t>(sampleRate);
    if (!g_owner) return;
    const int n = numFrames * numChannels;
    if (n <= 0 || n > (int)(sizeof(g_pcmFloat) / sizeof(g_pcmFloat[0]))) return;
    for (int i = 0; i < n; ++i) g_pcmFloat[i] = data[i] * (1.0f / 32768.0f);
    g_owner->deliverA2dpPcm(g_pcmFloat, (unsigned)numFrames, (unsigned)numChannels);
}

// A2DP media payload -> SBC decoder. Strip the AVDTP media header first: a 12-byte
// RTP header + 1-byte SBC header precede the SBC frames (feeding them to the
// decoder is what caused the "SBC decode: checksum error" at the bench).
void a2dpMediaHandler(uint8_t /*seid*/, uint8_t* packet, uint16_t size) {
    if (!g_sbcDecoder) return;
    constexpr unsigned kMediaHeader = 12 + 1;  // RTP header + SBC header
    if (size <= kMediaHeader) return;
    g_sbcDecoder->decode_signed_16(&g_sbcContext, 0, packet + kMediaHeader,
                                   static_cast<uint16_t>(size - kMediaHeader));
}

// Deferred HU-side AVRCP connect (see kAvrcpConnectDelayMs).
void avrcpConnectTimerHandler(btstack_timer_source_t*) {
    const carsh::bt::BdAddr a = toBdAddr(g_avrcpConnectAddr);
    // The phone opened AVRCP itself, A2DP went away, or the link dropped.
    if (!g_link.shouldInitiateAvrcp(a)) return;
    uint16_t cid = 0;
    uint8_t status = avrcp_connect(g_avrcpConnectAddr, &cid);
    if (status == ERROR_CODE_SUCCESS) {
        g_link.avrcpConnectStarted(a);
        CLogger::Get()->Write(FromBt, LogNotice, "AVRCP: connecting to %s",
                              bd_addr_to_str(g_avrcpConnectAddr));
    } else {
        CLogger::Get()->Write(FromBt, LogWarning, "AVRCP: connect to %s refused (0x%02x)",
                              bd_addr_to_str(g_avrcpConnectAddr), status);
    }
}

void scheduleAvrcpConnect(const bd_addr_t addr) {
    btstack_run_loop_remove_timer(&g_avrcpConnectTimer);
    bd_addr_copy(g_avrcpConnectAddr, addr);
    btstack_run_loop_set_timer_handler(&g_avrcpConnectTimer, &avrcpConnectTimerHandler);
    btstack_run_loop_set_timer(&g_avrcpConnectTimer, kAvrcpConnectDelayMs);
    btstack_run_loop_add_timer(&g_avrcpConnectTimer);
}

// A2DP sink events. BTstack's sink accepts the phone's SBC configuration by
// itself; the decoder parses the SBC frame headers, so the configuration
// subevents need no handling here. What matters is connection state: which link
// has A2DP, and whether a user connect reached a stream or failed on the way.
void a2dpPacketHandler(uint8_t type, uint16_t, uint8_t* packet, uint16_t) {
    if (type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;
    uint8_t sub = hci_event_a2dp_meta_get_subevent_code(packet);
    bd_addr_t addr;
    switch (sub) {
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED: {
            // Both directions. On an outgoing failure (page timeout, SDP or L2CAP
            // refused) BTstack reports it here, then again as a failed
            // STREAM_ESTABLISHED; the tracker reports the connect failure once.
            uint8_t status = a2dp_subevent_signaling_connection_established_get_status(packet);
            uint16_t cid = a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
            a2dp_subevent_signaling_connection_established_get_bd_addr(packet, addr);
            CLogger::Get()->Write(FromBt, LogNotice,
                                  "A2DP signaling %s (status 0x%02x, cid 0x%04x) %s",
                                  status == ERROR_CODE_SUCCESS ? "up" : "FAILED", status, cid,
                                  bd_addr_to_str(addr));
            g_link.a2dpSignalingEstablished(status, cid, toBdAddr(addr));
            // Bring up AVRCP (metadata + transport keys) for the connected phone.
            if (status == ERROR_CODE_SUCCESS && g_link.connected() &&
                g_link.connectedAddress() == toBdAddr(addr)) {
                scheduleAvrcpConnect(addr);
            }
            break;
        }
        case A2DP_SUBEVENT_STREAM_ESTABLISHED: {
            uint8_t status = a2dp_subevent_stream_established_get_status(packet);
            a2dp_subevent_stream_established_get_bd_addr(packet, addr);
            CLogger::Get()->Write(FromBt, LogNotice, "A2DP stream %s (status 0x%02x, cid 0x%04x)",
                                  status == ERROR_CODE_SUCCESS ? "ESTABLISHED" : "FAILED",
                                  status, a2dp_subevent_stream_established_get_a2dp_cid(packet));
            g_link.a2dpStreamEstablished(status, toBdAddr(addr));
            break;
        }
        case A2DP_SUBEVENT_STREAM_RELEASED:
            // Only the media stream; the signaling channel (and the phone) can
            // still be up — e.g. the phone closes the stream to reconfigure.
            CLogger::Get()->Write(FromBt, LogNotice, "A2DP stream released");
            break;
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            CLogger::Get()->Write(FromBt, LogNotice, "A2DP signaling released");
            g_link.a2dpSignalingReleased(
                a2dp_subevent_signaling_connection_released_get_a2dp_cid(packet));
            break;
        default:
            CLogger::Get()->Write(FromBt, LogNotice, "A2DP event 0x%02x", sub);
            break;
    }
}

// AVRCP connection events (avrcp_register_packet_handler): CONNECTION_ESTABLISHED
// / CONNECTION_RELEASED only. Controller and target events have their own
// handlers below.
void avrcpPacketHandler(uint8_t type, uint16_t, uint8_t* packet, uint16_t) {
    if (type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    switch (hci_event_avrcp_meta_get_subevent_code(packet)) {
        case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED: {
            uint8_t status = avrcp_subevent_connection_established_get_status(packet);
            uint16_t cid = avrcp_subevent_connection_established_get_avrcp_cid(packet);
            bd_addr_t addr;
            avrcp_subevent_connection_established_get_bd_addr(packet, addr);
            g_link.avrcpEstablished(status, cid, toBdAddr(addr));
            if (status != ERROR_CODE_SUCCESS) {
                CLogger::Get()->Write(FromBt, LogWarning, "AVRCP: connection to %s failed (0x%02x)",
                                      bd_addr_to_str(addr), status);
                break;
            }
            CLogger::Get()->Write(FromBt, LogNotice, "AVRCP: connected to %s (cid 0x%04x)",
                                  bd_addr_to_str(addr), cid);
            if (cid != g_link.avrcpCid()) break;  // not the connected phone's AVRCP
            // Subscribe to playback-status + track-changed notifications and pull the
            // current track's attributes, so the now-playing subevents below start
            // arriving and refresh on every change. Without this the controller is
            // connected but the phone never volunteers metadata. [hw] bench check.
            avrcp_controller_enable_notification(
                cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED);
            avrcp_controller_enable_notification(
                cid, AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED);
            avrcp_controller_get_now_playing_info(cid);
            break;
        }
        case AVRCP_SUBEVENT_CONNECTION_RELEASED: {
            uint16_t cid = avrcp_subevent_connection_released_get_avrcp_cid(packet);
            CLogger::Get()->Write(FromBt, LogNotice, "AVRCP: released (cid 0x%04x)", cid);
            if (cid == g_link.avrcpCid()) g_nowPlaying.reset();
            g_link.avrcpReleased(cid);
            break;
        }
        default:
            break;
    }
}

// Publish the assembled now-playing metadata (once any text is known). Called on
// every attribute, not only on NOW_PLAYING_INFO_DONE: BTstack v1.6.1 never emits
// INFO_DONE for a single-packet GetElementAttributes response — its parser counts
// the attribute bytes against a length that also includes the NumAttributes byte
// (avrcp_controller.c list_size vs list_offset) — so waiting for DONE would leave
// Now Playing on its placeholder. A title/artist/album burst costs a few copies.
void publishNowPlaying() {
    if (g_owner && g_nowPlaying.hasText()) g_owner->deliverMetadata(g_nowPlaying.metadata());
}

// AVRCP controller events (avrcp_controller_register_packet_handler): the phone's
// notifications and now-playing metadata. BTstack calls this handler without a
// NULL check once AVRCP is up, so it must be registered before power-on.
void avrcpControllerPacketHandler(uint8_t type, uint16_t, uint8_t* packet, uint16_t) {
    if (type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    const uint16_t cid = g_link.avrcpCid();
    if (cid == 0) return;  // no AVRCP to the connected phone (yet)
    using carsh::bt::AvrcpAttr;
    switch (hci_event_avrcp_meta_get_subevent_code(packet)) {
        case AVRCP_SUBEVENT_NOTIFICATION_TRACK_CHANGED:
            // New track: drop the previous track's attributes and re-query this
            // track's metadata (the fresh attributes arrive via the subevents below).
            g_nowPlaying.reset();
            avrcp_controller_get_now_playing_info(cid);
            break;
        // Now-playing attributes arrive one subevent at a time; accumulate them
        // (the value bytes are length-delimited, not NUL-terminated). The
        // assembly + duration/status mapping is host-tested in BtParse_test.
        case AVRCP_SUBEVENT_NOW_PLAYING_TITLE_INFO:
            g_nowPlaying.setAttribute(
                uint8_t(AvrcpAttr::Title),
                (const char*)avrcp_subevent_now_playing_title_info_get_value(packet),
                avrcp_subevent_now_playing_title_info_get_value_len(packet));
            publishNowPlaying();
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_ARTIST_INFO:
            g_nowPlaying.setAttribute(
                uint8_t(AvrcpAttr::Artist),
                (const char*)avrcp_subevent_now_playing_artist_info_get_value(packet),
                avrcp_subevent_now_playing_artist_info_get_value_len(packet));
            publishNowPlaying();
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_ALBUM_INFO:
            g_nowPlaying.setAttribute(
                uint8_t(AvrcpAttr::Album),
                (const char*)avrcp_subevent_now_playing_album_info_get_value(packet),
                avrcp_subevent_now_playing_album_info_get_value_len(packet));
            publishNowPlaying();
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_SONG_LENGTH_MS_INFO:
            g_nowPlaying.setDurationMs(
                avrcp_subevent_now_playing_song_length_ms_info_get_song_length(packet));
            publishNowPlaying();
            break;
        case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_STATUS_CHANGED:
            g_nowPlaying.setPlaybackStatus(
                avrcp_subevent_notification_playback_status_changed_get_play_status(packet));
            if (g_owner) g_owner->deliverMetadata(g_nowPlaying.metadata());
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_INFO_DONE:
            publishNowPlaying();  // multi-packet responses do end with DONE
            break;
        default:
            break;  // OPERATION_START/COMPLETE, NOTIFICATION_STATE, ...: nothing to do
    }
}

// AVRCP target events (avrcp_target_register_packet_handler): the phone acting as
// controller toward carsh. carsh's TG is a Category-2 monitor/amplifier and does
// not advertise absolute volume (no avrcp_target_support_event), so phones keep
// scaling their own volume and carsh's volume rail stays the one master. BTstack
// asserts on a missing handler for any of these, so it must exist even though
// there is little to do.
void avrcpTargetPacketHandler(uint8_t type, uint16_t, uint8_t* packet, uint16_t) {
    if (type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    switch (hci_event_avrcp_meta_get_subevent_code(packet)) {
        case AVRCP_SUBEVENT_PLAY_STATUS_QUERY:
            // BTstack waits for the app to answer GetPlayStatus. carsh is not the
            // player: answer "stopped, length/position unknown" (0xFFFFFFFF) so the
            // phone doesn't sit out its command timeout.
            avrcp_target_play_status(avrcp_subevent_play_status_query_get_avrcp_cid(packet),
                                     0xFFFFFFFFu, 0xFFFFFFFFu, AVRCP_PLAYBACK_STATUS_STOPPED);
            break;
        case AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED:
            // SetAbsoluteVolume (BTstack has already accepted it). Logged only.
            CLogger::Get()->Write(
                FromBt, LogDebug, "AVRCP TG: phone set absolute volume %u",
                avrcp_subevent_notification_volume_changed_get_absolute_volume(packet));
            break;
        case AVRCP_SUBEVENT_OPERATION:
            // Pass-through key from the phone (BTstack has already accepted it).
            CLogger::Get()->Write(FromBt, LogDebug, "AVRCP TG: operation 0x%02x %s",
                                  avrcp_subevent_operation_get_operation_id(packet),
                                  avrcp_subevent_operation_get_button_pressed(packet)
                                      ? "pressed" : "released");
            break;
        default:
            break;
    }
}

// HCI events: the Classic ACL link lifecycle (the stable address ProfileManager
// keys on, R10) + GAP inquiry results.
void hciPacketHandler(uint8_t type, uint16_t, uint8_t* packet, uint16_t) {
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            // The headline bring-up signal: HCI_STATE_WORKING means the controller
            // (incl. the Realtek firmware download) came up successfully.
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                CLogger::Get()->Write(FromBt, LogNotice,
                                      "HCI WORKING — controller up (firmware loaded)");
                // Re-assert visibility now that the stack is up (setting it before
                // power-on doesn't always latch). The phone should now see "carsh".
                gap_connectable_control(1);
                gap_discoverable_control(1);
                CLogger::Get()->Write(FromBt, LogNotice, "discoverable + connectable as 'carsh'");
            } else {
                CLogger::Get()->Write(FromBt, LogNotice, "HCI state -> %u",
                                      btstack_event_state_get_state(packet));
            }
            break;
        case HCI_EVENT_COMMAND_COMPLETE:
            // First sign the command/event USB path works (e.g. HCI Reset reply).
            CLogger::Get()->Write(FromBt, LogDebug, "HCI command complete (opcode 0x%04x)",
                                  hci_event_command_complete_get_command_opcode(packet));
            break;
        case HCI_EVENT_CONNECTION_COMPLETE: {
            // Also delivered for FAILED pages (e.g. 0x04 Page Timeout): BTstack
            // frees the link and no Disconnection Complete follows. Only a status-0
            // ACL link is a connected phone; the tracker turns a failure of the
            // user's connect into onConnectFailed.
            if (hci_event_connection_complete_get_link_type(packet) != HCI_LINK_TYPE_ACL) break;
            bd_addr_t addr;
            hci_event_connection_complete_get_bd_addr(packet, addr);
            uint8_t status = hci_event_connection_complete_get_status(packet);
            uint16_t handle = hci_event_connection_complete_get_connection_handle(packet);
            CLogger::Get()->Write(FromBt, LogNotice, "ACL link %s %s (status 0x%02x, handle 0x%04x)",
                                  status == ERROR_CODE_SUCCESS ? "up to" : "FAILED to",
                                  bd_addr_to_str(addr), status, handle);
            g_link.aclConnectionComplete(status, handle, toBdAddr(addr));
            break;
        }
        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            uint8_t status = hci_event_disconnection_complete_get_status(packet);
            uint16_t handle = hci_event_disconnection_complete_get_connection_handle(packet);
            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
            CLogger::Get()->Write(FromBt, LogNotice,
                                  "ACL link down (status 0x%02x, handle 0x%04x, reason 0x%02x)",
                                  status, handle, reason);
            g_link.aclDisconnectionComplete(status, handle, reason);
            break;
        }
        case GAP_EVENT_INQUIRY_RESULT: {
            // A device found during a scan -> the scan/connect UI's cache.
            carsh::bt::DiscoveredDevice d;
            bd_addr_t addr;
            gap_event_inquiry_result_get_bd_addr(packet, addr);
            d.address = toBdAddr(addr);
            d.classOfDevice = gap_event_inquiry_result_get_class_of_device(packet);
            if (gap_event_inquiry_result_get_rssi_available(packet))
                d.rssi = static_cast<int8_t>(gap_event_inquiry_result_get_rssi(packet));
            if (gap_event_inquiry_result_get_name_available(packet)) {
                d.name.assign(reinterpret_cast<const char*>(
                                  gap_event_inquiry_result_get_name(packet)),
                              gap_event_inquiry_result_get_name_len(packet));
            }
            CLogger::Get()->Write(FromBt, LogNotice, "scan: found %s (rssi %d)",
                                  d.name.empty() ? "(no name)" : d.name.c_str(), d.rssi);
            if (g_owner) g_owner->deliverDeviceDiscovered(d);
            break;
        }
        case GAP_EVENT_INQUIRY_COMPLETE:
            CLogger::Get()->Write(FromBt, LogNotice, "scan: complete");
            if (g_owner) g_owner->deliverScanComplete();
            break;
        default:
            break;
    }
}

// Build one SDP record into its static buffer and register it. Logs (rather than
// asserts) so a bad record is visible on the serial console at bring-up.
void registerSdpRecord(const char* what, const uint8_t* record, std::size_t capacity) {
    const uint32_t len = de_get_len(record);
    if (len > capacity) {
        CLogger::Get()->Write(FromBt, LogError, "SDP %s record overflows its buffer (%u > %u)",
                              what, (unsigned)len, (unsigned)capacity);
        return;
    }
    uint8_t status = sdp_register_service(record);
    CLogger::Get()->Write(FromBt, status == 0 ? LogNotice : LogError,
                          "SDP %s record %s (%u bytes, status 0x%02x)", what,
                          status == 0 ? "registered" : "NOT registered", (unsigned)len, status);
}

// SDP records for A2DP sink, AVRCP CT, AVRCP TG and Device ID, as in
// a2dp_sink_demo.c. Four records == MAX_NR_SERVICE_RECORD_ITEMS.
void registerSdpRecords() {
    // A2DP sink: carsh presents as a speaker (matches the Loudspeaker CoD below).
    memset(g_sdpA2dpSink, 0, sizeof(g_sdpA2dpSink));
    a2dp_sink_create_sdp_record(g_sdpA2dpSink, sdp_create_service_record_handle(),
                                AVDTP_SINK_FEATURE_MASK_SPEAKER, "carsh Audio Sink",
                                kSdpProvider);
    registerSdpRecord("A2DP sink", g_sdpA2dpSink, sizeof(g_sdpA2dpSink));

    // AVRCP controller: we send Category-1 (player) commands — play/pause/next/prev.
    memset(g_sdpAvrcpController, 0, sizeof(g_sdpAvrcpController));
    avrcp_controller_create_sdp_record(
        g_sdpAvrcpController, sdp_create_service_record_handle(),
        uint16_t(1u << AVRCP_CONTROLLER_SUPPORTED_FEATURE_CATEGORY_PLAYER_OR_RECORDER),
        "carsh AVRCP Controller", kSdpProvider);
    registerSdpRecord("AVRCP controller", g_sdpAvrcpController, sizeof(g_sdpAvrcpController));

    // AVRCP target: we receive Category-2 (monitor/amplifier) commands. Phones
    // look for this record too before opening AVRCP to a sink.
    memset(g_sdpAvrcpTarget, 0, sizeof(g_sdpAvrcpTarget));
    avrcp_target_create_sdp_record(
        g_sdpAvrcpTarget, sdp_create_service_record_handle(),
        uint16_t(1u << AVRCP_TARGET_SUPPORTED_FEATURE_CATEGORY_MONITOR_OR_AMPLIFIER),
        "carsh AVRCP Target", kSdpProvider);
    registerSdpRecord("AVRCP target", g_sdpAvrcpTarget, sizeof(g_sdpAvrcpTarget));

    // Device ID (PnP): identifies the Bluetooth stack (BTstack by BlueKitchen,
    // vendor id source = Bluetooth SIG) exactly as the BTstack demos do; phones
    // use it for interop quirks. Product/version 1/1.
    memset(g_sdpDeviceId, 0, sizeof(g_sdpDeviceId));
    device_id_create_sdp_record(g_sdpDeviceId, sdp_create_service_record_handle(),
                                DEVICE_ID_VENDOR_ID_SOURCE_BLUETOOTH,
                                BLUETOOTH_COMPANY_ID_BLUEKITCHEN_GMBH, 1, 1);
    registerSdpRecord("Device ID", g_sdpDeviceId, sizeof(g_sdpDeviceId));
}

}  // namespace

bool btGlueInit(CircleBluetooth* owner) {
    g_owner = owner;
    CLogger::Get()->Write(FromBt, LogNotice, "BT bring-up start (Realtek USB dongle)");
    hci_dump_init(&g_btDump);          // BTstack log_info/error -> serial
    hci_dump_enable_packet_log(false); // messages only, no per-packet dumps

    // Connection bookkeeping -> the IBluetooth callbacks (via CircleBluetooth).
    carsh::bt::LinkTracker::Callbacks linkCb;
    linkCb.onConnected = [](const carsh::bt::BdAddr& a) {
        if (g_owner) g_owner->deliverConnected(a);
    };
    linkCb.onDisconnected = [](const carsh::bt::BdAddr&) {
        if (g_owner) g_owner->deliverDisconnected();
    };
    linkCb.onConnectFailed = [](const carsh::bt::BdAddr& a, int reason) {
        CLogger::Get()->Write(FromBt, LogWarning, "connect failed (reason 0x%02x)", reason);
        if (g_owner) g_owner->deliverConnectFailed(a, reason);
    };
    g_link.setCallbacks(linkCb);

    // 1. Memory pools + embedded run loop (pumped by CircleBtRunLoopTask).
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_embedded_get_instance());

    // 2. HCI over the Circle USB transport (stage-1 dongle path). If no dongle is
    //    enumerated, transport open() fails and HCI never reaches WORKING — but
    //    the call structure below still links and runs.
    hci_init(circleHciTransportInstance(), nullptr);

    // 3. Realtek chipset: load the rtlbt firmware + config blob from SD by USB
    //    product id. The RTL8761BU (UB500, USB 0bda:8771) wants files
    //    "rtl8761bu_fw" + "rtl8761bu_config" under the folder below.
    btstack_chipset_realtek_set_firmware_folder_path(kRealtekFirmwareFolder);
    btstack_chipset_realtek_set_config_folder_path(kRealtekFirmwareFolder);
    // CRITICAL: the chipset picks the firmware blob by USB product id; without it
    // it can't find the table entry and SKIPS the firmware download (controller
    // then runs in ROM mode — HCI responds but the radio is dead: no inquiry
    // results, not discoverable). UB500 = RTL8761BU = 0bda:8771.
    // TODO(hw): read the real idProduct from the enumerated dongle for other models.
    btstack_chipset_realtek_set_product_id(0x8771);
    hci_set_chipset(btstack_chipset_realtek_instance());
    // REQUIRED for the Realtek 2-phase init: phase 1 (read LMP subversion) runs at
    // CUSTOM_PRE_INIT, then phase 2 (read ROM version + DOWNLOAD FIRMWARE) at
    // CUSTOM_INIT. Without this the pre-init phase is skipped, phase 1 runs late and
    // returns DONE, and the firmware never downloads — controller stays in ROM mode.
    hci_enable_custom_pre_init();

    // Probe the firmware path up front: this is the one unproven link — whether
    // newlib's fopen() actually reaches the mounted FatFs "SD:" volume. If this
    // fails at the bench, the chipset's own firmware load will too, and we know to
    // switch to a FatFs-backed reader instead of guessing.
    {
        const char* probe = "SD:/firmware/rtlbt/rtl8761bu_fw";
        FILE* f = std::fopen(probe, "rb");
        if (f) {
            std::fseek(f, 0, SEEK_END);
            long sz = std::ftell(f);
            std::fclose(f);
            CLogger::Get()->Write(FromBt, LogNotice, "firmware probe OK: %s (%ld bytes)",
                                  probe, sz);
        } else {
            CLogger::Get()->Write(FromBt, LogError,
                                  "firmware probe FAILED: cannot fopen %s "
                                  "(check SD path / newlib FatFs glue)", probe);
        }
    }

    // 4. L2CAP + SDP, then the A2DP sink (SBC) + AVRCP CT/TG profiles.
    l2cap_init();
    sdp_init();

    a2dp_sink_init();
    avrcp_init();
    avrcp_controller_init();
    avrcp_target_init();

    a2dp_sink_register_packet_handler(&a2dpPacketHandler);
    a2dp_sink_register_media_handler(&a2dpMediaHandler);
    avdtp_stream_endpoint_t* ep = a2dp_sink_create_stream_endpoint(
        AVDTP_AUDIO, AVDTP_CODEC_SBC,
        g_sbcCapabilities, sizeof(g_sbcCapabilities),
        g_sbcConfiguration, sizeof(g_sbcConfiguration));
    if (ep) g_localSeid = avdtp_local_seid(ep);

    // All three AVRCP handlers, as in a2dp_sink_demo.c. avrcp_register_packet_handler
    // only carries CONNECTION_ESTABLISHED/RELEASED; controller and target events go
    // through their own callbacks, which BTstack invokes without a NULL check (or
    // behind btstack_assert) — leaving them unregistered halts the board on the
    // first AVRCP notification, pass-through response or SetAbsoluteVolume.
    avrcp_register_packet_handler(&avrcpPacketHandler);
    avrcp_controller_register_packet_handler(&avrcpControllerPacketHandler);
    avrcp_target_register_packet_handler(&avrcpTargetPacketHandler);

    // 5. SDP records (before power-on, so the first query already sees them).
    registerSdpRecords();

    g_sbcDecoder = btstack_sbc_decoder_bluedroid_init_instance(&g_sbcContext);
    g_sbcDecoder->configure(&g_sbcContext, SBC_MODE_STANDARD, &sbcPcmHandler, nullptr);

    // 6. HCI connect/disconnect events + GAP inquiry results.
    g_hciEventCb.callback = &hciPacketHandler;
    hci_add_event_handler(&g_hciEventCb);

    // 7. Be visible as an audio device: the phone sees "carsh" in its Bluetooth
    //    list (discoverable) and can connect to us as an A2DP speaker (connectable).
    //    Class of device = Audio/Video major, Loudspeaker minor + Audio service so
    //    phones present carsh as a speaker. (Re-asserted on WORKING, above.)
    gap_set_local_name("carsh");
    gap_set_class_of_device(0x200414);
    gap_discoverable_control(1);
    gap_connectable_control(1);

    // 8. Power on; the controller reaches HCI_STATE_WORKING asynchronously.
    hci_power_control(HCI_POWER_ON);
    return true;
}

void btGlueProcess() {
    // Deliver any HCI events the USB IRQ queued, in THIS (task) context, so the
    // commands hci.c issues in response are sent from here — not from the USB
    // completion handler (which would deadlock the USB stack). Then pump BTstack.
    circleHciTransportProcess();
    btstack_run_loop_embedded_execute_once();
}

void btGlueSendAvrcp(int command) {
    const uint16_t cid = g_link.avrcpCid();
    if (cid == 0) return;  // no AVRCP to the connected phone
    switch (command) {  // carsh::bt::AvrcpCommand order: 0=Play,1=Pause,2=Next,3=Prev
        case 0: avrcp_controller_play(cid); break;
        case 1: avrcp_controller_pause(cid); break;
        case 2: avrcp_controller_forward(cid); break;
        case 3: avrcp_controller_backward(cid); break;
        default: break;
    }
}

void btGlueStartScan() {
    // Results arrive in hciPacketHandler as GAP_EVENT_INQUIRY_RESULT /
    // GAP_EVENT_INQUIRY_COMPLETE.
    gap_inquiry_start(5 /* * 1.28 s ≈ 6.4 s window */);
}

void btGlueStopScan() {
    gap_inquiry_stop();
}

void btGlueConnect(const unsigned char* addr6) {
    gap_inquiry_stop();  // inquiry + paging cannot run together (contract)
    bd_addr_t addr;
    for (int i = 0; i < 6; ++i) addr[i] = addr6[i];
    const carsh::bt::BdAddr a = toBdAddr(addr6);
    // BTstack v1.6.1's a2dp_sink_establish_stream() dereferences an uninitialised
    // cid (btstack_assert -> halt) when an AVDTP connection to that address already
    // exists but is not open yet (avdtp_connect returns success without setting
    // it) — e.g. a second CONNECT tap, or the phone connecting to us meanwhile.
    // If one exists, that connect is already under way (or done): nothing to do.
    if (avdtp_get_connection_for_bd_addr(addr) != nullptr) {
        CLogger::Get()->Write(FromBt, LogNotice, "connect: A2DP to %s already up or in progress",
                              bd_addr_to_str(addr));
        return;
    }
    g_link.connectRequested(a);
    uint16_t cid = 0;
    uint8_t status = a2dp_sink_establish_stream(addr, &cid);
    if (status != ERROR_CODE_SUCCESS) {
        g_link.connectRejected(a, status);  // -> deliverConnectFailed
        return;
    }
    g_link.connectStarted(a, cid);
}

void btGlueDisconnect() {
    // Drop the connected phone's ACL link. That closes A2DP *and* AVRCP (either
    // alone would leave the link — and so connected() — up), and BTstack reports
    // the channel releases + Disconnection Complete, which clear the state.
    const uint16_t handle = g_link.connectedHandle();
    if (handle != carsh::bt::LinkTracker::kNoHandle) {
        gap_disconnect(handle);
        return;
    }
    // No link yet: cancel a user connect still in flight (BTstack reports it as a
    // failed A2DP signaling connection -> onConnectFailed).
    if (uint16_t cid = g_link.pendingA2dpCid()) a2dp_sink_disconnect(cid);
}

uint32_t btGlueA2dpSampleRate() { return g_btSampleRate; }

}  // namespace carsh::device

#endif  // CARSH_HAVE_BTSTACK
