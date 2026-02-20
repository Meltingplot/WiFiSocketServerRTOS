# WiFiSocketServerRTOS: Connection Stability Analysis

## Summary

Investigation of connection instability in the Duet3D WiFiSocketServerRTOS firmware under load (file upload + multiple DWC polling clients). The original `closePending` code had the right idea — defer close to avoid blocking — but contained bugs that caused resource exhaustion and connection stalls.

An intermediate approach of removing closePending entirely (closing connections immediately) was tested. It worked on fast WiFi but blocked the SPI main loop for up to 2 seconds per close on slow WiFi, freezing all communication with the SAM processor.

The final fix keeps closePending but corrects its bugs: waits for both unsent and unacked data to drain (rather than the original unacked-only check), removes the blocking netconn_shutdown, uses a short sendtimeout, and adds null safety. Combined with ERR_MEM resilience and several pre-existing bug fixes in the original code.

---

## Why Not Just Close Immediately?

`netconn_close()` is a blocking call — it sends a TCP FIN and waits for FIN/ACK, blocking up to `sendtimeout` (default 2000ms). `Close()` runs in the SPI main loop context. Blocking there freezes SPI communication with the SAM processor, causing cascading failures: the SAM reports SPI timeouts, stops sending data, and all connections stall.

The original code's closePending deferred the blocking `netconn_close()` to `Poll()`, which was the right approach. But its implementation had bugs that caused resource exhaustion under DWC connection churn.

---

## What Was Broken in the Original closePending

### Bug 1: Waiting for Unacked Instead of Unsent

The main issue. The original waited for `conn->pcb.tcp->unacked` to drain (up to 4 seconds). For DWC HTTP connections, this wait is futile:

```
T=0ms    Server sends HTTP response → data enters unacked queue
T=0ms    RRF calls connClose → enters closePending
T=~50ms  Client receives data, queues delayed ACK
T=~55ms  Browser calls close() → sends RST (cancels delayed ACK)
T=~57ms  lwIP receives RST → frees PCB
```

The ACK is never sent because the client's RST preempts it. closePending held a PCB and connection slot waiting for nothing. Under DWC churn (~12 conn/s) with lwIP's pool of ~10 PCBs, this exhausted the pool. lwIP's `tcp_kill_prio()` then killed the oldest connection — the file upload.

**Unsent vs unacked**: Unsent data is queued but not yet handed to the IP layer. Unacked data has been transmitted but not yet acknowledged by the remote end. The code now waits for both queues to drain before closing. While lwIP does retain the TCP PCB after `netconn_delete()` and can continue retransmitting, if `netconn_close()` times out the resulting RST would abandon any unacked segments. Waiting for the unacked queue to empty as well eliminates this window.

### Bug 2: netconn_shutdown() Blocking in Close()

The original Close() called `netconn_shutdown(conn, true, false)` before entering closePending. This is a synchronous call through `tcpip_apimsg` — it blocks the SPI main loop until the lwIP thread processes it. And shutting down just the receive side was pointless since we don't read data during closePending anyway.

### Bug 3: No Null Guards

`conn->pcb.tcp->unacked` was accessed without null checks. If lwIP freed the PCB during the closePending wait (e.g., RST received), this crashed.

### Bug 4: Timeout Went to Aborted Instead of Free

When the closePending timeout expired, the original called `Terminate(false)` which set the state to `aborted` instead of `free`. The aborted state requires a round-trip to RRF to reclaim the slot — adding latency under the highest-pressure conditions when you need slots freed fastest.

### Bug 5: closeReady Re-entrance

The original used a `closeReady` intermediate state: closePending→closeReady→Close(). The Close() re-entrance could block the SPI main loop again with `netconn_close()`.

---

## How It Works Now

### Close() — Never Blocks

```cpp
case ConnState::connected:
    closeTimer = millis();
    SetState(ConnState::closePending);  // returns immediately
    break;
```

No netconn_shutdown, no netconn_close, no blocking. Just set a timer and defer to Poll().

### Poll() closePending — Non-blocking with Notify

```cpp
else if (state == ConnState::closePending)
{
    const bool timeout = millis() - closeTimer >= MaxSendWaitTime;  // 2s
    if (!conn || !conn->pcb.tcp || !conn->pcb.tcp->unsent || timeout)
    {
        if (conn)
        {
            netconn_set_sendtimeout(conn, timeout ? 1 : 100);  // short timeout
            netconn_close(conn);
            netconn_delete(conn);
            conn = nullptr;
        }
        FreePbuf();
        SetState(ConnState::free);
        if (listener)
        {
            listener->Notify();     // wake ListenerTask to accept from backlog
        }
    }
}
```

- **Null guards** — handles PCB freed by lwIP (RST/timeout during wait)
- **Unsent check** — frees slot as soon as data reaches IP layer
- **100ms sendtimeout** — enough for FIN/ACK, won't block the main loop long
- **1ms on timeout** — abort path, don't wait at all
- **Direct free + Notify** — slot available immediately, listener wakes to drain backlog

### Data Safety: Waiting for Both Unsent and Unacked

The code now waits for both `unsent` and `unacked` queues to drain before proceeding with the close. This ensures all data has been acknowledged by the remote end before we call `netconn_close()`.

In the normal case, `netconn_close()` sends FIN and `netconn_delete()` frees the netconn struct while the TCP PCB continues to live in lwIP's internal lists for the TIME_WAIT period. Since both queues are empty at this point, there is no data at risk.

On timeout (2 seconds), we proceed regardless — at that point the connection is likely dead and the short sendtimeout (1ms) on `netconn_close()` ensures we don't block. Any remaining unacked data is abandoned, but we've already waited the maximum reasonable time.

When `Write()` calls `netconn_write_partly()`, data is **copied** into lwIP-owned pbufs (the `NETCONN_COPY` flag is set, and `LWIP_NETIF_TX_SINGLE_PBUF` forces a copy). When RRF reuses the slot for a new connection, it gets a brand new `netconn` with a brand new TCP PCB — no shared memory with any old PCB.

---

## Pre-existing Bugs Fixed

These existed in the original code (commit 76ec022) independent of closePending:

| Bug | Impact | Fix |
|-----|--------|-----|
| NETCONN_MORE inverted (`push ? NETCONN_MORE : 0`) | Packet fragmentation — set "more data coming" when flushing | Swap ternary operands |
| Write() returns `length` not `total` | Silent data loss on partial writes | Return actual bytes written |
| Write() spins on ERR_WOULDBLOCK with written==0 | CPU spin when send buffer full | Break when no progress |
| ERR_MEM treated as fatal in Write() | Connections killed under memory pressure | Treat as retryable like ERR_WOULDBLOCK |
| ERR_WOULDBLOCK partial write falls through to Terminate() | Connections killed on partial writes | Guard with `rc != ERR_WOULDBLOCK && rc != ERR_MEM` |
| ERR_ABRT not recognized in Poll()/Write() | Wrong state transition (Terminate vs otherEndClosed) | Add to otherEndClosed set |
| ERR_TIMEOUT treated as error in Poll() | Spurious termination | Add to non-error set |
| Connect() returns true on failure | Caller thinks connection succeeded | Return false |
| CanWrite() dereferences null conn | Crash | Add null guard |
| Terminate() calls Notify() unconditionally | Crash on null listener; spurious notify on aborted state | `if (external && listener)` |
| Allocate() uses portMAX_DELAY | Deadlock if mutex stuck | Null check + 200ms timeout |
| Listener::Stop() use-after-free | Compares freed pointer | Save to local var before delete |
| FTP listener use-after-free | Connection holds deleted listener pointer | Clear pointer before Stop() |
| Rejected connections not drained | PCBs leak in accept backlog | Accept + immediately close |
| Report() missing "allocated" state | Wrong state display | Add string to array |

---

## Verification

### Fast WiFi
- 7 DWC clients + 1 API client + file upload
- 2,919 connections over 9 minutes, no stalls or errors
- File uploads completed without interruption

### Slow WiFi (Simulated 2G: 50 KB/s, 500ms Latency)
- 7 slow clients loading DWC simultaneously
- Initial burst saturated slots for ~10-15 minutes (large static assets)
- Self-recovered once browser caches populated
- 1 GB file upload completed without interruption
- All 7 slow clients served at steady state

---

## Remaining Considerations

- **MEMP_NUM_TCP_PCB**: ESP8266 default ~10, not explicitly configured. Sufficient with unsent-based closePending. Increasing to 16 would add headroom (~2 KB static RAM).
- **100ms sendtimeout on slow WiFi**: Times out before FIN/ACK on 500ms+ RTT links. RST is sent instead. Acceptable: data was already delivered (unsent drained), RST only affects teardown. Client sees NS_BINDING_ABORTED but response data was processed.
- **closeReady enum value**: No longer used but preserved in `ConnState` enum in `MessageFormats.h` for SPI protocol compatibility with RepRapFirmware.
