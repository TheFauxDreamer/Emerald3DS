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
// is later than that we wait on the bind event up to LINK_WAIT_US and then give
// up for this frame; the caller turns that into the game's own lag path rather
// than an error. Waiting is deliberate: it is what a GBA does while waiting on
// the cable, and the alternative is silent desync.
//
// Two threads:
// - The main thread runs the transport, which is short and must not block.
// - A worker runs the pairing calls. udsScanBeacons, udsConnectNetwork and
//   udsCreateNetwork block for 100 ms to more than a second. They used to run
//   in the touch handler, which is inside the frame loop, so a tap on SCAN
//   stopped the game and the sound for that time. A base 3DS log showed
//   `slow bottom.update 1213 ms`.
//
// The rule that keeps the two apart: while sBusy is set, the worker owns UDS
// and the main thread makes no UDS call at all. So there is no lock on the
// per-frame path, and the main thread never waits for a beacon sweep.
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

#define TICKS_PER_US      (SYSCLOCK_ARM11 / 1000000)
#define LINK_WAIT_TICKS   ((uint64_t)LINK_WAIT_US * TICKS_PER_US)

// One udsGetConnectionStatus for each frame at most, however many callers ask.
// The pump alone asked four times a frame and the LINK page asked twice more.
#define STATUS_MAX_AGE_TICKS ((uint64_t)8000 * TICKS_PER_US)

// Space for the UDS calls and snprintf. The scan buffer is a static, not a
// local.
#define LINK_STACK_SIZE   (16 * 1024)

// Tagged with the sender's frame so a late or duplicated packet can be placed
// rather than guessed at.
typedef struct {
    uint32_t frame;
    uint8_t  cmd[CTR_LINK_CMD_BYTES];
} LinkPacket;

// ---- state -----------------------------------------------------------------
//
// Everything below the lock is published: the worker writes it while it holds
// sLock, and every reader takes sLock. sBind is the exception that proves the
// rule. The main thread reads it on the per-frame path with no lock, which is
// safe only because the worker writes it while sBusy is set and the main thread
// touches no UDS while sBusy is set.

static LightLock sLock;
static int       sLockReady;

// The lock is made on first use, not at boot, because a session that never
// links must not pay for one. Always from the main thread: the first call is
// from the LINK page or from the game's pump, and the worker is made after it.
static void ensure_lock(void)
{
    if (!sLockReady) {
        LightLock_Init(&sLock);
        sLockReady = 1;
    }
}

static int sUdsUp;                                       // worker thread only
static int sState = CTR_LINK_IDLE;
static int sIsHost;

static udsNetworkStruct sNetwork;                        // worker thread only
static udsBindContext   sBind;

static uint32_t sFrame;                                  // our own counter
static LinkPacket sLatest[CTR_LINK_MAX_PLAYERS];         // newest per player
static int        sHave[CTR_LINK_MAX_PLAYERS];

// Scan results, kept so the UI can list them across frames.
static udsNetworkScanInfo sScan[LINK_MAX_SCAN];
static char               sScanName[LINK_MAX_SCAN][CTR_LINK_NAME_LEN];
static int                sScanCount;

// The cached connection status, and when it was taken.
static CtrLinkStatus sStatus = { CTR_LINK_IDLE, 1, 0, 0 };
static uint64_t      sStatusStamp;

// ---- the worker ------------------------------------------------------------

enum { REQ_NONE = 0, REQ_HOST, REQ_SCAN, REQ_JOIN, REQ_STOP };

static Thread       sWorker;
static LightEvent   sWake;
static volatile int sWorkerUp;
static volatile int sWorkerQuit;
static volatile int sBusy;
static volatile int sReq;
static volatile int sReqArg;
static int          sPrevState;   // what to put back when a request ends

// What the worker did, for the main thread to log. The worker must not call
// CtrLog: its queue has no lock and belongs to the main thread (3ds/host/log.c).
// One slot is enough, because only one request runs at a time and the main
// thread drains this at least thirty times a second.
enum { RPT_NONE = 0, RPT_UDS_FAIL, RPT_HOST, RPT_SCAN, RPT_JOIN, RPT_STOP };
static volatile int      sReport;
static volatile uint32_t sReportA, sReportB;

static void report(int kind, uint32_t a, uint32_t b)
{
    sReportA = a;
    sReportB = b;
    __dmb();
    sReport = kind;
}

// Main thread only.
static void drain_report(void)
{
    int kind = sReport;
    uint32_t a, b;

    if (kind == RPT_NONE)
        return;

    __dmb();
    a = sReportA;
    b = sReportB;
    sReport = RPT_NONE;

    switch (kind) {
    case RPT_UDS_FAIL:
        CtrLog("emerald3ds: link udsInit failed rc=0x%08lX\n", (unsigned long)a);
        break;
    case RPT_HOST:
        if (a == 0)
            CtrLog("emerald3ds: link hosting\n");
        else
            CtrLog("emerald3ds: link host failed rc=0x%08lX\n", (unsigned long)a);
        break;
    case RPT_SCAN:
        CtrLog("emerald3ds: link scan found %lu in %lu ms\n",
               (unsigned long)a, (unsigned long)b);
        break;
    case RPT_JOIN:
        if (a == 0)
            CtrLog("emerald3ds: link joined\n");
        else
            CtrLog("emerald3ds: link join failed rc=0x%08lX\n", (unsigned long)a);
        break;
    case RPT_STOP:
        CtrLog("emerald3ds: link stopped\n");
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- helpers ---

static int ensure_uds(void)
{
    Result rc;

    if (sUdsUp)
        return 1;

    // The username is what other consoles see in the beacon. Emerald's own
    // trainer name is game-side and this file must not reach for it, so the
    // console's own name is used instead.
    rc = udsInit(LINK_SHAREDMEM, NULL);
    if (R_FAILED(rc)) {
        report(RPT_UDS_FAIL, (uint32_t)rc, 0);
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

// Publish a state with no other result. The next status read then does its own
// IPC rather than answering from a cache that predates the change.
static void publish_state(int state, int isHost)
{
    ensure_lock();
    LightLock_Lock(&sLock);
    sState = state;
    sIsHost = isHost;
    sStatus.state = (uint8_t)state;
    sStatus.isHost = (uint8_t)isHost;
    sStatusStamp = 0;
    LightLock_Unlock(&sLock);
}

// ---------------------------------------------------------------- pairing ---
//
// These four run on the worker. Each one blocks.

static void do_stop(void)
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
    publish_state(CTR_LINK_IDLE, 0);
}

static void do_host(void)
{
    Result rc;

    do_stop();
    if (!ensure_uds()) {
        publish_state(CTR_LINK_FAILED, 0);
        return;
    }

    udsGenerateDefaultNetworkStruct(&sNetwork, LINK_WLANCOMM_ID, LINK_ID8,
                                    CTR_LINK_MAX_PLAYERS);

    rc = udsCreateNetwork(&sNetwork, LINK_PASSPHRASE, sizeof(LINK_PASSPHRASE),
                          &sBind, LINK_CHANNEL, LINK_RECVBUF);
    if (R_FAILED(rc)) {
        report(RPT_HOST, (uint32_t)rc, 0);
        publish_state(CTR_LINK_FAILED, 0);
        return;
    }

    reset_frames();
    publish_state(CTR_LINK_HOSTING, 1);
    report(RPT_HOST, 0, 0);
}

static void do_scan(void)
{
    static uint8_t buf[LINK_SCAN_BUFSZ] __attribute__((aligned(4)));
    udsNetworkScanInfo *nets = NULL;
    // A scan does not change whether this console hosts or is connected, so put
    // that back. Anything else ends idle, a failure included: the scan is the
    // newer result and the panel must not keep showing the older one.
    int back = (sPrevState == CTR_LINK_HOSTING || sPrevState == CTR_LINK_CONNECTED)
             ? sPrevState : CTR_LINK_IDLE;
    size_t total = 0;
    unsigned int t0 = CtrTimeNowMs();

    if (!ensure_uds()) {
        publish_state(back, sIsHost);
        return;
    }

    // The sweep itself, with no lock held. It is the whole reason this file has
    // a worker thread.
    if (R_FAILED(udsScanBeacons(buf, sizeof(buf), &nets, &total,
                                LINK_WLANCOMM_ID, LINK_ID8, NULL, false))) {
        LightLock_Lock(&sLock);
        sScanCount = 0;
        LightLock_Unlock(&sLock);
        publish_state(back, sIsHost);
        report(RPT_SCAN, 0, CtrTimeNowMs() - t0);
        return;
    }

    // `nets` points into `buf`, which only this thread touches, so the copy is
    // the only part that needs the lock.
    LightLock_Lock(&sLock);
    sScanCount = 0;
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
    LightLock_Unlock(&sLock);

    publish_state(back, sIsHost);
    report(RPT_SCAN, (uint32_t)sScanCount, CtrTimeNowMs() - t0);
}

static void do_join(int index)
{
    udsNetworkStruct net;
    Result rc;

    LightLock_Lock(&sLock);
    if (index < 0 || index >= sScanCount) {
        // The list changed under the tap. Nothing was torn down, so this is
        // simply idle again.
        LightLock_Unlock(&sLock);
        publish_state(CTR_LINK_IDLE, sIsHost);
        return;
    }
    net = sScan[index].network;
    LightLock_Unlock(&sLock);

    if (!ensure_uds()) {
        publish_state(CTR_LINK_FAILED, 0);
        return;
    }

    rc = udsConnectNetwork(&net, LINK_PASSPHRASE, sizeof(LINK_PASSPHRASE),
                           &sBind, UDS_BROADCAST_NETWORKNODEID,
                           UDSCONTYPE_Client, LINK_CHANNEL, LINK_RECVBUF);
    if (R_FAILED(rc)) {
        report(RPT_JOIN, (uint32_t)rc, 0);
        publish_state(CTR_LINK_FAILED, 0);
        return;
    }

    reset_frames();
    publish_state(CTR_LINK_CONNECTED, 0);
    report(RPT_JOIN, 0, 0);
}

static void run_request(int req, int arg)
{
    switch (req) {
    case REQ_HOST: do_host();     break;
    case REQ_SCAN: do_scan();     break;
    case REQ_JOIN: do_join(arg);  break;
    case REQ_STOP: do_stop(); report(RPT_STOP, 0, 0); break;
    default: break;
    }
}

static void link_worker(void *arg)
{
    (void)arg;

    for (;;) {
        LightEvent_Wait(&sWake);
        if (sWorkerQuit)
            break;

        if (sBusy) {
            __dmb();
            run_request(sReq, sReqArg);

            // The published result must be visible before the main thread is
            // free to touch UDS again.
            __dmb();
            sBusy = 0;
        }
    }
}

// Started on the first pairing request, not at boot: a session that never opens
// the LINK page costs no thread.
static int worker_start(void)
{
    s32 prio = 0x30;

    if (sWorkerUp)
        return 1;

    // One step below the main thread, on the same core, like the SD writer
    // (3ds/host/io_thread.c). These calls sleep in IPC, so a lower priority
    // costs them nothing and guarantees they never take the core from a frame.
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    prio += 1;
    if (prio > 0x3F)
        prio = 0x3F;

    LightEvent_Init(&sWake, RESET_ONESHOT);
    sWorkerQuit = 0;

    sWorker = threadCreate(link_worker, NULL, LINK_STACK_SIZE, prio, 0, false);
    if (sWorker == NULL) {
        // Say so. Without this line, "pairing runs in the background" and
        // "pairing still stops the game" give the same log.
        CtrLog("emerald3ds: link thread not started; pairing stays inline\n");
        return 0;
    }

    sWorkerUp = 1;
    CtrLog("emerald3ds: link thread started (priority 0x%02lX)\n",
           (unsigned long)prio);
    return 1;
}

// Hand a blocking call to the worker and return at once. `busyState` is what
// the LINK page shows while it runs.
static void post(int req, int arg, int busyState)
{
    if (sBusy)
        return;   // one at a time; the UI disables its buttons while busy

    ensure_lock();
    sPrevState = sState;

    if (!worker_start()) {
        // No thread, so the old behaviour: block here. Worse than a stall is no
        // wireless at all.
        publish_state(busyState, sIsHost);
        run_request(req, arg);
        return;
    }

    publish_state(busyState, sIsHost);

    sReq = req;
    sReqArg = arg;
    __dmb();
    sBusy = 1;
    LightEvent_Signal(&sWake);
}

void Ctr3dsLinkHost(void)
{
    post(REQ_HOST, 0, CTR_LINK_WORKING);
}

void Ctr3dsLinkScan(void)
{
    post(REQ_SCAN, 0, CTR_LINK_SCANNING);
}

void Ctr3dsLinkJoin(int index)
{
    post(REQ_JOIN, index, CTR_LINK_JOINING);
}

void Ctr3dsLinkStop(void)
{
    post(REQ_STOP, 0, CTR_LINK_WORKING);
}

int Ctr3dsLinkBusy(void)
{
    return sBusy;
}

int Ctr3dsLinkScanCount(void)
{
    int n;

    ensure_lock();
    LightLock_Lock(&sLock);
    n = sScanCount;
    LightLock_Unlock(&sLock);
    return n;
}

void Ctr3dsLinkScanName(int index, char *out, int outSize)
{
    if (out == NULL || outSize <= 0)
        return;

    out[0] = '\0';
    ensure_lock();
    LightLock_Lock(&sLock);
    if (index >= 0 && index < sScanCount)
        snprintf(out, (size_t)outSize, "%s", sScanName[index]);
    LightLock_Unlock(&sLock);
}

int Ctr3dsLinkScanPlayers(int index)
{
    int n = 0;

    ensure_lock();
    LightLock_Lock(&sLock);
    if (index >= 0 && index < sScanCount)
        n = sScan[index].network.total_nodes;
    LightLock_Unlock(&sLock);
    return n;
}

// ----------------------------------------------------------------- status ---

// One IPC round trip for each frame at most. This used to be a plain getter
// that every caller repeated: the pump asked four times a frame and the LINK
// page twice more, for one value that changes when a console joins or leaves.
//
// It also writes sState, which is why it is not the getter any more. A getter
// that changes state cannot be called from a draw path.
static void refresh_status(void)
{
    udsConnectionStatus st;
    uint64_t now;
    int state, isHost;

    ensure_lock();
    drain_report();

    // The worker owns UDS while it runs. Answer from the cache.
    if (sBusy)
        return;

    now = svcGetSystemTick();

    LightLock_Lock(&sLock);
    if (sStatusStamp != 0 && now - sStatusStamp < STATUS_MAX_AGE_TICKS) {
        LightLock_Unlock(&sLock);
        return;
    }
    state = sState;
    isHost = sIsHost;
    LightLock_Unlock(&sLock);

    if (state != CTR_LINK_HOSTING && state != CTR_LINK_CONNECTED) {
        LightLock_Lock(&sLock);
        sStatus.state       = (uint8_t)state;
        sStatus.playerCount = 1;
        sStatus.localId     = 0;
        sStatus.isHost      = (uint8_t)isHost;
        sStatusStamp        = now;
        LightLock_Unlock(&sLock);
        return;
    }

    if (R_FAILED(udsGetConnectionStatus(&st))) {
        // Stamp it anyway. A link that answers with an error must not make
        // every caller of this frame repeat the round trip.
        LightLock_Lock(&sLock);
        sStatusStamp = now;
        LightLock_Unlock(&sLock);
        return;
    }

    {
        int players = st.total_nodes;
        int local   = node_to_player(st.cur_NetworkNodeID);

        if (players > CTR_LINK_MAX_PLAYERS)
            players = CTR_LINK_MAX_PLAYERS;
        if (players < 1)
            players = 1;
        if (local < 0 || local >= CTR_LINK_MAX_PLAYERS)
            local = 0;

        // Hosting alone is not yet a link; the game must not be told it has a
        // partner until one is actually present.
        if (players > 1)
            state = CTR_LINK_CONNECTED;
        else if (isHost)
            state = CTR_LINK_HOSTING;

        LightLock_Lock(&sLock);
        sState              = state;
        sStatus.state       = (uint8_t)state;
        sStatus.playerCount = (uint8_t)players;
        sStatus.localId     = (uint8_t)local;
        sStatus.isHost      = (uint8_t)isHost;
        sStatusStamp        = now;
        LightLock_Unlock(&sLock);
    }
}

void Ctr3dsLinkGetStatus(CtrLinkStatus *out)
{
    if (out == NULL)
        return;

    refresh_status();

    LightLock_Lock(&sLock);
    *out = sStatus;
    LightLock_Unlock(&sLock);
}

// Scalar view for src/link.c, which must not see CtrLinkStatus.
//
// src/link.c calls this one first on each frame. It refreshes the cache that
// the other two and Ctr3dsLinkExchange() then read, so one frame of the pump
// costs one IPC round trip rather than four.
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

// One line for each run of missed frames, not one for each miss. A miss storm
// must not fill the log, and the run length is what tells a late peer from a
// dead one.
static unsigned sMissRun;

static void note_exchange(int ready)
{
    if (!ready) {
        if (sMissRun == 0)
            CtrLog("emerald3ds: link missed frame %lu\n", (unsigned long)sFrame);
        if (sMissRun < 0xFFFFFFFFu)
            sMissRun++;
        return;
    }

    if (sMissRun > 0) {
        CtrLog("emerald3ds: link caught up after %u missed frames\n", sMissRun);
        sMissRun = 0;
    }
}

int Ctr3dsLinkExchange(const void *sendCmd, void *recvCmds)
{
    CtrLinkStatus st;
    LinkPacket out;
    uint8_t *dst = (uint8_t *)recvCmds;
    uint64_t deadline;
    uint32_t target;
    int players, local;
    int ready;

    memset(recvCmds, 0, CTR_LINK_MAX_PLAYERS * CTR_LINK_CMD_BYTES);

    // The worker owns UDS while a pairing call runs, and a pairing call means
    // there is no link to exchange over anyway.
    if (sBusy)
        return 0;

    // The cache that Ctr3dsLinkIsConnected() refreshed at the top of this same
    // frame, in src/link.c's pump. Reading it costs a lock, not a round trip.
    Ctr3dsLinkGetStatus(&st);
    if (st.state != CTR_LINK_CONNECTED || st.playerCount < 2)
        return 0;

    players = st.playerCount;
    local   = st.localId;

    out.frame = sFrame;
    memcpy(out.cmd, sendCmd, CTR_LINK_CMD_BYTES);

    if (R_FAILED(udsSendTo(UDS_BROADCAST_NETWORKNODEID, LINK_CHANNEL,
                           UDS_SENDFLAG_Default, &out, sizeof(out)))) {
        // A failed send is a dropped frame, not a dead link: the caller
        // reports the miss and we send this same frame again next time. The
        // counter must NOT advance here, or this console runs ahead of a peer
        // that never saw the frame. See the note at the end of this function.
        note_exchange(0);
        return 0;
    }

    // One frame behind. Frame 0 has nothing to wait for yet.
    target = (sFrame == 0) ? 0 : sFrame - 1;

    drain();
    ready = peers_ready(target, players, local);

    // Bounded wait. This stall is the lockstep.
    //
    // It used to call udsWaitDataAvailable(&sBind, false, false) in a loop. The
    // third argument is `wait`, and false makes that a zero-timeout poll, so
    // the loop spun on IPC for the whole budget: up to 8 ms of core 0 taken
    // from a frame that, on a base 3DS, had none to spare, plus hundreds of nwm
    // round trips. The bind event is what that call waits on, and
    // svcWaitSynchronization takes a timeout, so this sleeps instead and wakes
    // the moment a packet lands.
    deadline = svcGetSystemTick() + LINK_WAIT_TICKS;
    while (!ready) {
        uint64_t now = svcGetSystemTick();

        if (now >= deadline)
            break;

        if (R_FAILED(svcWaitSynchronization(sBind.event,
                                            (s64)((deadline - now) * 1000 / TICKS_PER_US))))
            break;   // the budget ran out

        svcClearEvent(sBind.event);
        drain();
        ready = peers_ready(target, players, local);
    }

    for (int p = 0; p < players; p++) {
        if (p == local)
            memcpy(dst + p * CTR_LINK_CMD_BYTES, sendCmd, CTR_LINK_CMD_BYTES);
        else if (sHave[p])
            memcpy(dst + p * CTR_LINK_CMD_BYTES, sLatest[p].cmd, CTR_LINK_CMD_BYTES);
    }

    // Advance ONLY on success. This is what makes the exchange a lockstep
    // rather than two consoles each counting their own frames.
    //
    // The counter used to advance every call. Each console then counted at its
    // own frame rate, so a console running faster than its peer, which is what
    // a New 3DS against an Old 3DS is, drew permanently ahead: peers_ready()
    // compares the peer's frame against ours, so once the gap opened it never
    // closed, every exchange failed and the game showed a communication error
    // within seconds. One dropped frame on either side did the same thing.
    //
    // Holding the counter back makes a stalled peer stall us too, which is the
    // point: the pair runs at the slower console's rate instead of drifting.
    if (ready)
        sFrame++;

    note_exchange(ready);
    return ready;
}

// ------------------------------------------------------------ diagnostics ---

// Called from TrySetLinkErrorBuffer() in src/link.c, the one place every link
// error passes through. Without it, Emerald's own error screen and a real fault
// look the same in a log: the game stops talking and nothing says why.
void Ctr3dsLinkLogError(unsigned int status, int sendCount, int recvCount)
{
    CtrLog("emerald3ds: link error status=%08X send=%d recv=%d missrun=%u "
           "frame=%lu players=%u\n",
           status, sendCount, recvCount, sMissRun, (unsigned long)sFrame,
           (unsigned)sStatus.playerCount);
}

void CtrLinkExit(void)
{
    // Stop the worker first, so the teardown below owns UDS with no race.
    if (sWorkerUp) {
        sWorkerQuit = 1;
        LightEvent_Signal(&sWake);
        threadJoin(sWorker, U64_MAX);
        threadFree(sWorker);
        sWorker = NULL;
        sWorkerUp = 0;
        sBusy = 0;
    }

    do_stop();
    drain_report();

    if (sUdsUp) {
        udsExit();
        sUdsUp = 0;
    }
}
