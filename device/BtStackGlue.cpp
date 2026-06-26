// BtStackGlue.cpp — the BTstack integration, entirely gated behind
// CARSH_HAVE_BTSTACK. Without the flag it provides trivial stubs so the device
// image links today. With the flag (and the BTstack source on the include path
// + the BCM/CYW firmware on SD), this becomes the A2DP sink + AVRCP CT/TG bridge.
//
// This is a SCAFFOLD with the real BTstack call structure and TODO markers — it
// is not claimed working; complete it at the bench against a controller. The
// reference is BTstack's port/raspi (UART transport + btstack_chipset_bcm
// firmware download) and example/a2dp_sink_demo.c / avrcp_*.
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
}  // namespace carsh::device

#else
// ---------------------------------------------------------------------------
// Live build: BTstack present (CARSH_HAVE_BTSTACK). Classic-only A2DP sink +
// AVRCP CT/TG over the Circle USB HCI transport, modelled on BTstack's
// example/a2dp_sink_demo.c + avrcp examples. The HCI command/event path is real;
// the A2DP **media** path depends on the USB ACL bulk transfer that Circle's
// CUSBBluetoothDevice does not yet expose (TODO(hw), see CircleHciTransport.h),
// so audio cannot actually flow until that gap is closed at the bench.
#include "CircleHciTransport.h"
#include "bt/NowPlaying.h"  // platform-independent AVRCP metadata assembly

// BTstack is C; include its headers with C linkage. Specific headers only (NOT
// the umbrella btstack.h, which drags in BLE/mesh we don't build).
extern "C" {
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_embedded.h"
#include "btstack_event.h"
#include "hci.h"
#include "gap.h"
#include "l2cap.h"
#include "classic/sdp_server.h"
#include "classic/avdtp_util.h"  // avdtp_local_seid()
#include "classic/a2dp_sink.h"
#include "classic/avrcp.h"
#include "classic/avrcp_controller.h"
#include "classic/avrcp_target.h"
#include "classic/btstack_sbc.h"
#include "classic/btstack_sbc_bluedroid.h"
#include "btstack_chipset_realtek.h"
}

namespace carsh::device {
namespace {

CircleBluetooth* g_owner = nullptr;
btstack_packet_callback_registration_t g_hciEventCb;

// Active connection identifiers, learned from BTstack connection events.
uint16_t g_a2dpCid = 0;
uint16_t g_avrcpCid = 0;
uint8_t  g_localSeid = 0;

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
void sbcPcmHandler(int16_t* data, int numFrames, int numChannels, int /*sampleRate*/,
                   void* /*ctx*/) {
    if (!g_owner) return;
    const int n = numFrames * numChannels;
    if (n <= 0 || n > (int)(sizeof(g_pcmFloat) / sizeof(g_pcmFloat[0]))) return;
    for (int i = 0; i < n; ++i) g_pcmFloat[i] = data[i] * (1.0f / 32768.0f);
    g_owner->deliverA2dpPcm(g_pcmFloat, (unsigned)numFrames, (unsigned)numChannels);
}

// A2DP media payload (one AVDTP media packet) -> SBC decoder.
void a2dpMediaHandler(uint8_t /*seid*/, uint8_t* packet, uint16_t size) {
    if (g_sbcDecoder) {
        g_sbcDecoder->decode_signed_16(&g_sbcContext, 0 /*packet_status_flag*/, packet, size);
    }
}

// A2DP control events: stream open/close. TODO(hw): on SBC config event, call
// a2dp_sink_set_config_sbc / a2dp_sink_start_stream_accept against the negotiated
// SEIDs (the demo's STREAM_ENDPOINT_CONFIGURATION_SBC handling).
void a2dpPacketHandler(uint8_t type, uint16_t, uint8_t* packet, uint16_t) {
    if (type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;
    switch (hci_event_a2dp_meta_get_subevent_code(packet)) {
        case A2DP_SUBEVENT_STREAM_ESTABLISHED:
            g_a2dpCid = a2dp_subevent_stream_established_get_a2dp_cid(packet);
            break;
        case A2DP_SUBEVENT_STREAM_RELEASED:
            g_a2dpCid = 0;
            break;
        default:
            break;
    }
}

// AVRCP controller events: connection + now-playing metadata.
void avrcpPacketHandler(uint8_t type, uint16_t, uint8_t* packet, uint16_t) {
    if (type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    using carsh::bt::AvrcpAttr;
    switch (hci_event_avrcp_meta_get_subevent_code(packet)) {
        case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED:
            g_avrcpCid = avrcp_subevent_connection_established_get_avrcp_cid(packet);
            // TODO(hw): enable track-changed / playback-status notifications and
            // request the initial now-playing info (avrcp_controller_get_now_playing_info)
            // here so the subevents below start arriving.
            break;
        case AVRCP_SUBEVENT_CONNECTION_RELEASED:
            g_avrcpCid = 0;
            g_nowPlaying.reset();
            break;
        // Now-playing attributes arrive one subevent at a time; accumulate them
        // (the value bytes are length-delimited, not NUL-terminated). The
        // assembly + duration/status mapping is host-tested in BtParse_test.
        case AVRCP_SUBEVENT_NOW_PLAYING_TITLE_INFO:
            g_nowPlaying.setAttribute(
                uint8_t(AvrcpAttr::Title),
                (const char*)avrcp_subevent_now_playing_title_info_get_value(packet),
                avrcp_subevent_now_playing_title_info_get_value_len(packet));
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_ARTIST_INFO:
            g_nowPlaying.setAttribute(
                uint8_t(AvrcpAttr::Artist),
                (const char*)avrcp_subevent_now_playing_artist_info_get_value(packet),
                avrcp_subevent_now_playing_artist_info_get_value_len(packet));
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_ALBUM_INFO:
            g_nowPlaying.setAttribute(
                uint8_t(AvrcpAttr::Album),
                (const char*)avrcp_subevent_now_playing_album_info_get_value(packet),
                avrcp_subevent_now_playing_album_info_get_value_len(packet));
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_SONG_LENGTH_MS_INFO:
            g_nowPlaying.setDurationMs(
                avrcp_subevent_now_playing_song_length_ms_info_get_song_length(packet));
            break;
        case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_STATUS_CHANGED:
            g_nowPlaying.setPlaybackStatus(
                avrcp_subevent_notification_playback_status_changed_get_play_status(packet));
            if (g_owner) g_owner->deliverMetadata(g_nowPlaying.metadata());
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_INFO_DONE:
            // All attributes for this track delivered — publish the assembled set.
            if (g_owner && g_nowPlaying.hasText()) {
                g_owner->deliverMetadata(g_nowPlaying.metadata());
            }
            break;
        default:
            break;
    }
}

// HCI events: connect/disconnect of the Classic link (the stable address
// ProfileManager keys on, R10).
void hciPacketHandler(uint8_t type, uint16_t, uint8_t* packet, uint16_t) {
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_CONNECTION_COMPLETE: {
            bd_addr_t addr;
            hci_event_connection_complete_get_bd_addr(packet, addr);
            if (g_owner) g_owner->deliverConnected(toBdAddr(addr));
            break;
        }
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            if (g_owner) g_owner->deliverDisconnected();
            break;
        default:
            break;
    }
}

}  // namespace

bool btGlueInit(CircleBluetooth* owner) {
    g_owner = owner;

    // 1. Memory pools + embedded run loop (pumped by CircleBtRunLoopTask).
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_embedded_get_instance());

    // 2. HCI over the Circle USB transport (stage-1 dongle path). If no dongle is
    //    enumerated, transport open() fails and HCI never reaches WORKING — but
    //    the call structure below still links and runs.
    hci_init(circleHciTransportInstance(), nullptr);

    // 3. Realtek chipset: load the rtlbt firmware + config blob from SD by USB
    //    product id (R3). TODO(hw): confirm the folder + that the dongle's PID has
    //    a matching blob shipped under SD:/firmware/rtlbt.
    btstack_chipset_realtek_set_firmware_folder_path(kRealtekFirmwareFolder);
    btstack_chipset_realtek_set_config_folder_path(kRealtekFirmwareFolder);
    hci_set_chipset(btstack_chipset_realtek_instance());

    // 4. L2CAP + SDP, then the A2DP sink (SBC) + AVRCP CT/TG profiles.
    l2cap_init();
    sdp_init();

    a2dp_sink_init();
    a2dp_sink_register_packet_handler(&a2dpPacketHandler);
    a2dp_sink_register_media_handler(&a2dpMediaHandler);
    avdtp_stream_endpoint_t* ep = a2dp_sink_create_stream_endpoint(
        AVDTP_AUDIO, AVDTP_CODEC_SBC,
        g_sbcCapabilities, sizeof(g_sbcCapabilities),
        g_sbcConfiguration, sizeof(g_sbcConfiguration));
    if (ep) g_localSeid = avdtp_local_seid(ep);

    avrcp_init();
    avrcp_controller_init();
    avrcp_target_init();
    avrcp_register_packet_handler(&avrcpPacketHandler);

    g_sbcDecoder = btstack_sbc_decoder_bluedroid_init_instance(&g_sbcContext);
    g_sbcDecoder->configure(&g_sbcContext, SBC_MODE_STANDARD, &sbcPcmHandler, nullptr);

    // 5. HCI connect/disconnect events.
    g_hciEventCb.callback = &hciPacketHandler;
    hci_add_event_handler(&g_hciEventCb);

    // 6. Power on; the controller reaches HCI_STATE_WORKING asynchronously.
    hci_power_control(HCI_POWER_ON);
    return true;
}

void btGlueProcess() {
    btstack_run_loop_embedded_execute_once();
}

void btGlueSendAvrcp(int command) {
    if (g_avrcpCid == 0) return;  // no controller connection yet
    switch (command) {  // carsh::bt::AvrcpCommand order: 0=Play,1=Pause,2=Next,3=Prev
        case 0: avrcp_controller_play(g_avrcpCid); break;
        case 1: avrcp_controller_pause(g_avrcpCid); break;
        case 2: avrcp_controller_forward(g_avrcpCid); break;
        case 3: avrcp_controller_backward(g_avrcpCid); break;
        default: break;
    }
}

void btGlueStartScan() {
    gap_inquiry_start(5 /* * 1.28 s ≈ 6.4 s window */);
    // GAP inquiry results are delivered to hciPacketHandler as GAP_EVENT_*; the
    // parse into carsh::bt::DiscoveredDevice + deliverDeviceDiscovered is wired
    // alongside the host-tested parser (TODO(hw): enable the GAP event branch).
}

void btGlueStopScan() {
    gap_inquiry_stop();
}

void btGlueConnect(const unsigned char* addr6) {
    gap_inquiry_stop();  // inquiry + paging cannot run together (contract)
    bd_addr_t addr;
    for (int i = 0; i < 6; ++i) addr[i] = addr6[i];
    uint8_t status = a2dp_sink_establish_stream(addr, &g_a2dpCid);
    if (status != ERROR_CODE_SUCCESS && g_owner) {
        g_owner->deliverConnectFailed(toBdAddr(addr6), status);
    }
}

void btGlueDisconnect() {
    if (g_a2dpCid) a2dp_sink_disconnect(g_a2dpCid);
}

}  // namespace carsh::device

#endif  // CARSH_HAVE_BTSTACK
