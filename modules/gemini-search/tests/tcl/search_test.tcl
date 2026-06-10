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
  r FT.CREATE bad_idx SCHEMA name BADTYPE
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

puts "\n=== Boolean combination queries ==="

# Use numtest index (category TAG, price NUMERIC, rating NUMERIC)
# Existing docs after earlier tests: p3(shoes,199,4.0), p4(boots,399,4.8),
# p5(sandals,79,3.5), p2(hat,999,5.0)
# Re-add fresh docs for boolean tests
test "FT.CREATE boolean test index" {
  r FT.CREATE boolidx SCHEMA name TAG color TAG price NUMERIC
} {OK}

test "FT.ADD boolean test docs" {
  r FT.ADD boolidx b1 FIELDS name shoes color red price 100
  r FT.ADD boolidx b2 FIELDS name hat color blue price 50
  r FT.ADD boolidx b3 FIELDS name shoes color blue price 200
  r FT.ADD boolidx b4 FIELDS name boots color red price 300
  r FT.ADD boolidx b5 FIELDS name sandals color green price 80
} {OK}

test_assert "AND: @name:{shoes} @color:{red}" {
  set result [r FT.SEARCH boolidx "@name:{shoes} @color:{red}"]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1, got $total" }
  set id [lindex $result 1]
  if {$id ne "b1"} { error "Expected b1, got $id" }
}

test_assert "AND: tag + numeric range" {
  set result [r FT.SEARCH boolidx {@name:{shoes} @price:[100 200]}]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2 (b1,b3), got $total" }
}

test_assert "OR: @name:{shoes} | @name:{boots}" {
  set result [r FT.SEARCH boolidx {@name:{shoes} | @name:{boots}}]
  set total [lindex $result 0]
  if {$total != 3} { error "Expected 3 (b1,b3,b4), got $total" }
}

test_assert "NOT: -@name:{shoes}" {
  set result [r FT.SEARCH boolidx {-@name:{shoes}}]
  set total [lindex $result 0]
  if {$total != 3} { error "Expected 3 (b2,b4,b5), got $total" }
}

test_assert {Grouped: (@name:{shoes} | @name:{hat}) @price:[0 100]} {
  set result [r FT.SEARCH boolidx {(@name:{shoes} | @name:{hat}) @price:[0 100]}]
  set total [lindex $result 0]
  # shoes@100=b1, hat@50=b2 match OR; both have price<=100
  if {$total != 2} { error "Expected 2 (b1,b2), got $total" }
}

test_assert {NOT with AND: -@color:{red} @price:[0 150]} {
  set result [r FT.SEARCH boolidx {-@color:{red} @price:[0 150]}]
  set total [lindex $result 0]
  # non-red: b2(blue,50), b3(blue,200), b5(green,80)
  # price<=150: b2(50), b5(80)
  if {$total != 2} { error "Expected 2 (b2,b5), got $total" }
}

test_assert {Complex: (@color:{red} | @color:{blue}) @price:[100 +inf]} {
  set result [r FT.SEARCH boolidx {(@color:{red} | @color:{blue}) @price:[100 +inf]}]
  set total [lindex $result 0]
  # red|blue: b1(red,100), b2(blue,50), b3(blue,200), b4(red,300)
  # price>=100: b1(100), b3(200), b4(300)
  if {$total != 3} { error "Expected 3 (b1,b3,b4), got $total" }
}

test_assert "OR precedence: @name:{shoes} @color:{red} | @name:{hat}" {
  set result [r FT.SEARCH boolidx {@name:{shoes} @color:{red} | @name:{hat}}]
  set total [lindex $result 0]
  # AND(shoes,red)=b1 OR hat=b2 => 2
  if {$total != 2} { error "Expected 2 (b1,b2), got $total" }
}

puts "\n=== RETURN / SORTBY / LIMIT ==="

# Use boolidx from boolean tests: b1(shoes,red,100), b2(hat,blue,50),
# b3(shoes,blue,200), b4(boots,red,300), b5(sandals,green,80)

test_assert "RETURN: only specified fields" {
  set result [r FT.SEARCH boolidx * RETURN 1 name]
  set total [lindex $result 0]
  if {$total != 5} { error "Expected 5 docs, got $total" }
  # Check first doc's fields — should only have "name" field
  set fields [lindex $result 2]
  if {[llength $fields] != 2} {
    error "Expected 2 field elements (name + value), got [llength $fields]: $fields"
  }
  if {[lindex $fields 0] ne "name"} {
    error "Expected field 'name', got [lindex $fields 0]"
  }
}

test_assert "RETURN 2 fields" {
  set result [r FT.SEARCH boolidx * RETURN 2 name price]
  set fields [lindex $result 2]
  if {[llength $fields] != 4} {
    error "Expected 4 field elements, got [llength $fields]"
  }
}

test_assert "RETURN 0: no fields, just doc IDs" {
  set result [r FT.SEARCH boolidx * RETURN 0]
  set total [lindex $result 0]
  if {$total != 5} { error "Expected 5, got $total" }
  # Each doc's field array should be empty (0 elements)
  set fields [lindex $result 2]
  if {[llength $fields] != 0} {
    error "Expected empty fields array, got [llength $fields]: $fields"
  }
}

test_assert "SORTBY price ASC" {
  set result [r FT.SEARCH boolidx * SORTBY price ASC]
  set total [lindex $result 0]
  if {$total != 5} { error "Expected 5, got $total" }
  # Order by price: b2(50), b5(80), b1(100), b3(200), b4(300)
  set ids {}
  for {set i 1} {$i < [llength $result]} {incr i 2} {
    lappend ids [lindex $result $i]
  }
  if {$ids ne {b2 b5 b1 b3 b4}} {
    error "Expected {b2 b5 b1 b3 b4}, got {$ids}"
  }
}

test_assert "SORTBY price DESC" {
  set result [r FT.SEARCH boolidx * SORTBY price DESC]
  set ids {}
  for {set i 1} {$i < [llength $result]} {incr i 2} {
    lappend ids [lindex $result $i]
  }
  if {$ids ne {b4 b3 b1 b5 b2}} {
    error "Expected {b4 b3 b1 b5 b2}, got {$ids}"
  }
}

test_assert "SORTBY name ASC (lexicographic)" {
  set result [r FT.SEARCH boolidx * SORTBY name ASC]
  set ids {}
  for {set i 1} {$i < [llength $result]} {incr i 2} {
    lappend ids [lindex $result $i]
  }
  # boots(b4), hat(b2), sandals(b5), shoes(b1), shoes(b3)
  set first [lindex $ids 0]
  if {$first ne "b4"} { error "Expected b4 (boots) first, got $first" }
  set second [lindex $ids 1]
  if {$second ne "b2"} { error "Expected b2 (hat) second, got $second" }
}

test_assert "LIMIT 0 2: first 2 results" {
  set result [r FT.SEARCH boolidx * SORTBY price ASC LIMIT 0 2]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2, got $total" }
  set ids {}
  for {set i 1} {$i < [llength $result]} {incr i 2} {
    lappend ids [lindex $result $i]
  }
  if {$ids ne {b2 b5}} { error "Expected {b2 b5}, got {$ids}" }
}

test_assert "LIMIT 2 2: skip 2, take 2" {
  set result [r FT.SEARCH boolidx * SORTBY price ASC LIMIT 2 2]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2, got $total" }
  set ids {}
  for {set i 1} {$i < [llength $result]} {incr i 2} {
    lappend ids [lindex $result $i]
  }
  if {$ids ne {b1 b3}} { error "Expected {b1 b3}, got {$ids}" }
}

test_assert "LIMIT beyond results" {
  set result [r FT.SEARCH boolidx * SORTBY price ASC LIMIT 4 10]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1, got $total" }
}

test_assert "LIMIT offset equals total" {
  set result [r FT.SEARCH boolidx * LIMIT 5 10]
  set total [lindex $result 0]
  if {$total != 0} { error "Expected 0, got $total" }
}

test_assert "Combined: RETURN + SORTBY + LIMIT" {
  set result [r FT.SEARCH boolidx * RETURN 1 name SORTBY price DESC LIMIT 0 3]
  set total [lindex $result 0]
  if {$total != 3} { error "Expected 3, got $total" }
  # Top 3 by price DESC: b4(300), b3(200), b1(100)
  set first_id [lindex $result 1]
  if {$first_id ne "b4"} { error "Expected b4 first, got $first_id" }
  set first_fields [lindex $result 2]
  if {[llength $first_fields] != 2} {
    error "Expected 2 field elements with RETURN 1, got [llength $first_fields]"
  }
}

test_assert "RETURN with missing field returns empty string" {
  set result [r FT.SEARCH boolidx {@name:{shoes}} RETURN 1 nonexistent]
  set fields [lindex $result 2]
  set val [lindex $fields 1]
  if {$val ne ""} { error "Expected empty string for missing field, got '$val'" }
}

test_error "SORTBY unknown field" {
  r FT.SEARCH boolidx * SORTBY unknown_field ASC
} {ERR SORTBY field not in schema}

test_error "LIMIT negative offset" {
  r FT.SEARCH boolidx * LIMIT -1 10
} {ERR LIMIT*}

test_error "LIMIT negative count" {
  r FT.SEARCH boolidx * LIMIT 0 -1
} {ERR LIMIT*}

puts "\n=== RDB persistence ==="

test "FT.CREATE persistence test index" {
  r FT.CREATE persistidx SCHEMA tag TAG num NUMERIC
} {OK}

test "FT.ADD docs for persistence" {
  r FT.ADD persistidx pd1 FIELDS tag hello num 42
  r FT.ADD persistidx pd2 FIELDS tag world num 99
} {OK}

test_assert "Data survives BGSAVE + restart" {
  r BGSAVE
  after 2000

  global redis_fd port module_path
  catch {redis_command $redis_fd SHUTDOWN SAVE}
  catch {close $redis_fd}
  after 1000

  start_redis $module_path $port
  set redis_fd [redis_connect localhost $port]

  set info [r FT.INFO persistidx]
  set num_docs_label [lindex $info 2]
  set num_docs [lindex $info 3]
  if {$num_docs_label ne "num_docs"} {
    error "Expected 'num_docs' label, got '$num_docs_label'"
  }
  if {$num_docs != 2} {
    error "Expected 2 docs after restart, got $num_docs"
  }
}

test_assert "FT.SEARCH works after restart" {
  set result [r FT.SEARCH persistidx {@tag:{hello}}]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 result for tag hello, got $total" }
  set doc_id [lindex $result 1]
  if {$doc_id ne "pd1"} { error "Expected pd1, got $doc_id" }
}

test_assert "Numeric search works after restart" {
  set result [r FT.SEARCH persistidx {@num:[50 100]}]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 result for num 50-100, got $total" }
}

test_assert "FT._LIST shows index after restart" {
  set list [r FT._LIST]
  if {[lsearch $list "persistidx"] < 0} {
    error "persistidx not in FT._LIST after restart: $list"
  }
}

puts "\n=== Phase 8: Auto-Indexing (ON HASH) ==="

# Clean up all prior indices first
catch { r FT.DROPINDEX myidx }
catch { r FT.DROPINDEX numonly }
catch { r FT.DROPINDEX wide }
catch { r FT.DROPINDEX tagonly }
catch { r FT.DROPINDEX case_idx }
catch { r FT.DROPINDEX numtest }
catch { r FT.DROPINDEX vecidx }
catch { r FT.DROPINDEX cosidx }
catch { r FT.DROPINDEX boolidx }
catch { r FT.DROPINDEX persistidx }
r FLUSHDB

puts "\n--- FT.CREATE ON HASH ---"

test "FT.CREATE with ON HASH PREFIX" {
  r FT.CREATE autoidx ON HASH PREFIX 1 product: SCHEMA name TAG price NUMERIC
} {OK}

test_assert "FT.INFO shows index_definition for ON HASH index" {
  set info [r FT.INFO autoidx]
  set found 0
  for {set i 0} {$i < [llength $info]} {incr i} {
    if {[lindex $info $i] eq "index_definition"} {
      set found 1
      set def [lindex $info [expr {$i + 1}]]
      if {[lindex $def 0] ne "key_type"} {
        error "Expected 'key_type', got '[lindex $def 0]'"
      }
      if {[lindex $def 1] ne "HASH"} {
        error "Expected 'HASH', got '[lindex $def 1]'"
      }
      if {[lindex $def 2] ne "product:"} {
        error "Expected prefix 'product:', got '[lindex $def 2]'"
      }
      break
    }
  }
  if {!$found} { error "index_definition not found in FT.INFO output" }
}

puts "\n--- HSET triggers auto-indexing ---"

test "HSET triggers indexing" {
  r HSET product:1 name shoes price 299
} {2}

test_assert "FT.SEARCH finds auto-indexed doc" {
  set result [r FT.SEARCH autoidx {@name:{shoes}}]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1, got $total" }
  set doc_id [lindex $result 1]
  if {$doc_id ne "product:1"} { error "Expected product:1, got $doc_id" }
}

test_assert "FT.INFO shows num_docs after auto-index" {
  set info [r FT.INFO autoidx]
  for {set i 0} {$i < [llength $info]} {incr i} {
    if {[lindex $info $i] eq "num_docs"} {
      set num [lindex $info [expr {$i + 1}]]
      if {$num != 1} { error "Expected 1 doc, got $num" }
      break
    }
  }
}

test "HSET second doc" {
  r HSET product:2 name hat price 49
} {2}

test_assert "FT.SEARCH match-all returns both auto-indexed docs" {
  set result [r FT.SEARCH autoidx *]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2, got $total" }
}

test_assert "FT.SEARCH numeric range on auto-indexed docs" {
  set result [r FT.SEARCH autoidx {@price:[100 500]}]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 (product:1=299), got $total" }
  set doc_id [lindex $result 1]
  if {$doc_id ne "product:1"} { error "Expected product:1, got $doc_id" }
}

puts "\n--- HSET update triggers re-indexing ---"

test "HSET update existing key" {
  r HSET product:1 name boots price 399
} {0}

test_assert "FT.SEARCH reflects updated value" {
  set result [r FT.SEARCH autoidx {@name:{boots}}]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1, got $total" }
}

test_assert "Old tag value no longer matches" {
  set result [r FT.SEARCH autoidx {@name:{shoes}}]
  set total [lindex $result 0]
  if {$total != 0} { error "Expected 0, got $total" }
}

puts "\n--- DEL removes from index ---"

test "DEL removes hash and triggers de-indexing" {
  r DEL product:1
} {1}

test_assert "FT.SEARCH no longer finds deleted key" {
  set result [r FT.SEARCH autoidx {@name:{boots}}]
  set total [lindex $result 0]
  if {$total != 0} { error "Expected 0 after DEL, got $total" }
}

test_assert "Other docs remain after DEL" {
  set result [r FT.SEARCH autoidx *]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 remaining, got $total" }
}

puts "\n--- Prefix mismatch ---"

test "HSET non-matching prefix does not index" {
  r HSET order:1 name widget price 99
} {2}

test_assert "Non-matching key not in index" {
  set result [r FT.SEARCH autoidx *]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 (only product:2), got $total" }
}

puts "\n--- Multiple prefixes ---"

test "FT.CREATE with multiple prefixes" {
  r FT.CREATE multiprefix ON HASH PREFIX 2 item: thing: SCHEMA label TAG
} {OK}

test "HSET with first prefix" {
  r HSET item:1 label alpha
} {1}

test "HSET with second prefix" {
  r HSET thing:1 label beta
} {1}

test_assert "Both prefixes indexed" {
  set result [r FT.SEARCH multiprefix *]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2, got $total" }
}

test_assert "Search by tag across prefixes" {
  set result [r FT.SEARCH multiprefix {@label:{alpha}}]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 for alpha, got $total" }
  set doc_id [lindex $result 1]
  if {$doc_id ne "item:1"} { error "Expected item:1, got $doc_id" }
}

puts "\n--- Existing keys scanned on FT.CREATE ---"

# Pre-populate some hashes, then create an index that matches them
r HSET widget:1 color red size 10
r HSET widget:2 color blue size 20
r HSET widget:3 color red size 30
r HSET gadget:1 color green size 5

test "FT.CREATE scans existing keys" {
  r FT.CREATE scanidx ON HASH PREFIX 1 widget: SCHEMA color TAG size NUMERIC
} {OK}

test_assert "Existing matching keys are auto-indexed on create" {
  set result [r FT.SEARCH scanidx *]
  set total [lindex $result 0]
  if {$total != 3} { error "Expected 3 existing widgets, got $total" }
}

test_assert "Tag search on scanned keys" {
  set result [r FT.SEARCH scanidx {@color:{red}}]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2 red widgets, got $total" }
}

test_assert "Numeric range on scanned keys" {
  set result [r FT.SEARCH scanidx {@size:[15 35]}]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2 (widget:2=20, widget:3=30), got $total" }
}

test_assert "Non-matching prefix not included in scan" {
  set result [r FT.SEARCH scanidx {@color:{green}}]
  set total [lindex $result 0]
  if {$total != 0} { error "Expected 0 (gadget:1 doesn't match prefix), got $total" }
}

puts "\n--- FT.ADD still works alongside ON HASH ---"

test "FT.ADD still works on ON HASH index" {
  r FT.ADD autoidx manual:1 FIELDS name manual_item price 999
} {OK}

test_assert "FT.ADD doc found by search" {
  set result [r FT.SEARCH autoidx {@name:{manual_item}}]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1, got $total" }
}

puts "\n--- ON HASH with boolean queries ---"

r HSET product:10 name jacket price 500
r HSET product:11 name jacket price 100
r HSET product:12 name gloves price 25

test_assert "Boolean AND on auto-indexed docs" {
  set result [r FT.SEARCH autoidx {@name:{jacket} @price:[200 600]}]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 (product:10=500), got $total" }
}

test_assert "Boolean OR on auto-indexed docs" {
  set result [r FT.SEARCH autoidx {@name:{jacket} | @name:{gloves}}]
  set total [lindex $result 0]
  if {$total != 3} { error "Expected 3 (2 jackets + 1 gloves), got $total" }
}

puts "\n--- ON HASH with SORTBY/LIMIT ---"

test_assert "SORTBY on auto-indexed docs" {
  set result [r FT.SEARCH autoidx {@name:{jacket}} SORTBY price ASC]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2 jackets, got $total" }
  set first_id [lindex $result 1]
  if {$first_id ne "product:11"} { error "Expected product:11 first (price 100), got $first_id" }
}

test_assert "LIMIT on auto-indexed docs" {
  set result [r FT.SEARCH autoidx * SORTBY price ASC LIMIT 0 2]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2, got $total" }
}

puts "\n--- ON HASH persistence ---"

test_assert "ON HASH index survives BGSAVE + restart" {
  r BGSAVE
  after 2000

  global redis_fd port module_path
  catch {redis_command $redis_fd SHUTDOWN SAVE}
  catch {close $redis_fd}
  after 1000

  start_redis $module_path $port
  set redis_fd [redis_connect localhost $port]

  set info [r FT.INFO autoidx]
  set found_def 0
  for {set i 0} {$i < [llength $info]} {incr i} {
    if {[lindex $info $i] eq "index_definition"} {
      set found_def 1
      break
    }
  }
  if {!$found_def} { error "index_definition lost after restart" }

  # Verify docs survived
  set result [r FT.SEARCH autoidx *]
  set total [lindex $result 0]
  if {$total < 4} { error "Expected at least 4 docs after restart, got $total" }
}

test_assert "HSET still triggers indexing after restart" {
  r HSET product:99 name reloaded price 1
  set result [r FT.SEARCH autoidx {@name:{reloaded}}]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 for reloaded, got $total" }
}

puts "\n--- Error cases ---"

test_error "FT.CREATE ON without HASH" {
  r FT.CREATE bad ON STRING PREFIX 1 x: SCHEMA f TAG
} {ERR only HASH is supported for ON}

test_error "FT.CREATE ON HASH without PREFIX" {
  r FT.CREATE bad ON HASH SCHEMA f TAG
} {ERR expected PREFIX after ON HASH}

test_error "FT.CREATE ON HASH PREFIX without count" {
  r FT.CREATE bad ON HASH PREFIX SCHEMA f TAG
} {ERR PREFIX count must be a positive integer}

test_error "FT.CREATE ON HASH PREFIX 0" {
  r FT.CREATE bad ON HASH PREFIX 0 SCHEMA f TAG
} {ERR PREFIX count must be a positive integer}

test_error "FT.CREATE ON HASH PREFIX count exceeds args" {
  r FT.CREATE bad ON HASH PREFIX 3 x: y:
} {ERR not enough PREFIX values}

puts "\n=== Phase 9: Full-Text Search (TEXT + BM25) ==="

puts "\n--- FT.CREATE with TEXT fields ---"

test "FT.CREATE with TEXT field" {
  r FT.CREATE textidx SCHEMA title TEXT body TEXT category TAG
} {OK}

test_assert "FT.INFO shows TEXT field type" {
  set info [r FT.INFO textidx]
  set fields [lindex $info 5]
  set f0 [lindex $fields 0]
  if {[lindex $f0 0] ne "title"} { error "Expected field 'title'" }
  if {[lindex $f0 1] ne "TEXT"} { error "Expected type TEXT, got [lindex $f0 1]" }
  set f1 [lindex $fields 1]
  if {[lindex $f1 0] ne "body"} { error "Expected field 'body'" }
  if {[lindex $f1 1] ne "TEXT"} { error "Expected type TEXT, got [lindex $f1 1]" }
}

puts "\n--- FT.ADD with TEXT fields ---"

test "FT.ADD doc with text" {
  r FT.ADD textidx doc1 FIELDS title "Quick Brown Fox" body "The quick brown fox jumps over the lazy dog" category animals
} {OK}

test "FT.ADD more text docs" {
  r FT.ADD textidx doc2 FIELDS title "Fast Red Car" body "A fast red car drives quickly down the highway" category vehicles
  r FT.ADD textidx doc3 FIELDS title "Brown Bear" body "The brown bear sleeps quietly in the dark forest" category animals
  r FT.ADD textidx doc4 FIELDS title "Quick Guide to Redis" body "Redis is a quick in-memory database used for caching" category tech
  r FT.ADD textidx doc5 FIELDS title "Fox Hunting" body "Fox hunting is a controversial outdoor activity" category sports
} {OK}

puts "\n--- @field:term single term search ---"

test_assert "FT.SEARCH @title:quick finds docs with quick in title" {
  set result [r FT.SEARCH textidx "@title:quick"]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2 (doc1, doc4), got $total" }
}

test_assert "FT.SEARCH @body:fox finds docs with fox in body" {
  set result [r FT.SEARCH textidx "@body:fox"]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2 (doc1, doc5), got $total" }
}

test_assert "FT.SEARCH @title:nonexistent returns 0" {
  set result [r FT.SEARCH textidx "@title:nonexistent"]
  set total [lindex $result 0]
  if {$total != 0} { error "Expected 0, got $total" }
}

puts "\n--- @field:(multi term) search ---"

test_assert "FT.SEARCH @title:(quick brown) matches union of terms" {
  set result [r FT.SEARCH textidx {@title:(quick brown)}]
  set total [lindex $result 0]
  # doc1 has "Quick Brown Fox" (both terms), doc3 has "Brown Bear", doc4 has "Quick Guide..."
  if {$total != 3} { error "Expected 3, got $total" }
}

test_assert "FT.SEARCH @body:(bear forest) matches docs with either term" {
  set result [r FT.SEARCH textidx {@body:(bear forest)}]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 (doc3 has both), got $total" }
}

puts "\n--- Bare term search across all TEXT fields ---"

test_assert "Bare term 'brown' searches all TEXT fields" {
  set result [r FT.SEARCH textidx "brown"]
  set total [lindex $result 0]
  # doc1 title+body has brown, doc3 title+body has brown
  if {$total != 2} { error "Expected 2, got $total" }
}

test_assert "Bare term 'redis' searches all TEXT fields" {
  set result [r FT.SEARCH textidx "redis"]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 (doc4), got $total" }
}

puts "\n--- Case insensitivity ---"

test_assert "FT.SEARCH text is case insensitive" {
  set result [r FT.SEARCH textidx "@title:QUICK"]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2, got $total" }
}

puts "\n--- Stop words filtered ---"

test_assert "Stop words don't match" {
  set result [r FT.SEARCH textidx "@body:the"]
  set total [lindex $result 0]
  if {$total != 0} { error "Expected 0 (the is a stop word), got $total" }
}

puts "\n--- TEXT + TAG combined queries ---"

test_assert "TEXT AND TAG: @body:brown @category:{animals}" {
  set result [r FT.SEARCH textidx {@body:brown @category:{animals}}]
  set total [lindex $result 0]
  # doc1 (brown fox, animals) and doc3 (brown bear, animals)
  if {$total != 2} { error "Expected 2, got $total" }
}

test_assert "TEXT AND TAG: @body:quick @category:{tech}" {
  set result [r FT.SEARCH textidx {@body:quick @category:{tech}}]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 (doc4), got $total" }
}

test_assert "TEXT OR TAG: @title:fox | @category:{tech}" {
  set result [r FT.SEARCH textidx {@title:fox | @category:{tech}}]
  set total [lindex $result 0]
  # doc1 (fox in title), doc4 (tech category), doc5 (fox in title)
  if {$total != 3} { error "Expected 3, got $total" }
}

test_assert "NOT TEXT: -@title:quick" {
  set result [r FT.SEARCH textidx {-@title:quick}]
  set total [lindex $result 0]
  if {$total != 3} { error "Expected 3 (doc2, doc3, doc5), got $total" }
}

puts "\n--- TEXT with RETURN/SORTBY/LIMIT ---"

test_assert "RETURN on TEXT search" {
  set result [r FT.SEARCH textidx "@title:quick" RETURN 1 title]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2, got $total" }
  set fields [lindex $result 2]
  if {[llength $fields] != 2} { error "Expected 2 field elements" }
}

test_assert "SORTBY on TEXT search" {
  set result [r FT.SEARCH textidx "@title:quick" SORTBY title ASC]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2, got $total" }
}

test_assert "LIMIT on TEXT search" {
  set result [r FT.SEARCH textidx * LIMIT 0 2]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2, got $total" }
}

puts "\n--- FT.DEL removes from TEXT index ---"

test "FT.DEL doc with text fields" {
  r FT.DEL textidx doc1
} {OK}

test_assert "FT.SEARCH @title:quick after delete" {
  set result [r FT.SEARCH textidx "@title:quick"]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 (doc4 only), got $total" }
}

test_assert "FT.SEARCH @body:fox after delete" {
  set result [r FT.SEARCH textidx "@body:fox"]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 (doc5 only), got $total" }
}

puts "\n--- FT.ADD replace updates TEXT index ---"

test "FT.ADD replace text doc" {
  r FT.ADD textidx doc2 FIELDS title "Blue Airplane" body "A blue airplane flies high in the sky" category vehicles
} {OK}

test_assert "Old text no longer matches" {
  set result [r FT.SEARCH textidx "@title:red"]
  set total [lindex $result 0]
  if {$total != 0} { error "Expected 0 after replacing red car, got $total" }
}

test_assert "New text matches" {
  set result [r FT.SEARCH textidx "@title:airplane"]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1, got $total" }
}

puts "\n--- ON HASH auto-indexing with TEXT fields ---"

test "FT.CREATE ON HASH with TEXT fields" {
  r FT.CREATE autotextidx ON HASH PREFIX 1 article: SCHEMA headline TEXT content TEXT
} {OK}

test "HSET hash with TEXT fields" {
  r HSET article:1 headline "Breaking News Today" content "Major events happened across the world today"
  r HSET article:2 headline "Sports Update" content "The local team won the championship game today"
  r HSET article:3 headline "Tech News" content "New programming language released for developers"
} {2}

test_assert "FT.SEARCH auto-indexed TEXT docs" {
  set result [r FT.SEARCH autotextidx "@headline:news"]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2 (article:1, article:3), got $total" }
}

test_assert "FT.SEARCH bare term on auto-indexed docs" {
  set result [r FT.SEARCH autotextidx "today"]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2 (article:1, article:2), got $total" }
}

test_assert "HSET update triggers re-indexing for TEXT" {
  r HSET article:1 headline "Old Story" content "Nothing interesting happened"
  set result [r FT.SEARCH autotextidx "@headline:breaking"]
  set total [lindex $result 0]
  if {$total != 0} { error "Expected 0 after update, got $total" }
}

test_assert "DEL removes from auto-indexed TEXT" {
  r DEL article:2
  set result [r FT.SEARCH autotextidx "today"]
  set total [lindex $result 0]
  if {$total != 0} { error "Expected 0 after DEL, got $total" }
}

puts "\n--- TEXT persistence ---"

test "FT.CREATE TEXT persistence test" {
  r FT.CREATE persisttext SCHEMA name TEXT score NUMERIC
} {OK}

test "FT.ADD TEXT persistence docs" {
  r FT.ADD persisttext td1 FIELDS name "hello world program" score 10
  r FT.ADD persisttext td2 FIELDS name "goodbye world" score 20
} {OK}

test_assert "TEXT index survives BGSAVE + restart" {
  r BGSAVE
  after 2000

  global redis_fd port module_path
  catch {redis_command $redis_fd SHUTDOWN SAVE}
  catch {close $redis_fd}
  after 1000

  start_redis $module_path $port
  set redis_fd [redis_connect localhost $port]

  set result [r FT.SEARCH persisttext "@name:hello"]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 after restart, got $total" }
  set doc_id [lindex $result 1]
  if {$doc_id ne "td1"} { error "Expected td1, got $doc_id" }
}

test_assert "TEXT bare term search works after restart" {
  set result [r FT.SEARCH persisttext "world"]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2 after restart, got $total" }
}

puts "\n--- Error cases ---"

test_error "TAG syntax on TEXT field" {
  r FT.SEARCH textidx "@title:{quick}"
} {ERR field is not a TAG field}

test_error "NUMERIC syntax on TEXT field" {
  r FT.SEARCH textidx {@title:[0 100]}
} {ERR field is not a NUMERIC field}

puts "\n=== Phase 10: KNN Pre-Filter + AOF Rewrite ==="

puts "\n--- KNN Pre-Filter ---"

# Create an index with TAG + VECTOR fields for pre-filter tests
test "FT.CREATE for KNN pre-filter tests" {
  r FT.CREATE filteridx SCHEMA category TAG price NUMERIC embedding VECTOR FLAT DIM 3 DISTANCE_METRIC L2
} {OK}

test "FT.ADD docs for KNN pre-filter" {
  # shoes: close to (1,0,0)
  r FT.ADD filteridx f1 FIELDS category shoes price 100 embedding [float_blob {1.0 0.0 0.0}]
  # shoes: close to (0,1,0)
  r FT.ADD filteridx f2 FIELDS category shoes price 200 embedding [float_blob {0.0 1.0 0.0}]
  # hat: close to (1,0,0)
  r FT.ADD filteridx f3 FIELDS category hat price 50 embedding [float_blob {0.9 0.1 0.0}]
  # hat: far from (1,0,0)
  r FT.ADD filteridx f4 FIELDS category hat price 300 embedding [float_blob {0.0 0.0 1.0}]
  # boots: close to (1,0,0)
  r FT.ADD filteridx f5 FIELDS category boots price 150 embedding [float_blob {0.8 0.2 0.0}]
} {OK}

test_assert "KNN with * pre-filter returns all (baseline)" {
  set query [float_blob {1.0 0.0 0.0}]
  set result [r FT.SEARCH filteridx {*=>[KNN 2 @embedding $q]} PARAMS 2 q $query]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2, got $total" }
  # f1 should be nearest (exact match), then f3 (0.9,0.1,0)
  set first [lindex $result 1]
  if {$first ne "f1"} { error "Expected f1 nearest, got $first" }
}

test_assert "KNN pre-filter @category:{shoes}" {
  set query [float_blob {1.0 0.0 0.0}]
  set result [r FT.SEARCH filteridx {@category:{shoes}=>[KNN 2 @embedding $q]} PARAMS 2 q $query]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2 shoes, got $total" }
  # f1 (shoes, 1,0,0) should be nearest, then f2 (shoes, 0,1,0)
  set first [lindex $result 1]
  if {$first ne "f1"} { error "Expected f1 nearest shoe, got $first" }
  set second [lindex $result 3]
  if {$second ne "f2"} { error "Expected f2 second shoe, got $second" }
}

test_assert "KNN pre-filter @category:{hat}" {
  set query [float_blob {1.0 0.0 0.0}]
  set result [r FT.SEARCH filteridx {@category:{hat}=>[KNN 1 @embedding $q]} PARAMS 2 q $query]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1, got $total" }
  set first [lindex $result 1]
  # f3 (hat, 0.9,0.1,0) is closer than f4 (hat, 0,0,1) to (1,0,0)
  if {$first ne "f3"} { error "Expected f3 nearest hat, got $first" }
}

test_assert "KNN pre-filter with numeric range" {
  set query [float_blob {1.0 0.0 0.0}]
  set result [r FT.SEARCH filteridx {@price:[0 150]=>[KNN 3 @embedding $q]} PARAMS 2 q $query]
  set total [lindex $result 0]
  # price <= 150: f1(100), f2(200 excluded), f3(50), f5(150)
  # KNN top 3 from {f1, f3, f5} nearest to (1,0,0)
  if {$total != 3} { error "Expected 3, got $total" }
  set first [lindex $result 1]
  if {$first ne "f1"} { error "Expected f1, got $first" }
}

test_assert "KNN pre-filter with boolean AND" {
  set query [float_blob {1.0 0.0 0.0}]
  set result [r FT.SEARCH filteridx {@category:{shoes} @price:[0 150]=>[KNN 2 @embedding $q]} PARAMS 2 q $query]
  set total [lindex $result 0]
  # shoes AND price<=150: only f1(shoes,100)
  if {$total != 1} { error "Expected 1, got $total" }
  set first [lindex $result 1]
  if {$first ne "f1"} { error "Expected f1, got $first" }
}

test_assert "KNN pre-filter with OR" {
  set query [float_blob {1.0 0.0 0.0}]
  set result [r FT.SEARCH filteridx {(@category:{shoes} | @category:{boots})=>[KNN 2 @embedding $q]} PARAMS 2 q $query]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2, got $total" }
  # shoes|boots: f1, f2, f5. Top 2 by L2 to (1,0,0): f1(0), f5(0.08)
  set first [lindex $result 1]
  if {$first ne "f1"} { error "Expected f1 nearest, got $first" }
}

test_assert "KNN pre-filter empty result" {
  set query [float_blob {1.0 0.0 0.0}]
  set result [r FT.SEARCH filteridx {@category:{nonexistent}=>[KNN 5 @embedding $q]} PARAMS 2 q $query]
  set total [lindex $result 0]
  if {$total != 0} { error "Expected 0, got $total" }
}

puts "\n--- AOF Rewrite ---"

test "FT.CREATE for AOF test" {
  r FT.CREATE aofidx SCHEMA tag TAG num NUMERIC
} {OK}

test "FT.ADD docs for AOF" {
  r FT.ADD aofidx ad1 FIELDS tag hello num 42
  r FT.ADD aofidx ad2 FIELDS tag world num 99
} {OK}

test_assert "Data survives BGREWRITEAOF + restart (appendonly)" {
  # Enable appendonly — this triggers an initial AOF write
  r CONFIG SET appendonly yes
  after 2000
  # Now trigger an explicit rewrite to exercise our AofRewriteSearch callback
  catch { r BGREWRITEAOF }
  after 2000

  global redis_fd port module_path
  catch {redis_command $redis_fd SHUTDOWN SAVE}
  catch {close $redis_fd}
  after 1000

  start_redis $module_path $port
  set redis_fd [redis_connect localhost $port]

  set info [r FT.INFO aofidx]
  set num_docs_label [lindex $info 2]
  set num_docs [lindex $info 3]
  if {$num_docs != 2} {
    error "Expected 2 docs after AOF restart, got $num_docs"
  }

  set result [r FT.SEARCH aofidx {@tag:{hello}}]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1 for hello after AOF restart, got $total" }
}

puts "\n=== Phase 11: VECTOR HNSW ==="

puts "\n--- FT.CREATE with HNSW ---"

test "FT.CREATE with HNSW VECTOR field" {
  r FT.CREATE hnswi SCHEMA label TAG embedding VECTOR HNSW DIM 3 DISTANCE_METRIC L2 M 8 EF_CONSTRUCTION 100
} {OK}

test_assert "FT.INFO shows HNSW params" {
  set info [r FT.INFO hnswi]
  set fields [lindex $info 5]
  set vf [lindex $fields 1]
  if {[llength $vf] != 12} {
    error "Expected 12 elements for HNSW field, got [llength $vf]: $vf"
  }
  if {[lindex $vf 0] ne "embedding"} { error "Expected embedding" }
  if {[lindex $vf 1] ne "VECTOR"} { error "Expected VECTOR" }
  if {[lindex $vf 3] ne "HNSW"} { error "Expected HNSW, got [lindex $vf 3]" }
  if {[lindex $vf 5] != 3} { error "Expected dim 3, got [lindex $vf 5]" }
  if {[lindex $vf 7] ne "L2"} { error "Expected L2, got [lindex $vf 7]" }
  if {[lindex $vf 9] != 8} { error "Expected M=8, got [lindex $vf 9]" }
  if {[lindex $vf 11] != 100} { error "Expected ef_construction=100, got [lindex $vf 11]" }
}

test "FT.CREATE HNSW with default M/EF" {
  r FT.CREATE hnswdef SCHEMA vec VECTOR HNSW DIM 2 DISTANCE_METRIC COSINE
} {OK}

test_assert "FT.INFO HNSW defaults M=16, EF=200" {
  set info [r FT.INFO hnswdef]
  set fields [lindex $info 5]
  set vf [lindex $fields 0]
  if {[lindex $vf 9] != 16} { error "Expected M=16, got [lindex $vf 9]" }
  if {[lindex $vf 11] != 200} { error "Expected ef=200, got [lindex $vf 11]" }
}

puts "\n--- FT.ADD + FT.SEARCH with HNSW ---"

test "FT.ADD HNSW docs" {
  r FT.ADD hnswi h1 FIELDS label x_axis embedding [float_blob {1.0 0.0 0.0}]
  r FT.ADD hnswi h2 FIELDS label y_axis embedding [float_blob {0.0 1.0 0.0}]
  r FT.ADD hnswi h3 FIELDS label z_axis embedding [float_blob {0.0 0.0 1.0}]
  r FT.ADD hnswi h4 FIELDS label diagonal embedding [float_blob {1.0 1.0 0.0}]
} {OK}

test_assert "HNSW KNN returns nearest" {
  set query [float_blob {1.0 0.0 0.0}]
  set result [r FT.SEARCH hnswi {*=>[KNN 2 @embedding $q]} PARAMS 2 q $query]
  set total [lindex $result 0]
  if {$total != 2} { error "Expected 2, got $total" }
  set first [lindex $result 1]
  if {$first ne "h1"} { error "Expected h1 nearest, got $first" }
}

test_assert "HNSW KNN top-1" {
  set query [float_blob {0.0 0.9 0.0}]
  set result [r FT.SEARCH hnswi {*=>[KNN 1 @embedding $q]} PARAMS 2 q $query]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1, got $total" }
  set first [lindex $result 1]
  if {$first ne "h2"} { error "Expected h2 (y_axis), got $first" }
}

test_assert "HNSW KNN has __vec_score" {
  set query [float_blob {1.0 0.0 0.0}]
  set result [r FT.SEARCH hnswi {*=>[KNN 1 @embedding $q]} PARAMS 2 q $query]
  set fields [lindex $result 2]
  set score_idx [lsearch $fields "__vec_score"]
  if {$score_idx < 0} { error "__vec_score not found" }
  set score [lindex $fields [expr {$score_idx + 1}]]
  if {$score > 0.001} { error "Expected ~0 for exact match, got $score" }
}

puts "\n--- HNSW KNN with pre-filter ---"

test_assert "HNSW KNN pre-filter @label:{x_axis|diagonal}" {
  set query [float_blob {0.9 0.1 0.0}]
  set result [r FT.SEARCH hnswi {@label:{x_axis|diagonal}=>[KNN 1 @embedding $q]} PARAMS 2 q $query]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1, got $total" }
  set first [lindex $result 1]
  if {$first ne "h1"} { error "Expected h1 (x_axis closest to 0.9,0.1,0), got $first" }
}

puts "\n--- FT.DEL from HNSW index ---"

test "FT.DEL from HNSW" {
  r FT.DEL hnswi h1
} {OK}

test_assert "HNSW search after delete" {
  set query [float_blob {1.0 0.0 0.0}]
  set result [r FT.SEARCH hnswi {*=>[KNN 1 @embedding $q]} PARAMS 2 q $query]
  set total [lindex $result 0]
  if {$total != 1} { error "Expected 1, got $total" }
  set first [lindex $result 1]
  if {$first ne "h4"} { error "Expected h4 (diagonal nearest after h1 deleted), got $first" }
}

puts "\n--- HNSW persistence ---"

test "FT.CREATE HNSW persistence index" {
  r FT.CREATE hnswpersist SCHEMA vec VECTOR HNSW DIM 2 DISTANCE_METRIC L2 M 4 EF_CONSTRUCTION 50
} {OK}

test "FT.ADD HNSW persistence docs" {
  r FT.ADD hnswpersist hp1 FIELDS vec [float_blob {1.0 0.0}]
  r FT.ADD hnswpersist hp2 FIELDS vec [float_blob {0.0 1.0}]
} {OK}

test_assert "HNSW survives BGSAVE + restart" {
  r BGSAVE
  after 2000

  global redis_fd port module_path
  catch {redis_command $redis_fd SHUTDOWN SAVE}
  catch {close $redis_fd}
  after 1000

  start_redis $module_path $port
  set redis_fd [redis_connect localhost $port]

  set info [r FT.INFO hnswpersist]
  set fields [lindex $info 5]
  set vf [lindex $fields 0]
  if {[lindex $vf 3] ne "HNSW"} { error "Expected HNSW after restart" }
  if {[lindex $vf 9] != 4} { error "Expected M=4, got [lindex $vf 9]" }

  set query [float_blob {1.0 0.0}]
  set result [r FT.SEARCH hnswpersist {*=>[KNN 1 @vec $q]} PARAMS 2 q $query]
  set first [lindex $result 1]
  if {$first ne "hp1"} { error "Expected hp1 after restart, got $first" }
}

puts "\n--- Error cases ---"

test_error "FT.CREATE VECTOR unknown algorithm" {
  r FT.CREATE bad SCHEMA v VECTOR UNKNOWN DIM 2 DISTANCE_METRIC L2
} {ERR unknown VECTOR algorithm*}

test_error "FT.SEARCH HNSW wrong dimension" {
  set bad [float_blob {1.0}]
  r FT.SEARCH hnswi {*=>[KNN 1 @embedding $q]} PARAMS 2 q $bad
} {ERR query vector dimension mismatch}

puts "\n=== Phase 12: FT.AGGREGATE ==="

puts "\n--- Setup ---"

test "FT.CREATE aggregate test index" {
  r FT.CREATE aggidx SCHEMA category TAG brand TAG price NUMERIC rating NUMERIC
} {OK}

test "FT.ADD aggregate test data" {
  r FT.ADD aggidx p1 FIELDS category shoes brand nike price 120 rating 4.5
  r FT.ADD aggidx p2 FIELDS category shoes brand adidas price 90 rating 4.0
  r FT.ADD aggidx p3 FIELDS category shoes brand nike price 200 rating 4.8
  r FT.ADD aggidx p4 FIELDS category hat brand nike price 30 rating 3.5
  r FT.ADD aggidx p5 FIELDS category hat brand adidas price 25 rating 4.2
  r FT.ADD aggidx p6 FIELDS category boots brand puma price 150 rating 4.0
  r FT.ADD aggidx p7 FIELDS category boots brand nike price 180 rating 4.6
  r FT.ADD aggidx p8 FIELDS category boots brand adidas price 130 rating 3.8
} {OK}

puts "\n--- GROUPBY + COUNT ---"

test_assert "FT.AGGREGATE GROUPBY category REDUCE COUNT" {
  set result [r FT.AGGREGATE aggidx * GROUPBY 1 @category REDUCE COUNT 0 AS count]
  if {[llength $result] != 3} {
    error "Expected 3 groups, got [llength $result]"
  }
  # Find shoes group
  set found 0
  foreach row $result {
    set cat_idx [lsearch $row "category"]
    if {$cat_idx >= 0} {
      set cat_val [lindex $row [expr {$cat_idx + 1}]]
      set cnt_idx [lsearch $row "count"]
      set cnt_val [lindex $row [expr {$cnt_idx + 1}]]
      if {$cat_val eq "shoes" && $cnt_val == 3} { incr found }
      if {$cat_val eq "hat" && $cnt_val == 2} { incr found }
      if {$cat_val eq "boots" && $cnt_val == 3} { incr found }
    }
  }
  if {$found != 3} { error "Expected 3 correct group counts, found $found" }
}

puts "\n--- GROUPBY + SUM ---"

test_assert "FT.AGGREGATE GROUPBY category REDUCE SUM price" {
  set result [r FT.AGGREGATE aggidx * GROUPBY 1 @category REDUCE SUM 1 @price AS total_price]
  foreach row $result {
    set cat_idx [lsearch $row "category"]
    set cat_val [lindex $row [expr {$cat_idx + 1}]]
    set tp_idx [lsearch $row "total_price"]
    set tp_val [lindex $row [expr {$tp_idx + 1}]]
    if {$cat_val eq "shoes"} {
      # 120 + 90 + 200 = 410
      set expected 410.0
      if {abs($tp_val - $expected) > 0.01} {
        error "shoes total_price: expected $expected, got $tp_val"
      }
    }
  }
}

puts "\n--- GROUPBY + AVG ---"

test_assert "FT.AGGREGATE GROUPBY category REDUCE AVG price" {
  set result [r FT.AGGREGATE aggidx * GROUPBY 1 @category REDUCE AVG 1 @price AS avg_price]
  foreach row $result {
    set cat_idx [lsearch $row "category"]
    set cat_val [lindex $row [expr {$cat_idx + 1}]]
    set ap_idx [lsearch $row "avg_price"]
    set ap_val [lindex $row [expr {$ap_idx + 1}]]
    if {$cat_val eq "hat"} {
      # (30 + 25) / 2 = 27.5
      if {abs($ap_val - 27.5) > 0.01} {
        error "hat avg_price: expected 27.5, got $ap_val"
      }
    }
  }
}

puts "\n--- GROUPBY + MIN/MAX ---"

test_assert "FT.AGGREGATE GROUPBY category REDUCE MIN/MAX" {
  set result [r FT.AGGREGATE aggidx * GROUPBY 1 @category REDUCE MIN 1 @price AS min_price REDUCE MAX 1 @price AS max_price]
  foreach row $result {
    set cat_idx [lsearch $row "category"]
    set cat_val [lindex $row [expr {$cat_idx + 1}]]
    set min_idx [lsearch $row "min_price"]
    set min_val [lindex $row [expr {$min_idx + 1}]]
    set max_idx [lsearch $row "max_price"]
    set max_val [lindex $row [expr {$max_idx + 1}]]
    if {$cat_val eq "boots"} {
      # min=130, max=180
      if {abs($min_val - 130) > 0.01} { error "boots min: expected 130, got $min_val" }
      if {abs($max_val - 180) > 0.01} { error "boots max: expected 180, got $max_val" }
    }
  }
}

puts "\n--- GROUPBY + COUNT_DISTINCT ---"

test_assert "FT.AGGREGATE GROUPBY category REDUCE COUNT_DISTINCT brand" {
  set result [r FT.AGGREGATE aggidx * GROUPBY 1 @category REDUCE COUNT_DISTINCT 1 @brand AS num_brands]
  foreach row $result {
    set cat_idx [lsearch $row "category"]
    set cat_val [lindex $row [expr {$cat_idx + 1}]]
    set nb_idx [lsearch $row "num_brands"]
    set nb_val [lindex $row [expr {$nb_idx + 1}]]
    if {$cat_val eq "shoes"} {
      # nike, adidas = 2
      if {$nb_val != 2} { error "shoes num_brands: expected 2, got $nb_val" }
    }
    if {$cat_val eq "boots"} {
      # puma, nike, adidas = 3
      if {$nb_val != 3} { error "boots num_brands: expected 3, got $nb_val" }
    }
  }
}

puts "\n--- Multiple reducers ---"

test_assert "FT.AGGREGATE with COUNT + AVG + SUM" {
  set result [r FT.AGGREGATE aggidx * GROUPBY 1 @category REDUCE COUNT 0 AS count REDUCE AVG 1 @price AS avg_price REDUCE SUM 1 @rating AS total_rating]
  if {[llength $result] != 3} { error "Expected 3 groups" }
  foreach row $result {
    set cnt_idx [lsearch $row "count"]
    set avg_idx [lsearch $row "avg_price"]
    set tr_idx [lsearch $row "total_rating"]
    if {$cnt_idx < 0} { error "count field missing" }
    if {$avg_idx < 0} { error "avg_price field missing" }
    if {$tr_idx < 0} { error "total_rating field missing" }
  }
}

puts "\n--- SORTBY ---"

test_assert "FT.AGGREGATE GROUPBY + SORTBY count DESC" {
  set result [r FT.AGGREGATE aggidx * GROUPBY 1 @category REDUCE COUNT 0 AS count SORTBY 2 @count DESC]
  if {[llength $result] != 3} { error "Expected 3, got [llength $result]" }
  set first [lindex $result 0]
  set cnt_idx [lsearch $first "count"]
  set cnt_val [lindex $first [expr {$cnt_idx + 1}]]
  # shoes=3 and boots=3 are tied at top
  if {$cnt_val != 3} { error "Expected highest count 3, got $cnt_val" }
}

test_assert "FT.AGGREGATE SORTBY ASC" {
  set result [r FT.AGGREGATE aggidx * GROUPBY 1 @category REDUCE COUNT 0 AS count SORTBY 2 @count ASC]
  set first [lindex $result 0]
  set cnt_idx [lsearch $first "count"]
  set cnt_val [lindex $first [expr {$cnt_idx + 1}]]
  if {$cnt_val != 2} { error "Expected lowest count 2 (hat), got $cnt_val" }
}

puts "\n--- LIMIT ---"

test_assert "FT.AGGREGATE LIMIT 0 2" {
  set result [r FT.AGGREGATE aggidx * GROUPBY 1 @category REDUCE COUNT 0 AS count SORTBY 2 @count DESC LIMIT 0 2]
  if {[llength $result] != 2} { error "Expected 2, got [llength $result]" }
}

test_assert "FT.AGGREGATE LIMIT 1 1" {
  set result [r FT.AGGREGATE aggidx * GROUPBY 1 @category REDUCE COUNT 0 AS count SORTBY 2 @count DESC LIMIT 1 1]
  if {[llength $result] != 1} { error "Expected 1, got [llength $result]" }
}

test_assert "FT.AGGREGATE LIMIT beyond" {
  set result [r FT.AGGREGATE aggidx * GROUPBY 1 @category REDUCE COUNT 0 AS count LIMIT 10 5]
  if {[llength $result] != 0} { error "Expected 0, got [llength $result]" }
}

puts "\n--- GROUPBY multiple fields ---"

test_assert "FT.AGGREGATE GROUPBY 2 fields" {
  set result [r FT.AGGREGATE aggidx * GROUPBY 2 @category @brand REDUCE COUNT 0 AS count]
  # shoes/nike=2, shoes/adidas=1, hat/nike=1, hat/adidas=1, boots/puma=1, boots/nike=1, boots/adidas=1
  if {[llength $result] != 7} { error "Expected 7 groups, got [llength $result]" }
}

puts "\n--- Filtered query ---"

test_assert "FT.AGGREGATE with filter query" {
  set result [r FT.AGGREGATE aggidx {@category:{shoes}} GROUPBY 1 @brand REDUCE COUNT 0 AS count]
  if {[llength $result] != 2} { error "Expected 2 brands in shoes, got [llength $result]" }
  foreach row $result {
    set b_idx [lsearch $row "brand"]
    set bv [lindex $row [expr {$b_idx + 1}]]
    set c_idx [lsearch $row "count"]
    set cv [lindex $row [expr {$c_idx + 1}]]
    if {$bv eq "nike" && $cv != 2} { error "nike count should be 2, got $cv" }
    if {$bv eq "adidas" && $cv != 1} { error "adidas count should be 1, got $cv" }
  }
}

puts "\n--- No GROUPBY (raw rows) ---"

test_assert "FT.AGGREGATE without GROUPBY returns raw rows" {
  set result [r FT.AGGREGATE aggidx * LIMIT 0 3]
  if {[llength $result] != 3} { error "Expected 3 rows, got [llength $result]" }
}

puts "\n--- Error cases ---"

test_error "FT.AGGREGATE nonexistent index" {
  r FT.AGGREGATE nosuchidx *
} {ERR index not found}

test_error "FT.AGGREGATE unknown option" {
  r FT.AGGREGATE aggidx * BADOPTION
} {ERR unknown AGGREGATE option}

test_error "FT.AGGREGATE REDUCE unknown function" {
  r FT.AGGREGATE aggidx * GROUPBY 1 @category REDUCE BADFUNCTION 0 AS x
} {ERR unknown REDUCE function}

test_error "FT.AGGREGATE wrong arity" {
  r FT.AGGREGATE aggidx
} {ERR*}

# ============================================================
# Phase 13 — Search Options Enhancement
# ============================================================

puts "\n--- Phase 13: Search Options ---"

r FT.CREATE p13idx SCHEMA title TEXT body TEXT status TAG price NUMERIC
r FT.ADD p13idx doc1 FIELDS title "the quick brown fox" body "jumps over the lazy dog" status active price 100
r FT.ADD p13idx doc2 FIELDS title "the slow red car" body "drives on the road" status active price 200
r FT.ADD p13idx doc3 FIELDS title "quick green turtle" body "swims in the sea" status inactive price 50

# --- NOCONTENT ---

test "NOCONTENT returns only doc IDs" {
  set res [r FT.SEARCH p13idx "@status:{active}" NOCONTENT]
  # result: count id1 id2 (no field arrays)
  lindex $res 0
} {2}

test "NOCONTENT result has no field arrays" {
  set res [r FT.SEARCH p13idx "@status:{active}" NOCONTENT]
  # should be [2, id1, id2] — length 3
  llength $res
} {3}

# --- WITHSCORES ---

test "WITHSCORES returns scores for TEXT query" {
  set res [r FT.SEARCH p13idx "quick" WITHSCORES]
  # result: count, id, [__search_score, score, fields...], ...
  set count [lindex $res 0]
  expr {$count == 2}
} {1}

test_assert "WITHSCORES score is positive" {
  set res [r FT.SEARCH p13idx "quick" WITHSCORES]
  # First result fields array should contain __search_score
  set fields [lindex $res 2]
  set score_idx [lsearch $fields "__search_score"]
  if {$score_idx < 0} {error "no __search_score in fields"}
  set score_val [lindex $fields [expr {$score_idx + 1}]]
  if {$score_val <= 0} {error "score should be positive, got $score_val"}
}

test_assert "WITHSCORES results sorted by score descending" {
  set res [r FT.SEARCH p13idx "quick" WITHSCORES]
  set f1 [lindex $res 2]
  set f2 [lindex $res 4]
  set si1 [lsearch $f1 "__search_score"]
  set si2 [lsearch $f2 "__search_score"]
  set s1 [lindex $f1 [expr {$si1 + 1}]]
  set s2 [lindex $f2 [expr {$si2 + 1}]]
  if {$s1 < $s2} {error "first score ($s1) should be >= second score ($s2)"}
}

test "WITHSCORES + NOCONTENT returns IDs only" {
  set res [r FT.SEARCH p13idx "quick" WITHSCORES NOCONTENT]
  set count [lindex $res 0]
  set total_items [llength $res]
  # Should be count + count ids = 1 + count
  expr {$total_items == $count + 1}
} {1}

# --- NOSTOPWORDS ---

test "Stop word 'the' not found without NOSTOPWORDS" {
  set res [r FT.SEARCH p13idx "the"]
  lindex $res 0
} {0}

test "NOSTOPWORDS: stop word 'the' is searchable" {
  set res [r FT.SEARCH p13idx "the" NOSTOPWORDS]
  lindex $res 0
} {3}

# --- VERBATIM (same as NOSTOPWORDS for now) ---

test "VERBATIM disables stop word filtering" {
  set res [r FT.SEARCH p13idx "the" VERBATIM]
  lindex $res 0
} {3}

# --- INFIELDS ---

test "INFIELDS limits search to specified field" {
  # "quick" appears in title of doc1 and doc3, body of neither
  set res [r FT.SEARCH p13idx "quick" INFIELDS 1 title]
  lindex $res 0
} {2}

test "INFIELDS body: 'quick' not in body" {
  set res [r FT.SEARCH p13idx "swims" INFIELDS 1 body]
  lindex $res 0
} {1}

test "INFIELDS title: 'swims' not in title" {
  set res [r FT.SEARCH p13idx "swims" INFIELDS 1 title]
  lindex $res 0
} {0}

# --- INKEYS ---

test "INKEYS limits search to specified keys" {
  set res [r FT.SEARCH p13idx "@status:{active}" INKEYS 1 doc1]
  lindex $res 0
} {1}

test_assert "INKEYS returns only the specified key" {
  set res [r FT.SEARCH p13idx "@status:{active}" INKEYS 1 doc1]
  set id [lindex $res 1]
  if {$id ne "doc1"} {error "expected doc1, got $id"}
}

test "INKEYS with multiple keys" {
  set res [r FT.SEARCH p13idx "*" INKEYS 2 doc1 doc3]
  lindex $res 0
} {2}

test "INKEYS with nonexistent key" {
  set res [r FT.SEARCH p13idx "*" INKEYS 1 nosuchkey]
  lindex $res 0
} {0}

# --- TIMEOUT ---

test "TIMEOUT option accepted" {
  set res [r FT.SEARCH p13idx "@status:{active}" TIMEOUT 5000]
  lindex $res 0
} {2}

# --- Combined options ---

test "NOCONTENT + INKEYS" {
  set res [r FT.SEARCH p13idx "*" NOCONTENT INKEYS 2 doc1 doc2]
  set count [lindex $res 0]
  set total [llength $res]
  list $count $total
} {2 3}

test "INFIELDS + INKEYS" {
  set res [r FT.SEARCH p13idx "quick" INFIELDS 1 title INKEYS 1 doc1]
  lindex $res 0
} {1}

test_assert "WITHSCORES + SORTBY: SORTBY overrides score order" {
  set res [r FT.SEARCH p13idx "@status:{active}" WITHSCORES SORTBY price ASC]
  set id1 [lindex $res 1]
  set id2 [lindex $res 3]
  # doc1 has price=100, doc2 has price=200, ASC order
  if {$id1 ne "doc1"} {error "expected doc1 first (price ASC), got $id1"}
}

test "NOSTOPWORDS + INFIELDS" {
  # "the" should be found in title when NOSTOPWORDS is on, limited to title field
  set res [r FT.SEARCH p13idx "the" NOSTOPWORDS INFIELDS 1 title]
  lindex $res 0
} {2}

# --- Error cases for new options ---

test_error "INFIELDS without count" {
  r FT.SEARCH p13idx "hello" INFIELDS
} {ERR*}

test_error "INKEYS without count" {
  r FT.SEARCH p13idx "hello" INKEYS
} {ERR*}

test_error "TIMEOUT without value" {
  r FT.SEARCH p13idx "hello" TIMEOUT
} {ERR*}

test_error "TIMEOUT negative value" {
  r FT.SEARCH p13idx "hello" TIMEOUT -1
} {ERR*}

r FT.DROPINDEX p13idx

# ============================================================
# Phase 14 — Prefix Search + Fuzzy Match + Optional Terms
# ============================================================

puts "\n--- Phase 14: Prefix + Fuzzy + Optional ---"

r FT.CREATE p14idx SCHEMA title TEXT body TEXT status TAG
r FT.ADD p14idx doc1 FIELDS title "hello world" body "greeting message" status active
r FT.ADD p14idx doc2 FIELDS title "help desk" body "support center" status active
r FT.ADD p14idx doc3 FIELDS title "helicopter ride" body "flying adventure" status inactive
r FT.ADD p14idx doc4 FIELDS title "world cup" body "football match" status active
r FT.ADD p14idx doc5 FIELDS title "hallo friend" body "german greeting" status active

# --- Prefix search ---

test "Prefix hel* matches hello, help, helicopter" {
  set res [r FT.SEARCH p14idx "hel*"]
  lindex $res 0
} {3}

test "Prefix wor* matches world" {
  set res [r FT.SEARCH p14idx "wor*"]
  lindex $res 0
} {2}

test "Prefix nonex* matches nothing" {
  set res [r FT.SEARCH p14idx "nonex*"]
  lindex $res 0
} {0}

test "Prefix on specific field @title:hel*" {
  set res [r FT.SEARCH p14idx "@title:hel*"]
  lindex $res 0
} {3}

test "Prefix @body:gre* matches greeting and german greeting" {
  set res [r FT.SEARCH p14idx "@body:gre*"]
  lindex $res 0
} {2}

test "Prefix with boolean AND: hel* @status:{active}" {
  set res [r FT.SEARCH p14idx "hel* @status:{active}"]
  lindex $res 0
} {2}

test "Prefix in parenthesized field: @title:(hel* cup)" {
  set res [r FT.SEARCH p14idx "@title:(hel* cup)"]
  lindex $res 0
} {4}

# --- Fuzzy search ---

test "Fuzzy %hello% (LD=1) matches hello and hallo" {
  set res [r FT.SEARCH p14idx "%hello%"]
  lindex $res 0
} {2}

test "Fuzzy %wrld% (LD=1) matches world" {
  set res [r FT.SEARCH p14idx "%wrld%"]
  lindex $res 0
} {2}

test "Fuzzy %%helo%% (LD=2) matches hello, help, hallo" {
  set res [r FT.SEARCH p14idx "%%helo%%"]
  lindex $res 0
} {3}

test "Fuzzy %nonexistent% matches nothing" {
  set res [r FT.SEARCH p14idx "%zzzzz%"]
  lindex $res 0
} {0}

test "Fuzzy on specific field @title:%hello%" {
  set res [r FT.SEARCH p14idx "@title:%hello%"]
  lindex $res 0
} {2}

test "Fuzzy %%%helpp%%% (LD=3) matches hello, help, hallo" {
  set res [r FT.SEARCH p14idx "%%%helpp%%%"]
  set count [lindex $res 0]
  expr {$count >= 2}
} {1}

# --- Optional terms ---

test "Optional ~bar: foo ~bar returns docs with foo" {
  set res [r FT.SEARCH p14idx "hello ~world"]
  set count [lindex $res 0]
  # Should return at least the docs with "hello"
  expr {$count >= 1}
} {1}

test_assert "Optional boosts score with WITHSCORES" {
  set res [r FT.SEARCH p14idx "hello ~world" WITHSCORES]
  set count [lindex $res 0]
  if {$count < 1} {error "expected at least 1 result"}
  # doc1 has both "hello" and "world" - should rank highest
  set first_id [lindex $res 1]
  if {$first_id ne "doc1"} {error "expected doc1 first (has both hello+world), got $first_id"}
}

test "Optional alone: ~hello returns all docs" {
  set res [r FT.SEARCH p14idx "~hello"]
  lindex $res 0
} {5}

# --- Combined prefix + fuzzy ---

test "Prefix + fuzzy combined: hel* %wrld%" {
  set res [r FT.SEARCH p14idx "hel* | %wrld%"]
  set count [lindex $res 0]
  expr {$count >= 3}
} {1}

# --- WITHSCORES + prefix ---

test_assert "Prefix with WITHSCORES returns scores" {
  set res [r FT.SEARCH p14idx "hel*" WITHSCORES]
  set count [lindex $res 0]
  if {$count < 1} {error "expected results"}
  set fields [lindex $res 2]
  set score_idx [lsearch $fields "__search_score"]
  if {$score_idx < 0} {error "no __search_score in fields"}
  set score_val [lindex $fields [expr {$score_idx + 1}]]
  if {$score_val <= 0} {error "score should be positive, got $score_val"}
}

# --- Fuzzy in parenthesized field expr ---

test "Fuzzy in parenthesized field: @title:(%hello% cup)" {
  set res [r FT.SEARCH p14idx "@title:(%hello% cup)"]
  set count [lindex $res 0]
  expr {$count >= 2}
} {1}

r FT.DROPINDEX p14idx

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
