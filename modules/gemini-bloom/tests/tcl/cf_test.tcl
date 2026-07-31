#!/usr/bin/env tclsh
#
# TCL integration tests for the redis_bloom module's Cuckoo Filter (CF.*)
# commands. Self-contained: starts a redis-server, loads the module, runs
# tests, shuts down.
#
# This file is intentionally independent from bloom_test.tcl (separate RESP
# client + test framework boilerplate) rather than appended to it: CF has 12
# commands, its own config namespace and its own error scenarios, and mixing
# both into one already-large file would hurt maintainability of both.
#
# Usage: tclsh cf_test.tcl [path/to/redis_bloom.so]

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

proc redis_read_raw_reply {fd} {
  gets $fd line
  set type [string index $line 0]
  set data [string range $line 1 end]
  set data [string trimright $data "\r"]

  switch -- $type {
    "+" { return [list $type $data] }
    "-" { return [list $type $data] }
    ":" { return [list $type $data] }
    "," { return [list $type $data] }
    "#" { return [list $type $data] }
    "_" { return [list $type ""] }
    "$" {
      set len $data
      if {$len == -1} { return [list $type "(nil)"] }
      set payload [read $fd [expr {$len + 2}]]
      return [list $type [string range $payload 0 end-2]]
    }
    "*" - "%" {
      set count $data
      if {$count == -1} { return [list $type "(nil)"] }
      set result {}
      set elems $count
      if {$type eq "%"} { set elems [expr {$count * 2}] }
      for {set i 0} {$i < $elems} {incr i} {
        lappend result [redis_read_raw_reply $fd]
      }
      return [list $type $result]
    }
    default {
      error "Unknown raw reply type: $type ($line)"
    }
  }
}

proc raw_command_reply {fd args} {
  set cmd "*[llength $args]\r\n"
  foreach arg $args {
    append cmd "\$[string length $arg]\r\n$arg\r\n"
  }
  puts -nonewline $fd $cmd
  flush $fd
  return [redis_read_raw_reply $fd]
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
      --logfile /tmp/cf_tcl_test.log \
      --dbfilename cf_tcl_test.rdb \
      --dir /tmp \
      --enable-debug-command yes \
      --repl-diskless-sync-delay 0 \
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
    set f [open /tmp/cf_tcl_test.log r]
    puts "Redis log:\n[read $f]"
    close $f
  }
  error "redis-server failed to start on port $port"
}

proc wait_redis_ready {host port} {
  for {set i 0} {$i < 100} {incr i} {
    if {![catch {
      set fd [redis_connect $host $port]
      set pong [redis_command $fd PING]
      close $fd
      set pong
    } result] && $result eq "PONG"} {
      return
    }
    catch {close $fd}
    after 100
  }
  error "redis-server did not become ready on port $port"
}

proc module_load_should_fail {name args} {
  global module_path
  set cfg_port [find_free_port]
  set cfg_dir "/tmp/cf_bad_cfg_$cfg_port"
  file delete -force $cfg_dir
  file mkdir $cfg_dir

  set cmd [list redis-server \
    --port $cfg_port \
    --daemonize yes \
    --loglevel warning \
    --logfile $cfg_dir/redis.log \
    --dbfilename dump.rdb \
    --dir $cfg_dir \
    --loadmodule $module_path]
  foreach arg $args { lappend cmd $arg }

  catch {exec {*}$cmd} err
  after 300
  if {![catch {set fd [redis_connect localhost $cfg_port]}]} {
    set module_accepted 0
    if {![catch {redis_command $fd CF.ADD bad_cfg_probe item}]} {
      set module_accepted 1
    }
    catch {redis_command $fd SHUTDOWN NOSAVE}
    catch {close $fd}
    if {$module_accepted} {
      file delete -force $cfg_dir
      error "$name: module load unexpectedly succeeded"
    }
  }
  file delete -force $cfg_dir
}

proc stop_redis {fd} {
  catch {redis_command $fd SHUTDOWN NOSAVE}
  catch {close $fd}
  after 200
  file delete -force /tmp/cf_tcl_test.rdb
  file delete -force /tmp/cf_tcl_test.log
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
  puts "Usage: tclsh cf_test.tcl \[path/to/redis_bloom.so\]"
  exit 1
}

puts "Module: $module_path"

# ============================================================
# Start server
# ============================================================

set port [find_free_port]
puts "Starting redis-server on port $port..."
start_redis $module_path $port
wait_redis_ready localhost $port
set redis_fd [redis_connect localhost $port]
puts "Connected.\n"

# ============================================================
# Test cases
# ============================================================

puts "=== CF.RESERVE ==="

test "CF.RESERVE creates a new filter" {
  r CF.RESERVE reserve_basic 1000
} {OK}

test_error "CF.RESERVE on existing key returns error" {
  r CF.RESERVE reserve_basic 1000
} {ERR*item exists*}

test_error "CF.RESERVE with invalid capacity (0)" {
  r CF.RESERVE reserve_cap0 0
} {ERR*capacity*}

test_error "CF.RESERVE with invalid capacity (negative)" {
  r CF.RESERVE reserve_capneg -1
} {ERR*capacity*}

test "CF.RESERVE with BUCKETSIZE" {
  r CF.RESERVE reserve_bs 1000 BUCKETSIZE 4
} {OK}

test "CF.RESERVE with MAXITERATIONS" {
  r CF.RESERVE reserve_mi 1000 MAXITERATIONS 50
} {OK}

test "CF.RESERVE with EXPANSION" {
  r CF.RESERVE reserve_exp 100 EXPANSION 4
} {OK}

test "CF.RESERVE with EXPANSION 0 (fixed size)" {
  r CF.RESERVE reserve_fixed 100 EXPANSION 0
} {OK}

test_error "CF.RESERVE with invalid BUCKETSIZE (0)" {
  r CF.RESERVE reserve_bs0 1000 BUCKETSIZE 0
} {ERR*bucket size*}

test_error "CF.RESERVE with invalid BUCKETSIZE (too large)" {
  r CF.RESERVE reserve_bs_big 1000 BUCKETSIZE 256
} {ERR*bucket size*}

test_error "CF.RESERVE with invalid MAXITERATIONS (0)" {
  r CF.RESERVE reserve_mi0 1000 MAXITERATIONS 0
} {ERR*max iterations*}

test_error "CF.RESERVE with invalid EXPANSION (negative)" {
  r CF.RESERVE reserve_expneg 1000 EXPANSION -1
} {ERR*expansion*}

test_error "CF.RESERVE duplicate BUCKETSIZE option" {
  r CF.RESERVE reserve_dup 1000 BUCKETSIZE 2 BUCKETSIZE 4
} {ERR*duplicate*}

test_error "CF.RESERVE unrecognized option" {
  r CF.RESERVE reserve_bad 1000 FOO bar
} {ERR*unrecognized*}

test_error "CF.RESERVE wrong arity" {
  r CF.RESERVE onlyname
} {ERR*wrong*}

test_error "CF.RESERVE wrong type distinction" {
  r SET reserve_string_key value
  r CF.RESERVE reserve_string_key 1000
} {WRONGTYPE*}

puts "\n=== CF.ADD / CF.ADDNX ==="

test "CF.ADD new item returns 1" {
  r CF.ADD add_basic hello
} {1}

test "CF.ADD duplicate item also returns 1 (cuckoo stores duplicates)" {
  r CF.ADD add_basic hello
} {1}

test_assert "CF.COUNT reflects two copies after duplicate CF.ADD" {
  set c [r CF.COUNT add_basic hello]
  if {$c != 2} { error "expected COUNT=2, got $c" }
}

test "CF.ADD auto-creates filter on missing key" {
  r CF.ADD add_auto testitem
} {1}

test_error "CF.ADD wrong arity" {
  r CF.ADD onlykey
} {ERR*wrong*}

test "CF.ADDNX new item returns 1" {
  r CF.ADDNX addnx_basic hello
} {1}

test "CF.ADDNX existing item returns 0" {
  r CF.ADDNX addnx_basic hello
} {0}

test_assert "CF.ADDNX does not create duplicate fingerprint" {
  set c [r CF.COUNT addnx_basic hello]
  if {$c != 1} { error "expected COUNT=1 after ADDNX no-op, got $c" }
}

puts "\n=== CF.INSERT / CF.INSERTNX ==="

test_assert "CF.INSERT inserts multiple items" {
  r DEL insert_basic
  set reply [r CF.INSERT insert_basic ITEMS a b c]
  if {$reply ne {1 1 1}} { error "expected {1 1 1}, got $reply" }
}

test_assert "CF.INSERT with CAPACITY creates filter with given capacity" {
  r DEL insert_cap
  r CF.INSERT insert_cap CAPACITY 500 ITEMS x
  set info [r CF.INFO insert_cap]
  set bucketsIdx [lsearch $info "Number of buckets"]
  set buckets [lindex $info [expr {$bucketsIdx + 1}]]
  set sizeIdx [lsearch $info "Bucket size"]
  set bucketSize [lindex $info [expr {$sizeIdx + 1}]]
  set totalSlots [expr {$buckets * $bucketSize}]
  if {$totalSlots < 500} { error "expected buckets * bucket size >= 500, got $totalSlots" }
}

test_error "CF.INSERT NOCREATE on missing key" {
  r DEL insert_nocreate
  r CF.INSERT insert_nocreate NOCREATE ITEMS a
} {ERR*key does not exist*}

test_assert "CF.INSERT NOCREATE on existing key succeeds" {
  r DEL insert_nc2
  r CF.RESERVE insert_nc2 100
  set reply [r CF.INSERT insert_nc2 NOCREATE ITEMS a b]
  if {$reply ne {1 1}} { error "expected {1 1}, got $reply" }
}

test_error "CF.INSERT CAPACITY + NOCREATE mutually exclusive" {
  r CF.INSERT insert_mut CAPACITY 100 NOCREATE ITEMS a
} {ERR*NOCREATE*CAPACITY*}

test_error "CF.INSERT without ITEMS keyword" {
  r CF.INSERT insert_noitems CAPACITY 100
} {ERR*ITEMS*}

test_assert "CF.INSERTNX skips existing items" {
  r DEL insertnx_basic
  r CF.INSERT insertnx_basic ITEMS dup1 dup2
  set reply [r CF.INSERTNX insertnx_basic ITEMS dup1 new1]
  if {$reply ne {0 1}} { error "expected {0 1}, got $reply" }
}

test_assert "CF.INSERT reports -1 per item on full fixed-size filter without stopping" {
  r DEL insert_full
  r CF.RESERVE insert_full 4 BUCKETSIZE 1 MAXITERATIONS 5 EXPANSION 0
  set items {}
  for {set i 0} {$i < 50} {incr i} { lappend items "full_item_$i" }
  set reply [r CF.INSERT insert_full ITEMS {*}$items]
  # Every reply element must be 1 or -1 (not truncated/omitted like BF.INSERT)
  if {[llength $reply] != 50} { error "expected 50 reply elements, got [llength $reply]" }
  set saw_full 0
  foreach v $reply {
    if {$v == -1} { set saw_full 1 }
    if {$v != 1 && $v != -1} { error "unexpected reply element: $v" }
  }
  if {!$saw_full} { error "expected at least one -1 (filter full) reply" }
}

puts "\n=== CF.EXISTS / CF.MEXISTS ==="

test "CF.EXISTS for present item" {
  r CF.EXISTS add_basic hello
} {1}

test "CF.EXISTS for absent item" {
  r CF.EXISTS add_basic never_added
} {0}

test "CF.EXISTS on missing key" {
  r CF.EXISTS no_such_cf_key item
} {0}

test_error "CF.EXISTS wrong arity" {
  r CF.EXISTS onlykey
} {ERR*wrong*}

test_assert "CF.MEXISTS returns per-item results" {
  r DEL mexists_basic
  r CF.INSERT mexists_basic ITEMS a b c
  set reply [r CF.MEXISTS mexists_basic a x c]
  if {$reply ne {1 0 1}} { error "expected {1 0 1}, got $reply" }
}

test_assert "CF.MEXISTS on missing key returns all zeros" {
  r DEL mexists_missing
  set reply [r CF.MEXISTS mexists_missing a b c]
  if {$reply ne {0 0 0}} { error "expected {0 0 0}, got $reply" }
}

puts "\n=== CF.DEL ==="

test_assert "CF.DEL removes an inserted item" {
  r DEL del_basic
  r CF.ADD del_basic gone
  set before [r CF.EXISTS del_basic gone]
  set deleted [r CF.DEL del_basic gone]
  set after [r CF.EXISTS del_basic gone]
  if {$before != 1 || $deleted != 1 || $after != 0} {
    error "before=$before deleted=$deleted after=$after"
  }
}

test "CF.DEL absent item returns 0" {
  r CF.DEL del_basic never_there
} {0}

test_error "CF.DEL on missing key" {
  r DEL del_missing
  r CF.DEL del_missing item
} {ERR*key does not exist*}

test_assert "CF.DEL removes only one of duplicate copies" {
  r DEL del_dup
  r CF.ADD del_dup twice
  r CF.ADD del_dup twice
  r CF.DEL del_dup twice
  set c [r CF.COUNT del_dup twice]
  set e [r CF.EXISTS del_dup twice]
  if {$c != 1 || $e != 1} { error "expected COUNT=1 EXISTS=1 after deleting one copy, got COUNT=$c EXISTS=$e" }
}

puts "\n=== CF.COUNT ==="

test "CF.COUNT for absent item is 0" {
  r DEL count_basic
  r CF.RESERVE count_basic 100
  r CF.COUNT count_basic absent
} {0}

test_assert "CF.COUNT increments with each CF.ADD" {
  r CF.ADD count_basic x
  r CF.ADD count_basic x
  r CF.ADD count_basic x
  set c [r CF.COUNT count_basic x]
  if {$c != 3} { error "expected COUNT=3, got $c" }
}

test "CF.COUNT on missing key is 0" {
  r DEL count_missing
  r CF.COUNT count_missing x
} {0}

puts "\n=== CF.INFO ==="

test "CF.INFO returns full info (8 fields)" {
  r DEL info_basic
  r CF.RESERVE info_basic 1000
  llength [r CF.INFO info_basic]
} {16}

test_assert "CF.INFO field order matches documentation" {
  set info [r CF.INFO info_basic]
  set expected {"Size" "Number of buckets" "Number of filters" \
                 "Number of items inserted" "Number of items deleted" \
                 "Bucket size" "Expansion rate" "Max iterations"}
  set idx 0
  foreach field $expected {
    set got [lindex $info $idx]
    if {$got ne $field} { error "field $idx: expected '$field', got '$got'" }
    incr idx 2
  }
}

test_assert "CF.INFO Number of items inserted/deleted tracked correctly" {
  r DEL info_counts
  r CF.RESERVE info_counts 100
  r CF.ADD info_counts a
  r CF.ADD info_counts b
  r CF.DEL info_counts a
  set info [r CF.INFO info_counts]
  set ins_idx [lsearch $info "Number of items inserted"]
  set del_idx [lsearch $info "Number of items deleted"]
  set inserted [lindex $info [expr {$ins_idx + 1}]]
  set deleted [lindex $info [expr {$del_idx + 1}]]
  if {$inserted != 2} { error "expected inserted=2, got $inserted" }
  if {$deleted != 1} { error "expected deleted=1, got $deleted" }
}

test_error "CF.INFO on missing key" {
  r CF.INFO no_such_key
} {ERR*key does not exist*}

test_error "CF.INFO wrong arity" {
  r CF.INFO
} {ERR*wrong*}

puts "\n=== RESP3 CF.INFO / CF.EXISTS ==="

test_assert "RESP3 CF.EXISTS returns integer reply type" {
  global port
  r DEL resp3_exists
  r CF.ADD resp3_exists probe
  set fd [redis_connect localhost $port]
  raw_command_reply $fd HELLO 3
  set reply [raw_command_reply $fd CF.EXISTS resp3_exists probe]
  close $fd
  set type [lindex $reply 0]
  set val [lindex $reply 1]
  if {$type ne ":" || $val != 1} {
    error "expected RESP3 integer 1, got type=$type val=$val"
  }
}

test_assert "RESP3 CF.INFO returns array reply" {
  global port
  set fd [redis_connect localhost $port]
  raw_command_reply $fd HELLO 3
  set reply [raw_command_reply $fd CF.INFO info_basic]
  close $fd
  set type [lindex $reply 0]
  if {$type ne "*"} { error "expected RESP3 array, got type=$type" }
}

puts "\n=== CF.SCANDUMP / CF.LOADCHUNK round-trip ==="

test_assert "SCANDUMP/LOADCHUNK preserves data" {
  r DEL cfdump_src cfdump_dst
  r CF.RESERVE cfdump_src 500
  for {set i 0} {$i < 200} {incr i} {
    r CF.ADD cfdump_src "cfdump_item_$i"
  }

  set cursor 0
  while {1} {
    set reply [r CF.SCANDUMP cfdump_src $cursor]
    set next_cursor [lindex $reply 0]
    set chunk_data [lindex $reply 1]

    if {$next_cursor == 0 && [string length $chunk_data] == 0} break

    r CF.LOADCHUNK cfdump_dst $next_cursor $chunk_data
    set cursor $next_cursor
  }

  for {set i 0} {$i < 200} {incr i} {
    set exists [r CF.EXISTS cfdump_dst "cfdump_item_$i"]
    if {$exists != 1} { error "False negative after LOADCHUNK for cfdump_item_$i" }
  }

  set src_info [r CF.INFO cfdump_src]
  set dst_info [r CF.INFO cfdump_dst]
  foreach field {"Number of filters" "Number of items inserted" "Bucket size"} {
    set si [lsearch $src_info $field]
    set di [lsearch $dst_info $field]
    set sv [lindex $src_info [expr {$si + 1}]]
    set dv [lindex $dst_info [expr {$di + 1}]]
    if {$sv ne $dv} { error "$field mismatch after LOADCHUNK: src=$sv dst=$dv" }
  }
}

test_assert "SCANDUMP/LOADCHUNK across multiple layers (auto-expansion)" {
  r DEL cfdump_multi_src cfdump_multi_dst
  r CF.RESERVE cfdump_multi_src 16 BUCKETSIZE 2 MAXITERATIONS 20 EXPANSION 2
  set items {}
  for {set i 0} {$i < 400} {incr i} {
    set item "cfmulti_item_$i"
    if {[r CF.ADD cfdump_multi_src $item] == 1} { lappend items $item }
  }
  set info [r CF.INFO cfdump_multi_src]
  set idx [lsearch $info "Number of filters"]
  set numFilters [lindex $info [expr {$idx + 1}]]
  if {$numFilters < 2} { error "expected multi-layer chain, got $numFilters layer(s)" }

  set cursor 0
  while {1} {
    set reply [r CF.SCANDUMP cfdump_multi_src $cursor]
    set next_cursor [lindex $reply 0]
    set chunk_data [lindex $reply 1]
    if {$next_cursor == 0 && [string length $chunk_data] == 0} break
    r CF.LOADCHUNK cfdump_multi_dst $next_cursor $chunk_data
    set cursor $next_cursor
  }

  foreach item $items {
    set exists [r CF.EXISTS cfdump_multi_dst $item]
    if {$exists != 1} { error "False negative after multi-layer LOADCHUNK for $item" }
  }
}

test_assert "SCANDUMP/LOADCHUNK preserves binary chunk payloads" {
  r DEL cfdump_bin_src cfdump_bin_dst
  r CF.RESERVE cfdump_bin_src 50
  set binary_items [list "null\x00byte" "line\r\nbreak" "brace\{value\}" "slash\\value"]
  foreach item $binary_items {
    r CF.ADD cfdump_bin_src $item
  }

  set cursor 0
  while {1} {
    set reply [r CF.SCANDUMP cfdump_bin_src $cursor]
    set next_cursor [lindex $reply 0]
    set chunk_data [lindex $reply 1]
    if {$next_cursor == 0 && [string length $chunk_data] == 0} break
    r CF.LOADCHUNK cfdump_bin_dst $next_cursor $chunk_data
    set cursor $next_cursor
  }

  foreach item $binary_items {
    set exists [r CF.EXISTS cfdump_bin_dst $item]
    if {$exists != 1} { error "False negative after binary LOADCHUNK for <$item>" }
  }
}

puts "\n=== LOADCHUNK half-restore safety ==="

test_error "Half-restored filter rejects EXISTS (loading state)" {
  r DEL cf_half_src cf_half_dst
  r CF.RESERVE cf_half_src 100
  r CF.ADD cf_half_src testitem

  set reply [r CF.SCANDUMP cf_half_src 0]
  set hdr_cursor [lindex $reply 0]
  set hdr_data [lindex $reply 1]
  r CF.LOADCHUNK cf_half_dst $hdr_cursor $hdr_data

  r CF.EXISTS cf_half_dst testitem
} {ERR filter is being loaded}

test_error "Half-restored filter rejects COUNT (loading state)" {
  r CF.COUNT cf_half_dst testitem
} {ERR filter is being loaded}

test_error "Half-restored filter rejects ADD (loading state)" {
  r CF.ADD cf_half_dst newitem
} {ERR filter is being loaded}

test_error "Half-restored filter rejects SCANDUMP (loading state)" {
  r CF.SCANDUMP cf_half_dst 0
} {ERR filter is being loaded}

test_error "LOADCHUNK cursor>1 rejects completed filter" {
  r DEL cf_lc_completed
  r CF.RESERVE cf_lc_completed 100
  r CF.ADD cf_lc_completed x
  r CF.LOADCHUNK cf_lc_completed 2 [string repeat \x00 128]
} {ERR received bad data}

test_error "LOADCHUNK on existing non-loading key rejects header re-send" {
  r DEL cf_lc_existing
  r CF.RESERVE cf_lc_existing 100
  set reply [r CF.SCANDUMP cf_lc_existing 0]
  r CF.LOADCHUNK cf_lc_existing 1 [lindex $reply 1]
} {ERR received bad data}

puts "\n=== Wrong type errors ==="

test_error "CF.ADD on string key" {
  r SET cf_wrongtype_string val
  r CF.ADD cf_wrongtype_string item
} {WRONGTYPE*}

test_error "CF.EXISTS on string key" {
  r CF.EXISTS cf_wrongtype_string item
} {WRONGTYPE*}

test_error "CF.INFO on string key" {
  r CF.INFO cf_wrongtype_string
} {WRONGTYPE*}

test_error "CF.DEL on string key" {
  r CF.DEL cf_wrongtype_string item
} {WRONGTYPE*}

test_error "CF.ADD on bloom-typed key" {
  r DEL cf_vs_bf
  r BF.RESERVE cf_vs_bf 0.01 100
  r CF.ADD cf_vs_bf item
} {WRONGTYPE*}

puts "\n=== Generic Redis command compatibility ==="

test_assert "RENAME preserves cuckoo data" {
  r DEL cf_rename_src cf_rename_dst
  r CF.RESERVE cf_rename_src 100
  r CF.ADD cf_rename_src item1
  r CF.ADD cf_rename_src item2
  r RENAME cf_rename_src cf_rename_dst
  set e1 [r CF.EXISTS cf_rename_dst item1]
  set e2 [r CF.EXISTS cf_rename_dst item2]
  if {$e1 != 1 || $e2 != 1} { error "Items missing after RENAME: item1=$e1 item2=$e2" }
}

test "RENAME removes old cuckoo key" {
  r CF.EXISTS cf_rename_src item1
} {0}

set copy_supported 1
r DEL cf_copy_probe_src cf_copy_probe_dst
r CF.RESERVE cf_copy_probe_src 10
r CF.ADD cf_copy_probe_src probe_item
if {[catch {r COPY cf_copy_probe_src cf_copy_probe_dst} err]} {
  if {[string match "*unknown command*" $err] || [string match "*not supported*" $err]} {
    set copy_supported 0
    puts "  (COPY not supported — skipping COPY tests)"
  }
}
r DEL cf_copy_probe_src cf_copy_probe_dst

if {$copy_supported} {
  test_assert "COPY cuckoo key preserves data" {
    r DEL cf_copy_src cf_copy_dst
    r CF.RESERVE cf_copy_src 200
    for {set i 0} {$i < 50} {incr i} {
      r CF.ADD cf_copy_src "cfcopy_item_$i"
    }
    r COPY cf_copy_src cf_copy_dst REPLACE
    for {set i 0} {$i < 50} {incr i} {
      set e [r CF.EXISTS cf_copy_dst "cfcopy_item_$i"]
      if {$e != 1} { error "False negative after COPY for cfcopy_item_$i" }
    }
  }

  test_assert "COPY cuckoo key: source remains intact" {
    for {set i 0} {$i < 50} {incr i} {
      set e [r CF.EXISTS cf_copy_src "cfcopy_item_$i"]
      if {$e != 1} { error "Source false negative after COPY for cfcopy_item_$i" }
    }
  }
}

test_assert "DUMP/RESTORE preserves cuckoo data" {
  r DEL cf_dr_src cf_dr_dst
  r CF.RESERVE cf_dr_src 200
  for {set i 0} {$i < 100} {incr i} {
    r CF.ADD cf_dr_src "cfdr_item_$i"
  }
  set dump [r DUMP cf_dr_src]
  r RESTORE cf_dr_dst 0 $dump
  for {set i 0} {$i < 100} {incr i} {
    set e [r CF.EXISTS cf_dr_dst "cfdr_item_$i"]
    if {$e != 1} { error "False negative after RESTORE for cfdr_item_$i" }
  }
}

test_assert "DUMP/RESTORE preserves CF.INFO metadata" {
  set src_info [r CF.INFO cf_dr_src]
  set dst_info [r CF.INFO cf_dr_dst]
  foreach field {"Number of buckets" "Number of filters" "Number of items inserted" "Bucket size"} {
    set si [lsearch $src_info $field]
    set di [lsearch $dst_info $field]
    set sv [lindex $src_info [expr {$si + 1}]]
    set dv [lindex $dst_info [expr {$di + 1}]]
    if {$sv ne $dv} { error "$field mismatch after RESTORE: src=$sv dst=$dv" }
  }
}

test_assert "RESTORE REPLACE overwrites existing cuckoo key" {
  r DEL cf_rr_src cf_rr_dst
  r CF.RESERVE cf_rr_src 100
  r CF.ADD cf_rr_src new_data
  r CF.RESERVE cf_rr_dst 100
  r CF.ADD cf_rr_dst old_data
  set dump [r DUMP cf_rr_src]
  r RESTORE cf_rr_dst 0 $dump REPLACE
  set e_new [r CF.EXISTS cf_rr_dst new_data]
  if {$e_new != 1} { error "new_data missing after RESTORE REPLACE" }
  set e_old [r CF.EXISTS cf_rr_dst old_data]
  if {$e_old != 0} { error "old_data still present after RESTORE REPLACE" }
}

puts "\n=== DEBUG DIGEST-VALUE / DEBUG RELOAD ==="

test_assert "DEBUG DIGEST-VALUE is stable across DEBUG RELOAD" {
  r DEL cf_digest_src
  r CF.RESERVE cf_digest_src 200
  for {set i 0} {$i < 50} {incr i} {
    r CF.ADD cf_digest_src "cfdigest_item_$i"
  }
  set before [r DEBUG DIGEST-VALUE cf_digest_src]
  r DEBUG RELOAD
  set after [r DEBUG DIGEST-VALUE cf_digest_src]
  if {$before ne $after} { error "Digest changed across DEBUG RELOAD: before=$before after=$after" }
}

test_assert "DEBUG DIGEST-VALUE changes after CF.ADD inserts a new item" {
  set before [r DEBUG DIGEST-VALUE cf_digest_src]
  r CF.ADD cf_digest_src "cfdigest_new_item"
  set after [r DEBUG DIGEST-VALUE cf_digest_src]
  if {$before eq $after} { error "Digest did not change after CF.ADD" }
}

test_assert "DEBUG RELOAD preserves all items across multiple layers" {
  r DEL cf_reload_multi
  r CF.RESERVE cf_reload_multi 16 BUCKETSIZE 2 MAXITERATIONS 20 EXPANSION 2
  set items {}
  for {set i 0} {$i < 300} {incr i} {
    set item "cfreload_item_$i"
    if {[r CF.ADD cf_reload_multi $item] == 1} { lappend items $item }
  }
  r DEBUG RELOAD
  foreach item $items {
    set e [r CF.EXISTS cf_reload_multi $item]
    if {$e != 1} { error "False negative after DEBUG RELOAD for $item" }
  }
}

puts "\n=== EXPIRE / TTL ==="

test_assert "EXPIRE works on cuckoo keys" {
  r DEL cf_expire_basic
  r CF.ADD cf_expire_basic item
  r EXPIRE cf_expire_basic 100
  set ttl [r TTL cf_expire_basic]
  if {$ttl <= 0 || $ttl > 100} { error "unexpected TTL: $ttl" }
}

test_assert "PERSIST removes TTL on cuckoo key" {
  r PERSIST cf_expire_basic
  set ttl [r TTL cf_expire_basic]
  if {$ttl != -1} { error "expected TTL=-1 after PERSIST, got $ttl" }
}

puts "\n=== TYPE / generic EXISTS ==="

test "TYPE reports module for cuckoo key" {
  r DEL cf_type_basic
  r CF.ADD cf_type_basic item
  r TYPE cf_type_basic
} {MBcuckoo-}

test_assert "Generic EXISTS finds cuckoo key" {
  set e [r EXISTS cf_type_basic]
  if {$e != 1} { error "expected EXISTS=1, got $e" }
}

test_assert "UNLINK removes cuckoo key" {
  r UNLINK cf_type_basic
  set e [r EXISTS cf_type_basic]
  if {$e != 0} { error "expected EXISTS=0 after UNLINK, got $e" }
}

puts "\n=== MULTI/EXEC transactions ==="

test_assert "Cuckoo commands work inside MULTI/EXEC" {
  r DEL cf_multi_basic
  r MULTI
  r CF.ADD cf_multi_basic a
  r CF.ADD cf_multi_basic b
  set reply [r EXEC]
  if {$reply ne {1 1}} { error "expected {1 1}, got $reply" }
  set e [r CF.EXISTS cf_multi_basic a]
  if {$e != 1} { error "item missing after MULTI/EXEC" }
}

puts "\n=== Module config tests ==="

test_assert "Default config creates filter with expected defaults" {
  r DEL cf_cfg_default
  r CF.ADD cf_cfg_default test
  set info [r CF.INFO cf_cfg_default]
  set idx [lsearch $info "Bucket size"]
  set bs [lindex $info [expr {$idx + 1}]]
  if {$bs != 2} { error "Default bucket size should be 2, got $bs" }
  set idx [lsearch $info "Expansion rate"]
  set exp [lindex $info [expr {$idx + 1}]]
  if {$exp != 1} { error "Default expansion should be 1, got $exp" }
  set idx [lsearch $info "Max iterations"]
  set mi [lindex $info [expr {$idx + 1}]]
  if {$mi != 20} { error "Default max iterations should be 20, got $mi" }
}

test_assert "Module load args override CF_BUCKET_SIZE, CF_INITIAL_SIZE, CF_EXPANSION, CF_MAX_ITERATIONS" {
  global module_path
  set cfg_port [find_free_port]
  set cfg_dir "/tmp/cf_cfg_$cfg_port"
  file delete -force $cfg_dir
  file mkdir $cfg_dir

  catch {
    exec redis-server \
      --port $cfg_port \
      --daemonize yes \
      --loglevel warning \
      --logfile $cfg_dir/redis.log \
      --dbfilename dump.rdb \
      --dir $cfg_dir \
      --loadmodule $module_path CF_BUCKET_SIZE 4 CF_INITIAL_SIZE 256 CF_EXPANSION 3 CF_MAX_ITERATIONS 30
  } err
  wait_redis_ready localhost $cfg_port
  set cfg_fd [redis_connect localhost $cfg_port]

  set old_fd $::redis_fd
  set ::redis_fd $cfg_fd
  r CF.ADD cfg_override item
  set info [r CF.INFO cfg_override]
  set idx [lsearch $info "Bucket size"]
  set bs [lindex $info [expr {$idx + 1}]]
  set idx [lsearch $info "Expansion rate"]
  set exp [lindex $info [expr {$idx + 1}]]
  set idx [lsearch $info "Max iterations"]
  set mi [lindex $info [expr {$idx + 1}]]
  set idx [lsearch $info "Number of buckets"]
  set buckets [lindex $info [expr {$idx + 1}]]
  set ::redis_fd $old_fd

  catch {redis_command $cfg_fd SHUTDOWN NOSAVE}
  catch {close $cfg_fd}
  file delete -force $cfg_dir

  if {$bs != 4} { error "configured bucket size should be 4, got $bs" }
  if {$exp != 3} { error "configured expansion should be 3, got $exp" }
  if {$mi != 30} { error "configured max iterations should be 30, got $mi" }
  if {$buckets < 64} { error "configured initial size should yield >=64 buckets, got $buckets" }
}

test_assert "Module load rejects CF_BUCKET_SIZE missing value" {
  module_load_should_fail "missing CF_BUCKET_SIZE" CF_BUCKET_SIZE
}

test_assert "Module load rejects CF_BUCKET_SIZE out of range" {
  module_load_should_fail "bad CF_BUCKET_SIZE" CF_BUCKET_SIZE 0
}

test_assert "Module load rejects CF_INITIAL_SIZE missing value" {
  module_load_should_fail "missing CF_INITIAL_SIZE" CF_INITIAL_SIZE
}

test_assert "Module load rejects CF_INITIAL_SIZE zero" {
  module_load_should_fail "bad CF_INITIAL_SIZE" CF_INITIAL_SIZE 0
}

test_assert "Module load rejects CF_MAX_ITERATIONS missing value" {
  module_load_should_fail "missing CF_MAX_ITERATIONS" CF_MAX_ITERATIONS
}

test_assert "Module load rejects CF_MAX_ITERATIONS zero" {
  module_load_should_fail "bad CF_MAX_ITERATIONS" CF_MAX_ITERATIONS 0
}

test_assert "Module load rejects CF_EXPANSION missing value" {
  module_load_should_fail "missing CF_EXPANSION" CF_EXPANSION
}

test_assert "Module load rejects CF_EXPANSION out of range" {
  module_load_should_fail "bad CF_EXPANSION" CF_EXPANSION -1
}

test_assert "Module load rejects CF_MAX_EXPANSIONS missing value" {
  module_load_should_fail "missing CF_MAX_EXPANSIONS" CF_MAX_EXPANSIONS
}

test_assert "Module load rejects CF_MAX_EXPANSIONS zero" {
  module_load_should_fail "bad CF_MAX_EXPANSIONS" CF_MAX_EXPANSIONS 0
}

test_assert "Module load rejects unknown config argument" {
  module_load_should_fail "unknown config" UNKNOWN_CF_ARG 1
}

puts "\n=== Replication (master/replica) ==="

test_assert "Cuckoo data replicates to a replica" {
  global port module_path

  set rep_port [find_free_port]
  set rep_dir "/tmp/cf_replica_$rep_port"
  file delete -force $rep_dir
  file mkdir $rep_dir

  catch {
    exec redis-server \
      --port $rep_port \
      --daemonize yes \
      --loglevel warning \
      --logfile $rep_dir/redis.log \
      --dbfilename replica.rdb \
      --dir $rep_dir \
      --loadmodule $module_path \
      --replicaof 127.0.0.1 $port
  }
  wait_redis_ready localhost $rep_port
  set rep_fd [redis_connect localhost $rep_port]

  set replica_linked 0
  for {set i 0} {$i < 50} {incr i} {
    set info [r INFO replication]
    if {[string match "*connected_slaves:1*" $info] ||
        [string match "*slave0:*state=online*" $info]} {
      set replica_linked 1
      break
    }
    after 100
  }
  if {!$replica_linked} { error "Master never saw replica as connected" }

  r DEL cf_repl_test
  r CF.RESERVE cf_repl_test 200
  for {set i 0} {$i < 50} {incr i} {
    r CF.ADD cf_repl_test "cfrepl_item_$i"
  }

  r WAIT 1 5000

  set all_found 1
  for {set i 0} {$i < 50} {incr i} {
    set e [redis_command $rep_fd CF.EXISTS cf_repl_test "cfrepl_item_$i"]
    if {$e != 1} { set all_found 0; break }
  }

  catch {redis_command $rep_fd SHUTDOWN NOSAVE}
  catch {close $rep_fd}
  after 200
  file delete -force $rep_dir

  if {!$all_found} { error "Some items missing on replica" }
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
file delete -force /tmp/cf_tcl_test.rdb

exit $test_failed
