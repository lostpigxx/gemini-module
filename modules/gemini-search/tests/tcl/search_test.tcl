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
  # info is a flat list: index_name <name> num_docs <n> fields <field_array>
  if {[llength $info] != 6} {
    error "Expected 6 elements, got [llength $info]: $info"
  }
  if {[lindex $info 0] ne "index_name"} {
    error "Expected 'index_name' label, got '[lindex $info 0]'"
  }
  if {[lindex $info 1] ne "myidx"} {
    error "Expected index name 'myidx', got '[lindex $info 1]'"
  }
  if {[lindex $info 2] ne "num_docs"} {
    error "Expected 'num_docs' label, got '[lindex $info 2]'"
  }
  if {[lindex $info 4] ne "fields"} {
    error "Expected 'fields' label, got '[lindex $info 4]'"
  }
  set fields [lindex $info 5]
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
  set fields [lindex $info 5]
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
  set fields [lindex $info 5]
  set f0 [lindex $fields 0]
  set f1 [lindex $fields 1]
  if {[lindex $f0 1] ne "TAG"} {
    error "Expected TAG, got [lindex $f0 1]"
  }
  if {[lindex $f1 1] ne "NUMERIC"} {
    error "Expected NUMERIC, got [lindex $f1 1]"
  }
}

puts "\n=== FT.ADD ==="

# Use myidx which has SCHEMA: name TAG, price NUMERIC
test "FT.ADD basic document" {
  r FT.ADD myidx doc1 FIELDS name shoes price 299
} {OK}

test "FT.ADD second document" {
  r FT.ADD myidx doc2 FIELDS name hat price 49
} {OK}

test "FT.ADD third document same tag" {
  r FT.ADD myidx doc3 FIELDS name shoes price 199
} {OK}

test_error "FT.ADD to nonexistent index" {
  r FT.ADD no_such_idx d1 FIELDS name foo
} {ERR index not found}

test_error "FT.ADD with unknown field" {
  r FT.ADD myidx d1 FIELDS unknown_field val
} {ERR field not in schema}

test_error "FT.ADD missing FIELDS keyword" {
  r FT.ADD myidx d1 name foo price 100
} {ERR syntax error*}

test_error "FT.ADD wrong arity" {
  r FT.ADD myidx doc1
} {ERR*}

test_error "FT.ADD odd field args" {
  r FT.ADD myidx doc1 FIELDS name
} {ERR*}

puts "\n=== FT.SEARCH ==="

test_assert "FT.SEARCH match-all returns all docs" {
  set result [r FT.SEARCH myidx *]
  set total [lindex $result 0]
  if {$total != 3} {
    error "Expected 3 results, got $total"
  }
}

test_assert "FT.SEARCH @name:{shoes} returns 2 docs" {
  set result [r FT.SEARCH myidx "@name:{shoes}"]
  set total [lindex $result 0]
  if {$total != 2} {
    error "Expected 2 results for shoes, got $total"
  }
  # doc1 and doc3 should be in results
  set ids {}
  for {set i 1} {$i < [llength $result]} {incr i 2} {
    lappend ids [lindex $result $i]
  }
  if {[lsearch $ids "doc1"] < 0} { error "doc1 not in results: $ids" }
  if {[lsearch $ids "doc3"] < 0} { error "doc3 not in results: $ids" }
}

test_assert "FT.SEARCH @name:{hat} returns doc2" {
  set result [r FT.SEARCH myidx "@name:{hat}"]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 result, got $total" }
  set doc_id [lindex $result 1]
  if {$doc_id ne "doc2"} { error "Expected doc2, got $doc_id" }
}

test_assert "FT.SEARCH @name:{nonexistent} returns 0" {
  set result [r FT.SEARCH myidx "@name:{nonexistent}"]
  set total [lindex $result 0]
  if {$total != 0} { error "Expected 0 results, got $total" }
}

test_assert "FT.SEARCH multi-value OR" {
  set result [r FT.SEARCH myidx "@name:{shoes|hat}"]
  set total [lindex $result 0]
  if {$total != 3} { error "Expected 3 results for shoes|hat, got $total" }
}

test_error "FT.SEARCH nonexistent index" {
  r FT.SEARCH no_such_idx *
} {ERR index not found}

test_error "FT.SEARCH invalid syntax" {
  r FT.SEARCH myidx "@bad"
} {ERR*}

test_error "FT.SEARCH wrong arity" {
  r FT.SEARCH myidx
} {ERR*}

puts "\n=== FT.SEARCH returns field values ==="

test_assert "FT.SEARCH result contains field values" {
  set result [r FT.SEARCH myidx "@name:{hat}"]
  # result: total doc_id [field1 val1 field2 val2]
  set fields_list [lindex $result 2]
  # Fields are sorted: name, price
  if {[llength $fields_list] != 4} {
    error "Expected 4 field elements, got [llength $fields_list]: $fields_list"
  }
  set idx_name [lsearch $fields_list "name"]
  if {$idx_name < 0} { error "Field 'name' not found in $fields_list" }
  set name_val [lindex $fields_list [expr {$idx_name + 1}]]
  if {$name_val ne "hat"} { error "Expected name=hat, got $name_val" }
}

puts "\n=== FT.ADD replace ==="

test "FT.ADD replace existing doc" {
  r FT.ADD myidx doc2 FIELDS name boots price 150
} {OK}

test_assert "FT.SEARCH old tag no longer matches after replace" {
  set result [r FT.SEARCH myidx "@name:{hat}"]
  set total [lindex $result 0]
  if {$total != 0} { error "Expected 0 after replacing hat with boots, got $total" }
}

test_assert "FT.SEARCH new tag matches after replace" {
  set result [r FT.SEARCH myidx "@name:{boots}"]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 result for boots, got $total" }
}

puts "\n=== FT.DEL ==="

test "FT.DEL existing doc" {
  r FT.DEL myidx doc1
} {OK}

test_error "FT.DEL already deleted" {
  r FT.DEL myidx doc1
} {ERR document not found}

test_error "FT.DEL nonexistent index" {
  r FT.DEL no_such_idx doc1
} {ERR index not found}

test_error "FT.DEL wrong arity" {
  r FT.DEL myidx
} {ERR*}

test_assert "FT.SEARCH after delete" {
  set result [r FT.SEARCH myidx "@name:{shoes}"]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 after deleting doc1, got $total" }
  set doc_id [lindex $result 1]
  if {$doc_id ne "doc3"} { error "Expected doc3, got $doc_id" }
}

puts "\n=== FT.INFO num_docs ==="

test_assert "FT.INFO shows correct num_docs" {
  set info [r FT.INFO myidx]
  set num_docs_label [lindex $info 2]
  set num_docs [lindex $info 3]
  if {$num_docs_label ne "num_docs"} {
    error "Expected 'num_docs' label, got '$num_docs_label'"
  }
  # doc1 deleted, doc2 replaced, doc3 still there => 2 docs
  if {$num_docs != 2} {
    error "Expected 2 docs, got $num_docs"
  }
}

puts "\n=== NUMERIC range queries ==="

# Create a fresh index for numeric tests
test "FT.CREATE numeric test index" {
  r FT.CREATE numtest SCHEMA category TAG price NUMERIC rating NUMERIC
} {OK}

test "FT.ADD docs with numeric fields" {
  r FT.ADD numtest p1 FIELDS category shoes price 299 rating 4.5
} {OK}

test "FT.ADD more docs" {
  r FT.ADD numtest p2 FIELDS category hat price 49 rating 3.0
  r FT.ADD numtest p3 FIELDS category shoes price 199 rating 4.0
  r FT.ADD numtest p4 FIELDS category boots price 399 rating 4.8
  r FT.ADD numtest p5 FIELDS category sandals price 79 rating 3.5
} {OK}

test_assert {FT.SEARCH @price:[100 300] closed interval} {
  set result [r FT.SEARCH numtest {@price:[100 300]}]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2 (p1=299, p3=199), got $total" }
}

test_assert {FT.SEARCH @price:[-inf 100] unbounded min} {
  set result [r FT.SEARCH numtest {@price:[-inf 100]}]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2 (p2=49, p5=79), got $total" }
}

test_assert {FT.SEARCH @price:[300 +inf] unbounded max} {
  set result [r FT.SEARCH numtest {@price:[300 +inf]}]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 (p4=399), got $total" }
}

test_assert {FT.SEARCH @price:[(49 (399] exclusive boundaries} {
  set result [r FT.SEARCH numtest {@price:[(49 (399]}]
  set total [lindex $result 0]
  # Docs: p2=49(excl), p5=79, p3=199, p1=299, p4=399(excl)
  if {$total != 3} { error "Expected 3 (p5=79, p3=199, p1=299), got $total" }
}

test_assert {FT.SEARCH @rating:[4.0 5.0]} {
  set result [r FT.SEARCH numtest {@rating:[4.0 5.0]}]
  set total [lindex $result 0]
  if {$total != 3} { error "Expected 3 (p1=4.5, p3=4.0, p4=4.8), got $total" }
}

test_assert {FT.SEARCH @price:[-inf +inf] returns all} {
  set result [r FT.SEARCH numtest {@price:[-inf +inf]}]
  set total [lindex $result 0]
  if {$total != 5} { error "Expected 5, got $total" }
}

test_error "FT.SEARCH numeric range on TAG field" {
  r FT.SEARCH numtest {@category:[1 10]}
} {ERR field is not a NUMERIC field}

test_error "FT.SEARCH TAG query on NUMERIC field" {
  r FT.SEARCH numtest "@price:{100}"
} {ERR field is not a TAG field}

puts "\n=== NUMERIC FT.DEL removes from index ==="

test "FT.DEL doc with numeric field" {
  r FT.DEL numtest p1
} {OK}

test_assert {FT.SEARCH @price:[200 400] after delete} {
  set result [r FT.SEARCH numtest {@price:[200 400]}]
  set total [lindex $result 0]
  # p1 (299) deleted, only p4 (399) remains in range
  if {$total != 1} { error "Expected 1 (p4), got $total" }
}

puts "\n=== NUMERIC FT.ADD replace ==="

test "FT.ADD replace updates numeric index" {
  r FT.ADD numtest p2 FIELDS category hat price 999 rating 5.0
} {OK}

test_assert "FT.SEARCH old price range excludes replaced doc" {
  set result [r FT.SEARCH numtest {@price:[0 100]}]
  set total [lindex $result 0]
  # p2 was 49 but now 999, p5=79 still there
  if {$total != 1} { error "Expected 1 (p5=79), got $total" }
}

test_assert "FT.SEARCH new price range includes replaced doc" {
  set result [r FT.SEARCH numtest {@price:[900 1000]}]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 (p2=999), got $total" }
}

puts "\n=== VECTOR KNN queries ==="

# Helper: create a float32 blob from a list of numbers
proc float_blob {values} {
  return [binary format f* $values]
}

test "FT.CREATE with VECTOR FLAT field" {
  r FT.CREATE vecidx SCHEMA label TAG embedding VECTOR FLAT DIM 3 DISTANCE_METRIC L2
} {OK}

test_assert "FT.INFO shows VECTOR field params" {
  set info [r FT.INFO vecidx]
  set fields [lindex $info 5]
  # Second field is the VECTOR field
  set vf [lindex $fields 1]
  # Should have 8 elements: name VECTOR algorithm FLAT dim 3 distance_metric L2
  if {[llength $vf] != 8} {
    error "Expected 8 elements for VECTOR field info, got [llength $vf]: $vf"
  }
  if {[lindex $vf 0] ne "embedding"} { error "Expected field name 'embedding'" }
  if {[lindex $vf 1] ne "VECTOR"} { error "Expected type VECTOR" }
  if {[lindex $vf 3] ne "FLAT"} { error "Expected algorithm FLAT, got [lindex $vf 3]" }
  if {[lindex $vf 5] != 3} { error "Expected dim 3, got [lindex $vf 5]" }
  if {[lindex $vf 7] ne "L2"} { error "Expected distance_metric L2, got [lindex $vf 7]" }
}

test "FT.ADD doc with vector" {
  set blob [float_blob {1.0 0.0 0.0}]
  r FT.ADD vecidx v1 FIELDS label x_axis embedding $blob
} {OK}

test "FT.ADD more vector docs" {
  set blob2 [float_blob {0.0 1.0 0.0}]
  r FT.ADD vecidx v2 FIELDS label y_axis embedding $blob2
  set blob3 [float_blob {0.0 0.0 1.0}]
  r FT.ADD vecidx v3 FIELDS label z_axis embedding $blob3
  set blob4 [float_blob {1.0 1.0 0.0}]
  r FT.ADD vecidx v4 FIELDS label diagonal embedding $blob4
} {OK}

test_assert "FT.SEARCH KNN returns closest vectors" {
  set query [float_blob {1.0 0.0 0.0}]
  set result [r FT.SEARCH vecidx {*=>[KNN 2 @embedding $blob]} PARAMS 2 blob $query]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2 KNN results, got $total" }
  # First result should be v1 (exact match, distance 0)
  set first_id [lindex $result 1]
  if {$first_id ne "v1"} { error "Expected v1 as nearest, got $first_id" }
  # Check __vec_score is present in fields
  set first_fields [lindex $result 2]
  set score_idx [lsearch $first_fields "__vec_score"]
  if {$score_idx < 0} { error "__vec_score not found in result fields: $first_fields" }
  set score_val [lindex $first_fields [expr {$score_idx + 1}]]
  # Score should be 0 for exact match
  if {$score_val > 0.001} { error "Expected score ~0 for exact match, got $score_val" }
}

test_assert "FT.SEARCH KNN top-1" {
  set query [float_blob {0.0 0.9 0.0}]
  set result [r FT.SEARCH vecidx {*=>[KNN 1 @embedding $blob]} PARAMS 2 blob $query]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 result, got $total" }
  set first_id [lindex $result 1]
  if {$first_id ne "v2"} { error "Expected v2 (y_axis) as nearest, got $first_id" }
}

test_error "FT.SEARCH KNN wrong dimension" {
  set bad_query [float_blob {1.0 0.0}]
  r FT.SEARCH vecidx {*=>[KNN 2 @embedding $blob]} PARAMS 2 blob $bad_query
} {ERR query vector dimension mismatch}

test_error "FT.SEARCH KNN on TAG field" {
  set query [float_blob {1.0 0.0 0.0}]
  r FT.SEARCH vecidx {*=>[KNN 2 @label $blob]} PARAMS 2 blob $query
} {ERR KNN field is not a VECTOR field}

test_error "FT.SEARCH KNN missing PARAMS" {
  r FT.SEARCH vecidx {*=>[KNN 2 @embedding $blob]}
} {ERR KNN param not found in PARAMS}

test "FT.DEL removes from vector index" {
  r FT.DEL vecidx v1
} {OK}

test_assert "FT.SEARCH KNN after delete" {
  set query [float_blob {1.0 0.0 0.0}]
  set result [r FT.SEARCH vecidx {*=>[KNN 1 @embedding $blob]} PARAMS 2 blob $query]
  set first_id [lindex $result 1]
  # v1 deleted, so v4 (diagonal 1,1,0) should be nearest to (1,0,0)
  if {$first_id ne "v4"} { error "Expected v4 after v1 deleted, got $first_id" }
}

test_error "FT.ADD vector wrong dimension" {
  set bad_blob [float_blob {1.0 0.0}]
  r FT.ADD vecidx vbad FIELDS label bad embedding $bad_blob
} {ERR vector dimension mismatch}

puts "\n=== VECTOR with COSINE metric ==="

test "FT.CREATE cosine index" {
  r FT.CREATE cosidx SCHEMA vec VECTOR FLAT DIM 2 DISTANCE_METRIC COSINE
} {OK}

test "FT.ADD to cosine index" {
  r FT.ADD cosidx d1 FIELDS vec [float_blob {1.0 0.0}]
  r FT.ADD cosidx d2 FIELDS vec [float_blob {0.0 1.0}]
  r FT.ADD cosidx d3 FIELDS vec [float_blob {0.707 0.707}]
} {OK}

test_assert "FT.SEARCH cosine KNN" {
  set query [float_blob {1.0 0.0}]
  set result [r FT.SEARCH cosidx {*=>[KNN 1 @vec $q]} PARAMS 2 q $query]
  set first_id [lindex $result 1]
  if {$first_id ne "d1"} { error "Expected d1 as cosine nearest, got $first_id" }
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
