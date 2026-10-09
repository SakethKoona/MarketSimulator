/*
 * MarketSimulator ingress API, version 1.
 *
 * An ingress adapter is anything that turns some outside protocol into
 * orders for the exchange: the built-in BOE gateway, a FIX engine, a
 * websocket bridge, a Python bot framework. An adapter is a shared library
 * exporting the four mktsim_ingress_* functions below. The exchange loads
 * it from the "ingress" list in its config, hands it this API table, and
 * runs it alongside the matching engine.
 *
 * Contract:
 *  - Plain C ABI, little-endian, no exceptions across the boundary.
 *  - A session is the unit of ownership: orders submitted on a session
 *    report back to that session, for their whole life.
 *  - Threading: an adapter may create threads. submit() and poll() on one
 *    session may be called from any thread but not concurrently with each
 *    other on the same session. Different sessions are independent.
 *  - submit() never blocks. It returns MKTSIM_OK when queued, otherwise an
 *    error, and nothing was queued.
 *  - Reports arrive by polling: poll() drains up to max reports into cb.
 *    For a NewOrder, the first report is Accepted or Rejected; later
 *    Execution reports may follow for the order's life; Cancelled or a
 *    final Execution with leaves 0 ends it.
 *  - The adapter owns client-facing identifiers. The exchange knows only
 *    the 64-bit order_id it assigns and the request_id the adapter sends.
 */
#ifndef MKTSIM_INGRESS_API_H
#define MKTSIM_INGRESS_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MKTSIM_INGRESS_API_VERSION 1u

typedef struct mktsim_exchange mktsim_exchange; /* opaque */
typedef struct mktsim_session mktsim_session;   /* opaque */

/* Order request kinds. */
enum { MKTSIM_REQ_NEW = 1, MKTSIM_REQ_CANCEL = 2, MKTSIM_REQ_MODIFY = 3 };
/* Sides, order types, time in force. */
enum { MKTSIM_BUY = 'B', MKTSIM_SELL = 'S' };
enum { MKTSIM_MARKET = 1, MKTSIM_LIMIT = 2 };
enum { MKTSIM_GTC = 0, MKTSIM_IOC = 3, MKTSIM_FOK = 4 };

typedef struct mktsim_order_req {
    uint64_t request_id; /* adapter-chosen, echoed on the first report */
    uint8_t kind;        /* MKTSIM_REQ_* */
    uint8_t side;        /* NEW: MKTSIM_BUY/SELL */
    uint8_t ord_type;    /* NEW: MKTSIM_MARKET/LIMIT */
    uint8_t tif;         /* NEW: MKTSIM_GTC/IOC/FOK */
    uint32_t symbol_id;  /* NEW: from resolve_symbol */
    uint32_t qty;        /* NEW: quantity. MODIFY: new total quantity */
    uint64_t price;      /* NEW/MODIFY: integer price units; MODIFY 0 keeps */
    uint64_t order_id;   /* CANCEL/MODIFY: the exchange order id */
} mktsim_order_req;

/* Report kinds. */
enum {
    MKTSIM_RPT_ACCEPTED = 1,  /* new order accepted; leaves_qty may be 0 */
    MKTSIM_RPT_REJECTED = 2,  /* request rejected; see status */
    MKTSIM_RPT_CANCELLED = 3, /* cancel done, or IOC/FOK remainder dropped */
    MKTSIM_RPT_MODIFIED = 4,  /* modify done */
    MKTSIM_RPT_EXECUTION = 5  /* one fill on order_id; request_id 0 if unsolicited */
};
/* Status codes on Rejected (mirror the engine's StatusCode). */
enum {
    MKTSIM_ST_OK = 0, MKTSIM_ST_SYMBOL_NOT_FOUND = 1, MKTSIM_ST_ORDER_NOT_FOUND = 2,
    MKTSIM_ST_FAILED = 3, MKTSIM_ST_NOT_ENOUGH_LIQUIDITY = 4, MKTSIM_ST_FOK_FAILED = 5,
    MKTSIM_ST_INVALID_PRICE = 6, MKTSIM_ST_INVALID_QTY = 7, MKTSIM_ST_DUPLICATE = 8
};

typedef struct mktsim_report {
    uint64_t request_id;
    uint64_t order_id;
    uint8_t kind;   /* MKTSIM_RPT_* */
    uint8_t status; /* MKTSIM_ST_* */
    uint8_t side;   /* MKTSIM_BUY/SELL */
    uint8_t _pad;
    uint32_t symbol_id;
    uint32_t qty;        /* ACCEPTED: order qty. MODIFIED: new qty */
    uint32_t last_qty;   /* EXECUTION: filled qty */
    uint32_t leaves_qty; /* ACCEPTED/MODIFIED: resting qty after the op */
    uint64_t price;      /* ACCEPTED/MODIFIED: order px. EXECUTION: fill px */
    uint64_t match_id;   /* EXECUTION: same id as on the public feed */
    uint64_t ts_ns;      /* wall clock */
} mktsim_report;

typedef struct mktsim_symbol_info {
    uint32_t symbol_id;
    char ticker[16]; /* NUL-terminated */
} mktsim_symbol_info;

/* submit() results. */
enum { MKTSIM_OK = 0, MKTSIM_EBUSY = 1, MKTSIM_EBADSYMBOL = 2, MKTSIM_EBADREQ = 3, MKTSIM_ECLOSED = 4 };

typedef void (*mktsim_report_fn)(void *user, const mktsim_report *report);

/* Functions the exchange provides. Stable for a given version. */
typedef struct mktsim_exchange_api {
    uint32_t version; /* MKTSIM_INGRESS_API_VERSION */
    /* Sessions. name is for logs (may be NULL). */
    mktsim_session *(*open_session)(mktsim_exchange *ex, const char *name);
    void (*close_session)(mktsim_session *s);
    /* Orders. */
    int (*submit)(mktsim_session *s, const mktsim_order_req *req);
    /* Drains up to max reports to cb; returns how many were delivered. */
    size_t (*poll)(mktsim_session *s, mktsim_report_fn cb, void *user, size_t max);
    /* Reference data. */
    int (*resolve_symbol)(mktsim_exchange *ex, const char *ticker, size_t len, uint32_t *out_id);
    size_t (*symbols)(mktsim_exchange *ex, mktsim_symbol_info *out, size_t max);
    /* Utilities. */
    uint64_t (*now_ns)(void); /* wall clock, ns since epoch */
    void (*log)(mktsim_exchange *ex, const char *adapter, const char *msg);
} mktsim_exchange_api;

/*
 * Entry points a plugin exports (looked up with dlsym).
 *   init:    called once; config_json is this adapter's object from the
 *            exchange config (may be "{}"). Return 0 on success and set
 *            *state; nonzero aborts startup.
 *   start:   begin serving (spawn threads, listen). Return 0 on success.
 *   stop:    stop serving and join threads; called before destroy.
 *   destroy: free state. Sessions still open are closed by the exchange.
 */
typedef int (*mktsim_ingress_init_fn)(const mktsim_exchange_api *api, mktsim_exchange *ex,
                                      const char *config_json, void **state);
typedef int (*mktsim_ingress_start_fn)(void *state);
typedef void (*mktsim_ingress_stop_fn)(void *state);
typedef void (*mktsim_ingress_destroy_fn)(void *state);

#define MKTSIM_INGRESS_INIT_SYMBOL "mktsim_ingress_init"
#define MKTSIM_INGRESS_START_SYMBOL "mktsim_ingress_start"
#define MKTSIM_INGRESS_STOP_SYMBOL "mktsim_ingress_stop"
#define MKTSIM_INGRESS_DESTROY_SYMBOL "mktsim_ingress_destroy"

#ifdef __cplusplus
}
#endif
#endif
