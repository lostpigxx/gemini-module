# Stage 04 Runtime Matrix Failures

| Case | Area | Command | Expected | Actual | Classification | Evidence |
|---|---|---|---|---|---|---|
| metadata-acl-dryrun-default-add | metadata | `ACL DRYRUN default BF.ADD acl_key item` | default user can dry-run BF.ADD when Redis supports ACL DRYRUN; BLOCKED_ENV because Redis 6.2.16 does not expose ACL DRYRUN | error(ERR Unknown subcommand or wrong number of arguments for 'DRYRUN'. Try ACL HELP.) | BLOCKED | raw_resp.log#metadata-acl-dryrun-default-add |
| metadata-acl-ro-read | metadata | `ACL DRYRUN stage04_ro BF.EXISTS acl_key item` | read-only user can dry-run readonly BF.EXISTS when Redis supports ACL DRYRUN; BLOCKED_ENV because Redis 6.2.16 does not expose ACL DRYRUN | error(ERR Unknown subcommand or wrong number of arguments for 'DRYRUN'. Try ACL HELP.) | BLOCKED | raw_resp.log#metadata-acl-ro-read |
| metadata-acl-ro-write | metadata | `ACL DRYRUN stage04_ro BF.ADD acl_key item` | read-only user rejects write BF.ADD when Redis supports ACL DRYRUN; BLOCKED_ENV because Redis 6.2.16 does not expose ACL DRYRUN | error(ERR Unknown subcommand or wrong number of arguments for 'DRYRUN'. Try ACL HELP.) | BLOCKED | raw_resp.log#metadata-acl-ro-write |
