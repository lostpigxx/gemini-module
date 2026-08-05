#!/usr/bin/env tclsh
#
# TCL integration tests for the redis_bloom module's Top-K (TOPK.*)
# commands. Self-contained: starts a redis-server, loads the module, runs
# tests, shuts down.
#
# This file is intentionally independent from bloom_test.tcl/cf_test.tcl/
# cms_test.tcl (separate RESP client + test framework boilerplate) per
# project convention.
#
# Usage: tclsh topk_test.tcl [path/to/redis_bloom.so]

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
      --logfile /tmp/topk_tcl_test.log \
      --dbfilename topk_tcl_test.rdb \
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
  catch {
    set f [open /tmp/topk_tcl_test.log r]
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

proc stop_redis {fd} {
  catch {redis_command $fd SHUTDOWN NOSAVE}
  catch {close $fd}
  after 200
  file delete -force /tmp/topk_tcl_test.rdb
  file delete -force /tmp/topk_tcl_test.log
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
  puts "Usage: tclsh topk_test.tcl \[path/to/redis_bloom.so\]"
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

puts "=== TOPK.RESERVE ==="

test "TOPK.RESERVE creates a new sketch with defaults" {
  r TOPK.RESERVE topk_basic 3
} {OK}

test_error "TOPK.RESERVE on existing key returns error" {
  r TOPK.RESERVE topk_basic 3
} {ERR*exists*}

test "TOPK.RESERVE with explicit width/depth/decay" {
  r TOPK.RESERVE topk_explicit 5 16 4 0.8
} {OK}

test_error "TOPK.RESERVE with invalid topk (0)" {
  r TOPK.RESERVE topk_bad0 0
} {ERR*topk*}

test_error "TOPK.RESERVE with invalid decay" {
  r TOPK.RESERVE topk_bad_decay 3 16 4 1.5
} {ERR*decay*}

puts "\n=== TOPK.ADD / TOPK.QUERY / TOPK.LIST ==="

test_error "TOPK.ADD on missing key returns error" {
  r TOPK.ADD topk_missing item
} {ERR*}

test "TOPK.ADD single item into empty top-k (no eviction)" {
  r TOPK.ADD topk_basic a
} {(nil)}

test "TOPK.ADD fills remaining slots without eviction" {
  r TOPK.ADD topk_basic b c
} {(nil) (nil)}

test "TOPK.QUERY returns 1 for tracked items" {
  r TOPK.QUERY topk_basic a b c
} {1 1 1}

test "TOPK.QUERY returns 0 for untracked item" {
  r TOPK.QUERY topk_basic never_added
} {0}

test "TOPK.LIST returns tracked item names" {
  lsort [r TOPK.LIST topk_basic]
} {a b c}

test_assert "TOPK.LIST WITHCOUNT returns interleaved name/count pairs" {
  set result [r TOPK.LIST topk_basic WITHCOUNT]
  if {[llength $result] != 6} { error "expected 6 elements, got [llength $result]" }
}

test_error "TOPK.QUERY on missing key returns error" {
  r TOPK.QUERY topk_missing item
} {ERR*}

test_error "TOPK.ADD on wrong type key returns error" {
  r DEL topk_wrongtype
  r SET topk_wrongtype somestring
  r TOPK.ADD topk_wrongtype item
} {WRONGTYPE*}

puts "\n=== TOPK.INCRBY ==="

test_error "TOPK.INCRBY on missing key returns error" {
  r TOPK.INCRBY topk_missing item 1
} {ERR*}

test_error "TOPK.INCRBY with increment out of range" {
  r TOPK.INCRBY topk_basic a 100001
} {ERR*increment*}

test_error "TOPK.INCRBY with zero increment" {
  r TOPK.INCRBY topk_basic a 0
} {ERR*increment*}

test_assert "TOPK.INCRBY heavy item evicts a light item" {
  r DEL topk_evict
  r TOPK.RESERVE topk_evict 2 64 4 0.9
  r TOPK.ADD topk_evict light1
  r TOPK.ADD topk_evict light2
  set result [r TOPK.INCRBY topk_evict heavy 500]
  if {[llength $result] != 1} { error "expected single-element reply, got: $result" }
  set evicted [lindex $result 0]
  if {$evicted eq "(nil)"} { error "expected an eviction from a 500x incrby against k=2, got nil" }
  set q [r TOPK.QUERY topk_evict heavy]
  if {$q != 1} { error "expected heavy item to be tracked after eviction, got $q" }
}

puts "\n=== TOPK.COUNT ==="

test_assert "TOPK.COUNT on tracked item returns positive count" {
  set c [r TOPK.COUNT topk_basic a]
  if {$c <= 0} { error "expected positive count, got $c" }
}

test "TOPK.COUNT on absent item returns 0" {
  r TOPK.COUNT topk_basic totally_absent_xyz
} {0}

puts "\n=== TOPK.INFO ==="

test_assert "TOPK.INFO returns k/width/depth/decay fields" {
  set result [r TOPK.INFO topk_explicit]
  if {[llength $result] != 8} { error "expected 8 elements, got: $result" }
  array set info $result
  if {$info(k) != 5} { error "expected k=5, got $info(k)" }
  if {$info(width) != 16} { error "expected width=16, got $info(width)" }
  if {$info(depth) != 4} { error "expected depth=4, got $info(depth)" }
  if {abs($info(decay) - 0.8) > 0.0001} { error "expected decay~=0.8, got $info(decay)" }
}

test_error "TOPK.INFO on missing key returns error" {
  r TOPK.INFO topk_info_missing
} {ERR*}

puts "\n=== DEBUG DIGEST-VALUE / DEBUG RELOAD ==="

test_assert "DEBUG DIGEST-VALUE is stable across DEBUG RELOAD" {
  r DEL topk_digest_src
  r TOPK.RESERVE topk_digest_src 10 64 4 0.9
  for {set i 0} {$i < 30} {incr i} {
    r TOPK.ADD topk_digest_src "topkdigest_item_$i"
  }
  set before [r DEBUG DIGEST-VALUE topk_digest_src]
  r DEBUG RELOAD
  set after [r DEBUG DIGEST-VALUE topk_digest_src]
  if {$before ne $after} { error "Digest changed across DEBUG RELOAD: before=$before after=$after" }
}

test_assert "DEBUG DIGEST-VALUE changes after TOPK.ADD" {
  set before [r DEBUG DIGEST-VALUE topk_digest_src]
  for {set i 0} {$i < 5} {incr i} {
    r TOPK.ADD topk_digest_src topkdigest_new_item
  }
  set after [r DEBUG DIGEST-VALUE topk_digest_src]
  if {$before eq $after} { error "Digest did not change after TOPK.ADD" }
}

test_assert "DEBUG RELOAD preserves top-k list" {
  r DEL topk_reload_test
  r TOPK.RESERVE topk_reload_test 5 64 4 0.9
  for {set i 0} {$i < 5} {incr i} {
    set item "topkreload_item_$i"
    for {set j 0} {$j <= $i} {incr j} {
      r TOPK.ADD topk_reload_test $item
    }
  }
  set before [lsort [r TOPK.LIST topk_reload_test]]
  r DEBUG RELOAD
  set after [lsort [r TOPK.LIST topk_reload_test]]
  if {$before ne $after} { error "Top-k list changed after DEBUG RELOAD: before=$before after=$after" }
}

test_assert "DUMP / RESTORE round-trips a sketch" {
  r DEL topk_dr_src topk_dr_dst
  r TOPK.RESERVE topk_dr_src 3 32 4 0.9
  r TOPK.ADD topk_dr_src dumpitem
  set dump [r DUMP topk_dr_src]
  r RESTORE topk_dr_dst 0 $dump
  set q [r TOPK.QUERY topk_dr_dst dumpitem]
  if {$q != 1} { error "expected 1 after DUMP/RESTORE, got $q" }
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
file delete -force /tmp/topk_tcl_test.rdb

exit $test_failed
