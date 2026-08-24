# Transport layer — what file replay deliberately skipped

This project replays a captured file. A production feed handler consumes
MoldUDP64 multicast. This doc records what that transport layer does,
and exactly what would change here if the engine sat behind it. Field
sizes and semantics below are per the MoldUDP64 V 1.00 specification
(nasdaqtrader.com → Specifications → Data Products).

## What the wire actually carries

TotalView-ITCH is delivered inside MoldUDP64 downstream packets, sent
via UDP multicast. Each packet is a 20-byte header plus zero or more
message blocks:

| Offset | Size | Field |
|---|---|---|
| 0 | 10 | session (alphanumeric) |
| 10 | 8 | sequence number of the *first* message in the packet |
| 18 | 2 | message count |
| 20 | — | count × (2-byte message length + ITCH payload) |

Sequence numbers count *messages*, not packets; messages after the
first in a packet are implicitly numbered sequentially. The message
length excludes its own 2 bytes. Two packet types carry no blocks:
heartbeats (count 0, sent ~1/s, carrying the next expected sequence
number so loss is detectable even in quiet periods) and end-of-session
(count 0xFFFF, sent for a while in place of heartbeats; last chance to
re-request).

The mapping to this repo's input: strip the 20-byte headers, drop the
zero-block packets, concatenate the message blocks in sequence order —
the result is byte-for-byte the file format described in
[data.md](data.md). The sample file *is* the post-Mold byte stream of
one session; its 2-byte length framing is the message block framing,
preserved. Sequence numbers survive implicitly as message index.

## Gap detection and recovery

Detection is sequence arithmetic against one counter, `next_expected`:

- packet seq == next_expected → process all blocks, advance by count.
- packet seq > next_expected → gap of (seq − next_expected) messages.
- packet seq + count ≤ next_expected → duplicate, drop.
- partial overlap → process only messages ≥ next_expected.

Recovery path 1 — retransmission. A request packet (same 20-byte shape:
session, first requested seq, requested count) goes via UDP unicast to a
re-request server; the response is a standard downstream packet unicast
back, readable on the same socket as the multicast. Hard limit: only the
messages that completely fit in one UDP response packet are returned —
at ~38 bytes per book message, roughly 35 messages per ~1,400-byte
payload — so a large gap costs one round trip per packet's worth.

Recovery path 2 — snapshot. Glimpse serves the current book state as
ITCH-format messages over SoupBinTCP (a sequenced, logged-in TCP session
protocol); its end-of-snapshot message carries the multicast sequence
number the snapshot is current through. The client joins the multicast,
buffers, takes the snapshot, then applies buffered messages beyond that
sequence number. The choice is arithmetic: a gap of a few hundred
messages is a handful of retrans round trips; a mid-day start or a
multi-second outage (this day averages ~7.3k msgs/s across the session,
with far higher bursts) means thousands of round trips — re-sync from
snapshot instead.

## A/B feed arbitration

Nasdaq transmits each stream twice, on A and B multicast groups from
separate infrastructure. The receiver joins both and, per message,
first-arrival wins; the duplicate-drop rule above is the dedup — a
message with seq < next_expected is discarded regardless of which feed
carried it. If losses on the two feeds are independent, effective loss
is the product of the two loss rates.

Arbitration changes gap handling: a hole on feed A is usually filled by
feed B microseconds later, so a gap must age past a holdback timer
before it triggers a retrans request — fire immediately and you spam the
re-request server for messages already in flight. The escalation ladder
is: other feed → timer → retransmission → snapshot.

## What live multicast would change in this repo

Nothing behind the framing boundary. The decoder and book consume
length-prefixed messages in sequence order — exactly what the file
provides and exactly what a correct front-end emits after arbitration
and gap fill. What gets added is a front-end owning:

- two UDP sockets (A/B groups) plus the unicast retrans path;
- per-session state: the 10-byte session id, `next_expected` (u64);
- a holdback buffer of ahead-of-sequence packets while a gap is open,
  with the gap timer and in-flight retrans bookkeeping;
- a Glimpse client (SoupBinTCP session, login, snapshot splice) and a
  buffer-during-snapshot mode for late join.

## Scope, honestly

File replay was the right scope: the free sample file gives a full real
day (423,285,709 messages) against which decode and book logic are
falsifiable end to end; live TotalView multicast requires an extranet or
colo cross-connect and a market data agreement. The correctness results
transfer — the bytes are the same bytes.

The latency numbers do not transfer. [benchmarks.md](benchmarks.md)
measures throughput of decode + book on an mmap'd byte stream: 7.4M
msgs/s sustained, ~135 ns/message. That bounds *capacity* — roughly
10³× this day's average feed rate, so the engine is not the bottleneck
behind a live feed. It says nothing about tick-to-book *latency*, which
on a live handler is dominated by the receive path (kernel UDP is
microseconds; kernel-bypass NICs exist precisely because of this),
arbitration holdback, and loss-recovery stalls. No number in this repo
should be read as a wire-to-book latency claim.
