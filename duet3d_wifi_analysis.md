# WiFiSocketServerRTOS: Connection Stability Analysis

## Summary

Investigation of connection instability in the Duet3D WiFiSocketServerRTOS firmware under load (file upload + multiple DWC polling clients). The original `closePending` code had the right idea — defer close to avoid blocking — but contained bugs that caused connection stalls and resource exhaustion.

An intermediate approach of removing closePending entirely (closing connections immediately) was tested. It worked on fast WiFi but blocked the SPI main loop for up to 2 seconds per close on slow WiFi, freezing all communication with the SAM processor.

The final fix keeps closePending but corrects its bugs: adds the missing listener notification, waits for unsent data instead of unacked, uses a short sendtimeout, and adds null safety. Combined with ERR_MEM resilience and several pre-existing bug fixes in the original code.

---

## Why Not Just Close Immediately?

`netconn_close()` is a blocking call — it sends a TCP FIN and waits for FIN/ACK, blocking up to `sendtimeout` (default 2000ms). `Close()` runs in the SPI main loop context. Blocking there freezes SPI communication with the SAM processor, causing cascading failures: the SAM reports SPI timeouts, stops sending data, and all connections stall.

The original code's closePending deferred the blocking `netconn_close()` to `Poll()`, which was the right approach. But its implementation had three critical bugs.

---

## What Was Broken in the Original closePending

### Bug 1: Missing Listener Notification (The Flow Error)

**The most critical bug.** When `Poll()` completed a closePending→free transition, it never called `listener->Notify()`. The `ListenerTask` — the only consumer of the TCP accept backlog — blocks on `xTaskNotifyWait(portMAX_DELAY)` and only wakes from:

1. `ListenCallback` — fires on new incoming SYN (ISR context)
2. `Notify()` — explicit wake-up

Without Notify() on slot-free, this deadlock occurred:

```
T=0    All 8 slots occupied
T=1    Connection finishes → closePending
T=2    New SYN arrives → ListenCallback → sets notification bit
T=3    ListenerTask wakes, no free slot → can't accept
T=4    Poll() frees the slot
       ❌ No Notify() → ListenerTask stays blocked
       Backlog grows until next SYN triggers ListenCallback
```

In the worst case, all slots freed but no SYN arrived — server appeared completely dead with all slots showing free.

### Bug 2: Waiting for Unacked Instead of Unsent

The original waited for `conn->pcb.tcp->unacked` to drain (up to 4 seconds). For DWC HTTP connections, this wait is futile:

```
T=0ms    Server sends HTTP response → data enters unacked queue
T=0ms    RRF calls connClose → enters closePending
T=~50ms  Client receives data, queues delayed ACK
T=~55ms  Browser calls close() → sends RST (cancels delayed ACK)
T=~57ms  lwIP receives RST → frees PCB
```

The ACK is never sent because the client's RST preempts it. closePending held a PCB and connection slot waiting for nothing. Under DWC churn (~12 conn/s) with lwIP's pool of ~10 PCBs, this exhausted the pool. lwIP's `tcp_kill_prio()` then killed the oldest connection — the file upload.

**Unsent vs unacked**: Unsent data is queued but not yet handed to the IP layer. It drains in one `tcp_output()` call (microseconds on fast WiFi, one WiFi RTT on slow). Waiting for unsent is the correct threshold — once data reaches the IP layer, lwIP handles retransmission internally regardless of whether we hold the PCB.

### Bug 3: netconn_shutdown() Blocking in Close()

The original Close() called `netconn_shutdown(conn, true, false)` before entering closePending. This is a synchronous call through `tcpip_apimsg` — it blocks the SPI main loop until the lwIP thread processes it. And shutting down just the receive side was pointless since we don't read data during closePending anyway.

### Additional Issues in the Original closePending

- **No null guards** — `conn->pcb.tcp->unacked` crashed if lwIP freed the PCB during closePending (e.g., RST received)
- **Timeout went to aborted** — `Terminate(false)` set state to aborted instead of free, requiring a round-trip to RRF to reclaim the slot
- **closeReady re-entrance** — closeReady→Close() re-entered Close() which could block again with netconn_close

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
