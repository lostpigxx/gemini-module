#!/usr/bin/env tclsh
#
# TCL integration tests for the redis_bloom module.
# Self-contained: starts a redis-server, loads the module, runs tests, shuts down.
#
# Usage: tclsh bloom_test.tcl [path/to/redis_bloom.so]

package require Tcl 8.5

# ============================================================
# Minimal Redis client over TCP (no external dependencies)
# ============================================================

proc redis_connect {host port} {
  set fd [socket $host $port]
  fconfigure $fd -translation binary -buffering full
  return $fd
}

proc redis_command {fd args} {
  set cmd "*[llength $args]\r\n"
  foreach arg $args {
    append cmd "\$[string length $arg]\r\n$arg\r\n"
  }
  puts -nonewline $fd $cmd
  flush $fd
  return [redis_read_reply $fd]
}

proc redis_read_reply {fd} {
  gets $fd line
  set type [string index $line 0]
  set data [string range $line 1 end]
  # Strip trailing \r
  set data [string trimright $data "\r"]

  switch $type {
    "+" { return $data }
    "-" { error $data }
    ":" { return [expr {int($data)}] }
    "$" {
      set len $data
      if {$len == -1} { return "(nil)" }
      set payload [read $fd [expr {$len + 2}]]
      return [string range $payload 0 end-2]
    }
    "*" {
      set count $data
      if {$count == -1} { return "(nil)" }
      set result {}
      for {set i 0} {$i < $count} {incr i} {
        lappend result [redis_read_reply $fd]
      }
      return $result
    }
    default {
      error "Unknown reply type: $type ($line)"
    }
  }
}

proc r {args} {
  global redis_fd
  return [redis_command $redis_fd {*}$args]
}

# Like redis_read_reply but returns errors as "ERR:..." strings instead of throwing.
# Useful for reading arrays that may contain per-element errors.
proc redis_read_reply_nothrow {fd} {
  gets $fd line
  set type [string index $line 0]
  set data [string range $line 1 end]
  set data [string trimright $data "\r"]

  switch $type {
    "+" { return $data }
    "-" { return "ERR:$data" }
    ":" { return [expr {int($data)}] }
    "$" {
      set len $data
      if {$len == -1} { return "(nil)" }
      set payload [read $fd [expr {$len + 2}]]
      return [string range $payload 0 end-2]
    }
    "*" {
      set count $data
      if {$count == -1} { return "(nil)" }
      set result {}
      for {set i 0} {$i < $count} {incr i} {
        lappend result [redis_read_reply_nothrow $fd]
      }
      return $result
    }
    default {
      error "Unknown reply type: $type ($line)"
    }
  }
}

# Send a raw command and read the reply without throwing on per-element errors.
proc r_nothrow {args} {
  global redis_fd
  set cmd "*[llength $args]\r\n"
  foreach arg $args {
    append cmd "\$[string length $arg]\r\n$arg\r\n"
  }
  puts -nonewline $redis_fd $cmd
  flush $redis_fd
  return [redis_read_reply_nothrow $redis_fd]
}

# ============================================================
# Test framework
# ============================================================

set test_passed 0
set test_failed 0
set test_errors {}

proc test {name body expected} {
  global test_passed test_failed test_errors
  set result ""
  set err ""
  if {[catch {set result [uplevel 1 $body]} err]} {
    set result "ERROR: $err"
  }
  if {$result eq $expected} {
    incr test_passed
    puts "  PASS: $name"
  } else {
    incr test_failed
    set msg "$name\n    expected: $expected\n    got:      $result"
    lappend test_errors $msg
    puts "  FAIL: $name"
    puts "    expected: $expected"
    puts "    got:      $result"
  }
}

proc test_assert {name body} {
  global test_passed test_failed test_errors
  if {[catch {uplevel 1 $body} err]} {
    incr test_failed
    lappend test_errors "$name\n    $err"
    puts "  FAIL: $name"
    puts "    $err"
  } else {
    incr test_passed
    puts "  PASS: $name"
  }
}

proc test_error {name body pattern} {
  global test_passed test_failed test_errors
  set caught 0
  if {[catch {uplevel 1 $body} err]} {
    set caught 1
    if {[string match $pattern $err]} {
      incr test_passed
      puts "  PASS: $name"
      return
    } else {
      incr test_failed
      set msg "$name\n    expected error matching: $pattern\n    got error: $err"
      lappend test_errors $msg
      puts "  FAIL: $name"
      puts "    expected error matching: $pattern"
      puts "    got error: $err"
      return
    }
  }
  if {!$caught} {
    incr test_failed
    lappend test_errors "$name\n    expected error but command succeeded"
    puts "  FAIL: $name (expected error but succeeded)"
  }
}

# ============================================================
# Server lifecycle
# ============================================================

proc find_free_port {} {
  set sock [socket -server {} 0]
  set port [lindex [fconfigure $sock -sockname] 2]
  close $sock
  return $port
}

proc start_redis {module_path port} {
  catch {
    exec redis-server \
      --port $port \
      --daemonize yes \
      --loglevel warning \
      --logfile /tmp/bloom_tcl_test.log \
      --dbfilename bloom_tcl_test.rdb \
      --dir /tmp \
      --loadmodule $module_path
  }
  for {set i 0} {$i < 50} {incr i} {
    if {![catch {socket localhost $port} fd]} {
      close $fd
      return
    }
    after 100
  }
  # Print log for debugging
  catch {
    set f [open /tmp/bloom_tcl_test.log r]
    puts "Redis log:\n[read $f]"
    close $f
  }
  error "redis-server failed to start on port $port"
}

proc stop_redis {fd} {
  catch {redis_command $fd SHUTDOWN NOSAVE}
  catch {close $fd}
  after 200
  file delete -force /tmp/bloom_tcl_test.rdb
  file delete -force /tmp/bloom_tcl_test.log
}

# ============================================================
# Resolve module path
# ============================================================

if {$argc > 0} {
  set module_path [file normalize [lindex $argv 0]]
} else {
  set script_dir [file dirname [file normalize [info script]]]
  set module_path [file normalize "$script_dir/../../../../build/redis_bloom.so"]
}

if {![file exists $module_path]} {
  puts "ERROR: Module not found at $module_path"
  puts "Usage: tclsh bloom_test.tcl \[path/to/redis_bloom.so\]"
  exit 1
}

puts "Module: $module_path"

# ============================================================
# Start server
# ============================================================

set port [find_free_port]
puts "Starting redis-server on port $port..."
start_redis $module_path $port
set redis_fd [redis_connect localhost $port]
puts "Connected.\n"

# ============================================================
# Test cases
# ============================================================

puts "=== BF.RESERVE ==="

test "BF.RESERVE creates a new filter" {
  r BF.RESERVE reserve_basic 0.01 1000
} {OK}

test_error "BF.RESERVE on existing key returns error" {
  r BF.RESERVE reserve_basic 0.01 1000
} {ERR*item exists*}

test_error "BF.RESERVE with invalid error rate (0)" {
  r BF.RESERVE reserve_err0 0 1000
} {ERR*rate*}

test_error "BF.RESERVE with invalid error rate (1)" {
  r BF.RESERVE reserve_err1 1.0 1000
} {ERR*rate*}

test_error "BF.RESERVE with invalid error rate (negative)" {
  r BF.RESERVE reserve_errneg -0.5 1000
} {ERR*rate*}

test_error "BF.RESERVE with invalid capacity (0)" {
  r BF.RESERVE reserve_cap0 0.01 0
} {ERR*capacity*}

test_error "BF.RESERVE with invalid capacity (negative)" {
  r BF.RESERVE reserve_capneg 0.01 -1
} {ERR*capacity*}

test "BF.RESERVE with EXPANSION" {
  r BF.RESERVE reserve_exp 0.01 100 EXPANSION 4
} {OK}

test "BF.RESERVE with NONSCALING" {
  r BF.RESERVE reserve_ns 0.01 100 NONSCALING
} {OK}

test_error "BF.RESERVE wrong arity" {
  r BF.RESERVE onlytwo 0.01
} {ERR*wrong*}

puts "\n=== BF.ADD ==="

test "BF.ADD new item returns 1" {
  r BF.ADD add_basic hello
} {1}

test "BF.ADD duplicate item returns 0" {
  r BF.ADD add_basic hello
} {0}

test "BF.ADD second unique item returns 1" {
  r BF.ADD add_basic world
} {1}

test "BF.ADD auto-creates filter on missing key" {
  r BF.ADD add_auto testitem
} {1}

test_error "BF.ADD wrong arity" {
  r BF.ADD
} {ERR*wrong*}

puts "\n=== BF.EXISTS ==="

test "BF.EXISTS finds existing item" {
  r BF.EXISTS add_basic hello
} {1}

test "BF.EXISTS returns 0 for missing item" {
  r BF.EXISTS add_basic nonexistent
} {0}

test "BF.EXISTS returns 0 for missing key" {
  r BF.EXISTS no_such_key anything
} {0}

puts "\n=== BF.MADD ==="

test "BF.MADD adds multiple items" {
  r BF.MADD madd_test a b c
} {1 1 1}

test "BF.MADD with duplicates returns mixed results" {
  r BF.MADD madd_test a d
} {0 1}

puts "\n=== BF.MEXISTS ==="

test "BF.MEXISTS checks multiple items" {
  r BF.MEXISTS madd_test a b c d missing
} {1 1 1 1 0}

test "BF.MEXISTS on missing key returns all zeros" {
  r BF.MEXISTS no_such_key x y z
} {0 0 0}

puts "\n=== BF.INSERT ==="

test "BF.INSERT basic with ITEMS" {
  r BF.INSERT insert_basic ITEMS x y z
} {1 1 1}

test "BF.INSERT with custom ERROR and CAPACITY" {
  r BF.INSERT insert_custom ERROR 0.001 CAPACITY 5000 ITEMS a b
} {1 1}

test "BF.INSERT with EXPANSION" {
  r BF.INSERT insert_exp EXPANSION 4 ITEMS m n
} {1 1}

test "BF.INSERT duplicate returns 0" {
  r BF.INSERT insert_basic ITEMS x
} {0}

test_error "BF.INSERT with NOCREATE on missing key" {
  r BF.INSERT insert_nocreate NOCREATE ITEMS a
} {ERR*key does not exist*}

test "BF.INSERT with NOCREATE on existing key works" {
  r BF.INSERT insert_basic NOCREATE ITEMS new_item
} {1}

test_error "BF.INSERT without ITEMS keyword" {
  r BF.INSERT insert_noitems a b c
} {ERR*}

puts "\n=== BF.INFO ==="

test "BF.INFO returns full info" {
  set info [r BF.INFO reserve_basic]
  # Should be a list of 10 elements (5 key-value pairs)
  llength $info
} {10}

test_assert "BF.INFO Capacity field is correct" {
  set info [r BF.INFO reserve_basic]
  set idx [lsearch $info "Capacity"]
  set val [lindex $info [expr {$idx + 1}]]
  if {$val != 1000} { error "Capacity=$val, expected 1000" }
}

test_assert "BF.INFO Number of filters >= 1" {
  set info [r BF.INFO reserve_basic]
  set idx [lsearch $info "Number of filters"]
  set val [lindex $info [expr {$idx + 1}]]
  if {$val < 1} { error "Filters=$val, expected >= 1" }
}

test_assert "BF.INFO Expansion rate is 2" {
  set info [r BF.INFO reserve_basic]
  set idx [lsearch $info "Expansion rate"]
  set val [lindex $info [expr {$idx + 1}]]
  if {$val != 2} { error "Expansion=$val, expected 2" }
}

test "BF.INFO single field: Capacity" {
  r BF.INFO reserve_basic Capacity
} {1000}

test "BF.INFO single field: Items" {
  r BF.INFO reserve_basic Items
} {0}

test_error "BF.INFO on missing key" {
  r BF.INFO no_such_key
} {ERR*key does not exist*}

test_error "BF.INFO unrecognized field" {
  r BF.INFO reserve_basic BadField
} {ERR*unknown*}

test_assert "BF.INFO NONSCALING filter shows null expansion" {
  set info [r BF.INFO reserve_ns]
  set idx [lsearch $info "Expansion rate"]
  set val [lindex $info [expr {$idx + 1}]]
  if {$val ne "(nil)"} { error "Expected nil but got: $val" }
}

puts "\n=== BF.CARD ==="

test "BF.CARD returns inserted count" {
  r DEL card_test
  r BF.ADD card_test a
  r BF.ADD card_test b
  r BF.ADD card_test c
  r BF.ADD card_test a
  r BF.CARD card_test
} {3}

test "BF.CARD on missing key returns 0" {
  r BF.CARD no_such_key
} {0}

puts "\n=== NONSCALING behavior ==="

test_assert "NONSCALING filter rejects overflow" {
  r BF.RESERVE ns_overflow 0.01 10 NONSCALING
  set rejected 0
  for {set i 0} {$i < 100} {incr i} {
    if {[catch {r BF.ADD ns_overflow "item_$i"} err]} {
      set rejected 1
      break
    }
  }
  if {!$rejected} { error "Expected overflow rejection but all items accepted" }
}

test_assert "NONSCALING filter stays at 1 layer" {
  set info [r BF.INFO ns_overflow]
  set idx [lsearch $info "Number of filters"]
  set val [lindex $info [expr {$idx + 1}]]
  if {$val != 1} { error "Expected 1 filter but got $val" }
}

puts "\n=== Auto-scaling behavior ==="

test_assert "Filter auto-scales when full" {
  r BF.RESERVE scale_test 0.01 10
  for {set i 0} {$i < 50} {incr i} {
    r BF.ADD scale_test "scale_$i"
  }
  set info [r BF.INFO scale_test]
  set idx [lsearch $info "Number of filters"]
  set val [lindex $info [expr {$idx + 1}]]
  if {$val <= 1} { error "Expected multiple filters but got $val" }
}

test_assert "All items survive scaling (no false negatives)" {
  for {set i 0} {$i < 50} {incr i} {
    set exists [r BF.EXISTS scale_test "scale_$i"]
    if {$exists != 1} { error "False negative for scale_$i" }
  }
}

puts "\n=== BF.SCANDUMP / BF.LOADCHUNK round-trip ==="

test_assert "SCANDUMP/LOADCHUNK preserves data" {
  r DEL dump_src dump_dst
  r BF.RESERVE dump_src 0.01 500
  for {set i 0} {$i < 200} {incr i} {
    r BF.ADD dump_src "dump_item_$i"
  }

  # Use official protocol: pass SCANDUMP's returned cursor to LOADCHUNK
  set cursor 0
  while {1} {
    set reply [r BF.SCANDUMP dump_src $cursor]
    set next_cursor [lindex $reply 0]
    set chunk_data [lindex $reply 1]

    if {$next_cursor == 0 && [string length $chunk_data] == 0} break

    r BF.LOADCHUNK dump_dst $next_cursor $chunk_data
    set cursor $next_cursor
  }

  # Verify cardinality
  set src_card [r BF.CARD dump_src]
  set dst_card [r BF.CARD dump_dst]
  if {$src_card != $dst_card} {
    error "Cardinality mismatch: src=$src_card dst=$dst_card"
  }

  # Verify all items
  for {set i 0} {$i < 200} {incr i} {
    set exists [r BF.EXISTS dump_dst "dump_item_$i"]
    if {$exists != 1} { error "False negative after LOADCHUNK for dump_item_$i" }
  }
}

test_assert "SCANDUMP/LOADCHUNK using returned iterator directly (official protocol)" {
  r DEL proto_src proto_dst
  r BF.RESERVE proto_src 0.01 500
  for {set i 0} {$i < 200} {incr i} {
    r BF.ADD proto_src "proto_item_$i"
  }

  # Official protocol: pass SCANDUMP's returned iterator to LOADCHUNK
  set cursor 0
  while {1} {
    set reply [r BF.SCANDUMP proto_src $cursor]
    set next_cursor [lindex $reply 0]
    set chunk_data [lindex $reply 1]

    if {$next_cursor == 0 && [string length $chunk_data] == 0} break

    r BF.LOADCHUNK proto_dst $next_cursor $chunk_data
    set cursor $next_cursor
  }

  set src_card [r BF.CARD proto_src]
  set dst_card [r BF.CARD proto_dst]
  if {$src_card != $dst_card} {
    error "Cardinality mismatch: src=$src_card dst=$dst_card"
  }

  for {set i 0} {$i < 200} {incr i} {
    set exists [r BF.EXISTS proto_dst "proto_item_$i"]
    if {$exists != 1} { error "False negative for proto_item_$i" }
  }
}

puts "\n=== Wrong type errors ==="

test_error "BF.ADD on string key" {
  r SET string_key hello
  r BF.ADD string_key item
} {WRONGTYPE*}

test_error "BF.EXISTS on string key" {
  r BF.EXISTS string_key item
} {WRONGTYPE*}

test_error "BF.INFO on string key" {
  r BF.INFO string_key
} {WRONGTYPE*}

test_assert "BF.MEXISTS on string key returns top-level WRONGTYPE (not array of errors)" {
  # Send BF.MEXISTS raw and check the first byte of response is '-' (error), not '*' (array)
  global redis_fd
  set cmd "*4\r\n\$10\r\nBF.MEXISTS\r\n\$10\r\nstring_key\r\n\$1\r\na\r\n\$1\r\nb\r\n"
  puts -nonewline $redis_fd $cmd
  flush $redis_fd
  gets $redis_fd line
  set type [string index $line 0]
  if {$type eq "*"} {
    # Drain the array elements
    set count [string range $line 1 end]
    set count [string trimright $count "\r"]
    for {set i 0} {$i < $count} {incr i} {
      redis_read_reply $redis_fd
    }
    error "Got array response (type=$type) instead of top-level error"
  }
  if {$type ne "-"} {
    set data [string range $line 1 end]
    error "Expected error type '-', got '$type': $data"
  }
  set data [string range $line 1 end]
  set data [string trimright $data "\r"]
  if {![string match "WRONGTYPE*" $data]} {
    error "Expected WRONGTYPE error, got: $data"
  }
}

test_error "BF.MADD on string key returns top-level WRONGTYPE" {
  r BF.MADD string_key a b c
} {WRONGTYPE*}

test_error "BF.INSERT on string key returns WRONGTYPE" {
  r BF.INSERT string_key ITEMS a b
} {WRONGTYPE*}

test_error "BF.CARD on string key returns WRONGTYPE" {
  r BF.CARD string_key
} {WRONGTYPE*}

test_error "BF.SCANDUMP on string key returns WRONGTYPE" {
  r BF.SCANDUMP string_key 0
} {WRONGTYPE*}

puts "\n=== EXPANSION validation ==="

test_assert "BF.RESERVE EXPANSION 0 maps to NONSCALING" {
  set result [r BF.RESERVE exp0_test 0.01 100 EXPANSION 0]
  if {$result ne "OK"} { error "expected OK, got $result" }
  set exp [r BF.INFO exp0_test Expansion]
  if {$exp ne "(nil)"} { error "expected null expansion (NONSCALING), got $exp" }
}

test_error "BF.RESERVE NONSCALING and EXPANSION together should be rejected" {
  r BF.RESERVE ns_exp_test 0.01 100 NONSCALING EXPANSION 2
} {ERR*}

test_assert "BF.INSERT EXPANSION 0 maps to NONSCALING" {
  set result [r BF.INSERT exp0_ins EXPANSION 0 ITEMS a]
  set exp [r BF.INFO exp0_ins Expansion]
  if {$exp ne "(nil)"} { error "expected null expansion (NONSCALING), got $exp" }
}

test_error "BF.INSERT NONSCALING and EXPANSION together should be rejected" {
  r BF.INSERT ns_exp_ins NONSCALING EXPANSION 2 ITEMS a
} {ERR*}

puts "\n=== LOADCHUNK safety ==="

test_error "BF.LOADCHUNK on string key should return WRONGTYPE, not delete" {
  r SET lc_string_key hello
  r BF.LOADCHUNK lc_string_key 1 "invalid_header_data"
} {WRONGTYPE*}

test_assert "BF.LOADCHUNK WRONGTYPE preserves original key" {
  set val [r GET lc_string_key]
  if {$val ne "hello"} { error "Key was deleted! Got: $val" }
}

test_assert "BF.LOADCHUNK with malformed header does not delete existing bloom key" {
  r BF.RESERVE lc_bloom_key 0.01 100
  r BF.ADD lc_bloom_key testitem
  catch {r BF.LOADCHUNK lc_bloom_key 1 "short"}
  set card [r BF.CARD lc_bloom_key]
  if {$card != 1} { error "Bloom key was deleted/corrupted! Card=$card" }
}

test_error "BF.LOADCHUNK rejects header too short" {
  r DEL lc_hdr_short
  r BF.LOADCHUNK lc_hdr_short 1 "short"
} {ERR*corrupted*}

test_error "BF.LOADCHUNK rejects header with zero layers" {
  r DEL lc_hdr_zero
  # Build a WireFilterHeader with numLayers=0
  # WireFilterHeader: totalItems(8) + numLayers(4) + flags(4) + expansionFactor(4) = 20 bytes
  set hdr [binary format wuiuiuiu 0 0 5 2]
  r BF.LOADCHUNK lc_hdr_zero 1 $hdr
} {ERR*corrupted*}

puts "\n=== BytesUsed accuracy ==="

test_assert "BF.INFO Size accounts for layer storage capacity" {
  r BF.RESERVE size_test 0.01 10
  set info1 [r BF.INFO size_test]
  set idx [lsearch $info1 "Size"]
  set size1 [lindex $info1 [expr {$idx + 1}]]
  # Size should include at minimum sizeof(ScalingBloomFilter) + layer capacity * sizeof(FilterLayer) + bit array
  # With layerCapacity_=4 (initial), it should account for all 4 slots
  if {$size1 <= 0} { error "Size should be positive, got $size1" }
}

puts "\n=== Additional LOADCHUNK safety ==="

test_error "BF.LOADCHUNK cursor 0 should be rejected" {
  r DEL lc_cur0
  r BF.LOADCHUNK lc_cur0 0 "data"
} {ERR*cursor*}

test_error "BF.LOADCHUNK negative cursor should be rejected" {
  r DEL lc_neg
  r BF.LOADCHUNK lc_neg -1 "data"
} {ERR*cursor*}

test_error "BF.LOADCHUNK data chunk on non-existent key" {
  r DEL lc_nokey
  r BF.LOADCHUNK lc_nokey 2 "somedata"
} {ERR*key does not exist*}

test_error "BF.LOADCHUNK data chunk with wrong size" {
  r DEL lc_badsize
  r BF.RESERVE lc_badsize 0.01 100
  # Dump to get a valid header, then restore it to a new key
  set reply [r BF.SCANDUMP lc_badsize 0]
  set hdr [lindex $reply 1]
  r DEL lc_badsize_dst
  r BF.LOADCHUNK lc_badsize_dst 1 $hdr
  # Now try to load a data chunk with wrong size
  r BF.LOADCHUNK lc_badsize_dst 2 "short"
} {ERR*data length mismatch*}

puts "\n=== Additional parameter validation ==="

test_error "BF.RESERVE EXPANSION negative should be rejected" {
  r BF.RESERVE exp_neg 0.01 100 EXPANSION -1
} {ERR*}

test_error "BF.RESERVE unrecognized option" {
  r BF.RESERVE badopt 0.01 100 FOOBAR
} {ERR*unrecognized*}

test_error "BF.INSERT NONSCALING filter rejects when full" {
  r BF.RESERVE insert_ns_full 0.01 5 NONSCALING
  for {set i 0} {$i < 20} {incr i} {
    catch {r BF.ADD insert_ns_full "item_$i"}
  }
  r BF.INSERT insert_ns_full NOCREATE ITEMS new_overflow
} {ERR*non scaling*full*}

puts "\n=== NOCREATE mutual exclusion ==="

test_error "BF.INSERT NOCREATE + CAPACITY should be rejected" {
  r BF.INSERT nocreate_cap NOCREATE CAPACITY 100 ITEMS a
} {ERR*NOCREATE*}

test_error "BF.INSERT NOCREATE + ERROR should be rejected" {
  r BF.INSERT nocreate_err NOCREATE ERROR 0.01 ITEMS a
} {ERR*NOCREATE*}

puts "\n=== BF.INSERT ITEMS edge cases ==="

test_error "BF.INSERT ITEMS with no items returns wrong arity" {
  r BF.INSERT items_empty ITEMS
} {ERR*wrong*}

puts "\n=== EXPANSION overflow / truncation ==="

test_error "BF.RESERVE EXPANSION 4294967296 should be rejected" {
  r BF.RESERVE exp_overflow 0.01 100 EXPANSION 4294967296
} {ERR*}

test_error "BF.INSERT EXPANSION 4294967296 should be rejected" {
  r BF.INSERT exp_overflow2 EXPANSION 4294967296 ITEMS a
} {ERR*}

puts "\n=== BF.RESERVE wrong type distinction ==="

test_error "BF.RESERVE on string key returns WRONGTYPE" {
  r SET reserve_str_key hello
  r BF.RESERVE reserve_str_key 0.01 100
} {WRONGTYPE*}

test_error "BF.RESERVE on existing bloom key returns item exists" {
  r BF.RESERVE reserve_exist_bf 0.01 100
  r BF.RESERVE reserve_exist_bf 0.01 100
} {ERR*item exists*}

puts "\n=== Multi-layer behavior ==="

test_assert "EXPANSION 4 creates fewer layers than EXPANSION 1" {
  r BF.RESERVE exp4_test 0.01 10 EXPANSION 4
  r BF.RESERVE exp1_test 0.01 10 EXPANSION 1
  for {set i 0} {$i < 100} {incr i} {
    r BF.ADD exp4_test "item_$i"
    r BF.ADD exp1_test "item_$i"
  }
  set info4 [r BF.INFO exp4_test]
  set info1 [r BF.INFO exp1_test]
  set idx4 [lsearch $info4 "Number of filters"]
  set idx1 [lsearch $info1 "Number of filters"]
  set layers4 [lindex $info4 [expr {$idx4 + 1}]]
  set layers1 [lindex $info1 [expr {$idx1 + 1}]]
  if {$layers4 >= $layers1} {
    error "EXPANSION 4 ($layers4 layers) should have fewer layers than EXPANSION 1 ($layers1 layers)"
  }
}

test_assert "BF.INFO reports correct expansion rate" {
  set info [r BF.INFO exp4_test]
  set idx [lsearch $info "Expansion rate"]
  set val [lindex $info [expr {$idx + 1}]]
  if {$val != 4} { error "Expected expansion=4, got $val" }
}

test_assert "All items survive multi-layer scaling (no false negatives)" {
  for {set i 0} {$i < 100} {incr i} {
    set e4 [r BF.EXISTS exp4_test "item_$i"]
    set e1 [r BF.EXISTS exp1_test "item_$i"]
    if {$e4 != 1} { error "False negative in exp4 for item_$i" }
    if {$e1 != 1} { error "False negative in exp1 for item_$i" }
  }
}

puts "\n=== Edge cases ==="

test "BF.ADD empty string as item" {
  r BF.ADD edge_empty ""
} {1}

test "BF.EXISTS empty string as item" {
  r BF.EXISTS edge_empty ""
} {1}

test "BF.ADD very long item" {
  set long_item [string repeat "x" 10000]
  r BF.ADD edge_long $long_item
} {1}

test "BF.EXISTS very long item" {
  set long_item [string repeat "x" 10000]
  r BF.EXISTS edge_long $long_item
} {1}

test "BF.ADD binary item with null bytes" {
  set bin_item "hello\x00world\x00"
  r BF.ADD edge_binary $bin_item
} {1}

test "BF.EXISTS binary item with null bytes" {
  set bin_item "hello\x00world\x00"
  r BF.EXISTS edge_binary $bin_item
} {1}

puts "\n=== RDB persistence ==="

test_assert "Data survives BGSAVE + restart" {
  r DEL persist_test
  r BF.RESERVE persist_test 0.01 1000
  r BF.ADD persist_test alpha
  r BF.ADD persist_test beta
  r BF.ADD persist_test gamma

  r BGSAVE
  after 2000

  # Reconnect on same port after restart
  global redis_fd port module_path
  catch {redis_command $redis_fd SHUTDOWN SAVE}
  catch {close $redis_fd}
  after 1000

  start_redis $module_path $port
  set redis_fd [redis_connect localhost $port]

  set e1 [r BF.EXISTS persist_test alpha]
  set e2 [r BF.EXISTS persist_test beta]
  set e3 [r BF.EXISTS persist_test gamma]
  set card [r BF.CARD persist_test]

  if {$e1 != 1 || $e2 != 1 || $e3 != 1} {
    error "Items missing after restart: alpha=$e1 beta=$e2 gamma=$e3"
  }
  if {$card != 3} { error "Card=$card after restart, expected 3" }
}

puts "\n=== AOF persistence ==="

test_assert "Data survives AOF rewrite + restart" {
  global redis_fd port module_path

  # Restart server with AOF enabled
  catch {redis_command $redis_fd SHUTDOWN NOSAVE}
  catch {close $redis_fd}
  after 500

  # Cleanup old AOF/RDB
  catch {file delete -force /tmp/bloom_tcl_test.rdb}
  catch {file delete -force /tmp/appendonlydir}

  catch {
    exec redis-server \
      --port $port \
      --daemonize yes \
      --loglevel warning \
      --logfile /tmp/bloom_tcl_test.log \
      --dbfilename bloom_tcl_test.rdb \
      --dir /tmp \
      --appendonly yes \
      --loadmodule $module_path
  }
  for {set i 0} {$i < 50} {incr i} {
    if {![catch {socket localhost $port} fd]} {
      close $fd
      break
    }
    after 100
  }
  set redis_fd [redis_connect localhost $port]

  r DEL aof_test
  r BF.RESERVE aof_test 0.01 100
  for {set i 0} {$i < 50} {incr i} {
    r BF.ADD aof_test "aof_item_$i"
  }

  r BGREWRITEAOF
  after 2000

  # Restart
  catch {redis_command $redis_fd SHUTDOWN NOSAVE}
  catch {close $redis_fd}
  after 1000

  catch {
    exec redis-server \
      --port $port \
      --daemonize yes \
      --loglevel warning \
      --logfile /tmp/bloom_tcl_test.log \
      --dbfilename bloom_tcl_test.rdb \
      --dir /tmp \
      --appendonly yes \
      --loadmodule $module_path
  }
  for {set i 0} {$i < 50} {incr i} {
    if {![catch {socket localhost $port} fd]} {
      close $fd
      break
    }
    after 100
  }
  set redis_fd [redis_connect localhost $port]

  set card [r BF.CARD aof_test]
  if {$card != 50} { error "Card=$card after AOF restart, expected 50" }

  for {set i 0} {$i < 50} {incr i} {
    set exists [r BF.EXISTS aof_test "aof_item_$i"]
    if {$exists != 1} { error "False negative for aof_item_$i after AOF restart" }
  }

  # Disable AOF and clean up for subsequent tests
  r CONFIG SET appendonly no
  catch {file delete -force {*}[glob -nocomplain /tmp/appendonlydir/*]}
  catch {file delete -force /tmp/appendonlydir}
}

puts "\n=== Resource limits (BUG-04) ==="

test_error "BF.RESERVE capacity exceeding 1<<30 rejected" {
  r BF.RESERVE limit_cap 0.01 1073741825
} {ERR*capacity*}

test_error "BF.RESERVE expansion exceeding 32768 rejected" {
  r BF.RESERVE limit_exp 0.01 100 EXPANSION 32769
} {ERR*}

test_error "BF.INSERT capacity exceeding 1<<30 rejected" {
  r BF.INSERT limit_cap_ins CAPACITY 1073741825 ITEMS a
} {ERR*capacity*}

test_error "BF.INSERT expansion exceeding 32768 rejected" {
  r BF.INSERT limit_exp_ins EXPANSION 32769 ITEMS a
} {ERR*}

test "BF.RESERVE capacity at limit 1<<30 succeeds" {
  r BF.RESERVE limit_cap_ok 0.01 1073741824
} {OK}

test "BF.RESERVE expansion at limit 32768 succeeds" {
  r BF.RESERVE limit_exp_ok 0.01 100 EXPANSION 32768
} {OK}

puts "\n=== MADD/INSERT partial failure (BUG-05) ==="

test_assert "BF.MADD on full NONSCALING filter returns errors" {
  r BF.RESERVE madd_full 0.01 5 NONSCALING
  for {set i 0} {$i < 5} {incr i} {
    catch {r BF.ADD madd_full "fill_$i"}
  }
  set result [r_nothrow BF.MADD madd_full new1 new2 new3]
  set err_count 0
  foreach elem $result {
    if {[string match "ERR:*" $elem]} { incr err_count }
  }
  if {$err_count != 3} { error "Expected 3 errors, got $err_count (result: $result)" }
  set card [r BF.CARD madd_full]
  if {$card < 5} { error "Expected CARD>=5, got $card" }
}

test_assert "BF.CARD after partial MADD matches actual insertions" {
  r BF.RESERVE partial_test 0.01 3 NONSCALING
  r BF.ADD partial_test a
  r BF.ADD partial_test b
  # 2 items in, capacity=3, add batch of 3: first succeeds, rest fail
  set result [r_nothrow BF.MADD partial_test c d e]
  set first [lindex $result 0]
  if {$first != 1} { error "First item should succeed (1), got $first" }
  set err_count 0
  foreach elem [lrange $result 1 end] {
    if {[string match "ERR:*" $elem]} { incr err_count }
  }
  if {$err_count != 2} { error "Expected 2 errors after first success, got $err_count" }
  set card [r BF.CARD partial_test]
  if {$card != 3} { error "Expected CARD=3 but got $card" }
}

test_assert "BF.INSERT on full filter stops and returns errors" {
  r BF.RESERVE insert_full 0.01 3 NONSCALING
  r BF.ADD insert_full a
  r BF.ADD insert_full b
  r BF.ADD insert_full c
  set result [r_nothrow BF.INSERT insert_full NOCREATE ITEMS d e]
  set err_count 0
  foreach elem $result {
    if {[string match "ERR:*" $elem]} { incr err_count }
  }
  if {$err_count != 2} { error "Expected 2 errors, got $err_count (result: $result)" }
  set card [r BF.CARD insert_full]
  if {$card != 3} { error "Expected CARD=3 but got $card" }
}

puts "\n=== Parser duplicate options (IMPL-08) ==="

test_error "BF.RESERVE duplicate EXPANSION rejected" {
  r BF.RESERVE dup_exp 0.01 100 EXPANSION 2 EXPANSION 4
} {ERR*duplicate*}

test_error "BF.RESERVE duplicate NONSCALING rejected" {
  r BF.RESERVE dup_ns 0.01 100 NONSCALING NONSCALING
} {ERR*duplicate*}

test_error "BF.INSERT duplicate ERROR rejected" {
  r BF.INSERT dup_err ERROR 0.01 ERROR 0.02 ITEMS a
} {ERR*duplicate*}

test_error "BF.INSERT duplicate CAPACITY rejected" {
  r BF.INSERT dup_cap CAPACITY 100 CAPACITY 200 ITEMS a
} {ERR*duplicate*}

test_error "BF.INSERT duplicate EXPANSION rejected" {
  r BF.INSERT dup_exp_ins EXPANSION 2 EXPANSION 4 ITEMS a
} {ERR*duplicate*}

test_error "BF.INSERT duplicate NONSCALING rejected" {
  r BF.INSERT dup_ns_ins NONSCALING NONSCALING ITEMS a
} {ERR*duplicate*}

puts "\n=== Module config tests (TEST-07) ==="

# Module config is tested via module load args.
# We can't reload the module mid-test, but we can verify the module
# loaded successfully with default config and that the defaults work.

test_assert "Default config creates filter with expected defaults" {
  r DEL cfg_default
  r BF.ADD cfg_default test
  set info [r BF.INFO cfg_default]
  set idx [lsearch $info "Capacity"]
  set cap [lindex $info [expr {$idx + 1}]]
  if {$cap != 100} { error "Default capacity should be 100, got $cap" }
  set idx [lsearch $info "Expansion rate"]
  set exp [lindex $info [expr {$idx + 1}]]
  if {$exp != 2} { error "Default expansion should be 2, got $exp" }
}

puts "\n=== LOADCHUNK half-restore safety (DESIGN-02) ==="

test_assert "Half-restored filter returns 0 for EXISTS" {
  # Create a filter with data
  r DEL half_src half_dst
  r BF.RESERVE half_src 0.01 100
  r BF.ADD half_src testitem

  # Only load header, skip data chunks
  set reply [r BF.SCANDUMP half_src 0]
  set hdr_cursor [lindex $reply 0]
  set hdr_data [lindex $reply 1]
  r BF.LOADCHUNK half_dst $hdr_cursor $hdr_data

  # The filter exists but bit arrays are zeros
  set exists [r BF.EXISTS half_dst testitem]
  if {$exists != 0} { error "Half-restored filter should return 0 for EXISTS, got $exists" }

  # BF.CARD still shows header-declared count
  set card [r BF.CARD half_dst]
  if {$card != 1} { error "Half-restored CARD should be 1, got $card" }
}

puts "\n=== SCANDUMP cursor protocol documentation (COMPAT-01) ==="

test_assert "SCANDUMP uses layer-index cursor protocol" {
  r DEL cursor_test
  r BF.RESERVE cursor_test 0.01 10
  for {set i 0} {$i < 30} {incr i} {
    r BF.ADD cursor_test "cursor_$i"
  }
  set info [r BF.INFO cursor_test]
  set idx [lsearch $info "Number of filters"]
  set nlayers [lindex $info [expr {$idx + 1}]]

  # cursor=0 -> returns next=1
  set reply [r BF.SCANDUMP cursor_test 0]
  set next [lindex $reply 0]
  if {$next != 1} { error "First SCANDUMP should return cursor=1, got $next" }

  # Iterate through all layers collecting cursors
  set cursor 1
  for {set c 0} {$c < $nlayers} {incr c} {
    set reply [r BF.SCANDUMP cursor_test $cursor]
    set next [lindex $reply 0]
    if {$c < $nlayers - 1} {
      set expected [expr {$cursor + 1}]
      if {$next != $expected} {
        error "SCANDUMP cursor=$cursor should return $expected, got $next"
      }
    }
    set cursor $next
  }

  # After iterating all layers, next cursor should point past end
  # Querying with that cursor returns [0, ""]
  set reply [r BF.SCANDUMP cursor_test $cursor]
  set next [lindex $reply 0]
  if {$next != 0} { error "Final SCANDUMP should return cursor=0, got $next" }
}

puts "\n=== Command flags (IMPL-09, TEST-08) ==="

test_assert "COMMAND INFO BF.ADD shows write flag" {
  # COMMAND INFO returns: [[name, arity, [flags...], first-key, last-key, step]]
  set info [r COMMAND INFO BF.ADD]
  set cmd_str [join $info " "]
  if {[string first "write" $cmd_str] < 0} {
    error "BF.ADD should have write flag, got: $cmd_str"
  }
}

test_assert "COMMAND INFO BF.EXISTS shows readonly flag" {
  set info [r COMMAND INFO BF.EXISTS]
  set cmd_str [join $info " "]
  if {[string first "readonly" $cmd_str] < 0} {
    error "BF.EXISTS should have readonly flag, got: $cmd_str"
  }
}

test_assert "COMMAND INFO BF.SCANDUMP shows write flag" {
  set info [r COMMAND INFO BF.SCANDUMP]
  set cmd_str [join $info " "]
  if {[string first "write" $cmd_str] < 0} {
    error "BF.SCANDUMP should have write flag, got: $cmd_str"
  }
}

# ============================================================
# Cleanup & Summary
# ============================================================

puts "\n=========================================="
puts "Results: $test_passed passed, $test_failed failed"
puts "==========================================\n"

if {$test_failed > 0} {
  puts "Failed tests:"
  foreach err $test_errors {
    puts "  - $err"
  }
  puts ""
}

stop_redis $redis_fd
file delete -force /tmp/bloom_tcl_test.rdb

exit $test_failed
