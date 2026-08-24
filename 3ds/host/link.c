// Local wireless: the GBA cable link carried over 3DS UDS.
//
// A cable is a synchronous shared bus. Every console's frame is gated on the
// master's transfer, so four GBAs cannot drift apart. UDS gives us none of
// that, so lockstep has to be built here, and it is the whole difficulty of
// this file.
//
// The scheme is a one-frame jitter buffer plus a bounded wait:
//
//   frame N   send our command tagged N, then wait for every peer's N-1.
//
// Running a frame behind absorbs ordinary jitter without stalling. When a peer
// is later than that we block in udsWaitDataAvailable up to LINK_WAIT_US and
// then give up for this frame; the caller turns that into the game's own lag
// path rather than an error. Blocking is deliberate: it is what a GBA does
// while waiting on the cable, and the alternative is silent desync.
//
// Not attempted: the RFU / Union Room stack. The game already reports no
// wireless adapter, so it does not offer the Wireless Club, and this makes the
// Cable Club work instead.

#include <3ds.h>
#include <string.h>
#include <stdio.h>

#include "../bridge.h"
#include "trace.h"

// Private to this port, so only Emerald3DS builds see each other. Nothing here
// is compatible with a real cartridge.
#define LINK_WLANCOMM_ID  0x454D3344u   // 'EM3D'
#define LINK_ID8          0
#define LINK_CHANNEL      1
#define LINK_PASSPHRASE   "emerald3ds-link-v1"

#define LINK_SHAREDMEM    0x3000
#define LINK_RECVBUF      UDS_DEFAULT_RECVBUFSIZE
#define LINK_SCAN_BUFSZ   0x4000
#define LINK_MAX_SCAN     8

// Half a frame. Long enough to absorb a late packet, short enough that a dead
// peer costs visible slowdown rather than a hang.
#define LINK_WAIT_US      8000

// Tagged with the sender's frame so a late or duplicated packet can be placed
// rather than guessed at.
typedef struct {
    uint32_t frame;
    uint8_t  cmd[CTR_LINK_CMD_BYTES];
} LinkPacket;

static int  sUdsUp;
static int  sState = CTR_LINK_IDLE;
static int  sIsHost;

static udsNetworkStruct sNetwork;
static udsBindContext   sBind;

static uint32_t sFrame;                                  // our own counter
static LinkPacket sLatest[CTR_LINK_MAX_PLAYERS];         // newest per player
static int        sHave[CTR_LINK_MAX_PLAYERS];

// Scan results, kept so the UI can list them across frames.
static udsNetworkScanInfo sScan[LINK_MAX_SCAN];
static char               sScanName[LINK_MAX_SCAN][CTR_LINK_NAME_LEN];
static int                sScanCount;

// ---------------------------------------------------------------- helpers ---

static int ensure_uds(void)
{
    if (sUdsUp)
        return 1;

    // The username is what other consoles see in the beacon. Emerald's own
    // trainer name is game-side and this file must not reach for it, so the
    // console's own name is used instead.
    if (R_FAILED(udsInit(LINK_SHAREDMEM, NULL))) {
        CtrTrace("link: udsInit failed\n");
        return 0;
    }

    sUdsUp = 1;
    return 1;
}

static void reset_frames(void)
{
    sFrame = 0;
    memset(sHave, 0, sizeof(sHave));
    memset(sLatest, 0, sizeof(sLatest));
}

// UDS node ids are 1-based with the host at 1; the GBA's are 0-based with the
// master at 0. Everything above this file speaks the GBA's numbering.
static int node_to_player(u16 nodeId)
{
    return (int)nodeId - 1;
}

// ---------------------------------------------------------------- pairing ---

void Ctr3dsLinkHost(void)
{
    Ctr3dsLinkStop();
    if (!ensure_uds())
        return;

    udsGenerateDefaultNetworkStruct(&sNetwork, LINK_WLANCOMM_ID, LINK_ID8,
                                    CTR_LINK_MAX_PLAYERS);

    if (R_FAILED(udsCreateNetwork(&sNetwork, LINK_PASSPHRASE,
                                  sizeof(LINK_PASSPHRASE), &sBind,
                                  LINK_CHANNEL, LINK_RECVBUF))) {
        CtrTrace("link: udsCreateNetwork failed\n");
        sState = CTR_LINK_FAILED;
        return;
    }

    reset_frames();
    sIsHost = 1;
    sState  = CTR_LINK_HOSTING;
    CtrTrace("link: hosting\n");
}

void Ctr3dsLinkScan(void)
{
    static uint8_t buf[LINK_SCAN_BUFSZ] __attribute__((aligned(4)));
    udsNetworkScanInfo *nets = NULL;
    size_t total = 0;

    if (!ensure_uds())
        return;

    sScanCount = 0;
    sState = CTR_LINK_SCANNING;

    if (R_FAILED(udsScanBeacons(buf, sizeof(buf), &nets, &total,
                                LINK_WLANCOMM_ID, LINK_ID8, NULL, false))) {
        CtrTrace("link: udsScanBeacons failed\n");
        sState = CTR_LINK_IDLE;
        return;
    }

    for (size_t i = 0; i < total && sScanCount < LINK_MAX_SCAN; i++) {
        udsNodeInfo *host = &nets[i].nodes[0];

        sScan[sScanCount] = nets[i];

        // The node's username is UTF-16; the UI wants plain ASCII.
        {
            char *dst = sScanName[sScanCount];
            int n = 0;
            for (; n < CTR_LINK_NAME_LEN - 1; n++) {
                u16 c = host->username[n];
                if (c == 0)
                    break;
                dst[n] = (c < 0x20 || c > 0x7E) ? '?' : (char)c;
            }
            dst[n] = '\0';
            if (n == 0)
                snprintf(dst, CTR_LINK_NAME_LEN, "GAME %d", (int)i + 1);
        }

        sScanCount++;
    }

    sState = CTR_LINK_IDLE;
    CtrTrace("link: scan found %d\n", sScanCount);
}

int Ctr3dsLinkScanCount(void)
{
    return sScanCount;
}

void Ctr3dsLinkScanName(int index, char *out, int outSize)
{
    if (out == NULL || outSize <= 0)
        return;

    out[0] = '\0';
    if (index < 0 || index >= sScanCount)
        return;

    snprintf(out, (size_t)outSize, "%s", sScanName[index]);
}

int Ctr3dsLinkScanPlayers(int index)
{
    if (index < 0 || index >= sScanCount)
        return 0;

    return sScan[index].network.total_nodes;
}

void Ctr3dsLinkJoin(int index)
{
    if (index < 0 || index >= sScanCount)
        return;
    if (!ensure_uds())
        return;

    sState = CTR_LINK_JOINING;

    if (R_FAILED(udsConnectNetwork(&sScan[index].network, LINK_PASSPHRASE,
                                   sizeof(LINK_PASSPHRASE), &sBind,
                                   UDS_BROADCAST_NETWORKNODEID,
                                   UDSCONTYPE_Client, LINK_CHANNEL,
                                   LINK_RECVBUF))) {
        CtrTrace("link: udsConnectNetwork failed\n");
        sState = CTR_LINK_FAILED;
        return;
    }

    reset_frames();
    sIsHost = 0;
    sState  = CTR_LINK_CONNECTED;
    CtrTrace("link: joined\n");
}

void Ctr3dsLinkStop(void)
{
    if (sState == CTR_LINK_HOSTING || sState == CTR_LINK_CONNECTED ||
        sState == CTR_LINK_JOINING) {
        udsUnbind(&sBind);
        if (sIsHost)
            udsDestroyNetwork();
        else
            udsDisconnectNetwork();
    }

    reset_frames();
    sIsHost = 0;
    sState  = CTR_LINK_IDLE;
}

void Ctr3dsLinkGetStatus(CtrLinkStatus *out)
{
    udsConnectionStatus st;

    if (out == NULL)
        return;

    out->state       = (uint8_t)sState;
    out->playerCount = 1;
    out->localId     = 0;
    out->isHost      = (uint8_t)sIsHost;

    if (sState != CTR_LINK_HOSTING && sState != CTR_LINK_CONNECTED)
        return;

    if (R_FAILED(udsGetConnectionStatus(&st)))
        return;

    {
        int players = st.total_nodes;
        int local   = node_to_player(st.cur_NetworkNodeID);

        if (players > CTR_LINK_MAX_PLAYERS)
            players = CTR_LINK_MAX_PLAYERS;
        if (players < 1)
            players = 1;
        if (local < 0 || local >= CTR_LINK_MAX_PLAYERS)
            local = 0;

        out->playerCount = (uint8_t)players;
        out->localId     = (uint8_t)local;

        // Hosting alone is not yet a link; the game must not be told it has a
        // partner until one is actually present.
        if (players > 1)
            sState = CTR_LINK_CONNECTED;
        else if (sIsHost)
            sState = CTR_LINK_HOSTING;

        out->state = (uint8_t)sState;
    }
}

// Scalar view for src/link.c, which must not see CtrLinkStatus.
int Ctr3dsLinkIsConnected(void)
{
    CtrLinkStatus st;

    Ctr3dsLinkGetStatus(&st);
    return st.state == CTR_LINK_CONNECTED && st.playerCount >= 2;
}

int Ctr3dsLinkPlayerCount(void)
{
    CtrLinkStatus st;

    Ctr3dsLinkGetStatus(&st);
    return st.playerCount;
}

int Ctr3dsLinkLocalId(void)
{
    CtrLinkStatus st;

    Ctr3dsLinkGetStatus(&st);
    return st.localId;
}

// -------------------------------------------------------------- transport ---

// Move everything waiting in the receive buffer into the per-player slots.
// Returns how many packets were taken.
static int drain(void)
{
    LinkPacket pkt;
    size_t got = 0;
    u16 src = 0;
    int n = 0;

    while (R_SUCCEEDED(udsPullPacket(&sBind, &pkt, sizeof(pkt), &got, &src))
           && got == sizeof(pkt)) {
        int p = node_to_player(src);

        if (p >= 0 && p < CTR_LINK_MAX_PLAYERS) {
            // Keep the newest; an out-of-order duplicate must not go backwards.
            if (!sHave[p] || pkt.frame >= sLatest[p].frame) {
                sLatest[p] = pkt;
                sHave[p] = 1;
            }
        }
        n++;
        got = 0;
    }

    return n;
}

// Has every peer reached `target`? Our own slot is never waited on.
static int peers_ready(uint32_t target, int players, int local)
{
    for (int p = 0; p < players; p++) {
        if (p == local)
            continue;
        if (!sHave[p] || sLatest[p].frame < target)
            return 0;
    }

    return 1;
}

int Ctr3dsLinkExchange(const void *sendCmd, void *recvCmds)
{
    CtrLinkStatus st;
    LinkPacket out;
    uint8_t *dst = (uint8_t *)recvCmds;
    uint64_t deadline;
    uint32_t target;
    int ready;

    memset(recvCmds, 0, CTR_LINK_MAX_PLAYERS * CTR_LINK_CMD_BYTES);

    Ctr3dsLinkGetStatus(&st);
    if (st.state != CTR_LINK_CONNECTED || st.playerCount < 2)
        return 0;

    out.frame = sFrame;
    memcpy(out.cmd, sendCmd, CTR_LINK_CMD_BYTES);

    if (R_FAILED(udsSendTo(UDS_BROADCAST_NETWORKNODEID, LINK_CHANNEL,
                           UDS_SENDFLAG_Default, &out, sizeof(out)))) {
        // A failed send is a dropped frame, not a dead link: the caller
        // reports lag and we try again next frame.
        sFrame++;
        return 0;
    }

    // One frame behind. Frame 0 has nothing to wait for yet.
    target = (sFrame == 0) ? 0 : sFrame - 1;

    drain();
    ready = peers_ready(target, st.playerCount, st.localId);

    // Bounded block. This stall is the lockstep.
    deadline = svcGetSystemTick() + (uint64_t)(LINK_WAIT_US * (SYSCLOCK_ARM11 / 1000000));
    while (!ready && svcGetSystemTick() < deadline) {
        udsWaitDataAvailable(&sBind, false, false);
        drain();
        ready = peers_ready(target, st.playerCount, st.localId);
    }

    for (int p = 0; p < st.playerCount; p++) {
        if (p == st.localId)
            memcpy(dst + p * CTR_LINK_CMD_BYTES, sendCmd, CTR_LINK_CMD_BYTES);
        else if (sHave[p])
            memcpy(dst + p * CTR_LINK_CMD_BYTES, sLatest[p].cmd, CTR_LINK_CMD_BYTES);
    }

    sFrame++;
    return ready;
}

void CtrLinkExit(void)
{
    Ctr3dsLinkStop();

    if (sUdsUp) {
        udsExit();
        sUdsUp = 0;
    }
}
