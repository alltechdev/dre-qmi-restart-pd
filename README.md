# qmi-restart-pd

Tiny QMI clients used while bringing up WCN3990 wifi in a custom recovery
on dre (OnePlus Nord N200 5G). Both speak straight AF_QIPCRTR and produce
self-contained static aarch64-musl binaries.

- `restart-pd <pd-path>` — sends `QMI_SERVREG_NOTIF_RESTART_PD_REQ` (msg
  id 0x0024) to modem's SERVREG_NOTIF service (id 0x42, instance 180).
  Useful for asking modem firmware to (re)start a sub-PD like
  `msm/modem/wlan_pd`.
- `query-pd <pd-path>` — sends `QMI_SERVREG_NOTIF_QUERY_STATE_REQ`
  (msg id 0x0021) and prints the PD's current state.

Build: `make` with the musl cross-compiler on PATH. Both run on
`/linux/bin/` on the device.

Status note (2026-04): `restart-pd msm/modem/wlan_pd` returns
`result=1 error=9` (QMI INVALID_HANDLE) on this modem firmware.
The OnePlus modem firmware on dre apparently refuses RESTART_PD
without a prior in-band session/handle (the cnss-daemon role).
Tool kept around because it's useful to verify the QMI path works
and as a starting point for whatever comes next.
