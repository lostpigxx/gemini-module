#!/usr/bin/env tclsh
#
# TCL integration tests for the redis_search module.
# Self-contained: starts a redis-server, loads the module, runs tests, shuts down.
#
# Usage: tclsh search_test.tcl [path/to/redis_search.so]

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
      --logfile /tmp/search_tcl_test.log \
      --dbfilename search_tcl_test.rdb \
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
  catch {
    set f [open /tmp/search_tcl_test.log r]
    puts "Redis log:\n[read $f]"
    close $f
  }
  error "redis-server failed to start on port $port"
}

proc stop_redis {fd} {
  catch {redis_command $fd SHUTDOWN NOSAVE}
  catch {close $fd}
  after 200
  file delete -force /tmp/search_tcl_test.rdb
  file delete -force /tmp/search_tcl_test.log
}

# ============================================================
# Resolve module path
# ============================================================

if {$argc > 0} {
  set module_path [file normalize [lindex $argv 0]]
} else {
  set script_dir [file dirname [file normalize [info script]]]
  set module_path [file normalize "$script_dir/../../../../build/redis_search.so"]
}

if {![file exists $module_path]} {
  puts "ERROR: Module not found at $module_path"
  puts "Usage: tclsh search_test.tcl \[path/to/redis_search.so\]"
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

puts "=== FT._DEBUG ==="

test "FT._DEBUG returns OK" {
  r FT._DEBUG
} {GeminiSearch OK}

puts "\n=== FT.CREATE ==="

test "FT.CREATE basic index" {
  r FT.CREATE myidx SCHEMA name TAG price NUMERIC
} {OK}

test "FT.CREATE index with single TAG field" {
  r FT.CREATE tagonly SCHEMA status TAG
} {OK}

test "FT.CREATE index with single NUMERIC field" {
  r FT.CREATE numonly SCHEMA score NUMERIC
} {OK}

test "FT.CREATE index with many fields" {
  r FT.CREATE wide SCHEMA f1 TAG f2 NUMERIC f3 TAG f4 NUMERIC f5 TAG
} {OK}

test_error "FT.CREATE duplicate index name" {
  r FT.CREATE myidx SCHEMA a TAG
} {ERR index already exists}

test_error "FT.CREATE missing SCHEMA keyword (short)" {
  r FT.CREATE bad_idx name TAG
} {ERR wrong number of arguments*}

test_error "FT.CREATE missing SCHEMA keyword (long)" {
  r FT.CREATE bad_idx NOTSCHEMA name TAG price NUMERIC
} {ERR syntax error*}

test_error "FT.CREATE no fields after SCHEMA" {
  r FT.CREATE bad_idx SCHEMA
} {ERR*}

test_error "FT.CREATE odd number of field args" {
  r FT.CREATE bad_idx SCHEMA name
} {ERR*}

test_error "FT.CREATE unknown field type" {
  r FT.CREATE bad_idx SCHEMA name TEXT
} {ERR unknown field type*}

test_error "FT.CREATE duplicate field name" {
  r FT.CREATE bad_idx SCHEMA name TAG name NUMERIC
} {ERR duplicate field name*}

test_error "FT.CREATE wrong arity (no args)" {
  r FT.CREATE
} {ERR*}

puts "\n=== FT.INFO ==="

test_assert "FT.INFO returns correct schema" {
  set info [r FT.INFO myidx]
  # info is a flat list: index_name <name> fields <field_array>
  if {[llength $info] != 4} {
    error "Expected 4 elements, got [llength $info]: $info"
  }
  set idx_name_label [lindex $info 0]
  set idx_name [lindex $info 1]
  set fields_label [lindex $info 2]
  set fields [lindex $info 3]

  if {$idx_name_label ne "index_name"} {
    error "Expected 'index_name' label, got '$idx_name_label'"
  }
  if {$idx_name ne "myidx"} {
    error "Expected index name 'myidx', got '$idx_name'"
  }
  if {$fields_label ne "fields"} {
    error "Expected 'fields' label, got '$fields_label'"
  }
  if {[llength $fields] != 2} {
    error "Expected 2 fields, got [llength $fields]"
  }

  set f0 [lindex $fields 0]
  set f1 [lindex $fields 1]
  if {[lindex $f0 0] ne "name" || [lindex $f0 1] ne "TAG"} {
    error "First field mismatch: $f0"
  }
  if {[lindex $f1 0] ne "price" || [lindex $f1 1] ne "NUMERIC"} {
    error "Second field mismatch: $f1"
  }
}

test_error "FT.INFO nonexistent index" {
  r FT.INFO no_such_index
} {ERR index not found}

test_error "FT.INFO wrong arity" {
  r FT.INFO
} {ERR*}

puts "\n=== FT._LIST ==="

test_assert "FT._LIST returns all created indices" {
  set list [r FT._LIST]
  # We created: myidx, tagonly, numonly, wide (sorted)
  if {[llength $list] != 4} {
    error "Expected 4 indices, got [llength $list]: $list"
  }
  # List is sorted
  set expected {myidx numonly tagonly wide}
  if {$list ne $expected} {
    error "Expected '$expected', got '$list'"
  }
}

puts "\n=== FT.DROPINDEX ==="

test "FT.DROPINDEX existing index" {
  r FT.DROPINDEX tagonly
} {OK}

test_error "FT.DROPINDEX already dropped" {
  r FT.DROPINDEX tagonly
} {ERR index not found}

test_error "FT.DROPINDEX nonexistent" {
  r FT.DROPINDEX never_created
} {ERR index not found}

test_error "FT.DROPINDEX wrong arity" {
  r FT.DROPINDEX
} {ERR*}

test_assert "FT._LIST after drop" {
  set list [r FT._LIST]
  if {[llength $list] != 3} {
    error "Expected 3 indices after drop, got [llength $list]: $list"
  }
  if {[lsearch $list "tagonly"] >= 0} {
    error "Dropped index 'tagonly' still in list"
  }
}

test_error "FT.INFO after drop" {
  r FT.INFO tagonly
} {ERR index not found}

puts "\n=== FT.CREATE after drop (re-create) ==="

test "FT.CREATE re-create dropped index" {
  r FT.CREATE tagonly SCHEMA category TAG
} {OK}

test_assert "FT.INFO re-created index has new schema" {
  set info [r FT.INFO tagonly]
  set fields [lindex $info 3]
  if {[llength $fields] != 1} {
    error "Expected 1 field, got [llength $fields]"
  }
  set f [lindex $fields 0]
  if {[lindex $f 0] ne "category"} {
    error "Expected field 'category', got '[lindex $f 0]'"
  }
}

puts "\n=== Case insensitivity ==="

test "FT.CREATE with lowercase type names" {
  r FT.CREATE case_idx SCHEMA a tag b numeric
} {OK}

test_assert "FT.INFO shows normalized type names" {
  set info [r FT.INFO case_idx]
  set fields [lindex $info 3]
  set f0 [lindex $fields 0]
  set f1 [lindex $fields 1]
  if {[lindex $f0 1] ne "TAG"} {
    error "Expected TAG, got [lindex $f0 1]"
  }
  if {[lindex $f1 1] ne "NUMERIC"} {
    error "Expected NUMERIC, got [lindex $f1 1]"
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
file delete -force /tmp/search_tcl_test.rdb

exit $test_failed
