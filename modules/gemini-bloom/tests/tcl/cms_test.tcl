#!/usr/bin/env tclsh
#
# TCL integration tests for the redis_bloom module's Count-Min Sketch (CMS.*)
# commands. Self-contained: starts a redis-server, loads the module, runs
# tests, shuts down.
#
# This file is intentionally independent from bloom_test.tcl/cf_test.tcl
# (separate RESP client + test framework boilerplate) per project convention.
#
# Usage: tclsh cms_test.tcl [path/to/redis_bloom.so]

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
      --logfile /tmp/cms_tcl_test.log \
      --dbfilename cms_tcl_test.rdb \
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
    set f [open /tmp/cms_tcl_test.log r]
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
  file delete -force /tmp/cms_tcl_test.rdb
  file delete -force /tmp/cms_tcl_test.log
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
  puts "Usage: tclsh cms_test.tcl \[path/to/redis_bloom.so\]"
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

puts "=== CMS.INITBYDIM ==="

test "CMS.INITBYDIM creates a new sketch" {
  r CMS.INITBYDIM cms_basic 1000 5
} {OK}

test_error "CMS.INITBYDIM on existing key returns error" {
  r CMS.INITBYDIM cms_basic 1000 5
} {ERR*exists*}

test_error "CMS.INITBYDIM with invalid width (0)" {
  r CMS.INITBYDIM cms_width0 0 5
} {ERR*width*}

test_error "CMS.INITBYDIM with invalid depth (0)" {
  r CMS.INITBYDIM cms_depth0 100 0
} {ERR*depth*}

puts "\n=== CMS.INITBYPROB ==="

test "CMS.INITBYPROB creates a new sketch" {
  r CMS.INITBYPROB cms_prob 0.01 0.01
} {OK}

test_error "CMS.INITBYPROB on existing key returns error" {
  r CMS.INITBYPROB cms_prob 0.01 0.01
} {ERR*exists*}

test_error "CMS.INITBYPROB with invalid error rate" {
  r CMS.INITBYPROB cms_prob_bad -1 0.01
} {ERR*}

puts "\n=== CMS.INCRBY / CMS.QUERY ==="

test_error "CMS.INCRBY on missing key returns error" {
  r CMS.INCRBY cms_missing item 1
} {ERR*}

test "CMS.INCRBY single item" {
  r CMS.INCRBY cms_basic foo 5
} {5}

test "CMS.QUERY returns incremented count" {
  r CMS.QUERY cms_basic foo
} {5}

test "CMS.INCRBY multiple items" {
  r CMS.INCRBY cms_basic bar 3 baz 7
} {3 7}

test "CMS.QUERY multiple items" {
  r CMS.QUERY cms_basic foo bar baz
} {5 3 7}

test "CMS.QUERY on absent item returns 0" {
  r CMS.QUERY cms_basic never_added
} {0}

test "CMS.INCRBY accumulates on repeated calls" {
  r CMS.INCRBY cms_basic foo 10
} {15}

test_error "CMS.QUERY on missing key returns error" {
  r CMS.QUERY cms_missing item
} {ERR*}

test_error "CMS.INCRBY on wrong type key returns error" {
  r DEL cms_wrongtype
  r SET cms_wrongtype somestring
  r CMS.INCRBY cms_wrongtype item 1
} {WRONGTYPE*}

puts "\n=== CMS.MERGE ==="

test_assert "CMS.MERGE combines sources with weights" {
  r DEL cms_merge_dst cms_merge_a cms_merge_b
  r CMS.INITBYDIM cms_merge_dst 500 4
  r CMS.INITBYDIM cms_merge_a 500 4
  r CMS.INITBYDIM cms_merge_b 500 4
  r CMS.INCRBY cms_merge_a shared 10
  r CMS.INCRBY cms_merge_b shared 20
  r CMS.MERGE cms_merge_dst 2 cms_merge_a cms_merge_b WEIGHTS 1 2
  set result [r CMS.QUERY cms_merge_dst shared]
  if {$result < 50} { error "expected merged count >= 50, got $result" }
}

test_error "CMS.MERGE rejects mismatched width/depth" {
  r DEL cms_merge_dst2 cms_merge_small
  r CMS.INITBYDIM cms_merge_dst2 500 4
  r CMS.INITBYDIM cms_merge_small 100 4
  r CMS.MERGE cms_merge_dst2 1 cms_merge_small
} {ERR*}

test_error "CMS.MERGE on missing destination key returns error" {
  r CMS.MERGE cms_merge_missing 1 cms_merge_a
} {ERR*}

puts "\n=== CMS.INFO ==="

test "CMS.INFO returns width/depth/count fields" {
  r DEL cms_info_test
  r CMS.INITBYDIM cms_info_test 200 4
  r CMS.INCRBY cms_info_test x 3
  r CMS.INFO cms_info_test
} {width 200 depth 4 count 3}

test_error "CMS.INFO on missing key returns error" {
  r CMS.INFO cms_info_missing
} {ERR*}

puts "\n=== DEBUG DIGEST-VALUE / DEBUG RELOAD ==="

test_assert "DEBUG DIGEST-VALUE is stable across DEBUG RELOAD" {
  r DEL cms_digest_src
  r CMS.INITBYDIM cms_digest_src 500 4
  for {set i 0} {$i < 50} {incr i} {
    r CMS.INCRBY cms_digest_src "cmsdigest_item_$i" [expr {$i + 1}]
  }
  set before [r DEBUG DIGEST-VALUE cms_digest_src]
  r DEBUG RELOAD
  set after [r DEBUG DIGEST-VALUE cms_digest_src]
  if {$before ne $after} { error "Digest changed across DEBUG RELOAD: before=$before after=$after" }
}

test_assert "DEBUG DIGEST-VALUE changes after CMS.INCRBY" {
  set before [r DEBUG DIGEST-VALUE cms_digest_src]
  r CMS.INCRBY cms_digest_src cmsdigest_new_item 1
  set after [r DEBUG DIGEST-VALUE cms_digest_src]
  if {$before eq $after} { error "Digest did not change after CMS.INCRBY" }
}

test_assert "DEBUG RELOAD preserves counts" {
  r DEL cms_reload_test
  r CMS.INITBYDIM cms_reload_test 1000 5
  set items {}
  for {set i 0} {$i < 200} {incr i} {
    set item "cmsreload_item_$i"
    r CMS.INCRBY cms_reload_test $item [expr {$i + 1}]
    lappend items $item
  }
  set before {}
  foreach item $items { lappend before [r CMS.QUERY cms_reload_test $item] }
  r DEBUG RELOAD
  set after {}
  foreach item $items { lappend after [r CMS.QUERY cms_reload_test $item] }
  if {$before ne $after} { error "Counts changed after DEBUG RELOAD" }
}

test_assert "DUMP / RESTORE round-trips a sketch" {
  r DEL cms_dr_src cms_dr_dst
  r CMS.INITBYDIM cms_dr_src 300 4
  r CMS.INCRBY cms_dr_src dumpitem 42
  set dump [r DUMP cms_dr_src]
  r RESTORE cms_dr_dst 0 $dump
  set q [r CMS.QUERY cms_dr_dst dumpitem]
  if {$q != 42} { error "expected 42 after DUMP/RESTORE, got $q" }
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
file delete -force /tmp/cms_tcl_test.rdb

exit $test_failed
