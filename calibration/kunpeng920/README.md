# Kunpeng 920 calibration

This compact package is derived from
`../experiments/platform-calibration/kunpen183-20260722/derived/hardware-node-edges.csv`.
The source SHA-256 is
`8970f02773554ffaa7249690746ca522201dcf17031beff885f4ae233f838924`.
It contains no machine ID, boot ID, raw latency matrix, or benchmark trace.
Line endings are normalized to LF in this repository; `checksums.sha256`
records the installed copy.

The default ClickHouse relationship scales come from
`20260723-two-node-threadpool-v1-gate2-smoke-fixed-v2/summary/relation-scales.json`,
SHA-256 `cd9dcd08a33549564a017d94dfc5fe9aed54cf8f510a018e0637ce8dad5cd8c0`.
The exact values are pinned in `config/affinitygraph.toml`.
