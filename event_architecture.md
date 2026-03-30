# Event Architecture

## Purpose

This document defines the event stream emitted by the exchange so downstream clients can reconstruct the order book and consume trade activity in real time.

## Session Bootstrap

Before normal event flow starts, publish session metadata so clients can interpret IDs correctly.

- Include symbol-to-internal-ID mappings.
- Include any other static reference data required to decode event payloads.

## Event Model Overview

The event stream follows ITCH-like semantics with internal naming adjusted to this codebase.

- `AddEvent`: an order is accepted into the book.
- `CancelEvent`: an order is removed from the book.
- `ModifyEvent`: resting order quantity is updated.
- `OrderExecutedEvent`: resting order is executed (partial or full).
- `OrderReplaceEvent`: cancel-replace operation creates a new order identity.
- `TradeEvent`: execution that is not represented as a resting book update.

## Event Definitions

### `AddEvent`

Emitted when an order is inserted into the order book.

Fields:
- `order_ref_number` (recommended)
- `symbol`
- `side`
- `price`
- `qty`

Notes:
- Market orders generally do not rest on the book, so they usually do not produce `AddEvent`.
- Time-in-force details are optional for pure reconstruction consumers, but may be useful for richer analytics.

### `CancelEvent`

Emitted when a resting order is removed from the order book.

Fields:
- `order_ref_number` (recommended)
- `symbol`
- `side`
- `price`
- `qty`

Trigger condition:
- Order is removed from both the price level and the book's order lookup.

ITCH alignment:
- Often corresponds to delete/cancel style messages.

### `ModifyEvent`

Emitted when a resting order's displayed quantity changes without changing its identity.

Fields:
- `order_ref_number`
- `old_qty` (optional but useful)
- `new_qty`

Notes:
- Consumers can derive delta quantity from `old_qty - new_qty` when both are present.
- In ITCH-style feeds, some consumers rely only on current quantity with reference-based state.

### `OrderExecutedEvent`

Emitted when a resting order is matched in part or in full.

Fields:
- `order_ref_number`
- `executed_qty`
- `match_ref_number`

Notes:
- This represents passive-side depletion due to matching.
- Distinct from `ModifyEvent`, which represents explicit quantity adjustment rather than trade execution.
- Optional extension: include execution price if it can differ from displayed price.

### `OrderReplaceEvent`

Emitted for cancel-replace behavior where an old order is canceled and a new order is created.

Fields:
- `old_order_ref_number`
- `new_order_ref_number`
- `qty`
- `price`

Notes:
- This matches the modify-order case that changes identity or priority in a way modeled as replace.

### `TradeEvent`

Emitted when a trade occurs outside the normal resting-book update path.

Typical examples:
- IOC interactions
- marketable flow that does not leave a resting order state transition to publish

Fields:
- `order_ref_number` (set to `0` if following ITCH convention for this message type)
- `symbol`
- `side`
- `qty`
- `price`
- `match_ref_number`

## Fill/Match Coverage Rules

For full reconstruction and auditability, publish both perspectives of a match:

1. Resting-side update:
	Emit `OrderExecutedEvent` for any passive order that is partially or fully filled.
2. Incoming/aggressing-side trade visibility:
	Emit `TradeEvent` for the active-side execution record when that side is not represented as a resting book update.

## Ownership and Runtime Architecture

`Exchange` is the top-level owner and coordinator.

Current ownership model:
- `Exchange` owns `MatchingEngine`.
- `Exchange` owns `EventSink`.

Planned/possible integration points:
- TUI or operator console.
- Worker threads that publish events over UDP multicast.
- Ingestion interfaces for simulator/order flow input.

## Open Decisions

- Confirm whether every event must include `order_ref_number` for strict ITCH parity and simpler client state management.
- Decide whether to include time-in-force fields in public events.
- Decide whether an executed-with-price-difference variant is needed in current engine behavior.
- Define exact bootstrap message schema and versioning.
