# Stage 04 Runtime Matrix Normalized Results

- Total rows: 182
- Failed/blocking rows: 3

| Case | Area | Command | Expected | Actual | Classification | Result |
|---|---|---|---|---|---|---|
| env-hello2 | environment | `HELLO 2` | HELLO 2 succeeds | [bulk(server), bulk(redis), bulk(version), bulk(6.2.16), bulk(proto), int(2), bulk(id), int(4), bulk(mode), bulk(standalone), bulk(role), bulk(master), bulk(modules), [[bulk(name), bulk(GeminiBloom), bulk(ver), int(1)]]] | PASS | PASS |
| env-module-list | environment | `MODULE LIST` | GeminiBloom module is loaded | [[bulk(name), bulk(GeminiBloom), bulk(ver), int(1)]] | PASS | PASS |
| env-flushall | environment | `FLUSHALL` | OK | simple(OK) | PASS | PASS |
| happy-reserve | happy path | `BF.RESERVE hp_reserve 0.01 100 EXPANSION 2` | OK | simple(OK) | PASS | PASS |
| happy-add-new | happy path | `BF.ADD hp_reserve a` | new insert returns 1 | int(1) | PASS | PASS |
| happy-add-dup | happy path | `BF.ADD hp_reserve a` | duplicate returns 0 | int(0) | PASS | PASS |
| happy-madd | happy path | `BF.MADD hp_reserve b c a` | [1,1,0] | [int(1), int(1), int(0)] | PASS | PASS |
| happy-insert-create | happy path | `BF.INSERT hp_insert CAPACITY 10 ERROR 0.01 EXPANSION 2 ITEMS x y` | [1,1] | [int(1), int(1)] | PASS | PASS |
| happy-exists-present | happy path | `BF.EXISTS hp_insert x` | present returns 1 | int(1) | PASS | PASS |
| happy-exists-absent | happy path | `BF.EXISTS hp_insert z` | absent returns 0 | int(0) | PASS | PASS |
| happy-mexists | happy path | `BF.MEXISTS hp_insert x z` | [1,0] | [int(1), int(0)] | PASS | PASS |
| happy-info-full | happy path | `BF.INFO hp_insert` | full info array length 10 | [simple(Capacity), int(10), simple(Size), int(424), simple(Number of filters), int(1), simple(Number of items inserted), int(2), simple(Expansion rate), int(2)] | PASS | PASS |
| happy-info-items-field | happy path | `BF.INFO hp_insert ITEMS` | scalar int field | int(2) | DESIGN_INTENDED | PASS |
| happy-card | happy path | `BF.CARD hp_insert` | cardinality 2 | int(2) | PASS | PASS |
| info-field-capacity | BF.INFO fields | `BF.INFO hp_insert CAPACITY` | scalar integer field | int(10) | DESIGN_INTENDED | PASS |
| info-field-size | BF.INFO fields | `BF.INFO hp_insert SIZE` | scalar integer field | int(424) | DESIGN_INTENDED | PASS |
| info-field-filters | BF.INFO fields | `BF.INFO hp_insert FILTERS` | scalar integer field | int(1) | DESIGN_INTENDED | PASS |
| info-field-items | BF.INFO fields | `BF.INFO hp_insert ITEMS` | scalar integer field | int(2) | DESIGN_INTENDED | PASS |
| info-field-expansion | BF.INFO fields | `BF.INFO hp_insert EXPANSION` | scalar expansion integer | int(2) | DESIGN_INTENDED | PASS |
| info-field-invalid | BF.INFO fields | `BF.INFO hp_insert NOPE` | unknown field error | error(ERR unknown subcommand for BF.INFO) | PASS | PASS |
| missing-add-autocreate | missing key | `BF.ADD missing_add a` | auto-create and insert | int(1) | PASS | PASS |
| missing-madd-autocreate | missing key | `BF.MADD missing_madd a b` | auto-create and insert [1,1] | [int(1), int(1)] | PASS | PASS |
| missing-insert-nocreate | missing key | `BF.INSERT missing_insert_no NOCREATE ITEMS a` | missing key error | error(ERR key does not exist) | PASS | PASS |
| missing-insert-create | missing key | `BF.INSERT missing_insert_yes ITEMS a` | create and insert [1] | [int(1)] | PASS | PASS |
| missing-exists | missing key | `BF.EXISTS missing_nope a` | 0 | int(0) | PASS | PASS |
| missing-mexists | missing key | `BF.MEXISTS missing_nope a b` | [0,0] | [int(0), int(0)] | PASS | PASS |
| missing-info | missing key | `BF.INFO missing_nope` | ERR key does not exist | error(ERR key does not exist) | PASS | PASS |
| missing-card | missing key | `BF.CARD missing_nope` | 0 | int(0) | PASS | PASS |
| missing-scandump | missing key | `BF.SCANDUMP missing_nope 0` | ERR key does not exist | error(ERR key does not exist) | PASS | PASS |
| missing-loadchunk-data | missing key | `BF.LOADCHUNK missing_nope 2 data` | ERR key does not exist | error(ERR key does not exist) | PASS | PASS |
| dup-reserve | duplicate | `BF.RESERVE dup_key 0.01 100` | OK | simple(OK) | PASS | PASS |
| dup-add-a1 | duplicate | `BF.ADD dup_key a` | 1 | int(1) | PASS | PASS |
| dup-add-a2 | duplicate | `BF.ADD dup_key a` | 0 | int(0) | PASS | PASS |
| dup-madd | duplicate | `BF.MADD dup_key a a b` | [0,0,1] | [int(0), int(0), int(1)] | PASS | PASS |
| dup-insert | duplicate | `BF.INSERT dup_key ITEMS b b c` | [0,0,1] | [int(0), int(0), int(1)] | PASS | PASS |
| dup-card | duplicate | `BF.CARD dup_key` | cardinality 3 | int(3) | PASS | PASS |
| dup-info-items | duplicate | `BF.INFO dup_key ITEMS` | items scalar 3 | int(3) | DESIGN_INTENDED | PASS |
| binary-reserve | binary items | `BF.RESERVE bin_key 0.01 100` | OK | simple(OK) | PASS | PASS |
| binary-add-empty | binary items | `BF.ADD bin_key ` | insert returns 1 | int(1) | PASS | PASS |
| binary-exists-empty | binary items | `BF.EXISTS bin_key ` | exists returns 1 | int(1) | PASS | PASS |
| binary-add-nul | binary items | `BF.ADD bin_key 0x610062` | insert returns 1 | int(1) | PASS | PASS |
| binary-exists-nul | binary items | `BF.EXISTS bin_key 0x610062` | exists returns 1 | int(1) | PASS | PASS |
| binary-add-nonutf8 | binary items | `BF.ADD bin_key 0xfffe0078` | insert returns 1 | int(1) | PASS | PASS |
| binary-exists-nonutf8 | binary items | `BF.EXISTS bin_key 0xfffe0078` | exists returns 1 | int(1) | PASS | PASS |
| binary-add-long10k | binary items | `BF.ADD bin_key <bulk len=10240 sha-ish=4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c>` | insert returns 1 | int(1) | PASS | PASS |
| binary-exists-long10k | binary items | `BF.EXISTS bin_key <bulk len=10240 sha-ish=4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c>` | exists returns 1 | int(1) | PASS | PASS |
| binary-mexists | binary items | `BF.MEXISTS bin_key  0x610062 0x616273656e7400` | [1,1,0] | [int(1), int(1), int(0)] | PASS | PASS |
| boundary-capacity-zero | boundaries | `BF.RESERVE cap_zero 0.01 0` | capacity error | error(ERR expected a positive capacity value) | PASS | PASS |
| boundary-capacity-one | boundaries | `BF.RESERVE cap_one 0.01 1` | OK | simple(OK) | PASS | PASS |
| boundary-capacity-max-safe | boundaries | `BF.RESERVE cap_max 0.9 1073741824 NONSCALING` | max capacity accepted with safe fpRate | simple(OK) | PASS | PASS |
| boundary-capacity-over | boundaries | `BF.RESERVE cap_over 0.01 1073741825` | capacity error | error(ERR expected a positive capacity value) | PASS | PASS |
| boundary-error-zero | boundaries | `BF.RESERVE err_zero 0 10` | false-positive-rate rejected | error(ERR false positive rate must be in (0, 1)) | PASS | PASS |
| boundary-error-negative | boundaries | `BF.RESERVE err_negative -0.1 10` | false-positive-rate rejected | error(ERR false positive rate must be in (0, 1)) | PASS | PASS |
| boundary-error-one | boundaries | `BF.RESERVE err_one 1 10` | false-positive-rate rejected | error(ERR false positive rate must be in (0, 1)) | PASS | PASS |
| boundary-error-gtone | boundaries | `BF.RESERVE err_gtone 1.5 10` | false-positive-rate rejected | error(ERR false positive rate must be in (0, 1)) | PASS | PASS |
| boundary-error-nan | boundaries | `BF.RESERVE err_nan nan 10` | false-positive-rate rejected | error(ERR false positive rate must be in (0, 1)) | PASS | PASS |
| boundary-error-inf | boundaries | `BF.RESERVE err_inf inf 10` | false-positive-rate rejected | error(ERR false positive rate must be in (0, 1)) | PASS | PASS |
| boundary-error-text | boundaries | `BF.RESERVE err_text nope 10` | false-positive-rate rejected | error(ERR false positive rate must be in (0, 1)) | PASS | PASS |
| boundary-error-valid-small | boundaries | `BF.RESERVE err_valid_small 0.0001 10` | OK | simple(OK) | PASS | PASS |
| boundary-expansion-zero | boundaries | `BF.RESERVE exp_zero 0.01 10 EXPANSION 0` | OK non-scaling | simple(OK) | DESIGN_INTENDED | PASS |
| boundary-expansion-zero-info | boundaries | `BF.INFO exp_zero EXPANSION` | nil/null expansion for fixed | bulk(nil) | DESIGN_INTENDED | PASS |
| boundary-expansion-1 | boundaries | `BF.RESERVE exp_1 0.9 10 EXPANSION 1` | OK | simple(OK) | PASS | PASS |
| boundary-expansion-2 | boundaries | `BF.RESERVE exp_2 0.9 10 EXPANSION 2` | OK | simple(OK) | PASS | PASS |
| boundary-expansion-32768 | boundaries | `BF.RESERVE exp_32768 0.9 10 EXPANSION 32768` | OK | simple(OK) | PASS | PASS |
| boundary-expansion-over | boundaries | `BF.RESERVE exp_over 0.01 10 EXPANSION 32769` | expansion error | error(ERR bad expansion) | PASS | PASS |
| boundary-duplicate-expansion | boundaries | `BF.RESERVE dup_exp 0.01 10 EXPANSION 2 EXPANSION 3` | duplicate error | error(ERR duplicate EXPANSION option) | PASS | PASS |
| boundary-duplicate-nonscaling | boundaries | `BF.RESERVE dup_ns 0.01 10 NONSCALING NONSCALING` | duplicate error | error(ERR duplicate NONSCALING option) | PASS | PASS |
| boundary-unknown-option | boundaries | `BF.RESERVE unknown_opt 0.01 10 BOGUS` | unknown option error | error(ERR unrecognized option) | PASS | PASS |
| boundary-ns-exp | boundaries | `BF.RESERVE ns_exp 0.01 10 NONSCALING EXPANSION 2` | mutual exclusion error | error(ERR Nonscaling filters cannot expand) | PASS | PASS |
| boundary-insert-nocreate-capacity | boundaries | `BF.INSERT nokey NOCREATE CAPACITY 10 ITEMS a` | NOCREATE conflict | error(ERR NOCREATE cannot be used with CAPACITY or ERROR) | PASS | PASS |
| boundary-insert-nocreate-error | boundaries | `BF.INSERT nokey NOCREATE ERROR 0.01 ITEMS a` | NOCREATE conflict | error(ERR NOCREATE cannot be used with CAPACITY or ERROR) | PASS | PASS |
| boundary-insert-duplicate-error | boundaries | `BF.INSERT dup_err ERROR 0.01 ERROR 0.02 ITEMS a` | duplicate error | error(ERR duplicate ERROR option) | PASS | PASS |
| boundary-insert-duplicate-capacity | boundaries | `BF.INSERT dup_cap CAPACITY 10 CAPACITY 20 ITEMS a` | duplicate error | error(ERR duplicate CAPACITY option) | PASS | PASS |
| boundary-insert-exp0 | boundaries | `BF.INSERT ins_exp0 EXPANSION 0 ITEMS a` | maps to non-scaling | [int(1)] | DESIGN_INTENDED | PASS |
| boundary-insert-exp0-info | boundaries | `BF.INFO ins_exp0 EXPANSION` | nil/null expansion | bulk(nil) | DESIGN_INTENDED | PASS |
| fixed-reserve | nonscaling full | `BF.RESERVE fixed_key 0.001 2 NONSCALING` | OK | simple(OK) | PASS | PASS |
| fixed-add-a | nonscaling full | `BF.ADD fixed_key a` | 1 | int(1) | PASS | PASS |
| fixed-add-b | nonscaling full | `BF.ADD fixed_key b` | 1 | int(1) | PASS | PASS |
| fixed-add-c-full | nonscaling full | `BF.ADD fixed_key c` | full error | error(ERR non scaling filter is full) | PASS | PASS |
| fixed-add-a-dup-after-full | nonscaling full | `BF.ADD fixed_key a` | duplicate 0 after full | int(0) | PASS | PASS |
| fixed-card | nonscaling full | `BF.CARD fixed_key` | cardinality stable 2 | int(2) | PASS | PASS |
| partial-madd-reserve | partial failure | `BF.RESERVE partial_madd 0.001 2 NONSCALING` | OK | simple(OK) | PASS | PASS |
| partial-madd | partial failure | `BF.MADD partial_madd a b c d` | truncated [1,1,ERR] | [int(1), int(1), error(ERR non scaling filter is full)] | PASS | PASS |
| partial-madd-card | partial failure | `BF.CARD partial_madd` | cardinality 2 | int(2) | PASS | PASS |
| partial-madd-unprocessed | partial failure | `BF.EXISTS partial_madd d` | unprocessed item absent | int(0) | PASS | PASS |
| partial-insert-reserve | partial failure | `BF.RESERVE partial_insert 0.001 2 NONSCALING` | OK | simple(OK) | PASS | PASS |
| partial-insert | partial failure | `BF.INSERT partial_insert NOCREATE ITEMS a b c d` | truncated [1,1,ERR] | [int(1), int(1), error(ERR non scaling filter is full)] | PASS | PASS |
| partial-insert-card | partial failure | `BF.CARD partial_insert` | cardinality 2 | int(2) | PASS | PASS |
| partial-insert-unprocessed | partial failure | `BF.EXISTS partial_insert d` | unprocessed item absent | int(0) | PASS | PASS |
| sd-reserve | scandump/loadchunk | `BF.RESERVE sd_src 0.0001 4 EXPANSION 1` | OK | simple(OK) | PASS | PASS |
| sd-info-filters | scandump/loadchunk | `BF.INFO sd_src FILTERS` | multiple layers | int(8) | PASS | PASS |
| sd-scandump-shape-0 | scandump/loadchunk | `BF.SCANDUMP sd_src 0` | [next_cursor, bulk] | [int(1), bulk(bytes(len=444, hex=1e000000000000000800000005000000010000008000000000000000000400000000000004000000000000002d431ceb...))] | PASS | PASS |
| sd-scandump-shape-1 | scandump/loadchunk | `BF.SCANDUMP sd_src 1` | [next_cursor, bulk] | [int(2), bulk(bytes(len=128, hex=400400000200400000100000023040040010000010000000000000080214000000100400020000000000004402000000...))] | PASS | PASS |
| sd-scandump-shape-2 | scandump/loadchunk | `BF.SCANDUMP sd_src 2` | [next_cursor, bulk] | [int(3), bulk(bytes(len=128, hex=00100800000082000480100004a000010008000040020002800000100000000008000000001000080100000000004800...))] | PASS | PASS |
| sd-scandump-shape-3 | scandump/loadchunk | `BF.SCANDUMP sd_src 3` | [next_cursor, bulk] | [int(4), bulk(bytes(len=128, hex=006000810000000100800000000800000041000800000000080210012000020100000000800000008801100000010000...))] | PASS | PASS |
| sd-scandump-shape-4 | scandump/loadchunk | `BF.SCANDUMP sd_src 4` | [next_cursor, bulk] | [int(5), bulk(bytes(len=128, hex=10180000020008140000000800500000080000400008000000000900008000080510000008021100400800004c000800...))] | PASS | PASS |
| sd-scandump-shape-5 | scandump/loadchunk | `BF.SCANDUMP sd_src 5` | [next_cursor, bulk] | [int(6), bulk(bytes(len=128, hex=00080408002404000000008000000400200036000000000000020400000004a000900080008004000200000002004000...))] | PASS | PASS |
| sd-scandump-shape-6 | scandump/loadchunk | `BF.SCANDUMP sd_src 6` | [next_cursor, bulk] | [int(7), bulk(bytes(len=128, hex=010000002208020800020000001040000018040080000a00000000080410080208000020800804000000000505200000...))] | PASS | PASS |
| sd-scandump-shape-7 | scandump/loadchunk | `BF.SCANDUMP sd_src 7` | [next_cursor, bulk] | [int(8), bulk(bytes(len=128, hex=80008000a00000000000100080002000a8002280a0200408000000008200000088000900880000800000000080400000...))] | PASS | PASS |
| sd-scandump-shape-8 | scandump/loadchunk | `BF.SCANDUMP sd_src 8` | [next_cursor, bulk] | [int(9), bulk(bytes(len=128, hex=0000000000200800000082200882208a8220000000000000000802000000000000008020000000000000000000020000...))] | PASS | PASS |
| sd-scandump-shape-9 | scandump/loadchunk | `BF.SCANDUMP sd_src 9` | [next_cursor, bulk] | [int(0), bulk()] | PASS | PASS |
| sd-layer-index-cursors | scandump/loadchunk | `cursor sequence` | layer-index cursor increments by 1 | [1, 2, 3, 4, 5, 6, 7, 8, 9] | DESIGN_INTENDED | PASS |
| lc-load-header | scandump/loadchunk | `BF.LOADCHUNK sd_dst 1 <bulk len=444 sha-ish=1e000000000000000800000005000000>` | loading shell OK | simple(OK) | PASS | PASS |
| lc-load-chunk-1 | scandump/loadchunk | `BF.LOADCHUNK sd_dst 2 <bulk len=128 sha-ish=40040000020040000010000002304004>` | chunk OK | simple(OK) | PASS | PASS |
| lc-load-chunk-2 | scandump/loadchunk | `BF.LOADCHUNK sd_dst 3 <bulk len=128 sha-ish=00100800000082000480100004a00001>` | chunk OK | simple(OK) | PASS | PASS |
| lc-load-chunk-3 | scandump/loadchunk | `BF.LOADCHUNK sd_dst 4 <bulk len=128 sha-ish=00600081000000010080000000080000>` | chunk OK | simple(OK) | PASS | PASS |
| lc-load-chunk-4 | scandump/loadchunk | `BF.LOADCHUNK sd_dst 5 <bulk len=128 sha-ish=10180000020008140000000800500000>` | chunk OK | simple(OK) | PASS | PASS |
| lc-load-chunk-5 | scandump/loadchunk | `BF.LOADCHUNK sd_dst 6 <bulk len=128 sha-ish=00080408002404000000008000000400>` | chunk OK | simple(OK) | PASS | PASS |
| lc-load-chunk-6 | scandump/loadchunk | `BF.LOADCHUNK sd_dst 7 <bulk len=128 sha-ish=01000000220802080002000000104000>` | chunk OK | simple(OK) | PASS | PASS |
| lc-load-chunk-7 | scandump/loadchunk | `BF.LOADCHUNK sd_dst 8 <bulk len=128 sha-ish=80008000a00000000000100080002000>` | chunk OK | simple(OK) | PASS | PASS |
| lc-load-chunk-8 | scandump/loadchunk | `BF.LOADCHUNK sd_dst 9 <bulk len=128 sha-ish=0000000000200800000082200882208a>` | chunk OK | simple(OK) | PASS | PASS |
| lc-card-match | scandump/loadchunk | `BF.CARD sd_dst` | destination card equals source card | int(30) | PASS | PASS |
| lc-membership-00 | scandump/loadchunk | `BF.EXISTS sd_dst sd_item_0` | no false negative | int(1) | PASS | PASS |
| lc-membership-01 | scandump/loadchunk | `BF.EXISTS sd_dst sd_item_1` | no false negative | int(1) | PASS | PASS |
| lc-membership-02 | scandump/loadchunk | `BF.EXISTS sd_dst sd_item_2` | no false negative | int(1) | PASS | PASS |
| lc-membership-03 | scandump/loadchunk | `BF.EXISTS sd_dst sd_item_3` | no false negative | int(1) | PASS | PASS |
| lc-membership-04 | scandump/loadchunk | `BF.EXISTS sd_dst sd_item_4` | no false negative | int(1) | PASS | PASS |
| lc-membership-05 | scandump/loadchunk | `BF.EXISTS sd_dst sd_item_5` | no false negative | int(1) | PASS | PASS |
| lc-membership-06 | scandump/loadchunk | `BF.EXISTS sd_dst sd_item_6` | no false negative | int(1) | PASS | PASS |
| lc-membership-07 | scandump/loadchunk | `BF.EXISTS sd_dst sd_item_7` | no false negative | int(1) | PASS | PASS |
| lc-membership-08 | scandump/loadchunk | `BF.EXISTS sd_dst sd_item_8` | no false negative | int(1) | PASS | PASS |
| lc-membership-09 | scandump/loadchunk | `BF.EXISTS sd_dst sd_item_9` | no false negative | int(1) | PASS | PASS |
| loading-header | loading state | `BF.LOADCHUNK half_key 1 <bulk len=444 sha-ish=1e000000000000000800000005000000>` | header OK creates loading key | simple(OK) | PASS | PASS |
| loading-reject-bf-add | loading state | `BF.ADD half_key x` | ERR filter is being loaded | error(ERR filter is being loaded) | PASS | PASS |
| loading-reject-bf-madd | loading state | `BF.MADD half_key x y` | ERR filter is being loaded | error(ERR filter is being loaded) | PASS | PASS |
| loading-reject-bf-insert | loading state | `BF.INSERT half_key NOCREATE ITEMS x` | ERR filter is being loaded | error(ERR filter is being loaded) | PASS | PASS |
| loading-reject-bf-exists | loading state | `BF.EXISTS half_key x` | ERR filter is being loaded | error(ERR filter is being loaded) | PASS | PASS |
| loading-reject-bf-mexists | loading state | `BF.MEXISTS half_key x y` | ERR filter is being loaded | error(ERR filter is being loaded) | PASS | PASS |
| loading-reject-bf-info | loading state | `BF.INFO half_key` | ERR filter is being loaded | error(ERR filter is being loaded) | PASS | PASS |
| loading-reject-bf-card | loading state | `BF.CARD half_key` | ERR filter is being loaded | error(ERR filter is being loaded) | PASS | PASS |
| loading-reject-bf-scandump | loading state | `BF.SCANDUMP half_key 0` | ERR filter is being loaded | error(ERR filter is being loaded) | PASS | PASS |
| loading-bad-size | loading state | `BF.LOADCHUNK half_key 2 short` | data length mismatch | error(ERR data length mismatch for layer) | PASS | PASS |
| loading-complete-1 | loading state | `BF.LOADCHUNK half_key 2 <bulk len=128 sha-ish=40040000020040000010000002304004>` | chunk OK | simple(OK) | PASS | PASS |
| loading-complete-2 | loading state | `BF.LOADCHUNK half_key 3 <bulk len=128 sha-ish=00100800000082000480100004a00001>` | chunk OK | simple(OK) | PASS | PASS |
| loading-complete-3 | loading state | `BF.LOADCHUNK half_key 4 <bulk len=128 sha-ish=00600081000000010080000000080000>` | chunk OK | simple(OK) | PASS | PASS |
| loading-complete-4 | loading state | `BF.LOADCHUNK half_key 5 <bulk len=128 sha-ish=10180000020008140000000800500000>` | chunk OK | simple(OK) | PASS | PASS |
| loading-complete-5 | loading state | `BF.LOADCHUNK half_key 6 <bulk len=128 sha-ish=00080408002404000000008000000400>` | chunk OK | simple(OK) | PASS | PASS |
| loading-complete-6 | loading state | `BF.LOADCHUNK half_key 7 <bulk len=128 sha-ish=01000000220802080002000000104000>` | chunk OK | simple(OK) | PASS | PASS |
| loading-complete-7 | loading state | `BF.LOADCHUNK half_key 8 <bulk len=128 sha-ish=80008000a00000000000100080002000>` | chunk OK | simple(OK) | PASS | PASS |
| loading-complete-8 | loading state | `BF.LOADCHUNK half_key 9 <bulk len=128 sha-ish=0000000000200800000082200882208a>` | chunk OK | simple(OK) | PASS | PASS |
| loading-completed-reject-cursor2 | loading state | `BF.LOADCHUNK half_key 2 0x00000000000000000000000000000000` | completed key rejects cursor>1 | error(ERR received bad data) | PASS | PASS |
| existing-header-src | loading state | `BF.RESERVE existing_dst 0.01 10` | OK | simple(OK) | PASS | PASS |
| existing-header-old | loading state | `BF.ADD existing_dst old` | 1 | int(1) | PASS | PASS |
| existing-header-reject | loading state | `BF.LOADCHUNK existing_dst 1 <bulk len=444 sha-ish=1e000000000000000800000005000000>` | existing Bloom rejects header | error(ERR received bad data) | PASS | PASS |
| existing-header-preserve | loading state | `BF.EXISTS existing_dst old` | old data preserved | int(1) | PASS | PASS |
| wt-set | wrong type | `SET wrongtype value` | OK | simple(OK) | PASS | PASS |
| wrongtype-bf-reserve | wrong type | `BF.RESERVE wrongtype 0.01 10` | WRONGTYPE | error(WRONGTYPE Operation against a key holding the wrong kind of value) | PASS | PASS |
| wrongtype-bf-add | wrong type | `BF.ADD wrongtype item` | WRONGTYPE | error(WRONGTYPE Operation against a key holding the wrong kind of value) | PASS | PASS |
| wrongtype-bf-madd | wrong type | `BF.MADD wrongtype item` | WRONGTYPE | error(WRONGTYPE Operation against a key holding the wrong kind of value) | PASS | PASS |
| wrongtype-bf-insert | wrong type | `BF.INSERT wrongtype ITEMS item` | WRONGTYPE | error(WRONGTYPE Operation against a key holding the wrong kind of value) | PASS | PASS |
| wrongtype-bf-exists | wrong type | `BF.EXISTS wrongtype item` | WRONGTYPE | error(WRONGTYPE Operation against a key holding the wrong kind of value) | PASS | PASS |
| wrongtype-bf-mexists | wrong type | `BF.MEXISTS wrongtype item` | WRONGTYPE | error(WRONGTYPE Operation against a key holding the wrong kind of value) | PASS | PASS |
| wrongtype-bf-info | wrong type | `BF.INFO wrongtype` | WRONGTYPE | error(WRONGTYPE Operation against a key holding the wrong kind of value) | PASS | PASS |
| wrongtype-bf-card | wrong type | `BF.CARD wrongtype` | WRONGTYPE | error(WRONGTYPE Operation against a key holding the wrong kind of value) | PASS | PASS |
| wrongtype-bf-scandump | wrong type | `BF.SCANDUMP wrongtype 0` | WRONGTYPE | error(WRONGTYPE Operation against a key holding the wrong kind of value) | PASS | PASS |
| wrongtype-bf-loadchunk | wrong type | `BF.LOADCHUNK wrongtype 1 <bulk len=444 sha-ish=1e000000000000000800000005000000>` | WRONGTYPE | error(WRONGTYPE Operation against a key holding the wrong kind of value) | PASS | PASS |
| wrongtype-preserve | wrong type | `GET wrongtype` | string key preserved | bulk(value) | PASS | PASS |
| metadata-command-info | metadata | `COMMAND INFO BF.RESERVE BF.ADD BF.MADD BF.INSERT BF.EXISTS BF.MEXISTS BF.INFO BF.CARD BF.SCANDUMP BF.LOADCHUNK` | metadata returned for all BF commands | [[bulk(BF.RESERVE), int(-1), [simple(write), simple(denyoom)], int(1), int(1), int(1), []], [bulk(BF.ADD), int(-1), [simple(write), simple(denyoom)], int(1), int(1), int(1), []], [bulk(BF.MADD), int(-1), [simple(write), simple(denyoom)], int(1), int(1), int(1), []], [bulk(BF.INSERT), int(-1), [simple(write), simple(denyoom)], int(1), int(1), int(1), []], [bulk(BF.EXISTS), int(-1), [simple(readonly)], int(1), int(1), int(1), []], [bulk(BF.MEXISTS), int(-1), [simple(readonly)], int(1), int(1), int(1), []], [bulk(BF.INFO), int(-1), [simple(readonly)], int(1), int(1), int(1), []], [bulk(BF.CARD), int(-1), [simple(readonly)], int(1), int(1), int(1), []], [bulk(BF.SCANDUMP), int(-1), [simple(readonly), simple(fast)], int(1), int(1), int(1), []], [bulk(BF.LOADCHUNK), int(-1), [simple(write), simple(denyoom)], int(1), int(1), int(1), []]] | PASS | PASS |
| metadata-getkeys-reserve | metadata | `COMMAND GETKEYS BF.RESERVE cmd_key 0.01 10` | [cmd_key] | [bulk(cmd_key)] | PASS | PASS |
| metadata-getkeys-add | metadata | `COMMAND GETKEYS BF.ADD cmd_key item` | [cmd_key] | [bulk(cmd_key)] | PASS | PASS |
| metadata-getkeys-madd | metadata | `COMMAND GETKEYS BF.MADD cmd_key a b` | [cmd_key] | [bulk(cmd_key)] | PASS | PASS |
| metadata-getkeys-insert | metadata | `COMMAND GETKEYS BF.INSERT cmd_key ITEMS a` | [cmd_key] | [bulk(cmd_key)] | PASS | PASS |
| metadata-getkeys-exists | metadata | `COMMAND GETKEYS BF.EXISTS cmd_key a` | [cmd_key] | [bulk(cmd_key)] | PASS | PASS |
| metadata-getkeys-mexists | metadata | `COMMAND GETKEYS BF.MEXISTS cmd_key a` | [cmd_key] | [bulk(cmd_key)] | PASS | PASS |
| metadata-getkeys-info | metadata | `COMMAND GETKEYS BF.INFO cmd_key` | [cmd_key] | [bulk(cmd_key)] | PASS | PASS |
| metadata-getkeys-card | metadata | `COMMAND GETKEYS BF.CARD cmd_key` | [cmd_key] | [bulk(cmd_key)] | PASS | PASS |
| metadata-getkeys-scandump | metadata | `COMMAND GETKEYS BF.SCANDUMP cmd_key 0` | [cmd_key] | [bulk(cmd_key)] | PASS | PASS |
| metadata-getkeys-loadchunk | metadata | `COMMAND GETKEYS BF.LOADCHUNK cmd_key 1 data` | [cmd_key] | [bulk(cmd_key)] | PASS | PASS |
| metadata-acl-dryrun-default-add | metadata | `ACL DRYRUN default BF.ADD acl_key item` | default user can dry-run BF.ADD when Redis supports ACL DRYRUN; BLOCKED_ENV because Redis 6.2.16 does not expose ACL DRYRUN | error(ERR Unknown subcommand or wrong number of arguments for 'DRYRUN'. Try ACL HELP.) | BLOCKED | BLOCKED |
| metadata-acl-setuser | metadata | `ACL SETUSER stage04_ro on nopass ~* +@read -@write` | OK | simple(OK) | PASS | PASS |
| metadata-acl-ro-read | metadata | `ACL DRYRUN stage04_ro BF.EXISTS acl_key item` | read-only user can dry-run readonly BF.EXISTS when Redis supports ACL DRYRUN; BLOCKED_ENV because Redis 6.2.16 does not expose ACL DRYRUN | error(ERR Unknown subcommand or wrong number of arguments for 'DRYRUN'. Try ACL HELP.) | BLOCKED | BLOCKED |
| metadata-acl-ro-write | metadata | `ACL DRYRUN stage04_ro BF.ADD acl_key item` | read-only user rejects write BF.ADD when Redis supports ACL DRYRUN; BLOCKED_ENV because Redis 6.2.16 does not expose ACL DRYRUN | error(ERR Unknown subcommand or wrong number of arguments for 'DRYRUN'. Try ACL HELP.) | BLOCKED | BLOCKED |
| metadata-acl-deluser | metadata | `ACL DELUSER stage04_ro` | user deleted | int(1) | PASS | PASS |
| resp3-hello3 | RESP3 | `HELLO 3` | HELLO 3 succeeds | {bulk(server): bulk(redis), bulk(version): bulk(6.2.16), bulk(proto): int(3), bulk(id): int(4), bulk(mode): bulk(standalone), bulk(role): bulk(master), bulk(modules): [{bulk(name): bulk(GeminiBloom), bulk(ver): int(1)}]} | PASS | PASS |
| resp3-add | RESP3 | `BF.ADD resp3_key a` | well-formed integer reply | int(1) | DESIGN_INTENDED | PASS |
| resp3-exists | RESP3 | `BF.EXISTS resp3_key a` | well-formed integer reply | int(1) | DESIGN_INTENDED | PASS |
| resp3-mexists | RESP3 | `BF.MEXISTS resp3_key a b` | well-formed array reply | [int(1), int(0)] | DESIGN_INTENDED | PASS |
| resp3-info-full | RESP3 | `BF.INFO resp3_key` | well-formed array reply | [simple(Capacity), int(100), simple(Size), int(440), simple(Number of filters), int(1), simple(Number of items inserted), int(1), simple(Expansion rate), int(2)] | DESIGN_INTENDED | PASS |
| resp3-info-field | RESP3 | `BF.INFO resp3_key ITEMS` | scalar field reply | int(1) | DESIGN_INTENDED | PASS |
| resp3-scandump | RESP3 | `BF.SCANDUMP resp3_key 0` | array [cursor, bulk] | [int(1), bulk(bytes(len=73, hex=01000000000000000100000005000000020000009000000000000000800400000000000001000000000000007b14ae47...))] | DESIGN_INTENDED | PASS |
| cleanup-hello2 | cleanup | `HELLO 2` | HELLO 2 succeeds | [bulk(server), bulk(redis), bulk(version), bulk(6.2.16), bulk(proto), int(2), bulk(id), int(4), bulk(mode), bulk(standalone), bulk(role), bulk(master), bulk(modules), [[bulk(name), bulk(GeminiBloom), bulk(ver), int(1)]]] | PASS | PASS |
| cleanup-flushall | cleanup | `FLUSHALL` | OK | simple(OK) | PASS | PASS |
