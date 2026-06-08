/*
 * MeshCoreCompanion.h
 *
 * Arduino/C++ convenience wrapper over the portable meshcore_companion C core.
 * Inject any Stream (Serial1 on a Grove UART, USB CDC, SoftwareSerial, ...),
 * call loop() often, and register lambda callbacks.
 *
 * The wrapper auto-drains the radio's message queue: when the radio sends the
 * MsgWaiting push it transparently issues SyncNextMessage repeatedly until the
 * queue is empty, delivering each message through onChannelMessage / onChannelData.
 *
 * SPDX-License-Identifier: MIT
 * Author: Scott Penrose / Digital Dimensions.
 */
#ifndef MESHCORE_COMPANION_HPP
#define MESHCORE_COMPANION_HPP

#include <Arduino.h>
#include <functional>
#include "meshcore_companion.h"

class MeshCoreCompanion {
public:
    using TextMsgCb     = std::function<void(const mc_channel_msg_t&)>;
    using DataMsgCb     = std::function<void(const mc_channel_data_t&)>;
    using ContactMsgCb  = std::function<void(const mc_contact_msg_t&)>;
    using DeviceInfoCb  = std::function<void(const mc_device_info_t&)>;
    using ChannelInfoCb = std::function<void(const mc_channel_info_t&)>;
    using SelfInfoCb     = std::function<void(const mc_self_info_t&)>;
    using MsgSentCb      = std::function<void(const mc_msg_sent_t&)>;
    using StatsCb        = std::function<void(const mc_stats_t&)>;
    using EventCb        = std::function<void(const mc_event_t&)>;

    explicit MeshCoreCompanion(Stream &io) : _io(io) {}

    /* Reset the receiver and (by default) send AppStart + DeviceQuery so the
     * radio reports SelfInfo and DeviceInfo. */
    void begin(bool sendHandshake = true);

    /* Pump: read available serial bytes, decode and dispatch frames.
     * Call this every loop iteration. Non-blocking. */
    void loop();

    /* ---- commands (fire-and-forget; replies arrive via callbacks) ---- */
    void appStart(const char *name = "esp32");
    void deviceQuery(uint8_t appTargetVer = 1);
    void getDeviceTime();
    void setDeviceTime(uint32_t epochSecs);
    void sendSelfAdvert(bool flood = true);
    void getChannel(uint8_t idx);
    void setChannel(uint8_t idx, const char *name, const uint8_t secret[MC_SECRET_LEN]);
    /* Set a channel using a 32-hex-char PSK string (-> 16 bytes). false if malformed. */
    bool setChannelHexSecret(uint8_t idx, const char *name, const char *hex32);
    /* senderTs == 0 uses the tracked device time if known, else 0. */
    void sendChannelText(uint8_t idx, const char *text, uint32_t senderTs = 0);
    /* Direct message / CLI command to a contact (dst = pubkey or 6-byte prefix).
     * senderTs == 0 uses the tracked device time if known. */
    void sendTextMessage(const uint8_t *dst, size_t dstLen, const char *text, uint32_t senderTs = 0);
    void sendCommand(const uint8_t *dst, size_t dstLen, const char *cmd, uint32_t senderTs = 0);
    void syncNextMessage();
    void drainMessages();              /* start a manual sync-drain loop */
    void getStats(uint8_t statsType);

    /* ---- behaviour ---- */
    void setAutoSync(bool on) { _autoSync = on; }

    /* ---- device time (valid after a CurrTime response) ---- */
    bool     haveDeviceTime() const { return _haveTime; }
    uint32_t deviceEpochNow() const;

    /* ---- callbacks ---- */
    void onChannelMessage(TextMsgCb cb)   { _onText = cb; }
    void onChannelData(DataMsgCb cb)      { _onData = cb; }
    void onContactMessage(ContactMsgCb cb){ _onContact = cb; }
    void onDeviceInfo(DeviceInfoCb cb)    { _onDevInfo = cb; }
    void onChannelInfo(ChannelInfoCb cb)  { _onChanInfo = cb; }
    void onSelfInfo(SelfInfoCb cb)        { _onSelfInfo = cb; }
    void onMsgSent(MsgSentCb cb)          { _onMsgSent = cb; }
    void onStats(StatsCb cb)              { _onStats = cb; }
    void onEvent(EventCb cb)              { _onEvent = cb; }   /* every parsed frame */

private:
    void sendPayload(const uint8_t *payload, size_t len);
    void dispatch(const mc_event_t &ev);

    Stream &_io;
    mc_rx_t _rx;
    uint8_t _scratch[MC_RX_BUFSZ];
    uint8_t _frame[MC_RX_BUFSZ + 3];

    bool _autoSync = true;
    bool _draining = false;

    bool     _haveTime  = false;
    uint32_t _epochBase = 0;
    uint32_t _millisBase = 0;

    TextMsgCb     _onText;
    DataMsgCb     _onData;
    ContactMsgCb  _onContact;
    DeviceInfoCb  _onDevInfo;
    ChannelInfoCb _onChanInfo;
    SelfInfoCb    _onSelfInfo;
    MsgSentCb     _onMsgSent;
    StatsCb       _onStats;
    EventCb       _onEvent;
};

#endif /* MESHCORE_COMPANION_HPP */
