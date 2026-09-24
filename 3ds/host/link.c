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
// Lockstep is not enough on its own. A cable also delivers every command
// exactly once and in order, and the block transfers above this file have no
// sequence numbers to repair a stream that does not. So each peer gets a ring
// indexed by the sender's frame, and a command is consumed from it as it is
// handed to the game. See sRing.
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

// The handshake words, copied from include/link.h rather than shared, because
// this file speaks no game types. MASTER_HANDSHAKE and SLAVE_HANDSHAKE.
#define CTR_LINK_MASTER_HANDSHAKE 0x8FFFu
#define CTR_LINK_SLAVE_HANDSHAKE  0xB9A0u

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
//
// `hs` carries the handshake word, which is what a cable puts on the wire
// during LINK_STATE_HANDSHAKE in place of a command. It rides every packet
// because a console has to keep saying it until the whole network agrees, the
// same way DoHandshake() re-drives REG_SIOMLT_SEND on every serial interrupt.
// A console that stopped once it was satisfied would strand a peer that had
// not heard it yet.
//
// `phase` is why the word needs its own field rather than a reserved frame
// number. A handshake packet carries no command, so it must not enter the
// command ring: doing so would fill frame 0's slot with an empty command and
// the peer's real first command would be dropped on top of it.
//
// Adding it changed the packet size, so two consoles must run the same build.
// A mismatched pair reads as a short packet and shows up as `short` in the
// period line rather than as silence.
//
// Every packet also repeats the commands before it, and that repetition is
// what makes the transport reliable rather than merely ordered.
//
// A cable cannot lose a transfer. UDS can, and a lockstep that only ever sends
// its current frame cannot recover from one: the peer that needed the lost
// frame waits for a number the sender has already moved past, and the sender
// waits for the frame the peer can no longer produce. Both sides then sit
// there until the lag tolerance closes the link. A console test deadlocked
// exactly that way, the host stranded at frame 27 wanting 26 while the client
// sat at 25 wanting 24.
//
// Carrying the last LINK_HISTORY commands repairs a gap with no acknowledgement
// and no retransmit request: the next packet through already holds what was
// lost. The lockstep keeps the two within about two frames of each other, so
// this depth is many times what a healthy pair needs, and it is the margin that
// a console dropping a third of its frames actually spends.
#define LINK_HISTORY 8

typedef struct {
    uint32_t frame;                   // the newest command here
    uint16_t hs;                      // handshake word, on every packet
    uint8_t  phase;                   // PHASE_HANDSHAKE, PHASE_LIVE or PHASE_BYE
    uint8_t  count;                   // commands present, 0..LINK_HISTORY
    // [0] is `frame`, [1] is frame - 1, and so on.
    uint8_t  cmd[LINK_HISTORY][CTR_LINK_CMD_BYTES];
} LinkPacket;

// PHASE_BYE says "this console is leaving", sent once when the HOME menu
// suspends us. Without it a peer cannot tell a suspended console from a slow
// one: a suspended 3DS stays a UDS node and simply stops sending, so
// Ctr3dsLinkIsConnected() keeps answering yes and the peer spends the whole
// LINK_LAG_TOLERANCE_MS before erroring. A console log of exactly that reported
// 121 consecutive misses, every one of them called "peer late", with "down 0".
//
// An older build ignores it: drain() drops any phase that is not PHASE_LIVE.
enum { PHASE_HANDSHAKE = 0, PHASE_LIVE = 1, PHASE_BYE = 2 };

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

// Set by the main thread when the console comes back from a suspend; cleared by
// the worker in ensure_uds(). A suspend hands the wireless to the system applet,
// which re-initialises NWM and leaves this process holding a session it no
// longer owns: every later call then answers RM_UDS / RS_INVALIDSTATE /
// RD_NOT_AUTHORIZED, the 0xC8A113EA a console log caught, and a "scan" that
// returns in 15 ms instead of 400 because it never reaches the radio.
//
// Nothing recovered from that before: sUdsUp is what ensure_uds() tests and it
// was cleared only at process shutdown, so wireless stayed dead for the rest of
// the run and every retry answered "wireless failed".
static volatile int sUdsStale;
static int sState = CTR_LINK_IDLE;
static int sIsHost;

// Whether sBind currently holds a live bind context, and so whether there is
// anything to tear down.
//
// This must NOT be inferred from sState. sState is the UI's word for what the
// panel shows, and post() sets it to the busy state BEFORE the worker runs, so
// by the time do_stop() looks, an established link reads as CTR_LINK_WORKING.
// do_stop() used to test sState, skipped the teardown on exactly that path, and
// let udsCreateNetwork() run against a bind context that was still live. HOST
// after any earlier HOST or JOIN crashed. Resource ownership gets its own flag.
static int sBound;

static udsNetworkStruct sNetwork;                        // worker thread only
static udsBindContext   sBind;

static uint32_t sFrame;                                  // our own counter

// The receive side, one ring for each peer, indexed by the sender's frame.
//
// This was one slot per peer that kept only the newest packet. A cable does
// not work that way: it delivers every command exactly once, in order, and the
// block-transfer layer above it depends on that absolutely. LINKCMD_CONT_BLOCK
// carries no sequence number, just 14 bytes appended at sBlockRecv[i].pos, so
// one dropped command leaves a block that never completes, and one repeated
// command fills it with rubbish that then fails its magic check.
//
// A peer may legitimately lead: sFrame advances only on success and
// peers_ready() accepted frame >= sFrame - 1, so two of a peer's packets can
// land between two of our pumps. With one slot the older one was overwritten
// and lost, and the newer one was handed up twice. A New 3DS hosting for an
// Old 3DS lost about 21 commands and repeated about 8 in every 600 frames.
//
// The ring is what makes a peer running ahead free instead of lossy. Its depth
// is far past the window peers_ready() allows, so it only fills if the
// lockstep itself has already failed, and that case is reported rather than
// absorbed.
#define LINK_RING 16

static uint8_t  sRingFull[CTR_LINK_MAX_PLAYERS][LINK_RING];
static uint32_t sRingFrame[CTR_LINK_MAX_PLAYERS][LINK_RING];
static uint8_t  sRingCmd[CTR_LINK_MAX_PLAYERS][LINK_RING][CTR_LINK_CMD_BYTES];

// The send side: what this console has said, so it can say it again.
//
// sHeld is the command for the frame in flight. It latches on the first
// transmission and does not change while that frame is retried, because a frame
// number has to mean one command. The pump builds each send from the head of
// gLink.sendQueue and that head changes when the queue was empty on the first
// attempt and filled before the second, so without the latch the peer could
// take the empty command for a frame while the pump popped the real one.
// Ctr3dsLinkExchange() reports through `tookCmd` whether the caller's command
// was the one that went out, and the pump pops only then.
static uint8_t sHeld[CTR_LINK_CMD_BYTES];
static int     sHeldValid;

// The last LINK_HISTORY commands, for the repetition that repairs a peer's gap.
static uint8_t  sSentCmd[LINK_HISTORY][CTR_LINK_CMD_BYTES];
static uint32_t sSentFrame[LINK_HISTORY];
static uint8_t  sSentValid[LINK_HISTORY];

// The newest frame each peer has sent, kept for the report alone. The ring
// cannot answer this: a slot is cleared as the game takes it, so a healthy peer
// would read as silent the moment its command was delivered.
static uint32_t sPeerNewest[CTR_LINK_MAX_PLAYERS];
static uint8_t  sPeerSeen[CTR_LINK_MAX_PLAYERS];

// ---- the handshake ---------------------------------------------------------
//
// This file used to declare the link established as soon as UDS reported two
// nodes. That is far too early. On a cable the state leaves HANDSHAKE only
// after the master asserts handshakeAsMaster, which the game does when the host
// player confirms, and DoHandshake() then sees MASTER_HANDSHAKE on the wire
// with the player count unchanged across two frames.
//
// Establishing at pairing time broke the Cable Club in two ways. Every cancel
// in the link-up chain is guarded by IsLinkConnectionEstablished() == FALSE, so
// B Button: Cancel did nothing from the moment two consoles saw each other. And
// the two consoles went live on different frames, so their link callbacks began
// out of step.
//
// What each console currently puts on the wire, and what it has seen.
static uint16_t sHsOut;                  // 0 until the game asks for one
static uint16_t sHsFrom[CTR_LINK_MAX_PLAYERS];
static int      sHsStablePlayers;        // the count seen on the last frame
static int      sHsDone;
static uint8_t  sHsLogged;

// Set when a peer sends a handshake packet, cleared with the session. It is the
// proof that the peer is in THIS handshake. See drain().
static uint8_t  sHsSeen[CTR_LINK_MAX_PLAYERS];

// Scan results, kept so the UI can list them across frames.
static udsNetworkScanInfo sScan[LINK_MAX_SCAN];
static char               sScanName[LINK_MAX_SCAN][CTR_LINK_NAME_LEN];
static int                sScanCount;

// The cached connection status, and when it was taken.
static CtrLinkStatus sStatus = { CTR_LINK_IDLE, 1, 0, 0 };
static uint64_t      sStatusStamp;

// ---- telemetry -------------------------------------------------------------
//
// The event lines in this file say when a link BROKE. They say nothing about
// how one that still works is behaving, and a wireless fault is nearly always
// visible as drift before it is visible as a failure. So count the traffic and
// report it on a period, the way the frame profiler does.
//
// Reported only while the game is pumping a link, so an idle session stays
// silent.
//
// Everything here is written on the main thread, from the pump, and read there
// too. It is diagnostics: no lock, and no claim to be exact across a tear.
#define LINK_STAT_PERIOD 600   // pumped frames for each report

enum { MISS_UNKNOWN = 0, MISS_DOWN, MISS_BUSY, MISS_SEND, MISS_LATE,
       MISS_GONE, MISS_KINDS };

static const char *const kMissWhy[MISS_KINDS] = {
    "unknown", "link down", "pairing busy", "send failed", "peer late",
    "peer left"
};

// Set when a peer says PHASE_BYE. The lag tolerance is for a peer that is late,
// and a peer that has said goodbye is not coming back, so waiting the full
// three seconds only delays an error that is already certain.
static int sPeerGone;

// Latched so a link coming apart logs the bad node id once, not every frame.
static int      sNodeIdBad;

// TRUE while this console is on a network, read by the main thread so it can
// refuse sleep. Set on the worker, read with no lock: it is one int and a
// frame's worth of staleness either way does not matter.
static volatile int sNoSleep;

static int      sMissWhy;                // one of the above, set at the source
static unsigned sMissBy[MISS_KINDS];     // how many of each in this period

static unsigned sStatFrames, sStatOk;
static unsigned sStatusFailRun;
static int      sLoggedPlayers = -1;
static int      sLoggedIds = -1;

// The bounded wait, which is where a marginal link shows itself first.
static unsigned long long sWaitSum, sWaitWorst;
static unsigned sWaitN, sDeadlineHits;

// The same ticks again, for the display divider rather than for the report.
//
// The divider in 3ds/host/main.c measures the game's frame as the span between
// two of its hooks and calls all of it work. The link's wait is inside that
// span and is not work: it is this console asleep, waiting on a peer. Counted
// as work it pushes the estimate past the threshold and halves the display on a
// scene that was never expensive, which then costs the pair two waits instead
// of one and makes the peer later still.
//
// Read and cleared by the divider each frame, the way CtrVideoLastWaitTicks()
// serves the same purpose for the wait inside C3D_FrameBegin.
static unsigned long long sBlockedTicks;

// The receive side. A short, repeated or dropped packet is invisible
// otherwise: all three look exactly like "the peer said nothing".
//
// sRxLost counts a packet that arrived for a ring slot still holding an
// unconsumed command from an older frame, which means the peer has lapped us.
// It must read 0. It is the counter that would have named this bug.
static unsigned sRxPackets, sRxShort, sRxStale, sRxLost;

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

// The worker logs for itself.
//
// CtrLog is safe from any thread: svcOutputDebugString takes no lock, and
// log_to_file holds sQueueLock (3ds/host/log.c). Only CtrProfile and
// CtrLogSlow are main-thread-only, because their tables are unlocked.
//
// This matters more than tidiness. Each blocking UDS call gets a line BEFORE it
// runs, so if the console faults inside one, the last line in the log names the
// call that did it. Handing finished results to the main thread to print, which
// is what this file did first, loses exactly the case worth diagnosing.

// ---------------------------------------------------------------- helpers ---

static int ensure_uds(void)
{
    Result rc;

    // Release a session the system took while this console was suspended,
    // before asking for a new one. Here rather than on the resume itself
    // because every wireless operation already passes through this function,
    // so a rebuild cannot be missed or dropped, however busy the worker was
    // when the console woke.
    if (sUdsUp && sUdsStale) {
        CtrLog("emerald3ds: link releasing the session a suspend took\n");
        udsExit();
        sUdsUp = 0;
        sBound = 0;             // udsExit took the bind with it
        memset(&sBind, 0, sizeof sBind);
    }
    sUdsStale = 0;

    if (sUdsUp)
        return 1;

    // The username is what other consoles see in the beacon. Emerald's own
    // trainer name is game-side and this file must not reach for it, so the
    // console's own name is used instead.
    CtrLog("emerald3ds: link udsInit\n");
    rc = udsInit(LINK_SHAREDMEM, NULL);
    if (R_FAILED(rc)) {
        CtrLog("emerald3ds: link udsInit failed rc=0x%08lX\n",
               (unsigned long)rc);
        return 0;
    }

    sUdsUp = 1;
    return 1;
}

// A new NETWORK. The transport goes with it, and so do the counters that
// report on one network.
//
// The transport half is Ctr3dsLinkNewSession(), because the game also starts a
// new link many times on one network. See that function.
static void reset_frames(void)
{
    Ctr3dsLinkNewSession("pairing");

    sLoggedPlayers = -1;
    sLoggedIds = -1;
    sStatusFailRun = 0;
    sStatFrames = sStatOk = 0;
    sWaitSum = sWaitWorst = 0;
    sWaitN = sDeadlineHits = 0;
    sRxPackets = sRxShort = sRxStale = sRxLost = 0;
    memset(sMissBy, 0, sizeof sMissBy);
}

// UDS node ids are 1-based with the host at 1; the GBA's are 0-based with the
// master at 0. Everything above this file speaks the GBA's numbering.
static int node_to_player(u16 nodeId)
{
    return (int)nodeId - 1;
}

// The id this console may publish, given which end of the network it is.
//
// A console that created the network is node 1, which is player 0, which is the
// master; a client is never any of those. That invariant is stated in
// CheckMasterOrSlave's comment (src/link.c) and was enforced nowhere, so both
// places below that publish an id could hand a client a 0.
//
// It matters because the id is read from UDS every frame and pushed straight
// into REG_SIOCNT, with no latch: one unreadable frame is enough for
// GetMultiplayerId() to answer 0 on BOTH consoles, and then every "am I player
// 1?" test in the trade and battle code takes the same branch on both sides.
// That is the fault 584dd3a fixed; a console log caught it again by this route.
// 584dd3a's own check cannot see it, because it compares the id against the
// register it just wrote it to. isHost is the one independent fact, so use it.
//
// 1 rather than a remembered id: any non-zero id keeps this console a slave,
// which is the safe answer when UDS cannot say more.
static int sane_local_id(int local, int isHost)
{
    if (isHost)
        return 0;

    return (local < 1 || local >= CTR_LINK_MAX_PLAYERS) ? 1 : local;
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
    if (sBound) {
        CtrLog("emerald3ds: link teardown (%s)\n", sIsHost ? "host" : "client");
        udsUnbind(&sBind);
        if (sIsHost)
            udsDestroyNetwork();
        else
            udsDisconnectNetwork();
        sBound = 0;
        memset(&sBind, 0, sizeof sBind);
    }

    sNoSleep = 0;

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

    CtrLog("emerald3ds: link udsCreateNetwork\n");
    rc = udsCreateNetwork(&sNetwork, LINK_PASSPHRASE, sizeof(LINK_PASSPHRASE),
                          &sBind, LINK_CHANNEL, LINK_RECVBUF);
    if (R_FAILED(rc)) {
        CtrLog("emerald3ds: link host failed rc=0x%08lX\n", (unsigned long)rc);
        publish_state(CTR_LINK_FAILED, 0);
        return;
    }

    sBound = 1;
    sNoSleep = 1;
    reset_frames();
    publish_state(CTR_LINK_HOSTING, 1);
    CtrLog("emerald3ds: link hosting\n");
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
    Result rc;

    if (!ensure_uds()) {
        publish_state(back, sIsHost);
        return;
    }

    // The sweep itself, with no lock held. It is the whole reason this file has
    // a worker thread.
    CtrLog("emerald3ds: link udsScanBeacons\n");
    rc = udsScanBeacons(buf, sizeof(buf), &nets, &total,
                        LINK_WLANCOMM_ID, LINK_ID8, NULL, false);
    if (R_FAILED(rc)) {
        LightLock_Lock(&sLock);
        sScanCount = 0;
        LightLock_Unlock(&sLock);
        publish_state(back, sIsHost);
        // With the code. This path used to print the elapsed time alone, and
        // that omission is why a dead UDS session could only be identified
        // from the host path: a 15 ms "scan" says something is wrong, and
        // rc=0xC8A113EA says exactly what.
        CtrLog("emerald3ds: link scan failed after %u ms rc=0x%08lX\n",
               CtrTimeNowMs() - t0, (unsigned long)rc);
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
    CtrLog("emerald3ds: link scan found %d in %u ms\n",
           sScanCount, CtrTimeNowMs() - t0);
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

    // Take the copy first, then let go of anything already held. Joining while
    // hosting, or twice over, would otherwise bind sBind a second time without
    // unbinding the first, which is the same fault do_host() had.
    do_stop();

    if (!ensure_uds()) {
        publish_state(CTR_LINK_FAILED, 0);
        return;
    }

    CtrLog("emerald3ds: link udsConnectNetwork index %d\n", index);
    rc = udsConnectNetwork(&net, LINK_PASSPHRASE, sizeof(LINK_PASSPHRASE),
                           &sBind, UDS_BROADCAST_NETWORKNODEID,
                           UDSCONTYPE_Client, LINK_CHANNEL, LINK_RECVBUF);
    if (R_FAILED(rc)) {
        CtrLog("emerald3ds: link join failed rc=0x%08lX\n", (unsigned long)rc);
        publish_state(CTR_LINK_FAILED, 0);
        return;
    }

    sBound = 1;
    sNoSleep = 1;
    reset_frames();
    publish_state(CTR_LINK_CONNECTED, 0);
    CtrLog("emerald3ds: link joined\n");
}

static void run_request(int req, int arg)
{
    switch (req) {
    case REQ_HOST: do_host();     break;
    case REQ_SCAN: do_scan();     break;
    case REQ_JOIN: do_join(arg);  break;
    case REQ_STOP: do_stop(); CtrLog("emerald3ds: link stopped\n"); break;
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
        sStatus.localId     = (uint8_t)sane_local_id(0, isHost);
        sStatus.isHost      = (uint8_t)isHost;
        sStatusStamp        = now;
        LightLock_Unlock(&sLock);
        return;
    }

    {
        Result rc = udsGetConnectionStatus(&st);

        if (R_FAILED(rc)) {
            // One line for a run of these, not one for each frame. A link
            // whose status call has started failing goes quiet in every other
            // way, so without this the log simply stops mentioning it.
            if (sStatusFailRun++ == 0)
                CtrLog("emerald3ds: link status failed rc=0x%08lX\n",
                       (unsigned long)rc);

            // Stamp it anyway. A link that answers with an error must not make
            // every caller of this frame repeat the round trip.
            LightLock_Lock(&sLock);
            sStatusStamp = now;
            LightLock_Unlock(&sLock);
            return;
        }

        if (sStatusFailRun > 0) {
            CtrLog("emerald3ds: link status ok after %u failures\n",
                   sStatusFailRun);
            sStatusFailRun = 0;
        }
    }

    {
        int players = st.total_nodes;
        int local   = node_to_player(st.cur_NetworkNodeID);

        if (players > CTR_LINK_MAX_PLAYERS)
            players = CTR_LINK_MAX_PLAYERS;
        if (players < 1)
            players = 1;

        // An id UDS cannot name is "we are not on a working network", NOT "we
        // are node 0" -- and node 0 is the master.
        //
        // This used to clamp to 0, which handed a client whose connection was
        // coming apart the HOST's id: gLink.localId, gLink.isMaster and
        // Ctr3dsSetSioMultiId() all followed, GetMultiplayerId() then answered
        // 0 on BOTH consoles, and every "am I player 1?" test in the trade and
        // battle code took the same branch on both sides. A console log caught
        // exactly that, a second `link ids` line flipping local=1 to local=0
        // mid-session. It is the fault 584dd3a fixed, by another route.
        //
        // Saying one player is what closes it: Ctr3dsLinkIsConnected() wants
        // two, so the pump reports a miss and returns before it can write any
        // of the above.
        if (local < 0 || local >= CTR_LINK_MAX_PLAYERS) {
            if (!sNodeIdBad) {
                sNodeIdBad = 1;
                CtrLog("emerald3ds: link node id %u invalid, link is down\n",
                       (unsigned)st.cur_NetworkNodeID);
            }
            players = 1;
            local = 0;
        }

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
        sStatus.localId     = (uint8_t)sane_local_id(local, isHost);
        sStatus.isHost      = (uint8_t)isHost;
        sStatusStamp        = now;
        LightLock_Unlock(&sLock);

        // Who is on the network, logged when it changes and not before. This
        // is the line that answers "did the other console actually arrive",
        // and later "when exactly did it leave", which no other line says.
        if (players != sLoggedPlayers) {
            CtrLog("emerald3ds: link peers %d (this console id %d, %s)\n",
                   players, local, isHost ? "host" : "client");
            sLoggedPlayers = players;
        }
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
    int up;

    Ctr3dsLinkGetStatus(&st);
    up = st.state == CTR_LINK_CONNECTED && st.playerCount >= 2;

    // The pump calls this first on every frame, and counts a miss when it says
    // no. Claim the reason here; Ctr3dsLinkExchange overwrites it with a more
    // exact one when it gets far enough to know better.
    if (!up)
        sMissWhy = MISS_DOWN;

    return up;
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

// Move everything waiting in the receive buffer into the per-player rings.
// Returns how many packets were taken.
static int drain(void)
{
    LinkPacket pkt;
    size_t got = 0;
    u16 src = 0;
    int n = 0;

    while (R_SUCCEEDED(udsPullPacket(&sBind, &pkt, sizeof(pkt), &got, &src))) {
        int p;

        if (got == 0)
            break;            // nothing left, the usual way out

        if (got != sizeof(pkt)) {
            // Not one of ours, or truncated. Counted, because otherwise it is
            // indistinguishable from the peer saying nothing at all.
            sRxShort++;
            got = 0;
            continue;
        }

        sRxPackets++;
        p = node_to_player(src);

        if (p >= 0 && p < CTR_LINK_MAX_PLAYERS) {
            // A peer leaving, before any of the bookkeeping below: a goodbye
            // carries no frame, no command and no handshake word, so none of it
            // applies and letting it through would move sPeerNewest.
            if (pkt.phase == PHASE_BYE) {
                if (!sPeerGone) {
                    sPeerGone = 1;
                    CtrLog("emerald3ds: link peer %d left (suspended or quit)\n", p);
                }
                n++;
                got = 0;
                continue;
            }

            // The oldest frame still owed to the game. Anything older has been
            // handed up already.
            uint32_t want = (sFrame == 0) ? 0 : sFrame - 1;
            int32_t  ahead = (int32_t)(pkt.frame - want);

            if (!sPeerSeen[p] || pkt.frame > sPeerNewest[p]) {
                sPeerNewest[p] = pkt.frame;
                sPeerSeen[p] = 1;
            }

            // The handshake word is not part of the command stream, so take
            // it off every packet, including the ones the ring discards. A
            // handshake packet that is lost is then repaired by the live
            // packets behind it.
            //
            // But a live packet can only REPAIR an agreement, never start one.
            // The game opens and closes a link many times on one network, and
            // a peer that has not closed yet still carries the old word on its
            // live packets. Without this gate, a console that has just reset
            // agrees with that stale word on its first frame, and the two go
            // live on different frames again. A handshake packet is the proof
            // that the peer is in THIS handshake, because it sends one only
            // while it is.
            if (pkt.phase == PHASE_HANDSHAKE)
                sHsSeen[p] = 1;

            if (pkt.hs != 0 && sHsSeen[p])
                sHsFrom[p] = pkt.hs;

            (void)ahead;

            if (pkt.phase != PHASE_LIVE) {
                n++;
                got = 0;
                continue;         // no command to file
            }

            // File the packet's newest command and every repeat behind it. The
            // repeats are the repair: the one the peer still owes us is very
            // often in there rather than in the packet that went missing.
            {
                unsigned count = pkt.count;
                unsigned i;

                if (count > LINK_HISTORY)
                    count = LINK_HISTORY;     // it came off the wire

                for (i = 0; i < count; i++) {
                    uint32_t f;
                    unsigned slot;

                    if (pkt.frame < i)
                        break;                // before the session began
                    f = pkt.frame - i;

                    if ((int32_t)(f - want) < 0) {
                        // Already handed up. Every packet repeats these, so
                        // count them once per packet, not once per entry.
                        if (i == 0)
                            sRxStale++;
                        break;                // and everything older too
                    }
                    if ((int32_t)(f - want) >= LINK_RING) {
                        // Further ahead than the ring can hold while we still
                        // owe `want`. The lockstep keeps a peer within about
                        // two frames, so this means the lockstep itself has
                        // broken, and it is worth one loud line.
                        if (sRxLost++ == 0)
                            CtrLog("emerald3ds: link ring lapped, p%d sent %lu "
                                   "while we still owe %lu\n",
                                   p, (unsigned long)f, (unsigned long)want);
                        continue;
                    }

                    slot = f % LINK_RING;
                    if (sRingFull[p][slot] && sRingFrame[p][slot] == f) {
                        if (i == 0)
                            sRxStale++;
                        continue;             // already have it
                    }

                    memcpy(sRingCmd[p][slot], pkt.cmd[i], CTR_LINK_CMD_BYTES);
                    sRingFrame[p][slot] = f;
                    sRingFull[p][slot] = 1;
                }
            }
        }
        n++;
        got = 0;
    }

    return n;
}

// Has every peer sent exactly `target`? Our own ring is never waited on.
//
// The test is equality, not "has reached". Each command must be handed up once
// and in order, so a peer that has run ahead does not excuse the frame we still
// owe the game.
static int peers_ready(uint32_t target, int players, int local)
{
    unsigned slot = target % LINK_RING;

    for (int p = 0; p < players; p++) {
        if (p == local)
            continue;
        if (!sRingFull[p][slot] || sRingFrame[p][slot] != target)
            return 0;
    }

    return 1;
}

// How many frames a peer is running behind us. -1 means it has said nothing at
// all, which is what a dead console looks like.
static long peer_behind(int p)
{
    if (p < 0 || p >= CTR_LINK_MAX_PLAYERS || !sPeerSeen[p])
        return -1L;

    return (long)sFrame - (long)sPeerNewest[p];
}

// How long a peer may go quiet before the game is told the link has lagged.
//
// This used to be a count of 10 frames, game-side. A frame count cannot be
// right for both consoles at once: 10 frames is 167 ms on a 60 fps New 3DS and
// 333 ms on a 30 fps Old 3DS, so a mixed pair did not even agree on when to
// give up. Worse, either console blocks for more than 100 ms whenever it
// flushes its save to the card, and a trade does that repeatedly, so the old
// limit ended the session on the first save of every trade.
//
// Three seconds is far longer than any save flush and far shorter than a
// player's patience. A peer that is merely slow stalls this console instead,
// which is what the lockstep is for.
#define LINK_LAG_TOLERANCE_MS 3000
#define LINK_LAG_MIN_MISSES   2

// One report partway through a stall, well before the tolerance gives up, so a
// log shows what the link was waiting for while there was still a link.
#define LINK_STALL_REPORT_MISSES 60

// One line for each run of missed frames, not one for each miss. A miss storm
// must not fill the log, and the run length is what tells a late peer from a
// dead one.
static unsigned     sMissRun;
static unsigned int sMissStartMs;

static void stats_report(void)
{
    unsigned waitMeanUs =
        sWaitN ? (unsigned)(sWaitSum / sWaitN / TICKS_PER_US) : 0;
    CtrLinkStatus st;

    Ctr3dsLinkGetStatus(&st);

    CtrLog("emerald3ds: link %u frames id=%u/%u frame=%lu ok=%u miss=%u "
           "(late %u send %u busy %u down %u gone %u)\n",
           sStatFrames, (unsigned)st.localId, (unsigned)st.playerCount,
           (unsigned long)sFrame, sStatOk, sStatFrames - sStatOk,
           sMissBy[MISS_LATE], sMissBy[MISS_SEND], sMissBy[MISS_BUSY],
           sMissBy[MISS_DOWN], sMissBy[MISS_GONE]);

    CtrLog("emerald3ds: link wait mean %u us worst %u us over %u, timeouts %u, "
           "rx %u short %u stale %u lost %u\n",
           waitMeanUs,
           (unsigned)(sWaitWorst / TICKS_PER_US), sWaitN, sDeadlineHits,
           sRxPackets, sRxShort, sRxStale, sRxLost);

    // How far behind each peer is running. Zero is lockstep; a number that
    // grows across reports is the drift that ends a session.
    {
        char buf[64];
        int n = 0;

        for (int pl = 0;
             pl < st.playerCount && n < (int)sizeof buf - 12; pl++) {
            if (pl == st.localId)
                continue;
            long behind = peer_behind(pl);

            n += snprintf(buf + n, sizeof buf - (size_t)n, " p%d=%ld",
                          pl, behind);
        }
        if (n > 0)
            CtrLog("emerald3ds: link peer lag%s (frames behind, -1 = silent)\n",
                   buf);
    }

    sStatFrames = sStatOk = 0;
    sWaitSum = sWaitWorst = 0;
    sWaitN = sDeadlineHits = 0;
    sRxPackets = sRxShort = sRxStale = sRxLost = 0;
    memset(sMissBy, 0, sizeof sMissBy);
}

// Once for each pumped frame: src/link.c calls exactly one of NoteOk and
// NoteMiss every time round.
static void stats_tick(void)
{
    if (++sStatFrames >= LINK_STAT_PERIOD)
        stats_report();
}

// src/link.c drives these three: it is the only caller that sees every miss.
// Ctr3dsLinkExchange() cannot, because it returns early, and reports nothing,
// when the worker owns the wireless or the link is already down.
void Ctr3dsLinkNoteMiss(void)
{
    // A peer that said goodbye is gone, whatever the source claimed. Without
    // this every miss of a suspended peer reads "peer late", which is what made
    // a console log show 121 of them and "down 0".
    if (sPeerGone)
        sMissWhy = MISS_GONE;

    if (sMissWhy < 0 || sMissWhy >= MISS_KINDS)
        sMissWhy = MISS_UNKNOWN;

    sMissBy[sMissWhy]++;

    if (sMissRun == 0) {
        sMissStartMs = CtrTimeNowMs();
        // With the reason. "Our send failed" and "the peer was late" point at
        // different consoles, and the run used to report both the same way.
        CtrLog("emerald3ds: link missed frame %lu (%s)\n",
               (unsigned long)sFrame, kMissWhy[sMissWhy]);
    }

    if (sMissRun < 0xFFFFFFFFu)
        sMissRun++;

    // A stall that has lasted a second is not jitter. Say what this console is
    // waiting for and what the peer has actually sent, because "peer late"
    // alone cannot tell a slow peer from a gap that will never be filled.
    //
    // This is the line that would have named the deadlock: the host wanted
    // frame 26 from a peer whose newest was 25, while the peer wanted 24 from
    // a console that had moved to 27 and was no longer repeating it.
    if (sMissRun == LINK_STALL_REPORT_MISSES) {
        CtrLinkStatus st;
        char buf[96];
        int n = 0;

        // Empty until something is written into it. The loop below runs zero
        // times when the roster has dropped to this console alone, which is
        // exactly when a stall gets reported, and printing an unset buffer put
        // random bytes in the log.
        buf[0] = '\0';

        Ctr3dsLinkGetStatus(&st);
        for (int p = 0; p < st.playerCount && n < (int)sizeof buf - 24; p++) {
            if (p == st.localId)
                continue;
            n += snprintf(buf + n, sizeof buf - (size_t)n,
                          " p%d newest=%ld", p,
                          sPeerSeen[p] ? (long)sPeerNewest[p] : -1L);
        }
        CtrLog("emerald3ds: link stalled %u frames, we want %lu,%s\n",
               sMissRun, (unsigned long)(sFrame ? sFrame - 1 : 0),
               (n > 0) ? buf : " no peers on the network");
    }

    stats_tick();
}

void Ctr3dsLinkNoteOk(void)
{
    if (sMissRun > 0) {
        CtrLog("emerald3ds: link caught up after %u missed frames (%u ms)\n",
               sMissRun, CtrTimeNowMs() - sMissStartMs);
        sMissRun = 0;
    }

    sStatOk++;
    stats_tick();
}

// A run of at least LINK_LAG_MIN_MISSES, so one long frame can never trip it,
// AND longer than the tolerance.
int Ctr3dsLinkLagged(void)
{
    // A peer that said goodbye is not late, it is gone. The tolerance exists
    // for a console that is briefly behind; spending it here only delays an
    // error that is already certain.
    if (sPeerGone)
        return 1;

    if (sMissRun < LINK_LAG_MIN_MISSES)
        return 0;

    return (CtrTimeNowMs() - sMissStartMs) >= LINK_LAG_TOLERANCE_MS;
}

// One frame of the handshake, which is what SerialCB does in
// LINK_STATE_HANDSHAKE: put this console's word on the wire, read what came
// back, and say whether the network has agreed.
//
// `asMaster` is gLink.handshakeAsMaster, which LinkMain1 raises when the game
// asks the link to advance. It latches here because a cable clears the flag on
// every serial interrupt but keeps driving the word until every console has
// seen it.
//
// Returns 1 once the barrier passes: the master's word is on the wire AND the
// player count has not changed since the last frame. The stability test is
// DoHandshake()'s, and it stops a console arriving mid-handshake from being
// counted into a link that is already forming.
int Ctr3dsLinkHandshake(int asMaster)
{
    CtrLinkStatus st;
    LinkPacket out;
    int stable;

    if (asMaster)
        sHsOut = CTR_LINK_MASTER_HANDSHAKE;
    else if (sHsOut == 0)
        sHsOut = CTR_LINK_SLAVE_HANDSHAKE;

    // The worker owns UDS while a pairing call runs.
    if (sBusy)
        return 0;

    Ctr3dsLinkGetStatus(&st);
    if (st.state != CTR_LINK_CONNECTED || st.playerCount < 2) {
        sHsStablePlayers = 0;
        return 0;
    }

    memset(&out, 0, sizeof out);
    out.frame = sFrame;
    out.hs = sHsOut;
    out.phase = PHASE_HANDSHAKE;
    udsSendTo(UDS_BROADCAST_NETWORKNODEID, LINK_CHANNEL, UDS_SENDFLAG_Default,
              &out, sizeof(out));

    drain();

    if (sHsDone)
        return 1;

    stable = (sHsStablePlayers == st.playerCount);
    sHsStablePlayers = st.playerCount;
    if (!stable)
        return 0;

    // Node 0 is the host, and the host is the master. Every console waits on
    // the MASTER word, the master included: on a cable it reads its own word
    // back off the shared bus.
    if (st.localId == 0) {
        if (sHsOut != CTR_LINK_MASTER_HANDSHAKE)
            return 0;
    } else if (sHsFrom[0] != CTR_LINK_MASTER_HANDSHAKE) {
        return 0;
    }

    sHsDone = 1;
    if (!sHsLogged) {
        sHsLogged = 1;
        // The line that says the link went live when the host confirmed, and
        // not when the two consoles first saw each other.
        CtrLog("emerald3ds: link handshake done, players=%d, this console %s\n",
               (int)st.playerCount, st.localId == 0 ? "master" : "slave");
    }
    return 1;
}

// Copy this console's last commands into a packet, newest first, stopping at
// the first frame it no longer holds. Returns how many it wrote.
static int fill_history(uint8_t dst[LINK_HISTORY][CTR_LINK_CMD_BYTES],
                        uint32_t newest)
{
    int i;

    for (i = 0; i < LINK_HISTORY; i++) {
        uint32_t f;
        unsigned h;

        if (newest < (uint32_t)i)
            break;
        f = newest - (uint32_t)i;
        h = f % LINK_HISTORY;
        if (!sSentValid[h] || sSentFrame[h] != f)
            break;
        memcpy(dst[i], sSentCmd[h], CTR_LINK_CMD_BYTES);
    }

    return i;
}

int Ctr3dsLinkExchange(const void *sendCmd, void *recvCmds, int *tookCmd)
{
    CtrLinkStatus st;
    LinkPacket out;
    uint8_t *dst = (uint8_t *)recvCmds;
    uint64_t deadline;
    uint32_t target;
    int players, local;
    int ready;
    int took = 0;

    memset(recvCmds, 0, CTR_LINK_MAX_PLAYERS * CTR_LINK_CMD_BYTES);
    if (tookCmd != NULL)
        *tookCmd = 0;

    // The worker owns UDS while a pairing call runs, and a pairing call means
    // there is no link to exchange over anyway.
    if (sBusy) {
        sMissWhy = MISS_BUSY;
        return 0;
    }

    // The cache that Ctr3dsLinkIsConnected() refreshed at the top of this same
    // frame, in src/link.c's pump. Reading it costs a lock, not a round trip.
    Ctr3dsLinkGetStatus(&st);
    if (st.state != CTR_LINK_CONNECTED || st.playerCount < 2) {
        sMissWhy = MISS_DOWN;
        return 0;
    }

    players = st.playerCount;
    local   = st.localId;

    // Latch the command for this frame. A frame number has to mean exactly one
    // command, and the caller's offer can change between a failed attempt and
    // its retry, so only the first offer for a frame is taken. `tookCmd` tells
    // the caller which happened, and the pump pops its send queue only when its
    // command was the one that went out.
    if (!sHeldValid) {
        unsigned h = sFrame % LINK_HISTORY;

        memcpy(sHeld, sendCmd, CTR_LINK_CMD_BYTES);
        sHeldValid = 1;
        took = 1;

        memcpy(sSentCmd[h], sendCmd, CTR_LINK_CMD_BYTES);
        sSentFrame[h] = sFrame;
        sSentValid[h] = 1;
    }
    if (tookCmd != NULL)
        *tookCmd = took;

    memset(&out, 0, sizeof out);
    out.frame = sFrame;
    out.hs = sHsOut;
    out.phase = PHASE_LIVE;
    out.count = (uint8_t)fill_history(out.cmd, sFrame);

    if (R_FAILED(udsSendTo(UDS_BROADCAST_NETWORKNODEID, LINK_CHANNEL,
                           UDS_SENDFLAG_Default, &out, sizeof(out)))) {
        // A failed send is a dropped frame, not a dead link: the caller
        // reports the miss and we send this same frame again next time. The
        // counter must NOT advance here, or this console runs ahead of a peer
        // that never saw the frame. See the note at the end of this function.
        sMissWhy = MISS_SEND;
        return 0;
    }

    // Frame 0 primes the jitter buffer and takes nothing, because there is no
    // frame older than the first one.
    //
    // It used to wait for the peer's frame 0 and hand it up, and then frame 1
    // asked for frame 0 again, because the target was clamped at 0 for both.
    // The peer's first command reached the game twice.
    if (sFrame == 0) {
        drain();
        memcpy(dst + local * CTR_LINK_CMD_BYTES, sHeld, CTR_LINK_CMD_BYTES);
        sFrame++;
        sHeldValid = 0;
        return 1;
    }

    // One frame behind. That lag IS the jitter buffer: it absorbs a late
    // packet without stalling, and it is why the target is a frame we can
    // reasonably expect to be sitting in the ring already.
    target = sFrame - 1;

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
    {
        // Time the wait even when it turns out to be zero: the mean over a
        // period is what says whether a link is comfortable or on the edge.
        uint64_t tw = svcGetSystemTick();

        deadline = tw + LINK_WAIT_TICKS;
        while (!ready) {
            uint64_t now = svcGetSystemTick();

            if (now >= deadline) {
                sDeadlineHits++;
                break;
            }

            s64 left = (s64)((deadline - now) * 1000 / TICKS_PER_US);

            if (R_FAILED(svcWaitSynchronization(sBind.event, left))) {
                sDeadlineHits++;
                break;   // the budget ran out
            }

            svcClearEvent(sBind.event);
            drain();
            ready = peers_ready(target, players, local);
        }

        {
            uint64_t spent = svcGetSystemTick() - tw;

            sWaitSum += spent;
            sWaitN++;
            sBlockedTicks += spent;
            if (spent > sWaitWorst)
                sWaitWorst = spent;
        }
    }

    if (!ready)
        sMissWhy = MISS_LATE;

    // Hand up this frame's set, and only on success.
    //
    // The slot is consumed as it is read, so a command goes to the game exactly
    // once. Delivering on a miss as well is what repeated a peer's command: the
    // old code copied whatever the single slot held every time round, whether
    // or not anything new had arrived.
    if (ready) {
        unsigned slot = target % LINK_RING;

        for (int p = 0; p < players; p++) {
            if (p == local) {
                // Our own command comes back to us on a cable, off the shared
                // bus. There is no bus here, so echo it: the latched one, which
                // is what actually went out.
                memcpy(dst + p * CTR_LINK_CMD_BYTES, sHeld,
                       CTR_LINK_CMD_BYTES);
            } else {
                memcpy(dst + p * CTR_LINK_CMD_BYTES, sRingCmd[p][slot],
                       CTR_LINK_CMD_BYTES);
                sRingFull[p][slot] = 0;
            }
        }
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
    if (ready) {
        sFrame++;
        sHeldValid = 0;     // the next frame may take a new command
    }

    return ready;
}

// A new LINK, which is not the same thing as a new network.
//
// The game opens and closes a logical link many times inside one wireless
// session. A trade that is cancelled goes back to the Cable Club room, where
// Task_ReestablishLink calls OpenLink() again (src/cable_club.c). Each link-up
// that fails at the counter does the same, and one held A button is enough to
// confirm before the partner and fail one.
//
// All of the state below belongs to one link, and none of it used to be
// cleared between two. The result was that only the FIRST link on a network
// worked:
//
// - sHsDone stayed set, so Ctr3dsLinkHandshake() agreed on its first call and
//   did not wait for the peer. Each console then left LINK_STATE_HANDSHAKE on
//   its own frame, which is the split that the handshake exists to prevent.
// - sFrame, the rings and a latched sHeld carried over, so the first commands
//   of the new link met leftovers from the old one.
//
// The player-data block that follows a link-up carries no sequence number, so
// out of step it either never completes or fails its magic check. Both answers
// are Emerald's communication error, on both consoles, and neither writes a
// line to the log.
//
// sFrame going back to zero while the peer still counts is safe: drain() drops
// a frame older than the one it owes, and handshake packets do not use the
// ring at all. The pair agrees again as soon as the second console resets.
void Ctr3dsLinkNewSession(const char *why)
{
    // Quiet when there is nothing to clear. OpenLink() and CloseLink() both
    // call this, so a Cable Club visit that never links must not log twice.
    if (sFrame != 0 || sHsDone)
        CtrLog("emerald3ds: link session reset (%s) after %lu frames\n",
               why, (unsigned long)sFrame);

    sFrame = 0;
    memset(sRingFull, 0, sizeof(sRingFull));
    memset(sRingFrame, 0, sizeof(sRingFrame));
    memset(sRingCmd, 0, sizeof(sRingCmd));

    sHeldValid = 0;
    memset(sHeld, 0, sizeof(sHeld));
    memset(sSentValid, 0, sizeof(sSentValid));
    memset(sSentFrame, 0, sizeof(sSentFrame));
    memset(sSentCmd, 0, sizeof(sSentCmd));
    memset(sPeerSeen, 0, sizeof(sPeerSeen));
    memset(sPeerNewest, 0, sizeof(sPeerNewest));

    sPeerGone = 0;
    sNodeIdBad = 0;
    sMissRun = 0;

    sHsOut = 0;
    sHsDone = 0;
    sHsLogged = 0;
    sHsStablePlayers = 0;
    memset(sHsFrom, 0, sizeof(sHsFrom));
    memset(sHsSeen, 0, sizeof(sHsSeen));
}

// The HOME menu is about to freeze this console. Tell the peer once, so it can
// fail fast and correctly instead of calling us late for three seconds.
//
// Best effort by design: one broadcast, no retry, no wait for an answer. There
// is no second chance, and a goodbye that does not arrive leaves the peer
// exactly where it was before this existed.
//
// hs is zero because drain() harvests the handshake word off every accepted
// packet before it looks at the phase, and the size stays sizeof(LinkPacket)
// because the receiver rejects anything else as a short packet.
void Ctr3dsLinkSuspending(const char *why)
{
    LinkPacket out;
    CtrLinkStatus st;

    if (!sBound || sBusy)
        return;

    Ctr3dsLinkGetStatus(&st);
    if (st.state != CTR_LINK_CONNECTED || st.playerCount < 2)
        return;

    memset(&out, 0, sizeof out);
    out.phase = PHASE_BYE;
    udsSendTo(UDS_BROADCAST_NETWORKNODEID, LINK_CHANNEL, UDS_SENDFLAG_Default,
              &out, sizeof(out));

    CtrLog("emerald3ds: link goodbye sent (%s)\n", why);
}

// Coming back. Two separate things are broken and both have to be said.
//
// The LINK is over: a lockstep cannot survive a suspend, this console has been
// still for however long the menu or the sleep lasted, and the peer has either
// given up already or is about to.
//
// The SESSION is over too, and that is the one that used to be missed. The
// system re-initialises NWM while we are away, so the UDS handle this process
// holds is dead whether or not a network was up. Note the gate is sUdsUp, NOT
// sBound: a console that only pressed SCAN and then suspended has no bind to
// tear down and is just as unable to scan again.
void Ctr3dsLinkResumed(const char *why)
{
    CtrLinkStatus st;

    Ctr3dsLinkGetStatus(&st);

    if (sBound && (st.state == CTR_LINK_CONNECTED || st.state == CTR_LINK_HOSTING)
     && !sPeerGone) {
        sPeerGone = 1;
        CtrLog("emerald3ds: link ended by a %s on this console\n", why);
    }

    // Not a posted request: post() drops one while the worker is busy, and on
    // resume it may still be inside the udsScanBeacons it was suspended in. A
    // sticky flag cannot be dropped, and ensure_uds() is the one place every
    // wireless operation already passes through.
    sUdsStale = 1;
}

// Should the console refuse to sleep?
//
// Sleep takes the wireless away, which kills a live link outright, and a log
// caught exactly that on a console nobody had touched. Refusing it for the
// duration is cheaper than recovering from it. HOME is deliberately left alone:
// it is the only way out of a link that has wedged.
//
// A plain read of a flag the worker sets, so the main thread can call this
// every frame without an IPC round trip.
int Ctr3dsLinkNoSleep(void)
{
    return sNoSleep;
}

// ------------------------------------------------------------ diagnostics ---

// Called from TrySetLinkErrorBuffer() in src/link.c, the one place every link
// error passes through. Without it, Emerald's own error screen and a real fault
// look the same in a log: the game stops talking and nothing says why.
// Proof that the multiplayer id reached the register the game reads.
//
// `local` is what UDS says this console is; `sio` is what GetMultiplayerId()
// answers, read back from REG_SIOCNT. They must agree. They did not before
// Ctr3dsSetSioMultiId() existed: `sio` was 0 on every console, so both sides
// of a trade took the player-0 branch.
//
// Logged when it changes, not every frame, the same way the roster line is.
// How long this frame's exchange spent asleep waiting on a peer. Read and
// cleared, so each frame's figure is counted once.
unsigned long long Ctr3dsLinkTakeBlockedTicks(void)
{
    unsigned long long t = sBlockedTicks;

    sBlockedTicks = 0;
    return t;
}

void Ctr3dsLinkLogIds(int local, int sio, int isMaster)
{
    // host too, read here rather than passed in, so the seam stays the same
    // shape. local and sio are two views of one value and isMaster is derived
    // from local, so the three of them agreeing proves nothing. host is the
    // independent fact: "local=0 host=0" is a client calling itself the master,
    // which is the contradiction sane_local_id() now prevents. Print it so a
    // log shows it in one line rather than by correlating with a `link peers`
    // line a minute earlier.
    CtrLinkStatus st;
    int packed = (local & 3) | ((sio & 3) << 2) | ((isMaster ? 1 : 0) << 4);

    if (packed == sLoggedIds)
        return;

    Ctr3dsLinkGetStatus(&st);

    sLoggedIds = packed;
    CtrLog("emerald3ds: link ids: local=%d sio=%d master=%d host=%d\n",
           local, sio, isMaster ? 1 : 0, st.isHost ? 1 : 0);
}

void Ctr3dsLinkLogError(unsigned int status, int sendCount, int recvCount)
{
    CtrLog("emerald3ds: link error status=%08X send=%d recv=%d missrun=%u "
           "frame=%lu players=%u\n",
           status, sendCount, recvCount, sMissRun, (unsigned long)sFrame,
           (unsigned)sStatus.playerCount);
}

// A link fault that does NOT pass TrySetLinkErrorBuffer().
//
// That function logs every error the status word can name: lag, a full queue, a
// bad checksum. Four more routes reach the same error screen and said nothing,
// so a log showed only a link that stopped talking. `why` names the route.
void Ctr3dsLinkLogFault(const char *why)
{
    CtrLog("emerald3ds: link fault (%s) missrun=%u frame=%lu players=%u\n",
           why, sMissRun, (unsigned long)sFrame,
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

    if (sUdsUp) {
        udsExit();
        sUdsUp = 0;
    }
}
