<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# AS query publication without native timestamps

This checkpoint extends `76d11d69b1531d5df745107e1361a357382a6bdc` on
`codex/ray-tracing-fallback`. The complete mandatory AS/RQ/RTP goal remains
active. The user permits explicit newer optional-extension incompatibilities
and does not require an all-green test suite. This change addresses a baseline
query restriction rather than expanding optional interoperability.

The previous query backend required `timestampValidBits != 0` on its compute
family, although the AS property-write command does not require timestamps.
It used a native timestamp query as a private completion marker. Normal core
devices now use a host-queryable native Event for each replay operation.

## Publication and ownership

Source command capture allocates no event or payload. Each executed primary or
secondary occurrence creates its own initially unsignaled event with flags zero
and its own coherent payload. Publication preserves this sequence:

`CS payload writes -> CS-to-(CS|HOST) memory barrier -> real CS sentinel -> CmdSetEvent(CS)`.

The sentinel lies in the barrier's destination scope and the event signal's
source scope. Observing SET therefore follows that execution chain. The barrier
retains `SHADER_WRITE -> SHADER_READ|HOST_READ`; coherent payload storage makes
the resulting host-domain writes visible. `GetEventStatus` supplies observation,
not a replacement memory dependency. These requirements follow the pinned
[synchronization chapter](https://raw.githubusercontent.com/KhronosGroup/Vulkan-Docs/v1.4.357/chapters/synchronization.adoc)
and [memory model](https://raw.githubusercontent.com/KhronosGroup/Vulkan-Docs/v1.4.357/appendices/memorymodel.adoc).

An ordinary RESET observation delays harvesting. Negative native status codes
propagate; unexpected positive statuses fail with UNKNOWN. If a submitted
recording is independently known complete but its sole-setter event is still
RESET, retirement reports and retains UNKNOWN rather than discarding an
unpublished result as successful. Errors from void command-reset/free paths are
latched for the next public query.

SET and harvesting never release the event or payload. Existing private batch
fences, qualified later same-queue completion proofs, or explicit idle
checkpoints govern retirement. UNKNOWN/LOST quarantine remains intact. The old
direct host adapter cannot reuse a submitted Event operation; actual production
replay creates new occurrences and supports repeated/simultaneous source use.
No hidden queue, private semaphore signal or queue worker is introduced.

## Shared capabilities and admission

The shared software profile carries the marker choice through discovery,
private feature augmentation and actual device initialization:

| Backend | Requirements and behavior |
| --- | --- |
| Event | Core events, buffer int64 atomics and existing shader/resource prerequisites; no private hostQueryReset requirement or host-reset alias; compute families may report zero timestamp bits |
| Timestamp fallback | Only when native portability-subset events are unsupported; requires actual hostQueryReset, its application-version alias, at least one usable compute family and nonzero timestamp bits on every usable compute family |
| Neither | No software candidate; outputs remain transactional |

Core events are assumed only outside portability subset. When the native
extension exists, the full SDK portability structure is queried. If the
application enables portability subset and the Event backend is selected, the
private device feature copy enables only `events`; the other fourteen feature
bits and caller storage remain unchanged. The complete beta structure has a
local chain-size adapter without enabling provisional types project-wide.

The fallback's family check ignores non-compute families and is not imposed on
Event or ordinary non-RT devices. Missing native resolvers, invalid counts or
allocation failures cannot qualify that fallback. Its restriction declares an
unsupported substrate; it does not implement a timestamp-free fallback without
events.

Missing selected native marker entry points now fail initialization instead of
returning success with no query backend. Software RT devices with more than one
physical device are explicitly rejected before native creation; ordinary
non-RT groups pass through. This replaces a silent query-disable path and is
not an implementation of multi-device support. Protected pools remain
unsupported; complete feature/profile qualification for those combinations
remains open.

## Verification and retained failures

The [manifest](rt_query_events_2026-10-28.json) binds sources, DLLs, exact commands
and raw observations. The final DLL is
`1b51f3c42f0438d741f0f5fbcc643fa955ec6aaa507b724363eed2d0c9e33d4c`.
Native controls and software results use their own AS representations and sizes;
equal compacted sizes across representations are not required.

| Observation | Result |
| --- | --- |
| Unified offline checks | 47/47 passed; four affected final host checks also passed after fallback qualification |
| Final public GPU checks | 11/11 positive runs passed, including three native controls; one previous-DLL negative control failed as expected |
| External CTS with masked native RT and injected zero timestamps | 13/13 passed; 27282 guard checks, zero rejections/timestamp writes/validation markers; 20 Event signals and 20 Event status queries |
| Build integration | Standalone build and top-level RT target passed |

CTS is the external `D:/Codes/VK-GL-CTS` executable at declared revision
`d18e4c68543103371715845cd914c71e5859963e`. The thirteen cases cover base
compaction, serialization, RQ AS operations, a pipeline library, geometry
transform UPDATE and typed data spill. They overlap previous checkpoint slices
and are not additional unique conformance coverage. No complete-suite pass is
claimed or required.

The new public fixture executes six Reset/Write/Copy cycles using legacy and
Submit2 submissions, compares copied results with separate host GetResults,
checks nonzero availability and all padding, then checks values across command
buffer/pool reset and observes query reset becoming unavailable. Existing
replay/acquire fixtures supply independent serialization-size oracles,
primary/secondary occurrences, overlapping submissions, concurrent host reads,
and historical/cross-queue narrow-wait cases.

An explicit guard profile reports all queue-family timestamp bits as zero and
`timestampComputeAndGraphics=false`, while rejecting every native timestamp
write. It records real property/family queries, event signals/status polls and
strictly typed profile/version counters. Native controls permit native RT and
are never counted as no-native-RT boundary proof. Software runs additionally
mask native RT extensions/features/properties/formats/function lookup. Audits
must request both profiles explicitly; inherited injection is cleared by the
CTS runner, and forced dispatch cannot be mixed into this proof.

This injection is a regression on the RTX 3080, not qualification on a physical
timestamp-free or no-RT GPU. The old `327e1348...` DLL rejects the same
timestamp-zero public fixture, providing the retained implementation negative
control. A default successful run alone cannot establish the injected path.

Retained preliminary observations include:

- The first new fixture omitted a Fill-to-Copy transfer write dependency. Native
  validation identified the WAW; the corrected fixture retains the raw failure.
- One native acquire control initially completed correctly but lacked a device
  property query required by the strict profile auditor. The test helper now
  observes/reports the physical property; the original unaudited observation
  is not promoted to a verified injected-profile pass.
- The legacy `test_query_pool_gpu` produced native WAW diagnostics and false
  assumptions about strictly increasing compacted sizes and simultaneous-query
  rejection. Its cross-queue tests also expected the obsolete guard rejection.
  It is retired, with Git and raw evidence retained, rather than treated as
  evidence against the new supported behavior.

Before retiring that legacy fixture, its valid reset/value-lifetime assertions
were extracted into the new public test. Replay/acquire fixtures replace its
query result, padding, repeat, concurrency and source-wait coverage with
independent oracles. Its 64-iteration stress count is not claimed preserved.
The old host-reset call had no independent unavailable-result observation and
did not establish a unique GPU oracle.

## Remaining work

The Event route closes the specific timestamp prerequisite on the tested
singleton, unprotected compute substrate. Full M0-M5 are not complete:
device groups, protected/profile combinations, native query interactions,
complete synchronization and resource budgets, remaining compiler/AS numerical
and count coverage, and physical no-RT/cross-vendor/POSIX qualification remain
open. Generic reset/copy queue lists alone do not prove a legal graphics-only
AS-query case; query-type family constraints still apply. Continue from these
baseline gaps without requiring every newer optional extension or every CTS
case to pass.
