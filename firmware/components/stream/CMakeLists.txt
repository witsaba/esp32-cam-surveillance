# CMakeLists.txt — stream component (FW-15).
#
# 3-file split per design §File Changes:
#   stream.c           — task_start + loop iteration + counters
#   stream_fragment.c  — PURE planner (no IDF deps)
#   stream_sender.c    — thin IDF binary-send mapper
#
# PRIV_REQUIRES ws      — ws_handle_get() for the send target
#                capture— capture_queue_receive_timeout() consumer seam
#                boot   — BOOT_STEP_SUPERVISION_STREAM + task priq consts
idf_component_register(
    SRCS "stream.c"
         "stream_fragment.c"
         "stream_sender.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ws capture boot
)
