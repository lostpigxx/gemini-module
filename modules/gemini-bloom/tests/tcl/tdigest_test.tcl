#!/usr/bin/env tclsh
#
# TCL integration tests for the redis_bloom module's T-Digest (TDIGEST.*)
# commands. Self-contained: starts a redis-server, loads the module, runs
# tests, shuts down.
#
# This file is intentionally independent from bloom_test.tcl/cf_test.tcl/
# cms_test.tcl/topk_test.tcl (separate RESP client + test framework
# boilerplate) per project convention.
#
# Usage: tclsh tdigest_test.tcl [path/to/redis_bloom.so]

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
      --logfile /tmp/tdigest_tcl_test.log \
      --dbfilename tdigest_tcl_test.rdb \
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
    set f [open /tmp/tdigest_tcl_test.log r]
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
  file delete -force /tmp/tdigest_tcl_test.rdb
  file delete -force /tmp/tdigest_tcl_test.log
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
  puts "Usage: tclsh tdigest_test.tcl \[path/to/redis_bloom.so\]"
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

puts "=== TDIGEST.CREATE ==="

test "TDIGEST.CREATE creates a new sketch with default compression" {
  r TDIGEST.CREATE td_basic
} {OK}

test_error "TDIGEST.CREATE on existing key returns error" {
  r TDIGEST.CREATE td_basic
} {ERR*exists*}

test "TDIGEST.CREATE with explicit COMPRESSION" {
  r TDIGEST.CREATE td_explicit COMPRESSION 200
} {OK}

test_error "TDIGEST.CREATE with invalid compression" {
  r TDIGEST.CREATE td_bad COMPRESSION 0
} {ERR*compression*}

test_error "TDIGEST.CREATE with unrecognized option" {
  r TDIGEST.CREATE td_bad2 BOGUS 5
} {ERR*}

puts "\n=== TDIGEST.ADD ==="

test_error "TDIGEST.ADD on missing key returns error" {
  r TDIGEST.ADD td_missing 1.0
} {ERR*}

test "TDIGEST.ADD single value" {
  r TDIGEST.ADD td_basic 1.0
} {OK}

test "TDIGEST.ADD multiple values" {
  r TDIGEST.ADD td_basic 2.0 3.0 4.0
} {OK}

test_error "TDIGEST.ADD on wrong type key returns error" {
  r DEL td_wrongtype
  r SET td_wrongtype somestring
  r TDIGEST.ADD td_wrongtype 1.0
} {WRONGTYPE*}

puts "\n=== TDIGEST.MIN / TDIGEST.MAX ==="

test_error "TDIGEST.MIN on missing key returns error" {
  r TDIGEST.MIN td_missing
} {ERR*}

test_assert "TDIGEST.MIN on empty sketch returns nan" {
  set v [r TDIGEST.MIN td_explicit]
  if {$v ne "nan"} { error "expected nan, got $v" }
}

test_assert "TDIGEST.MIN returns tracked minimum" {
  set v [r TDIGEST.MIN td_basic]
  if {abs($v - 1.0) > 0.0001} { error "expected 1.0, got $v" }
}

test_assert "TDIGEST.MAX returns tracked maximum" {
  set v [r TDIGEST.MAX td_basic]
  if {abs($v - 4.0) > 0.0001} { error "expected 4.0, got $v" }
}

puts "\n=== TDIGEST.QUANTILE / TDIGEST.CDF ==="

test_assert "TDIGEST.QUANTILE on populated sketch approximates uniform distribution" {
  r DEL td_quantile
  r TDIGEST.CREATE td_quantile
  for {set i 1} {$i <= 1000} {incr i} {
    r TDIGEST.ADD td_quantile $i
  }
  set median [lindex [r TDIGEST.QUANTILE td_quantile 0.5] 0]
  if {abs($median - 500.0) > 50.0} { error "expected median~=500, got $median" }
}

test_assert "TDIGEST.QUANTILE at q=0 and q=1 returns exact min/max" {
  set lo [lindex [r TDIGEST.QUANTILE td_quantile 0.0] 0]
  set hi [lindex [r TDIGEST.QUANTILE td_quantile 1.0] 0]
  if {abs($lo - 1.0) > 0.0001} { error "expected q=0 -> 1.0, got $lo" }
  if {abs($hi - 1000.0) > 0.0001} { error "expected q=1 -> 1000.0, got $hi" }
}

test_assert "TDIGEST.QUANTILE accepts multiple quantiles" {
  set result [r TDIGEST.QUANTILE td_quantile 0.25 0.75]
  if {[llength $result] != 2} { error "expected 2 elements, got: $result" }
}

test_assert "TDIGEST.CDF is monotonic and bounded" {
  set c0 [lindex [r TDIGEST.CDF td_quantile 0] 0]
  set c500 [lindex [r TDIGEST.CDF td_quantile 500] 0]
  set c2000 [lindex [r TDIGEST.CDF td_quantile 2000] 0]
  if {$c0 != 0} { error "expected cdf(0)=0, got $c0" }
  if {$c2000 != 1} { error "expected cdf(2000)=1, got $c2000" }
  if {$c500 <= $c0 || $c500 >= $c2000} { error "expected c0 < c500 < c2000, got $c0 $c500 $c2000" }
}

test_error "TDIGEST.QUANTILE on missing key returns error" {
  r TDIGEST.QUANTILE td_missing 0.5
} {ERR*}

puts "\n=== TDIGEST.RANK / TDIGEST.REVRANK ==="

test_assert "TDIGEST.RANK on empty sketch returns -2" {
  r DEL td_rank_empty
  r TDIGEST.CREATE td_rank_empty
  set v [lindex [r TDIGEST.RANK td_rank_empty 1.0] 0]
  if {$v != -2} { error "expected -2, got $v" }
}

test_assert "TDIGEST.RANK below min returns -1" {
  set v [lindex [r TDIGEST.RANK td_quantile 0] 0]
  if {$v != -1} { error "expected -1, got $v" }
}

test_assert "TDIGEST.RANK above max returns total count" {
  set v [lindex [r TDIGEST.RANK td_quantile 5000] 0]
  if {$v != 1000} { error "expected 1000, got $v" }
}

test_assert "TDIGEST.REVRANK above max returns -1" {
  set v [lindex [r TDIGEST.REVRANK td_quantile 5000] 0]
  if {$v != -1} { error "expected -1, got $v" }
}

test_assert "TDIGEST.REVRANK below min returns total count" {
  set v [lindex [r TDIGEST.REVRANK td_quantile 0] 0]
  if {$v != 1000} { error "expected 1000, got $v" }
}

puts "\n=== TDIGEST.BYRANK / TDIGEST.BYREVRANK ==="

test_assert "TDIGEST.BYRANK at rank 0 returns exact min" {
  set v [lindex [r TDIGEST.BYRANK td_quantile 0] 0]
  if {abs($v - 1.0) > 0.0001} { error "expected 1.0, got $v" }
}

test_assert "TDIGEST.BYRANK at n-1 returns exact max" {
  set v [lindex [r TDIGEST.BYRANK td_quantile 999] 0]
  if {abs($v - 1000.0) > 0.0001} { error "expected 1000.0, got $v" }
}

test_assert "TDIGEST.BYRANK at rank>=n returns inf" {
  set v [lindex [r TDIGEST.BYRANK td_quantile 1000] 0]
  if {$v ne "inf"} { error "expected inf, got $v" }
}

test_assert "TDIGEST.BYREVRANK at revrank 0 returns exact max" {
  set v [lindex [r TDIGEST.BYREVRANK td_quantile 0] 0]
  if {abs($v - 1000.0) > 0.0001} { error "expected 1000.0, got $v" }
}

test_assert "TDIGEST.BYREVRANK at revrank>=n returns -inf" {
  set v [lindex [r TDIGEST.BYREVRANK td_quantile 1000] 0]
  if {$v ne "-inf"} { error "expected -inf, got $v" }
}

puts "\n=== TDIGEST.TRIMMED_MEAN ==="

test_assert "TDIGEST.TRIMMED_MEAN over full range approximates mean" {
  set v [r TDIGEST.TRIMMED_MEAN td_quantile 0.0 1.0]
  if {abs($v - 500.5) > 20.0} { error "expected ~500.5, got $v" }
}

test_error "TDIGEST.TRIMMED_MEAN on missing key returns error" {
  r TDIGEST.TRIMMED_MEAN td_missing 0.1 0.9
} {ERR*}

puts "\n=== TDIGEST.RESET ==="

test "TDIGEST.RESET clears the sketch" {
  r TDIGEST.RESET td_basic
} {OK}

test_assert "TDIGEST.MIN after RESET returns nan" {
  set v [r TDIGEST.MIN td_basic]
  if {$v ne "nan"} { error "expected nan after reset, got $v" }
}

puts "\n=== TDIGEST.MERGE ==="

test_assert "TDIGEST.MERGE combines sources into a new destination" {
  r DEL td_merge_a td_merge_b td_merge_dest
  r TDIGEST.CREATE td_merge_a
  r TDIGEST.CREATE td_merge_b
  for {set i 1} {$i <= 500} {incr i} { r TDIGEST.ADD td_merge_a $i }
  for {set i 501} {$i <= 1000} {incr i} { r TDIGEST.ADD td_merge_b $i }
  r TDIGEST.MERGE td_merge_dest 2 td_merge_a td_merge_b
  set lo [r TDIGEST.MIN td_merge_dest]
  set hi [r TDIGEST.MAX td_merge_dest]
  if {abs($lo - 1.0) > 0.0001} { error "expected min=1.0, got $lo" }
  if {abs($hi - 1000.0) > 0.0001} { error "expected max=1000.0, got $hi" }
}

test_assert "TDIGEST.MERGE with OVERRIDE replaces existing destination" {
  r DEL td_merge_c
  r TDIGEST.CREATE td_merge_c
  r TDIGEST.ADD td_merge_c 9999
  r TDIGEST.MERGE td_merge_c 1 td_merge_a OVERRIDE
  set hi [r TDIGEST.MAX td_merge_c]
  if {abs($hi - 500.0) > 0.0001} { error "expected max=500.0 after override merge, got $hi" }
}

test_error "TDIGEST.MERGE on missing source returns error" {
  r TDIGEST.MERGE td_merge_dest2 1 td_merge_missing
} {ERR*}

puts "\n=== TDIGEST.INFO ==="

test_assert "TDIGEST.INFO returns compression and bookkeeping fields" {
  set result [r TDIGEST.INFO td_explicit]
  if {[llength $result] != 18} { error "expected 18 elements, got: $result" }
  array set info $result
  if {$info(Compression) != 200} { error "expected Compression=200, got $info(Compression)" }
}

test_error "TDIGEST.INFO on missing key returns error" {
  r TDIGEST.INFO td_info_missing
} {ERR*}

puts "\n=== DEBUG DIGEST-VALUE / DEBUG RELOAD ==="

test_assert "DEBUG DIGEST-VALUE is stable across DEBUG RELOAD" {
  r DEL td_digest_src
  r TDIGEST.CREATE td_digest_src
  for {set i 1} {$i <= 200} {incr i} { r TDIGEST.ADD td_digest_src $i }
  set before [r DEBUG DIGEST-VALUE td_digest_src]
  r DEBUG RELOAD
  set after [r DEBUG DIGEST-VALUE td_digest_src]
  if {$before ne $after} { error "Digest changed across DEBUG RELOAD: before=$before after=$after" }
}

test_assert "DEBUG DIGEST-VALUE changes after TDIGEST.ADD" {
  set before [r DEBUG DIGEST-VALUE td_digest_src]
  for {set i 0} {$i < 50} {incr i} { r TDIGEST.ADD td_digest_src 99999 }
  set after [r DEBUG DIGEST-VALUE td_digest_src]
  if {$before eq $after} { error "Digest did not change after TDIGEST.ADD" }
}

test_assert "DEBUG RELOAD preserves quantile estimates" {
  r DEL td_reload_test
  r TDIGEST.CREATE td_reload_test
  for {set i 1} {$i <= 500} {incr i} { r TDIGEST.ADD td_reload_test $i }
  set before [lindex [r TDIGEST.QUANTILE td_reload_test 0.5] 0]
  r DEBUG RELOAD
  set after [lindex [r TDIGEST.QUANTILE td_reload_test 0.5] 0]
  if {abs($before - $after) > 0.0001} { error "Quantile changed after DEBUG RELOAD: before=$before after=$after" }
}

test_assert "DUMP / RESTORE round-trips a sketch" {
  r DEL td_dr_src td_dr_dst
  r TDIGEST.CREATE td_dr_src
  r TDIGEST.ADD td_dr_src 1.0 2.0 3.0
  set dump [r DUMP td_dr_src]
  r RESTORE td_dr_dst 0 $dump
  set hi [r TDIGEST.MAX td_dr_dst]
  if {abs($hi - 3.0) > 0.0001} { error "expected max=3.0 after DUMP/RESTORE, got $hi" }
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
file delete -force /tmp/tdigest_tcl_test.rdb

exit $test_failed
