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
  # info: index_name <name> num_docs <n> language <lang> fields <field_array>
  if {[llength $info] != 8} {
    error "Expected 8 elements, got [llength $info]: $info"
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
  if {[lindex $info 4] ne "language"} {
    error "Expected 'language' label, got '[lindex $info 4]'"
  }
  if {[lindex $info 6] ne "fields"} {
    error "Expected 'fields' label, got '[lindex $info 6]'"
  }
  set fields [lindex $info 7]
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
  set fields [lindex $info 7]
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
  set fields [lindex $info 7]
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
  set fields [lindex $info 7]
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

test_error "FT.CREATE ON with unsupported source type" {
  r FT.CREATE bad ON STRING PREFIX 1 x: SCHEMA f TAG
} {ERR ON requires HASH or JSON}

test_error "FT.CREATE ON HASH without PREFIX" {
  r FT.CREATE bad ON HASH SCHEMA f TAG
} {ERR expected PREFIX after ON HASH/JSON}

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
  set fields [lindex $info 7]
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
  set fields [lindex $info 7]
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
  set fields [lindex $info 7]
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
  set fields [lindex $info 7]
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
# Phase 15 — Exact Phrase + Proximity Search
# ============================================================

puts "\n--- Phase 15: Phrase + SLOP + INORDER ---"

r FT.CREATE p15idx SCHEMA title TEXT body TEXT status TAG
r FT.ADD p15idx doc1 FIELDS title "the quick brown fox" body "jumps over the lazy dog" status active
r FT.ADD p15idx doc2 FIELDS title "quick fox runs" body "the brown bear sleeps" status active
r FT.ADD p15idx doc3 FIELDS title "hello big world" body "greeting message here" status inactive
r FT.ADD p15idx doc4 FIELDS title "hello world" body "exact phrase doc" status active
r FT.ADD p15idx doc5 FIELDS title "world hello" body "reversed order" status active

# --- Exact phrase ---

test "Exact phrase: \"hello world\" matches only adjacent" {
  set res [r FT.SEARCH p15idx "\"hello world\""]
  lindex $res 0
} {1}

test_assert "Exact phrase result is doc4" {
  set res [r FT.SEARCH p15idx "\"hello world\""]
  set id [lindex $res 1]
  if {$id ne "doc4"} {error "expected doc4, got $id"}
}

test "Exact phrase: \"world hello\" matches doc5" {
  set res [r FT.SEARCH p15idx "\"world hello\""]
  lindex $res 0
} {1}

test "Exact phrase: \"quick brown\" matches doc1" {
  set res [r FT.SEARCH p15idx "\"quick brown\""]
  lindex $res 0
} {1}

test "Exact phrase: \"brown quick\" matches nothing (wrong order)" {
  set res [r FT.SEARCH p15idx "\"brown quick\""]
  lindex $res 0
} {0}

test "Exact phrase: \"hello big world\" matches doc3" {
  set res [r FT.SEARCH p15idx "\"hello big world\""]
  lindex $res 0
} {1}

test "Exact phrase: \"hello world\" does NOT match \"hello big world\"" {
  set res [r FT.SEARCH p15idx "\"hello world\""]
  # Only doc4 has adjacent hello world
  lindex $res 0
} {1}

# --- Field-specific phrase ---

test "Field phrase: @title:\"quick brown\" matches doc1" {
  set res [r FT.SEARCH p15idx "@title:\"quick brown\""]
  lindex $res 0
} {1}

test "Field phrase: @body:\"lazy dog\" matches doc1" {
  set res [r FT.SEARCH p15idx "@body:\"lazy dog\""]
  lindex $res 0
} {1}

test "Field phrase: @title:\"lazy dog\" matches nothing (wrong field)" {
  set res [r FT.SEARCH p15idx "@title:\"lazy dog\""]
  lindex $res 0
} {0}

# --- Phrase + boolean ---

test "Phrase + TAG: \"hello world\" @status:{active}" {
  set res [r FT.SEARCH p15idx "\"hello world\" @status:{active}"]
  lindex $res 0
} {1}

test "Phrase + TAG: \"quick brown\" @status:{active}" {
  set res [r FT.SEARCH p15idx "\"quick brown\" @status:{active}"]
  lindex $res 0
} {1}

# --- SLOP ---

test "SLOP 1: \"hello world\" matches docs with gap <= 1" {
  set res [r FT.SEARCH p15idx "\"hello world\"" SLOP 1]
  set count [lindex $res 0]
  # doc3 (hello big world), doc4 (hello world), doc5 (world hello, reversed)
  expr {$count == 3}
} {1}

test "SLOP 0: same as exact phrase" {
  set res [r FT.SEARCH p15idx "\"hello world\"" SLOP 0]
  lindex $res 0
} {1}

test "SLOP 2: \"quick fox\" matches \"quick brown fox\"" {
  set res [r FT.SEARCH p15idx "\"quick fox\"" SLOP 2]
  set count [lindex $res 0]
  expr {$count >= 1}
} {1}

# --- INORDER ---

test "INORDER: \"hello world\" does not match \"world hello\"" {
  set res [r FT.SEARCH p15idx "\"hello world\"" SLOP 10 INORDER]
  # doc4 has "hello world" (correct order), doc3 has "hello big world" (correct order)
  # doc5 has "world hello" — wrong order with INORDER
  set count [lindex $res 0]
  # doc4 and doc3 should match, doc5 should not
  expr {$count == 2}
} {1}

# --- Phrase with stop words ---

test "Phrase with stop words: \"the quick\" matches doc1" {
  set res [r FT.SEARCH p15idx "\"the quick\""]
  lindex $res 0
} {1}

test "Phrase with stop words: \"over the lazy\" matches doc1" {
  set res [r FT.SEARCH p15idx "\"over the lazy\""]
  lindex $res 0
} {1}

# --- Multi-word phrase ---

test "Multi-word phrase: \"the quick brown fox\"" {
  set res [r FT.SEARCH p15idx "\"the quick brown fox\""]
  lindex $res 0
} {1}

# --- Single-word phrase ---

test "Single-word phrase: \"hello\" acts as term search" {
  set res [r FT.SEARCH p15idx "\"hello\""]
  set count [lindex $res 0]
  expr {$count >= 2}
} {1}

# --- Phrase with WITHSCORES ---

test_assert "Phrase with WITHSCORES returns scores" {
  set res [r FT.SEARCH p15idx "\"hello world\"" WITHSCORES]
  set count [lindex $res 0]
  if {$count < 1} {error "expected results"}
  set fields [lindex $res 2]
  set score_idx [lsearch $fields "__search_score"]
  if {$score_idx < 0} {error "no __search_score in fields"}
}

# --- Error cases ---

test_error "Unclosed quote" {
  r FT.SEARCH p15idx "\"hello world"
} {ERR*}

test_error "SLOP without value" {
  r FT.SEARCH p15idx "\"hello\"" SLOP
} {ERR*}

r FT.DROPINDEX p15idx

# ============================================================
# Phase 16 — Stemming + Multi-Language Support
# ============================================================

puts "\n--- Phase 16: Stemming ---"

r FT.CREATE p16idx SCHEMA title TEXT body TEXT exact TEXT NOSTEM status TAG

r FT.ADD p16idx doc1 FIELDS title "running fast" body "the runners compete" exact "running fast" status active
r FT.ADD p16idx doc2 FIELDS title "run slowly" body "a quick jog" exact "run slowly" status active
r FT.ADD p16idx doc3 FIELDS title "swimming pool" body "swimmers dive deep" exact "swimming pool" status inactive
r FT.ADD p16idx doc4 FIELDS title "jumped high" body "the jumper leaps" exact "jumped high" status active

# --- Basic stemming ---

test "Stemming: 'run' matches 'running' and 'run'" {
  set res [r FT.SEARCH p16idx "run"]
  set count [lindex $res 0]
  expr {$count >= 2}
} {1}

test "Stemming: 'running' also matches 'run'" {
  set res [r FT.SEARCH p16idx "running"]
  set count [lindex $res 0]
  expr {$count >= 2}
} {1}

test "Stemming: 'swimmers' matches 'swimming'" {
  set res [r FT.SEARCH p16idx "swimmers"]
  set count [lindex $res 0]
  expr {$count >= 1}
} {1}

test "Stemming: 'jumped' matches 'jumper'" {
  set res [r FT.SEARCH p16idx "jumped"]
  set count [lindex $res 0]
  expr {$count >= 1}
} {1}

# --- VERBATIM disables stemming ---

test "VERBATIM: 'run' does NOT match 'running'" {
  set res [r FT.SEARCH p16idx "run" VERBATIM]
  set count [lindex $res 0]
  # Only doc2 has exact "run"
  expr {$count == 1}
} {1}

test "VERBATIM: 'running' does NOT match 'run'" {
  set res [r FT.SEARCH p16idx "running" VERBATIM]
  set count [lindex $res 0]
  # Only doc1 has exact "running"
  expr {$count == 1}
} {1}

# --- NOSTEM field ---

test "NOSTEM field: 'run' does NOT match 'running' in NOSTEM field" {
  set res [r FT.SEARCH p16idx "@exact:run"]
  set count [lindex $res 0]
  # Only doc2 has exact "run" in the exact field
  expr {$count == 1}
} {1}

test "NOSTEM field: 'running' does NOT match 'run' in NOSTEM field" {
  set res [r FT.SEARCH p16idx "@exact:running"]
  set count [lindex $res 0]
  expr {$count == 1}
} {1}

# --- Field-specific stemming ---

test "Field stemming: @title:run matches running and run" {
  set res [r FT.SEARCH p16idx "@title:run"]
  set count [lindex $res 0]
  expr {$count >= 2}
} {1}

# --- Phrase queries NOT affected by stemming ---

test "Phrase not stemmed: \"running fast\" matches exactly" {
  set res [r FT.SEARCH p16idx "\"running fast\""]
  lindex $res 0
} {1}

# --- LANGUAGE option ---

test "LANGUAGE english accepted in FT.CREATE" {
  r FT.CREATE p16lang LANGUAGE english SCHEMA title TEXT
  r FT.ADD p16lang d1 FIELDS title "running"
  set res [r FT.SEARCH p16lang "run"]
  set count [lindex $res 0]
  r FT.DROPINDEX p16lang
  expr {$count >= 1}
} {1}

test "LANGUAGE option in FT.SEARCH accepted" {
  set res [r FT.SEARCH p16idx "run" LANGUAGE english]
  set count [lindex $res 0]
  expr {$count >= 2}
} {1}

# --- FT.INFO shows NOSTEM and language ---

test_assert "FT.INFO shows language" {
  set info [r FT.INFO p16idx]
  set lang_idx -1
  for {set i 0} {$i < [llength $info]} {incr i} {
    if {[lindex $info $i] eq "language"} {
      set lang_idx [expr {$i + 1}]
      break
    }
  }
  if {$lang_idx < 0} {error "language not found in FT.INFO"}
  set lang [lindex $info $lang_idx]
  if {$lang ne "english"} {error "expected english, got $lang"}
}

test_assert "FT.INFO shows NOSTEM on field" {
  set info [r FT.INFO p16idx]
  set fields_idx -1
  for {set i 0} {$i < [llength $info]} {incr i} {
    if {[lindex $info $i] eq "fields"} {
      set fields_idx [expr {$i + 1}]
      break
    }
  }
  if {$fields_idx < 0} {error "fields not found in FT.INFO"}
  set fields [lindex $info $fields_idx]
  # Find the "exact" field — it should have NOSTEM
  set found 0
  foreach f $fields {
    if {[lindex $f 0] eq "exact"} {
      if {[llength $f] >= 3 && [lindex $f 2] eq "NOSTEM"} {
        set found 1
      } else {
        error "exact field missing NOSTEM: $f"
      }
    }
  }
  if {!$found} {error "exact field not found"}
}

# --- WITHSCORES + stemming ---

test_assert "WITHSCORES with stemming returns scores" {
  set res [r FT.SEARCH p16idx "run" WITHSCORES]
  set count [lindex $res 0]
  if {$count < 2} {error "expected at least 2 results with stemming"}
}

r FT.DROPINDEX p16idx

# ============================================================
# Phase 27: Persistence & Module Type Hardening
# ============================================================
puts "\n=== Phase 27: Persistence & ModuleType ==="

test_assert "MEMORY USAGE grows after adding docs" {
  r FT.CREATE p27mem SCHEMA body TEXT
  set m0 [r MEMORY USAGE p27mem]
  set big [string repeat "a" 4096]
  r FT.ADD p27mem d1 FIELDS body $big
  r FT.ADD p27mem d2 FIELDS body $big
  set m1 [r MEMORY USAGE p27mem]
  if {$m1 <= $m0} { error "Expected MEMORY USAGE to grow: before=$m0 after=$m1" }
  r FT.DROPINDEX p27mem
}

# Detect Redis major version for Redis 7+ callbacks (copy2, aux_save2)
set info_srv [r INFO server]
set redis_major 0
if {[regexp {redis_version:([0-9]+)\.} $info_srv -> rmaj]} {
  set redis_major $rmaj
}

if {$redis_major >= 7} {
  test_assert "COPY clones search index independently (Redis 7+)" {
    r FT.CREATE p27src SCHEMA tag TAG num NUMERIC
    r FT.ADD p27src d1 FIELDS tag alpha num 42
    set ok [r COPY p27src p27dst]
    if {$ok != 1} { error "Expected COPY to return 1, got $ok" }
    r FT.DROPINDEX p27src
    set res [r FT.SEARCH p27dst {@tag:{alpha}}]
    if {[lindex $res 0] != 1} { error "Expected 1 match in copied index, got $res" }
    r FT.DROPINDEX p27dst
  }
}

test_assert "NOSTEM, LANGUAGE, and synonyms survive BGSAVE + restart" {
  r FT.CREATE p27rdb LANGUAGE english SCHEMA exact TEXT NOSTEM name TEXT PHONETIC dm:en
  r FT.ADD p27rdb d1 FIELDS exact running name Jon
  r FT.SYNUPDATE p27rdb g1 fast quick

  r BGSAVE
  after 500
  global redis_fd port module_path
  catch {redis_command $redis_fd SHUTDOWN SAVE}
  catch {close $redis_fd}
  after 500

  start_redis $module_path $port
  set redis_fd [redis_connect localhost $port]

  # Verify NOSTEM preserved (run does NOT stem-match running)
  set res [r FT.SEARCH p27rdb "@exact:run"]
  if {[lindex $res 0] != 0} { error "Expected NOSTEM to block stem match after restart, got $res" }

  # Verify exact match still works
  set res2 [r FT.SEARCH p27rdb "@exact:running"]
  if {[lindex $res2 0] != 1} { error "Expected exact match after restart, got $res2" }

  # Verify synonyms preserved
  set syns [r FT.SYNDUMP p27rdb]
  if {[llength $syns] < 2} { error "Expected synonyms after restart, got $syns" }

  r FT.DROPINDEX p27rdb
}

test_assert "Wide document (>10 fields) survives command-AOF rewrite + restart" {
  r FT.CREATE p27wide SCHEMA f1 TAG f2 TAG f3 TAG f4 TAG f5 TAG f6 TAG f7 TAG f8 TAG f9 TAG f10 TAG f11 TAG f12 TAG
  r FT.ADD p27wide w1 FIELDS f1 v1 f2 v2 f3 v3 f4 v4 f5 v5 f6 v6 f7 v7 f8 v8 f9 v9 f10 v10 f11 v11 f12 v12

  r CONFIG SET aof-use-rdb-preamble no
  r CONFIG SET appendonly yes
  after 1000
  catch { r BGREWRITEAOF }
  after 1500

  global redis_fd port module_path
  catch {redis_command $redis_fd SHUTDOWN SAVE}
  catch {close $redis_fd}
  after 500

  start_redis $module_path $port
  set redis_fd [redis_connect localhost $port]

  set res [r FT.SEARCH p27wide {@f12:{v12}}]
  if {[lindex $res 0] != 1} { error "Expected 1 match for f12:v12 after AOF rewrite, got $res" }
  r FT.DROPINDEX p27wide
}

# Redis 7+ aux_save2 persistence check (aliases, suggest dicts, term dicts, config)
if {$redis_major >= 7} {
  test_assert "Redis 7 aux RDB persists aliases, suggest dicts, term dicts, and config" {
    r FT.CREATE p27aux SCHEMA tag TAG
    r FT.ALIASADD p27alias p27aux
    r FT.SUGADD p27sug "redis" 5.0 PAYLOAD "db"
    r FT.DICTADD p27dict alpha beta
    r FT.CONFIG SET TIMEOUT 1234

    r BGSAVE
    after 500
    global redis_fd port module_path
    catch {redis_command $redis_fd SHUTDOWN SAVE}
    catch {close $redis_fd}
    after 500

    start_redis $module_path $port
    set redis_fd [redis_connect localhost $port]

    set sug [r FT.SUGGET p27sug "red" WITHPAYLOADS]
    if {[lindex $sug 0] ne "redis" || [lindex $sug 1] ne "db"} {
      error "Suggest dict not persisted across restart: $sug"
    }
    set dict [r FT.DICTDUMP p27dict]
    if {[llength $dict] != 2} { error "Term dict not persisted: $dict" }
    set info_via_alias [r FT.INFO p27alias]
    if {[llength $info_via_alias] < 2} { error "Alias not persisted: $info_via_alias" }
    set cfg [r FT.CONFIG GET TIMEOUT]
    if {[lindex $cfg 1] ne "1234"} { error "Config not persisted: $cfg" }

    r FT.ALIASDEL p27alias
    r FT.DROPINDEX p27aux
  }
}

# ============================================================
# Phase 29: Partial Options & Runtime Lifecycle
# ============================================================
puts "\n=== Phase 29: Partial Options & Lifecycle ==="

test_assert "PHONETIC query matches Jon when searching John" {
  r FT.CREATE p29ph SCHEMA name TEXT PHONETIC dm:en
  r FT.ADD p29ph d1 FIELDS name "Jon"
  r FT.ADD p29ph d2 FIELDS name "Alice"
  set res [r FT.SEARCH p29ph "John"]
  if {[lindex $res 0] != 1} { error "Expected 1 phonetic match for John, got $res" }
  # VERBATIM disables phonetic expansion
  set res2 [r FT.SEARCH p29ph "John" VERBATIM]
  if {[lindex $res2 0] != 0} { error "Expected 0 matches with VERBATIM, got $res2" }
  r FT.DROPINDEX p29ph
}

test_assert "DIALECT 1 vs DIALECT 2 negation scoping differs" {
  r FT.CREATE p29dia SCHEMA a TAG b TAG
  r FT.ADD p29dia d1 FIELDS a x b y
  r FT.ADD p29dia d2 FIELDS a x b z
  r FT.ADD p29dia d3 FIELDS a w b y
  # DIALECT 2: -@a:{x} @b:{y} => (-a:x) AND b:y => only d3
  set r2 [r FT.SEARCH p29dia {-@a:{x} @b:{y}} DIALECT 2]
  if {[lindex $r2 0] != 1} { error "Expected 1 match in DIALECT 2, got $r2" }
  # DIALECT 1: -@a:{x} @b:{y} => -(a:x AND b:y) => d2 and d3
  set r1 [r FT.SEARCH p29dia {-@a:{x} @b:{y}} DIALECT 1]
  if {[lindex $r1 0] != 2} { error "Expected 2 matches in DIALECT 1, got $r1" }
  r FT.DROPINDEX p29dia
}

test_assert "FT.PROFILE supports SEARCH options and AGGREGATE pipeline" {
  r FT.CREATE p29prof SCHEMA cat TAG price NUMERIC
  r FT.ADD p29prof d1 FIELDS cat book price 10
  r FT.ADD p29prof d2 FIELDS cat book price 30
  r FT.ADD p29prof d3 FIELDS cat pen price 5

  set ps [r FT.PROFILE p29prof SEARCH QUERY "*" LIMIT 0 2]
  if {[llength [lindex $ps 0]] != 2} { error "Expected 2 limited results in SEARCH profile, got $ps" }

  set pa [r FT.PROFILE p29prof AGGREGATE QUERY "*" GROUPBY 1 @cat REDUCE COUNT 0 AS cnt SORTBY 2 @cnt DESC]
  if {[llength [lindex $pa 0]] != 2} { error "Expected 2 grouped rows in AGGREGATE profile, got $pa" }
  set pinfo [lindex $pa 1]
  if {[lsearch $pinfo "pipeline_time_us"] < 0} { error "Missing pipeline_time_us in AGGREGATE profile: $pinfo" }

  r FT.DROPINDEX p29prof
}

test_assert "FT.CURSOR MAXIDLE expires idle cursors" {
  r FT.CREATE p29cur SCHEMA tag TAG
  r FT.ADD p29cur d1 FIELDS tag a
  r FT.ADD p29cur d2 FIELDS tag a
  r FT.ADD p29cur d3 FIELDS tag a
  set res [r FT.AGGREGATE p29cur "*" WITHCURSOR COUNT 1 MAXIDLE 150]
  set cid [lindex $res 1]
  if {$cid == 0} { error "Expected non-zero cursor id" }
  after 250
  set got_err 0
  if {[catch {r FT.CURSOR READ p29cur $cid} err]} {
    set got_err 1
  }
  if {!$got_err} { error "Expected expired cursor to fail on READ" }
  r FT.DROPINDEX p29cur
}

test_assert "TEMPORARY index auto-expires after idle TTL" {
  r FT.CREATE p29tmp TEMPORARY 1 SCHEMA tag TAG
  r FT.ADD p29tmp d1 FIELDS tag a
  # Wait for maintenance timer (1s interval) + 1s TTL
  after 2300
  set got_err 0
  if {[catch {r FT.INFO p29tmp} err]} {
    set got_err 1
  }
  if {!$got_err} { error "Expected TEMPORARY index p29tmp to expire" }
}

# ============================================================
# Phase 30: Keyspace & Server Event Completeness
# ============================================================
puts "\n=== Phase 30: Keyspace & Server Events ==="

test_assert "ON HASH handles RENAME into, within, and out of prefix" {
  r FT.CREATE p30idx ON HASH PREFIX 1 p30: SCHEMA name TAG
  r HSET other:1 name alpha
  # Rename into prefix -> indexed
  r RENAME other:1 p30:1
  set r1 [r FT.SEARCH p30idx {@name:{alpha}}]
  if {[lindex $r1 0] != 1} { error "Expected 1 after RENAME into prefix, got $r1" }
  # Rename within prefix -> old removed, new indexed
  r RENAME p30:1 p30:2
  set r2 [r FT.SEARCH p30idx {@name:{alpha}} NOCONTENT]
  if {[lindex $r2 0] != 1 || [lindex $r2 1] ne "p30:2"} {
    error "Expected doc p30:2 after RENAME within prefix, got $r2"
  }
  # Rename out of prefix -> removed
  r RENAME p30:2 outside:2
  set r3 [r FT.SEARCH p30idx {@name:{alpha}}]
  if {[lindex $r3 0] != 0} { error "Expected 0 after RENAME out of prefix, got $r3" }
  r DEL outside:2
  r FT.DROPINDEX p30idx
}

test_assert "ON HASH handles UNLINK and HDEL of last indexed field" {
  r FT.CREATE p30del ON HASH PREFIX 1 p30d: SCHEMA name TAG
  r HSET p30d:1 name beta
  r HSET p30d:2 name gamma
  if {[lindex [r FT.SEARCH p30del "*"] 0] != 2} { error "Expected 2 docs initially" }

  # UNLINK removes doc
  r UNLINK p30d:1
  if {[lindex [r FT.SEARCH p30del "*"] 0] != 1} { error "Expected 1 doc after UNLINK" }

  # HDEL removing the only indexed field removes stale doc from index
  r HDEL p30d:2 name
  if {[lindex [r FT.SEARCH p30del "*"] 0] != 0} { error "Expected 0 docs after HDEL of last indexed field" }

  r DEL p30d:2
  r FT.DROPINDEX p30del
}

test_assert "FLUSHDB clears aliases and suggest/term dicts via server event" {
  r FT.CREATE p30f SCHEMA tag TAG
  r FT.ALIASADD p30fa p30f
  r FT.SUGADD p30fs "hello" 1.0
  r FT.DICTADD p30fd word1
  r FLUSHDB
  if {[r FT.SUGLEN p30fs] != 0} { error "Expected suggest dict cleared on FLUSHDB" }
  set got_err 0
  if {[catch {r FT.INFO p30fa}]} { set got_err 1 }
  if {!$got_err} { error "Expected alias cleared on FLUSHDB" }
}

# End-to-end ON JSON test when redis_json.so is built alongside redis_search.so
set json_so [file join [file dirname $module_path] "redis_json.so"]
if {[file exists $json_so]} {
  test_assert "ON JSON end-to-end with gemini-json (SET, MERGE, RENAME, UNLINK)" {
    set jport [find_free_port]
    catch {
      exec redis-server \
        --port $jport \
        --daemonize yes \
        --loglevel warning \
        --logfile /tmp/search_json_tcl.log \
        --dbfilename "" \
        --save "" \
        --loadmodule $json_so \
        --loadmodule $module_path
    }
    set jfd ""
    for {set i 0} {$i < 50} {incr i} {
      if {![catch {socket localhost $jport} fd]} {
        close $fd
        break
      }
      after 100
    }
    set jfd [redis_connect localhost $jport]
    redis_command $jfd FT.CREATE jidx ON JSON PREFIX 1 jdoc: SCHEMA \$.name AS name TAG \$.price AS price NUMERIC
    redis_command $jfd JSON.SET jdoc:1 \$ {{"name":"widget","price":25}}
    set jr1 [redis_command $jfd FT.SEARCH jidx {@name:{widget} @price:[20 30]}]
    if {[lindex $jr1 0] != 1} {
      catch {redis_command $jfd SHUTDOWN NOSAVE}
      catch {close $jfd}
      error "Expected 1 match after JSON.SET, got $jr1"
    }
    redis_command $jfd JSON.MERGE jdoc:1 \$ {{"price":99}}
    set jr2 [redis_command $jfd FT.SEARCH jidx {@price:[90 100]}]
    if {[lindex $jr2 0] != 1} {
      catch {redis_command $jfd SHUTDOWN NOSAVE}
      catch {close $jfd}
      error "Expected 1 match after JSON.MERGE, got $jr2"
    }
    redis_command $jfd RENAME jdoc:1 jdoc:2
    set jr3 [redis_command $jfd FT.SEARCH jidx {@name:{widget}} NOCONTENT]
    if {[lindex $jr3 0] != 1 || [lindex $jr3 1] ne "jdoc:2"} {
      catch {redis_command $jfd SHUTDOWN NOSAVE}
      catch {close $jfd}
      error "Expected jdoc:2 after RENAME, got $jr3"
    }
    redis_command $jfd UNLINK jdoc:2
    set jr4 [redis_command $jfd FT.SEARCH jidx {@name:{widget}}]
    catch {redis_command $jfd SHUTDOWN NOSAVE}
    catch {close $jfd}
    file delete -force /tmp/search_json_tcl.log
    if {[lindex $jr4 0] != 0} { error "Expected 0 matches after UNLINK, got $jr4" }
  }
}

# ============================================================
# Phase 31: RESP3 Dual-Protocol Verification
# ============================================================
puts "\n=== Phase 31: RESP3 Verification ==="

test_assert "RESP3 HELLO 3 works for FT.SEARCH, FT.AGGREGATE, FT.INFO, FT.PROFILE" {
  r FT.CREATE p31r3 SCHEMA cat TAG price NUMERIC
  r FT.ADD p31r3 d1 FIELDS cat book price 10
  r FT.ADD p31r3 d2 FIELDS cat book price 20

  global port
  set r3fd [redis_connect localhost $port]
  # Switch connection to RESP3
  puts -nonewline $r3fd "*2\r\n\$5\r\nHELLO\r\n\$1\r\n3\r\n"
  flush $r3fd
  # Drain HELLO 3 map response (read lines until FT.SEARCH works cleanly on a fresh ping)
  # Use a clean RESP3 request sequence: send PING to sync
  after 50
  fconfigure $r3fd -blocking 0
  set hello_raw [read $r3fd]
  fconfigure $r3fd -blocking 1
  if {[string length $hello_raw] == 0} {
    close $r3fd
    error "Empty HELLO 3 response"
  }

  # Send FT.SEARCH, FT.AGGREGATE, FT.INFO, FT.PROFILE over RESP3 connection
  # Valid FT.SEARCH over RESP3
  puts -nonewline $r3fd "*4\r\n\$9\r\nFT.SEARCH\r\n\$5\r\np31r3\r\n\$1\r\n*\r\n\$9\r\nNOCONTENT\r\n"
  flush $r3fd
  after 50
  fconfigure $r3fd -blocking 0
  set s_raw [read $r3fd]
  fconfigure $r3fd -blocking 1
  if {[string first "d1" $s_raw] < 0 || [string first "d2" $s_raw] < 0} {
    close $r3fd
    error "RESP3 FT.SEARCH missing expected docs: $s_raw"
  }

  # Valid FT.AGGREGATE over RESP3
  puts -nonewline $r3fd "*11\r\n\$12\r\nFT.AGGREGATE\r\n\$5\r\np31r3\r\n\$1\r\n*\r\n\$7\r\nGROUPBY\r\n\$1\r\n1\r\n\$4\r\n@cat\r\n\$6\r\nREDUCE\r\n\$5\r\nCOUNT\r\n\$1\r\n0\r\n\$2\r\nAS\r\n\$3\r\ncnt\r\n"
  flush $r3fd
  after 50
  fconfigure $r3fd -blocking 0
  set a_raw [read $r3fd]
  fconfigure $r3fd -blocking 1
  if {[string first "book" $a_raw] < 0 || [string first "cnt" $a_raw] < 0} {
    close $r3fd
    error "RESP3 FT.AGGREGATE missing expected group: $a_raw"
  }

  # Valid FT.INFO over RESP3
  puts -nonewline $r3fd "*2\r\n\$7\r\nFT.INFO\r\n\$5\r\np31r3\r\n"
  flush $r3fd
  after 50
  fconfigure $r3fd -blocking 0
  set i_raw [read $r3fd]
  fconfigure $r3fd -blocking 1
  if {[string first "p31r3" $i_raw] < 0} {
    close $r3fd
    error "RESP3 FT.INFO missing index name: $i_raw"
  }

  # Valid FT.PROFILE over RESP3
  puts -nonewline $r3fd "*5\r\n\$10\r\nFT.PROFILE\r\n\$5\r\np31r3\r\n\$6\r\nSEARCH\r\n\$5\r\nQUERY\r\n\$1\r\n*\r\n"
  flush $r3fd
  after 50
  fconfigure $r3fd -blocking 0
  set p_raw [read $r3fd]
  fconfigure $r3fd -blocking 1
  if {[string first "total_time_us" $p_raw] < 0} {
    close $r3fd
    error "RESP3 FT.PROFILE missing total_time_us: $p_raw"
  }

  close $r3fd
  r FT.DROPINDEX p31r3
}

# ============================================================
# Phase 32: Advanced Vector Search (VECTOR_RANGE, FLOAT64, EF_RUNTIME, AS)
# ============================================================
puts "\n=== Phase 32: Advanced Vector Search ==="

test_assert "VECTOR_RANGE filters by distance radius and yields custom distance alias" {
  r FT.CREATE p32vr SCHEMA cat TAG emb VECTOR FLAT DIM 2 DISTANCE_METRIC L2
  set v0 [binary format ff 0.0 0.0]
  set v1 [binary format ff 0.5 0.0]
  set v2 [binary format ff 5.0 5.0]
  r FT.ADD p32vr d0 FIELDS cat a emb $v0
  r FT.ADD p32vr d1 FIELDS cat a emb $v1
  r FT.ADD p32vr d2 FIELDS cat a emb $v2

  # Radius 1.0 around (0,0) matches d0 (dist=0) and d1 (dist=0.25), excludes d2 (dist=50)
  set res [r FT.SEARCH p32vr {@emb:[VECTOR_RANGE 1.0 $q]=>{$YIELD_DISTANCE_AS: dist}} PARAMS 2 q $v0]
  if {[lindex $res 0] != 2} { error "Expected 2 VECTOR_RANGE matches, got $res" }
  set flds [lindex $res 2]
  if {[lsearch $flds "dist"] < 0} { error "Expected yielded distance alias 'dist' in fields: $flds" }
  r FT.DROPINDEX p32vr
}

test_assert "VECTOR FLOAT64 type supports KNN and survives BGSAVE + restart" {
  r FT.CREATE p32f64 SCHEMA emb VECTOR HNSW TYPE FLOAT64 DIM 2 DISTANCE_METRIC L2 M 8 EF_CONSTRUCTION 64
  set d1 [binary format dd 1.0 0.0]
  set d2 [binary format dd 10.0 10.0]
  r FT.ADD p32f64 a1 FIELDS emb $d1
  r FT.ADD p32f64 a2 FIELDS emb $d2

  set res [r FT.SEARCH p32f64 {*=>[KNN 1 @emb $q EF_RUNTIME 50 AS my_dist]} PARAMS 2 q $d1]
  if {[lindex $res 0] != 1 || [lindex $res 1] ne "a1"} { error "Expected a1 for FLOAT64 KNN, got $res" }
  if {[lsearch [lindex $res 2] "my_dist"] < 0} { error "Expected custom AS alias 'my_dist', got $res" }

  # Verify FLOAT64 survives RDB restart
  r BGSAVE
  after 500
  global redis_fd port module_path
  catch {redis_command $redis_fd SHUTDOWN SAVE}
  catch {close $redis_fd}
  after 500
  start_redis $module_path $port
  set redis_fd [redis_connect localhost $port]

  set res2 [r FT.SEARCH p32f64 {*=>[KNN 1 @emb $q]} PARAMS 2 q $d1]
  if {[lindex $res2 0] != 1 || [lindex $res2 1] ne "a1"} { error "Expected a1 after restart, got $res2" }
  r FT.DROPINDEX p32f64
}

# ============================================================
# Phase 33: Advanced Schema & Indexing Options
# ============================================================
puts "\n=== Phase 33: Schema & Indexing Options ==="

test_assert "TAG SEPARATOR and CASESENSITIVE work properly" {
  r FT.CREATE p33tag SCHEMA tags TAG SEPARATOR ";" code TAG CASESENSITIVE
  r FT.ADD p33tag d1 FIELDS tags "Red;Green;Blue" code "AbC"
  r FT.ADD p33tag d2 FIELDS tags "Yellow" code "abc"

  # Default TAG is case-insensitive and split by ';'
  set r1 [r FT.SEARCH p33tag {@tags:{green}}]
  if {[lindex $r1 0] != 1} { error "Expected 1 match for green, got $r1" }
  # CASESENSITIVE distinguishes AbC vs abc
  set r2 [r FT.SEARCH p33tag {@code:{AbC}} NOCONTENT]
  if {[lindex $r2 0] != 1 || [lindex $r2 1] ne "d1"} { error "Expected only d1 for AbC, got $r2" }
  r FT.DROPINDEX p33tag
}

test_assert "SKIPINITIALSCAN skips pre-existing keys and FILTER filters indexed docs" {
  r HSET p33k:1 price 10
  r HSET p33k:2 price 200
  r FT.CREATE p33sk ON HASH PREFIX 1 p33k: SKIPINITIALSCAN FILTER "@price > 50" SCHEMA price NUMERIC
  # Pre-existing keys skipped
  if {[lindex [r FT.SEARCH p33sk "*"] 0] != 0} { error "Expected 0 docs with SKIPINITIALSCAN" }
  # New key failing filter is not indexed
  r HSET p33k:3 price 20
  if {[lindex [r FT.SEARCH p33sk "*"] 0] != 0} { error "Expected 0 docs when FILTER fails" }
  # New key passing filter is indexed
  r HSET p33k:4 price 150
  if {[lindex [r FT.SEARCH p33sk "*"] 0] != 1} { error "Expected 1 doc when FILTER passes" }
  # Updating key to fail filter removes it from index
  r HSET p33k:4 price 10
  if {[lindex [r FT.SEARCH p33sk "*"] 0] != 0} { error "Expected 0 docs after update fails FILTER" }
  r DEL p33k:1 p33k:2 p33k:3 p33k:4
  r FT.DROPINDEX p33sk
}

test_assert "FT.DROPINDEX DD deletes underlying indexed keys" {
  r FT.CREATE p33dd ON HASH PREFIX 1 p33d: SCHEMA name TAG
  r HSET p33d:1 name one
  r HSET p33d:2 name two
  r FT.DROPINDEX p33dd DD
  if {[r EXISTS p33d:1] != 0 || [r EXISTS p33d:2] != 0} {
    error "Expected FT.DROPINDEX DD to delete underlying keys"
  }
}

test_assert "FT.SEARCH FILTER numeric option filters with inclusive/exclusive bounds" {
  r FT.CREATE p33flt SCHEMA cat TAG price NUMERIC
  r FT.ADD p33flt d1 FIELDS cat a price 10
  r FT.ADD p33flt d2 FIELDS cat a price 50
  r FT.ADD p33flt d3 FIELDS cat a price 100
  set r1 [r FT.SEARCH p33flt "*" FILTER price 10 100]
  if {[lindex $r1 0] != 3} { error "Expected 3 for [10, 100], got $r1" }
  set r2 [r FT.SEARCH p33flt "*" FILTER price "(10" "(100"]
  if {[lindex $r2 0] != 1} { error "Expected 1 for (10, 100), got $r2" }
  r FT.DROPINDEX p33flt
}

# ============================================================
# Phase 34: Scoring & Result Enhancements
# ============================================================
puts "\n=== Phase 34: Scoring & Result Enhancements ==="

test_assert "RETURN AS renames projected fields" {
  r FT.CREATE p34ret SCHEMA title TEXT price NUMERIC
  r FT.ADD p34ret d1 FIELDS title "hello" price 42
  set res [r FT.SEARCH p34ret "*" RETURN 3 title AS heading]
  set flds [lindex $res 2]
  if {[lindex $flds 0] ne "heading" || [lindex $flds 1] ne "hello"} {
    error "Expected heading=hello, got $flds"
  }
  r FT.DROPINDEX p34ret
}

test_assert "SCORER, field WEIGHT, and EXPLAINSCORE work together" {
  r FT.CREATE p34sc SCHEMA title TEXT WEIGHT 5.0 body TEXT WEIGHT 1.0
  r FT.ADD p34sc d1 FIELDS title "redis" body "other"
  r FT.ADD p34sc d2 FIELDS title "other" body "redis"
  # Higher title WEIGHT ranks d1 before d2
  set res [r FT.SEARCH p34sc "redis" WITHSCORES EXPLAINSCORE SCORER DISMAX]
  if {[lindex $res 1] ne "d1"} { error "Expected weighted title match d1 first, got $res" }
  set flds [lindex $res 2]
  if {[lsearch $flds "__score_explanation"] < 0} {
    error "Expected __score_explanation in fields, got $flds"
  }
  r FT.DROPINDEX p34sc
}

test_assert "WITHSORTKEYS returns sort key alongside each doc" {
  r FT.CREATE p34sk SCHEMA price NUMERIC SORTABLE
  r FT.ADD p34sk d1 FIELDS price 30
  r FT.ADD p34sk d2 FIELDS price 10
  set res [r FT.SEARCH p34sk "*" SORTBY price ASC WITHSORTKEYS NOCONTENT]
  # [2, d2, "10", d1, "30"]
  if {[lindex $res 1] ne "d2" || [lindex $res 2] ne "10" ||
      [lindex $res 3] ne "d1" || [lindex $res 4] ne "30"} {
    error "Unexpected WITHSORTKEYS output: $res"
  }
  r FT.DROPINDEX p34sk
}

# ============================================================
# Phase 35: Multi-Language Stemming & Suffix/Infix Wildcards
# ============================================================
puts "\n=== Phase 35: Multi-Language Stemming & Wildcards ==="

test_assert "French, German, and Spanish stemming match inflected forms" {
  r FT.CREATE p35fr LANGUAGE french SCHEMA body TEXT
  r FT.ADD p35fr d1 FIELDS body "rapidement developpement"
  if {[lindex [r FT.SEARCH p35fr "rapide"] 0] != 1} { error "French stemmer failed for rapide/rapidement" }
  r FT.DROPINDEX p35fr

  r FT.CREATE p35de LANGUAGE german SCHEMA body TEXT
  r FT.ADD p35de d1 FIELDS body "freundlichkeit entwicklungen"
  if {[lindex [r FT.SEARCH p35de "freundlich"] 0] != 1} { error "German stemmer failed for freundlich/freundlichkeit" }
  r FT.DROPINDEX p35de

  r FT.CREATE p35es LANGUAGE spanish SCHEMA body TEXT
  r FT.ADD p35es d1 FIELDS body "rapidamente jugadores"
  if {[lindex [r FT.SEARCH p35es "rapida"] 0] != 1} { error "Spanish stemmer failed for rapida/rapidamente" }
  r FT.DROPINDEX p35es
}

test_assert "Suffix (*ing) and infix (*earc*) wildcards match terms" {
  r FT.CREATE p35wc SCHEMA body TEXT WITHSUFFIXTRIE
  r FT.ADD p35wc d1 FIELDS body "searching running"
  r FT.ADD p35wc d2 FIELDS body "walking talking"

  set r1 [r FT.SEARCH p35wc "*ing"]
  if {[lindex $r1 0] != 2} { error "Expected 2 suffix matches for *ing, got $r1" }
  set r2 [r FT.SEARCH p35wc "*earc*"]
  if {[lindex $r2 0] != 1} { error "Expected 1 infix match for *earc*, got $r2" }
  r FT.DROPINDEX p35wc
}

# ============================================================
# Phase 36: Close Remaining Plan v2 Loose Ends
# ============================================================
puts "\n=== Phase 36: Plan v2 Gap Closure ==="

test_assert "Synonyms expand automatically in FT.SEARCH" {
  r FT.CREATE p36syn SCHEMA body TEXT
  r FT.ADD p36syn d1 FIELDS body "automobile engine"
  r FT.SYNUPDATE p36syn g1 car automobile vehicle
  set res [r FT.SEARCH p36syn "car"]
  if {[lindex $res 0] != 1} { error "Expected synonym match for car -> automobile, got $res" }
  # VERBATIM disables synonym expansion
  set res2 [r FT.SEARCH p36syn "car" VERBATIM]
  if {[lindex $res2 0] != 0} { error "Expected 0 matches with VERBATIM, got $res2" }
  r FT.DROPINDEX p36syn
}

test_assert "LANGUAGE_FIELD selects per-document stemming language" {
  r FT.CREATE p36lf LANGUAGE_FIELD doc_lang SCHEMA doc_lang TAG body TEXT
  r FT.ADD p36lf d1 FIELDS doc_lang french body "rapidement"
  r FT.ADD p36lf d2 FIELDS doc_lang german body "freundlichkeit"
  set rfr [r FT.SEARCH p36lf "rapide" LANGUAGE french]
  if {[lindex $rfr 0] != 1} { error "Expected French doc match via LANGUAGE_FIELD, got $rfr" }
  set rde [r FT.SEARCH p36lf "freundlich" LANGUAGE german]
  if {[lindex $rde 0] != 1} { error "Expected German doc match via LANGUAGE_FIELD, got $rde" }
  r FT.DROPINDEX p36lf
}

test_assert "NOHL disables highlight and NOOFFSETS disables phrase matching" {
  r FT.CREATE p36nh NOHL SCHEMA body TEXT
  r FT.ADD p36nh d1 FIELDS body "hello world"
  set res [r FT.SEARCH p36nh "hello" HIGHLIGHT FIELDS 1 body]
  set flds [lindex $res 2]
  if {[string first "<b>" [lindex $flds 1]] >= 0} {
    error "Expected NOHL to suppress highlight tags, got $flds"
  }
  r FT.DROPINDEX p36nh

  r FT.CREATE p36no NOOFFSETS SCHEMA body TEXT
  r FT.ADD p36no d1 FIELDS body "hello world"
  set res2 [r FT.SEARCH p36no {"hello world"}]
  if {[lindex $res2 0] != 0} { error "Expected NOOFFSETS to disable phrase match, got $res2" }
  r FT.DROPINDEX p36no
}

test_assert "GEOSHAPE accepts FLAT and SPHERICAL coordinate system options" {
  r FT.CREATE p36gs SCHEMA area GEOSHAPE FLAT region GEOSHAPE SPHERICAL
  r FT.ADD p36gs d1 FIELDS area "POINT(5 5)" region "POINT(1 1)"
  set res [r FT.SEARCH p36gs {@area:[WITHIN $poly]} PARAMS 2 poly "POLYGON((0 0, 10 0, 10 10, 0 10, 0 0))"]
  if {[lindex $res 0] != 1} { error "Expected 1 match for GEOSHAPE FLAT, got $res" }
  r FT.DROPINDEX p36gs
}

test_assert "FT.CONFIG MAXEXPANSIONS and MINSTEMLEN take effect" {
  r FT.CREATE p36cfg SCHEMA body TEXT
  r FT.ADD p36cfg d1 FIELDS body "alpha1"
  r FT.ADD p36cfg d2 FIELDS body "alpha2"
  r FT.ADD p36cfg d3 FIELDS body "alpha3"
  r FT.CONFIG SET MAXEXPANSIONS 1
  set r1 [r FT.SEARCH p36cfg "alpha*"]
  r FT.CONFIG SET MAXEXPANSIONS 200
  if {[lindex $r1 0] != 1} { error "Expected MAXEXPANSIONS 1 to cap prefix matches to 1, got $r1" }

  r FT.ADD p36cfg d4 FIELDS body "running"
  r FT.CONFIG SET MINSTEMLEN 6
  set r2 [r FT.SEARCH p36cfg "run"]
  r FT.CONFIG SET MINSTEMLEN 3
  if {[lindex $r2 0] != 0} { error "Expected MINSTEMLEN 6 to skip stemming 3-char 'run', got $r2" }
  r FT.DROPINDEX p36cfg
}

# ============================================================
# Phase 40: Security & Boundary Hardening
# ============================================================
puts "\n=== Phase 40: Security & Boundary Hardening ==="

test_assert "Deeply nested query and expression rejected safely" {
  r FT.CREATE p40sec SCHEMA body TEXT num NUMERIC
  set deep_q "[string repeat "(" 100]hello[string repeat ")" 100]"
  set err1 0
  if {[catch {r FT.SEARCH p40sec $deep_q}]} { set err1 1 }
  if {!$err1} { error "Expected deeply nested query to be rejected" }

  set deep_e "[string repeat "(" 100]@num + 1[string repeat ")" 100]"
  set err2 0
  if {[catch {r FT.AGGREGATE p40sec "*" APPLY $deep_e AS x}]} { set err2 1 }
  if {!$err2} { error "Expected deeply nested expression to be rejected" }

  # Excessive vector DIM rejected
  set err3 0
  if {[catch {r FT.CREATE p40bad SCHEMA v VECTOR FLAT DIM 9999999 DISTANCE_METRIC L2}]} { set err3 1 }
  if {!$err3} { error "Expected excessive vector DIM to be rejected" }

  # NaN WEIGHT rejected
  set err4 0
  if {[catch {r FT.CREATE p40nan SCHEMA t TEXT WEIGHT nan}]} { set err4 1 }
  if {!$err4} { error "Expected NaN WEIGHT to be rejected" }

  r FT.DROPINDEX p40sec
}

# ============================================================
# Phase 41: Protocol & Ecosystem Alignment
# ============================================================
puts "\n=== Phase 41: Protocol & Ecosystem Alignment ==="

test_assert "LIMIT 0 0 returns total match count without docs" {
  r FT.CREATE p41lim SCHEMA title TEXT price NUMERIC SORTABLE
  r FT.ADD p41lim d1 FIELDS title "alpha" price 10
  r FT.ADD p41lim d2 FIELDS title "alpha beta" price 20
  r FT.ADD p41lim d3 FIELDS title "gamma" price 30

  set res [r FT.SEARCH p41lim "*" LIMIT 0 0]
  if {[llength $res] != 1 || [lindex $res 0] != 3} {
    error "Expected {3}, got {$res}"
  }
  set res2 [r FT.SEARCH p41lim "alpha" WITHSCORES LIMIT 0 0]
  if {[llength $res2] != 1 || [lindex $res2 0] != 2} {
    error "Expected {2} with WITHSCORES LIMIT 0 0, got {$res2}"
  }
  r FT.DROPINDEX p41lim
}

test_assert "FT.ADD PAYLOAD and FT.SEARCH WITHPAYLOADS" {
  r FT.CREATE p41pl SCHEMA title TEXT
  r FT.ADD p41pl d1 PAYLOAD "meta:one" FIELDS title "hello"
  r FT.ADD p41pl d2 FIELDS title "hello"

  set res [r FT.SEARCH p41pl "hello" WITHPAYLOADS NOCONTENT]
  if {[lindex $res 0] != 2} { error "Expected 2 docs, got $res" }
  # d1 -> meta:one, d2 -> (nil)
  if {[lindex $res 1] ne "d1" || [lindex $res 2] ne "meta:one"} {
    error "Expected d1 with payload meta:one, got $res"
  }
  if {[lindex $res 3] ne "d2" || [lindex $res 4] ne "(nil)"} {
    error "Expected d2 with (nil) payload, got $res"
  }
  r FT.DROPINDEX p41pl
}

test_assert "INDEXMISSING and INDEXEMPTY predicates work and validate schema" {
  r FT.CREATE p41me SCHEMA title TEXT INDEXMISSING INDEXEMPTY tag TAG
  r FT.ADD p41me d1 FIELDS title "present" tag "a"
  r FT.ADD p41me d2 FIELDS tag "b"
  r FT.ADD p41me d3 FIELDS title "" tag "c"

  set r1 [r FT.SEARCH p41me "ismissing(@title)" NOCONTENT]
  if {[lindex $r1 0] != 1 || [lindex $r1 1] ne "d2"} {
    error "Expected d2 for ismissing(@title), got $r1"
  }
  set r2 [r FT.SEARCH p41me "isempty(@title)" NOCONTENT]
  if {[lindex $r2 0] != 1 || [lindex $r2 1] ne "d3"} {
    error "Expected d3 for isempty(@title), got $r2"
  }
  set err_no_flag 0
  if {[catch {r FT.SEARCH p41me "ismissing(@tag)"}]} { set err_no_flag 1 }
  if {!$err_no_flag} { error "Expected error when INDEXMISSING not set on field" }

  r FT.DROPINDEX p41me
}

test_assert "Master-Replica PSYNC full sync and live command replication" {
  global port module_path redis_major

  # Create pre-existing state on master before replica connects (tests full PSYNC RDB + aux)
  r FT.CREATE p41rep SCHEMA title TEXT INDEXMISSING
  r FT.ADD p41rep d1 PAYLOAD "pl1" FIELDS title "hello"
  r FT.ADD p41rep d2 FIELDS title "world"
  r FT.ALIASADD p41alias p41rep
  r FT.SUGADD p41sug "redis" 10.0 PAYLOAD "db"
  r FT.DICTADD p41dict alpha beta

  set rep_port [find_free_port]
  set rep_rdb "search_tcl_replica_${rep_port}.rdb"
  set rep_log "/tmp/search_tcl_replica_${rep_port}.log"
  catch {
    exec redis-server \
      --port $rep_port \
      --daemonize yes \
      --loglevel warning \
      --logfile $rep_log \
      --dbfilename $rep_rdb \
      --dir /tmp \
      --loadmodule $module_path \
      --replicaof 127.0.0.1 $port
  }

  set rep_fd ""
  for {set i 0} {$i < 50} {incr i} {
    if {![catch {set rep_fd [redis_connect localhost $rep_port]}]} {
      break
    }
    after 100
  }
  if {$rep_fd eq ""} { error "Replica failed to start on port $rep_port" }

  # Wait for replica to complete full PSYNC
  set synced 0
  for {set i 0} {$i < 80} {incr i} {
    set info ""
    if {![catch {set info [redis_command $rep_fd INFO replication]}]} {
      if {[string match "*master_link_status:up*" $info] &&
          [string match "*master_sync_in_progress:0*" $info]} {
        set synced 1
        break
      }
    }
    after 100
  }
  if {!$synced} {
    catch {redis_command $rep_fd SHUTDOWN NOSAVE}
    catch {close $rep_fd}
    file delete -force "/tmp/$rep_rdb" $rep_log
    error "Replica did not complete PSYNC within timeout"
  }

  # Verify full PSYNC RDB (+ aux on Redis 7+) state on replica
  set rep_idx [expr {$redis_major >= 7 ? "p41alias" : "p41rep"}]
  set r_full [redis_command $rep_fd FT.SEARCH $rep_idx "hello" WITHPAYLOADS NOCONTENT]
  if {[lindex $r_full 0] != 1 || [lindex $r_full 1] ne "d1" || [lindex $r_full 2] ne "pl1"} {
    catch {redis_command $rep_fd SHUTDOWN NOSAVE}
    catch {close $rep_fd}
    file delete -force "/tmp/$rep_rdb" $rep_log
    error "Full PSYNC search/payload/alias mismatch on replica: $r_full"
  }
  if {$redis_major >= 7} {
    set r_sug [redis_command $rep_fd FT.SUGGET p41sug "red" WITHPAYLOADS]
    if {[lindex $r_sug 0] ne "redis" || [lindex $r_sug 1] ne "db"} {
      catch {redis_command $rep_fd SHUTDOWN NOSAVE}
      catch {close $rep_fd}
      file delete -force "/tmp/$rep_rdb" $rep_log
      error "Full PSYNC suggest dict mismatch on replica: $r_sug"
    }
  }

  # Live command replication after PSYNC
  r FT.ADD p41rep d3 PAYLOAD "pl3" FIELDS title "live"
  r FT.CREATE p41hash ON HASH PREFIX 1 p41h: SCHEMA name TEXT
  r HSET p41h:1 name "streamed"
  after 300

  set r_live [redis_command $rep_fd FT.SEARCH p41rep "live" WITHPAYLOADS NOCONTENT]
  set r_hash [redis_command $rep_fd FT.SEARCH p41hash "streamed" NOCONTENT]

  catch {redis_command $rep_fd SHUTDOWN NOSAVE}
  catch {close $rep_fd}
  file delete -force "/tmp/$rep_rdb" $rep_log

  r FT.ALIASDEL p41alias
  r FT.DROPINDEX p41rep
  r FT.DROPINDEX p41hash
  r DEL p41h:1

  if {[lindex $r_live 0] != 1 || [lindex $r_live 1] ne "d3" || [lindex $r_live 2] ne "pl3"} {
    error "Live FT.ADD replication mismatch on replica: $r_live"
  }
  if {[lindex $r_hash 0] != 1 || [lindex $r_hash 1] ne "p41h:1"} {
    error "Live ON HASH replication mismatch on replica: $r_hash"
  }
}

# ============================================================
# Phase 42: Hybrid Search (FT.HYBRID)
# ============================================================
puts "\n=== Phase 42: Hybrid Search (FT.HYBRID) ==="

test_assert "FT.HYBRID RRF boosts doc matching both text and vector" {
  r FT.CREATE p42hyb SCHEMA title TEXT tag TAG vec VECTOR FLAT DIM 2 DISTANCE_METRIC L2
  set v_close [binary format f* {1.0 0.0}]
  set v_mid   [binary format f* {0.9 0.1}]
  set v_far   [binary format f* {0.0 10.0}]

  # d1: exact text match only (far vector)
  r FT.ADD p42hyb d1 PAYLOAD "p1" FIELDS title "redis search engine redis" tag "db" vec $v_far
  # d2: strong in both text and vector -> should rank #1 in RRF
  r FT.ADD p42hyb d2 PAYLOAD "p2" FIELDS title "redis vector search" tag "ai" vec $v_mid
  # d3: closest vector only (no text match for redis)
  r FT.ADD p42hyb d3 PAYLOAD "p3" FIELDS title "unrelated document" tag "ai" vec $v_close

  # KNN 2 returns {d3, d2}, text returns {d1, d2} -> d2 is in BOTH branches and ranks #1
  set res [r FT.HYBRID p42hyb SEARCH "redis" VSIM @vec {$qv} KNN 2 COMBINE RRF PARAMS 2 qv $v_close WITHSCORES NOCONTENT]
  if {[lindex $res 0] != 3} { error "Expected 3 fused docs, got $res" }
  if {[lindex $res 1] ne "d2"} { error "Expected d2 ranked #1 by RRF overlap, got $res" }

  # LIMIT 0 0 returns total count only
  set cnt [r FT.HYBRID p42hyb SEARCH "redis" VSIM @vec {$qv} KNN 3 PARAMS 2 qv $v_close LIMIT 0 0]
  if {[llength $cnt] != 1 || [lindex $cnt 0] != 3} { error "Expected {3} for LIMIT 0 0, got $cnt" }

  # LIMIT 1 1 pagination
  set page [r FT.HYBRID p42hyb SEARCH "redis" VSIM @vec {$qv} KNN 3 PARAMS 2 qv $v_close LIMIT 1 1 NOCONTENT]
  if {[lindex $page 0] != 1} { error "Expected 1 doc on page, got $page" }
}

test_assert "FT.HYBRID LINEAR alpha switches ranking between text and vector" {
  set v_close [binary format f* {1.0 0.0}]

  # High alpha favors text (d1 has highest BM25 for 'redis')
  set rt [r FT.HYBRID p42hyb SEARCH "redis" VSIM @vec {$qv} KNN 3 COMBINE LINEAR ALPHA 0.95 PARAMS 2 qv $v_close NOCONTENT]
  if {[lindex $rt 1] ne "d1"} { error "Expected d1 #1 with ALPHA 0.95, got $rt" }

  # Low alpha favors vector (d3 has closest vector distance 0)
  set rv [r FT.HYBRID p42hyb SEARCH "redis" VSIM @vec {$qv} KNN 3 COMBINE LINEAR ALPHA 0.05 PARAMS 2 qv $v_close NOCONTENT]
  if {[lindex $rv 1] ne "d3"} { error "Expected d3 #1 with ALPHA 0.05, got $rv" }
}

test_assert "FT.HYBRID supports RRF weights, VSIM FILTER, RETURN AS, WITHPAYLOADS, EXPLAINSCORE, alias" {
  set v_close [binary format f* {1.0 0.0}]
  r FT.ALIASADD p42alias p42hyb

  # VSIM FILTER restricts vector branch to tag:db (only d1), while SEARCH matches d1 & d2
  set rf [r FT.HYBRID p42alias SEARCH "redis" VSIM @vec {$qv} KNN 3 FILTER "@tag:{db}" COMBINE RRF CONSTANT 10 TEXT_WEIGHT 1.0 VECTOR_WEIGHT 2.0 PARAMS 2 qv $v_close RETURN 3 title AS t WITHSCORES EXPLAINSCORE WITHPAYLOADS LIMIT 0 1]
  if {[lindex $rf 0] != 1} { error "Expected 1 doc, got $rf" }
  if {[lindex $rf 1] ne "d1"} { error "Expected d1 #1 with filtered VSIM, got $rf" }
  if {[lindex $rf 2] ne "p1"} { error "Expected payload p1, got $rf" }
  set fields [lindex $rf 3]
  if {[lsearch -exact $fields "__hybrid_score"] < 0} { error "Missing __hybrid_score: $fields" }
  if {[lsearch -exact $fields "__score_explanation"] < 0} { error "Missing __score_explanation: $fields" }
  if {[lsearch -exact $fields "t"] < 0} { error "Missing RETURN AS alias t: $fields" }

  r FT.ALIASDEL p42alias
  r FT.DROPINDEX p42hyb
}

test_assert "FT.HYBRID works with HNSW EF_RUNTIME and FLOAT64 vectors" {
  r FT.CREATE p42hnsw SCHEMA body TEXT v VECTOR HNSW TYPE FLOAT64 DIM 2 DISTANCE_METRIC COSINE M 8 EF_CONSTRUCTION 64
  set d1v [binary format q* {1.0 0.0}]
  set d2v [binary format q* {0.0 1.0}]
  r FT.ADD p42hnsw h1 FIELDS body "neural network" v $d1v
  r FT.ADD p42hnsw h2 FIELDS body "graph database" v $d2v

  set res [r FT.HYBRID p42hnsw SEARCH "neural" VSIM @v {$q} KNN 2 EF_RUNTIME 32 PARAMS 2 q $d1v NOCONTENT]
  if {[lindex $res 0] != 2 || [lindex $res 1] ne "h1"} {
    error "Expected h1 #1 for HNSW FLOAT64 hybrid, got $res"
  }
  r FT.DROPINDEX p42hnsw
}

test_assert "FT.HYBRID error validation" {
  r FT.CREATE p42err SCHEMA title TEXT v VECTOR FLAT DIM 2 DISTANCE_METRIC L2
  set good_v [binary format f* {1.0 0.0}]
  set bad_v  [binary format f* {1.0 0.0 0.0}]

  foreach bad_cmd [list \
    {FT.HYBRID no_such_idx SEARCH "a" VSIM @v $q PARAMS 2 q x} \
    {FT.HYBRID p42err SEARCH "a" PARAMS 2 q x} \
    {FT.HYBRID p42err VSIM @v $q PARAMS 2 q x} \
    {FT.HYBRID p42err SEARCH "a" VSIM @title $q PARAMS 2 q x} \
    {FT.HYBRID p42err SEARCH "a" VSIM @v $missing PARAMS 2 q x} \
    {FT.HYBRID p42err SEARCH "a" VSIM @v $q COMBINE LINEAR ALPHA 1.5 PARAMS 2 q x} \
    {FT.HYBRID p42err SEARCH "a" VSIM @v $q COMBINE RRF CONSTANT 0 PARAMS 2 q x} \
    {FT.HYBRID p42err SEARCH "a" VSIM @v $q COMBINE BADMETHOD PARAMS 2 q x} \
  ] {
    set failed 0
    if {[catch {r {*}$bad_cmd}]} { set failed 1 }
    if {!$failed} { error "Expected error for: $bad_cmd" }
  }
  # Dimension mismatch
  set dm_err 0
  if {[catch {r FT.HYBRID p42err SEARCH "a" VSIM @v {$q} PARAMS 2 q $bad_v}]} { set dm_err 1 }
  if {!$dm_err} { error "Expected dimension mismatch error" }

  r FT.DROPINDEX p42err
}

# ============================================================
# Phase 43: High-Perf HNSW, FLOAT16/BFLOAT16, SQ8 Quantization
# ============================================================
puts "\n=== Phase 43: HNSW, FLOAT16/BFLOAT16 & SQ8 ==="

test_assert "VECTOR FLAT TYPE FLOAT16 and BFLOAT16 KNN search" {
  # IEEE 754 float16: 1.0 = 0x3C00, 0.0 = 0x0000
  set f16_10 [binary format s* {0x3C00 0x0000}]
  set f16_01 [binary format s* {0x0000 0x3C00}]
  r FT.CREATE p43f16 SCHEMA v VECTOR FLAT TYPE FLOAT16 DIM 2 DISTANCE_METRIC L2
  r FT.ADD p43f16 a FIELDS v $f16_10
  r FT.ADD p43f16 b FIELDS v $f16_01

  set res [r FT.SEARCH p43f16 {*=>[KNN 1 @v $q]} PARAMS 2 q $f16_10 NOCONTENT]
  if {[lindex $res 0] != 1 || [lindex $res 1] ne "a"} {
    error "Expected a for FLOAT16 KNN, got $res"
  }
  r FT.DROPINDEX p43f16

  # BFloat16: 1.0 = 0x3F80, 0.0 = 0x0000
  set bf16_10 [binary format s* {0x3F80 0x0000}]
  set bf16_01 [binary format s* {0x0000 0x3F80}]
  r FT.CREATE p43bf16 SCHEMA v VECTOR HNSW TYPE BFLOAT16 DIM 2 DISTANCE_METRIC COSINE M 8 EF_CONSTRUCTION 64
  r FT.ADD p43bf16 x FIELDS v $bf16_10
  r FT.ADD p43bf16 y FIELDS v $bf16_01

  set res2 [r FT.SEARCH p43bf16 {*=>[KNN 1 @v $q]} PARAMS 2 q $bf16_01 NOCONTENT]
  if {[lindex $res2 0] != 1 || [lindex $res2 1] ne "y"} {
    error "Expected y for BFLOAT16 HNSW KNN, got $res2"
  }
  r FT.DROPINDEX p43bf16
}

test_assert "VECTOR QUANTIZATION SQ8 works on FLAT and HNSW and rejects bad params" {
  set v1 [binary format f* {1.0 0.0 0.0 0.0}]
  set v2 [binary format f* {0.0 1.0 0.0 0.0}]
  set v3 [binary format f* {0.9 0.1 0.0 0.0}]

  r FT.CREATE p43sq8f SCHEMA v VECTOR FLAT DIM 4 DISTANCE_METRIC L2 QUANTIZATION SQ8
  r FT.ADD p43sq8f d1 FIELDS v $v1
  r FT.ADD p43sq8f d2 FIELDS v $v2
  r FT.ADD p43sq8f d3 FIELDS v $v3
  set rf [r FT.SEARCH p43sq8f {*=>[KNN 2 @v $q]} PARAMS 2 q $v1 NOCONTENT]
  if {[lindex $rf 1] ne "d1" || [lindex $rf 2] ne "d3"} {
    error "Expected {d1 d3} for FLAT SQ8, got $rf"
  }
  r FT.DROPINDEX p43sq8f

  r FT.CREATE p43sq8h SCHEMA v VECTOR HNSW DIM 4 DISTANCE_METRIC L2 QUANTIZATION SQ8 M 8 EF_CONSTRUCTION 64
  r FT.ADD p43sq8h d1 FIELDS v $v1
  r FT.ADD p43sq8h d2 FIELDS v $v2
  r FT.ADD p43sq8h d3 FIELDS v $v3
  set rh [r FT.SEARCH p43sq8h {*=>[KNN 2 @v $q]} PARAMS 2 q $v1 NOCONTENT]
  if {[lindex $rh 1] ne "d1" || [lindex $rh 2] ne "d3"} {
    error "Expected {d1 d3} for HNSW SQ8, got $rh"
  }
  r FT.DROPINDEX p43sq8h

  # Invalid TYPE and QUANTIZATION rejected
  set e1 0
  if {[catch {r FT.CREATE p43bad1 SCHEMA v VECTOR FLAT TYPE INT4 DIM 2 DISTANCE_METRIC L2}]} { set e1 1 }
  set e2 0
  if {[catch {r FT.CREATE p43bad2 SCHEMA v VECTOR FLAT DIM 2 DISTANCE_METRIC L2 QUANTIZATION PQ99}]} { set e2 1 }
  if {!$e1 || !$e2} { error "Expected errors for invalid TYPE/QUANTIZATION" }
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
