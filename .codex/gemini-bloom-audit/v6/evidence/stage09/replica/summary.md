# Stage 09 Replica Summary

- `live`: `PASS`
- `fullsync`: `PASS`
- `reconnect`: `PASS`
- `loading_partial`: `FAIL_CARRY_FORWARD_GBV6-07-002`

Normal completed-filter live replication, fullsync, and reconnect checks are PASS only when all inserted items remain present on the replica.
`loading_partial` is expected to carry forward Stage 07 if a half-loaded key replays as a readable completed filter.
