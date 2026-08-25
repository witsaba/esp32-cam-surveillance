idf_component_register(
    SRCS "control_route.c"
         "control.c"
    INCLUDE_DIRS "include"
    # FW-18 — inbound command dispatcher. REQUIRES json: the PUBLIC
    # header control.h carries `const cJSON *` in control_parsed_t,
    # so the managed cJSON dep must be a declared requirement
    # (idf.py build enforces header-level deps). PRIV_REQUIRES: ws
    # for the reply-emission seam (ws_sink_send_text) — the
    # control↔ws MUTUAL PRIV edge (ws_server.c calls back into
    # control for ingest in U3) mirrors the proven boot↔health
    # pattern; boot for the supervision-step enum + task stack/prio
    # constants, closing the boot↔control mutual edge (boot
    # REQUIRES control public). Both edges are proven by
    # `idf.py build`.
    REQUIRES json
    PRIV_REQUIRES ws boot
)
