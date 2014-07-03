
declare void @do_safepoint()


define void @gc.safepoint_poll() {
entry:
  call void @do_safepoint()
  ret void
}
