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
        else if (_onUnparsed) _onUnparsed(_scratch, olen);   /* frame we couldn't decode */
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
    // Always (re)issue a sync — recovers a wedged drain (a lost reply leaving
    // _draining stuck true), not only when idle.  Safe to call periodically:
    // the radio answers each sync with the next queued message or NoMoreMessages.
    _draining = true;
    syncNextMessage();
}

void MeshCoreCompanion::getStats(uint8_t statsType) {
    uint8_t p[2]; sendPayload(p, mc_cmd_get_stats(p, sizeof(p), statsType));
}

/* ---- provisioning ---- */
void MeshCoreCompanion::setAdvertName(const char *name) {
    uint8_t p[1 + MC_MAX_TEXT];
    sendPayload(p, mc_cmd_set_advert_name(p, sizeof(p), name));
}
void MeshCoreCompanion::setRadioParams(float freqMHz, float bwKHz, uint8_t sf, uint8_t cr) {
    /* wire units: freq = MHz*1000 (kHz), bw = kHz*1000 (Hz) */
    uint32_t freq = (uint32_t)(freqMHz * 1000.0f + 0.5f);
    uint32_t bw   = (uint32_t)(bwKHz   * 1000.0f + 0.5f);
    uint8_t p[11];
    sendPayload(p, mc_cmd_set_radio_params(p, sizeof(p), freq, bw, sf, cr));
}
void MeshCoreCompanion::setTxPower(uint32_t dbm) {
    uint8_t p[5]; sendPayload(p, mc_cmd_set_tx_power(p, sizeof(p), dbm));
}

/* ---- contacts ---- */
void MeshCoreCompanion::getContacts(uint32_t sinceLastmod) {
    uint8_t p[5]; sendPayload(p, mc_cmd_get_contacts(p, sizeof(p), sinceLastmod));
}
void MeshCoreCompanion::addUpdateContact(const mc_contact_t &c) {
    uint8_t p[1 + 32 + 1 + 1 + 1 + 64 + 32 + 4 + 4 + 4];
    sendPayload(p, mc_cmd_add_update_contact(p, sizeof(p), &c));
}
void MeshCoreCompanion::removeContact(const uint8_t pubkey[32]) {
    uint8_t p[33]; sendPayload(p, mc_cmd_remove_contact(p, sizeof(p), pubkey));
}
void MeshCoreCompanion::resetPath(const uint8_t pubkey[32]) {
    uint8_t p[33]; sendPayload(p, mc_cmd_reset_path(p, sizeof(p), pubkey));
}
void MeshCoreCompanion::shareContact(const uint8_t pubkey[32]) {
    uint8_t p[33]; sendPayload(p, mc_cmd_share_contact(p, sizeof(p), pubkey));
}
void MeshCoreCompanion::getContactByKey(const uint8_t pubkey[32]) {
    uint8_t p[33]; sendPayload(p, mc_cmd_get_contact_by_key(p, sizeof(p), pubkey));
}
void MeshCoreCompanion::exportContact(const uint8_t pubkey[32]) {
    uint8_t p[33]; sendPayload(p, mc_cmd_export_contact(p, sizeof(p), pubkey));
}
void MeshCoreCompanion::importContact(const uint8_t *card, size_t len) {
    uint8_t p[MC_MAX_PAYLOAD]; sendPayload(p, mc_cmd_import_contact(p, sizeof(p), card, len));
}

/* ---- binary / anonymous requests ---- */
void MeshCoreCompanion::sendBinaryReq(const uint8_t dst[32], uint8_t reqType,
                                      const uint8_t *data, size_t len) {
    uint8_t p[MC_MAX_PAYLOAD];
    sendPayload(p, mc_cmd_send_binary_req(p, sizeof(p), dst, reqType, data, len));
}
void MeshCoreCompanion::sendAnonReq(const uint8_t dst[32], uint8_t reqType,
                                    const uint8_t *data, size_t len) {
    uint8_t p[MC_MAX_PAYLOAD];
    sendPayload(p, mc_cmd_send_anon_req(p, sizeof(p), dst, reqType, data, len));
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
    case MC_RESP_CONTACT:
    case MC_PUSH_NEW_ADVERT:
        if (_onContact2) _onContact2(ev.u.contact);
        break;
    case MC_RESP_END_OF_CONTACTS:
        if (_onContactsDone) _onContactsDone(ev.u.contacts_lastmod);
        break;
    case MC_PUSH_BINARY_RESP:
        if (_onBinaryResp) _onBinaryResp(ev.u.binary_resp);
        break;
    case MC_PUSH_MSG_WAITING:
        // Always (re)start the drain on a MsgWaiting push.  If a prior sync
        // reply was lost (e.g. the host stalled and its UART RX overflowed),
        // _draining can be left stuck true; gating on !_draining would then
        // ignore every later push and silently stop pulling messages.
        // Re-kicking unconditionally self-heals that.
        if (_autoSync) { _draining = true; syncNextMessage(); }
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
