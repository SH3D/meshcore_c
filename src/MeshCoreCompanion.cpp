/*
 * MeshCoreCompanion.cpp
 * SPDX-License-Identifier: MIT
 */
#include "MeshCoreCompanion.h"

void MeshCoreCompanion::begin(bool sendHandshake) {
    mc_rx_init(&_rx);
    _draining = false;
    if (sendHandshake) {
        appStart();        /* triggers SelfInfo */
        deviceQuery();     /* triggers DeviceInfo */
    }
}

void MeshCoreCompanion::sendPayload(const uint8_t *payload, size_t len) {
    if (len == 0) return;
    size_t flen = mc_frame_encode(payload, len, _frame, sizeof(_frame));
    if (flen) _io.write(_frame, flen);
}

void MeshCoreCompanion::loop() {
    uint8_t tmp[64];
    int avail;
    while ((avail = _io.available()) > 0) {
        size_t want = (avail < (int)sizeof(tmp)) ? (size_t)avail : sizeof(tmp);
        size_t n = _io.readBytes(tmp, want);
        if (n == 0) break;
        mc_rx_feed(&_rx, tmp, n);
    }
    size_t olen;
    while (mc_rx_poll(&_rx, _scratch, sizeof(_scratch), &olen)) {
        mc_event_t ev;
        if (mc_parse(_scratch, olen, &ev)) dispatch(ev);
    }
}

uint32_t MeshCoreCompanion::deviceEpochNow() const {
    if (!_haveTime) return 0;
    return _epochBase + (uint32_t)((millis() - _millisBase) / 1000UL);
}

/* ---- commands ---- */
void MeshCoreCompanion::appStart(const char *name) {
    uint8_t p[1 + 1 + 6 + 32];
    size_t n = mc_cmd_app_start(p, sizeof(p), name);
    sendPayload(p, n);
}

void MeshCoreCompanion::deviceQuery(uint8_t appTargetVer) {
    uint8_t p[2]; sendPayload(p, mc_cmd_device_query(p, sizeof(p), appTargetVer));
}

void MeshCoreCompanion::getDeviceTime() {
    uint8_t p[1]; sendPayload(p, mc_cmd_get_device_time(p, sizeof(p)));
}

void MeshCoreCompanion::setDeviceTime(uint32_t epochSecs) {
    uint8_t p[5]; sendPayload(p, mc_cmd_set_device_time(p, sizeof(p), epochSecs));
    /* optimistically track it locally too */
    _epochBase = epochSecs; _millisBase = millis(); _haveTime = true;
}

void MeshCoreCompanion::sendSelfAdvert(bool flood) {
    uint8_t p[2];
    sendPayload(p, mc_cmd_send_self_advert(p, sizeof(p), flood ? MC_ADVERT_FLOOD : MC_ADVERT_ZERO_HOP));
}

void MeshCoreCompanion::getChannel(uint8_t idx) {
    uint8_t p[2]; sendPayload(p, mc_cmd_get_channel(p, sizeof(p), idx));
}

void MeshCoreCompanion::setChannel(uint8_t idx, const char *name, const uint8_t secret[MC_SECRET_LEN]) {
    uint8_t p[2 + MC_NAME_LEN + MC_SECRET_LEN];
    sendPayload(p, mc_cmd_set_channel(p, sizeof(p), idx, name, secret));
}

bool MeshCoreCompanion::setChannelHexSecret(uint8_t idx, const char *name, const char *hex32) {
    if (!hex32) return false;
    uint8_t secret[MC_SECRET_LEN];
    for (int i = 0; i < MC_SECRET_LEN; i++) {
        char hi = hex32[i * 2], lo = hex32[i * 2 + 1];
        if (!hi || !lo) return false;
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int h = nib(hi), l = nib(lo);
        if (h < 0 || l < 0) return false;
        secret[i] = (uint8_t)((h << 4) | l);
    }
    setChannel(idx, name, secret);
    return true;
}

void MeshCoreCompanion::sendChannelText(uint8_t idx, const char *text, uint32_t senderTs) {
    if (senderTs == 0) senderTs = deviceEpochNow();   /* 0 if unknown */
    uint8_t p[1 + 1 + 1 + 4 + MC_MAX_TEXT];
    size_t n = mc_cmd_send_channel_text(p, sizeof(p), MC_TXT_PLAIN, idx, senderTs, text);
    sendPayload(p, n);
}

void MeshCoreCompanion::sendTextMessage(const uint8_t *dst, size_t dstLen, const char *text, uint32_t senderTs) {
    if (senderTs == 0) senderTs = deviceEpochNow();
    uint8_t p[1 + 1 + 1 + 4 + 32 + MC_MAX_TEXT];
    size_t n = mc_cmd_send_txt_msg(p, sizeof(p), MC_TXT_PLAIN, 0, senderTs, dst, dstLen, text);
    sendPayload(p, n);
}

void MeshCoreCompanion::sendCommand(const uint8_t *dst, size_t dstLen, const char *cmd, uint32_t senderTs) {
    if (senderTs == 0) senderTs = deviceEpochNow();
    uint8_t p[1 + 1 + 1 + 4 + 32 + MC_MAX_TEXT];
    size_t n = mc_cmd_send_cmd(p, sizeof(p), senderTs, dst, dstLen, cmd);
    sendPayload(p, n);
}

void MeshCoreCompanion::syncNextMessage() {
    uint8_t p[1]; sendPayload(p, mc_cmd_sync_next_message(p, sizeof(p)));
}

void MeshCoreCompanion::drainMessages() {
    if (!_draining) { _draining = true; syncNextMessage(); }
}

void MeshCoreCompanion::getStats(uint8_t statsType) {
    uint8_t p[2]; sendPayload(p, mc_cmd_get_stats(p, sizeof(p), statsType));
}

/* ---- dispatch ---- */
void MeshCoreCompanion::dispatch(const mc_event_t &ev) {
    if (_onEvent) _onEvent(ev);

    switch (ev.code) {
    case MC_RESP_CURR_TIME:
        _epochBase = ev.u.curr_time; _millisBase = millis(); _haveTime = true;
        break;
    case MC_RESP_DEVICE_INFO:
        if (_onDevInfo) _onDevInfo(ev.u.device_info);
        break;
    case MC_RESP_SELF_INFO:
        if (_onSelfInfo) _onSelfInfo(ev.u.self_info);
        break;
    case MC_RESP_CHANNEL_INFO:
        if (_onChanInfo) _onChanInfo(ev.u.channel_info);
        break;
    case MC_RESP_SENT:
        if (_onMsgSent) _onMsgSent(ev.u.msg_sent);
        break;
    case MC_RESP_STATS:
        if (_onStats) _onStats(ev.u.stats);
        break;
    case MC_PUSH_MSG_WAITING:
        if (_autoSync && !_draining) { _draining = true; syncNextMessage(); }
        break;
    case MC_RESP_CHANNEL_MSG_RECV:
    case MC_RESP_CHANNEL_MSG_RECV_V3:
        if (_onText) _onText(ev.u.channel_msg);
        if (_draining) syncNextMessage();
        break;
    case MC_RESP_CHANNEL_DATA_RECV:
        if (_onData) _onData(ev.u.channel_data);
        if (_draining) syncNextMessage();
        break;
    case MC_RESP_CONTACT_MSG_RECV:
    case MC_RESP_CONTACT_MSG_RECV_V3:
        if (_onContact) _onContact(ev.u.contact_msg);
        if (_draining) syncNextMessage();
        break;
    case MC_RESP_NO_MORE_MESSAGES:
        _draining = false;
        break;
    default:
        break;
    }
}
