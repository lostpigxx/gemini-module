#!/usr/bin/env tclsh
#
# TCL integration tests for the redis_json module.
# Self-contained: starts a redis-server, loads the module, runs tests, shuts down.
#
# Usage: tclsh json_test.tcl [path/to/redis_json.so]

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
      --logfile /tmp/json_tcl_test.log \
      --dbfilename json_tcl_test.rdb \
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
    set f [open /tmp/json_tcl_test.log r]
    puts "Redis log:\n[read $f]"
    close $f
  }
  error "redis-server failed to start on port $port"
}

proc stop_redis {fd} {
  catch {redis_command $fd SHUTDOWN NOSAVE}
  catch {close $fd}
  after 200
  file delete -force /tmp/json_tcl_test.rdb
  file delete -force /tmp/json_tcl_test.log
}

# ============================================================
# Resolve module path
# ============================================================

if {$argc > 0} {
  set module_path [file normalize [lindex $argv 0]]
} else {
  set script_dir [file dirname [file normalize [info script]]]
  set module_path [file normalize "$script_dir/../../../../build/redis_json.so"]
}

if {![file exists $module_path]} {
  puts "ERROR: Module not found at $module_path"
  puts "Usage: tclsh json_test.tcl \[path/to/redis_json.so\]"
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

puts "=== JSON.SET / JSON.GET ==="

test "JSON.SET root object" {
  r JSON.SET doc1 $ {{"name":"Alice","age":30}}
} {OK}

test "JSON.GET root" {
  r JSON.GET doc1 $
} {[{"name":"Alice","age":30}]}

test "JSON.GET nested key" {
  r JSON.GET doc1 $.name
} {["Alice"]}

test "JSON.GET legacy path" {
  r JSON.GET doc1 .name
} {"Alice"}

test "JSON.SET nested value" {
  r JSON.SET doc1 $.age 31
} {OK}

test "JSON.GET after nested set" {
  r JSON.GET doc1 $.age
} {[31]}

test "JSON.SET new key in object" {
  r JSON.SET doc1 $.email {{"addr":"alice@example.com"}}
} {OK}

test_assert "JSON.GET new key exists" {
  set result [r JSON.GET doc1 $.email]
  # Should contain addr
  if {[string first "addr" $result] < 0} {
    error "Expected email object, got: $result"
  }
}

test "JSON.SET with NX on existing path" {
  r JSON.SET doc1 $.name {"Bob"} NX
} {(nil)}

test "JSON.SET with NX on new path" {
  r JSON.SET doc1 $.nickname {"Ali"} NX
} {OK}

test "JSON.SET with XX on existing path" {
  r JSON.SET doc1 $.name {"Bob"} XX
} {OK}

test "JSON.SET with XX on missing path" {
  r JSON.SET doc1 $.missing {"val"} XX
} {(nil)}

test "JSON.SET with XX on missing key" {
  r JSON.SET nokey $ {"val"} XX
} {(nil)}

test_error "JSON.SET non-root on empty key" {
  r JSON.SET newkey $.a 1
} {ERR*root*}

puts "\n=== JSON.DEL ==="

test "JSON.DEL nested key" {
  r JSON.SET deldoc $ {{"a":1,"b":2,"c":3}}
  r JSON.DEL deldoc $.b
} {1}

test "JSON.GET after del" {
  r JSON.GET deldoc $
} {[{"a":1,"c":3}]}

test "JSON.DEL root" {
  r JSON.DEL deldoc
} {1}

test_assert "Key removed after root del" {
  set result [r EXISTS deldoc]
  if {$result != 0} { error "Key still exists" }
}

test "JSON.DEL on missing key returns 0" {
  r JSON.DEL nokey
} {0}

test "JSON.FORGET alias works" {
  r JSON.SET deldoc2 $ {{"a":1}}
  r JSON.FORGET deldoc2 $.a
} {1}

puts "\n=== JSON.TYPE ==="

test "JSON.TYPE object" {
  r JSON.SET typedoc $ {{"str":"hello","num":42,"arr":[1],"bool":true,"nil":null}}
  r JSON.TYPE typedoc .
} {object}

test "JSON.TYPE string" {
  r JSON.TYPE typedoc .str
} {string}

test "JSON.TYPE integer" {
  r JSON.TYPE typedoc .num
} {integer}

test "JSON.TYPE array" {
  r JSON.TYPE typedoc .arr
} {array}

test "JSON.TYPE boolean" {
  r JSON.TYPE typedoc .bool
} {boolean}

test "JSON.TYPE null" {
  r JSON.TYPE typedoc .nil
} {null}

puts "\n=== JSON.ARRAPPEND ==="

test "JSON.ARRAPPEND to existing array" {
  r JSON.SET arrdoc $ {{"arr":[1,2]}}
  r JSON.ARRAPPEND arrdoc .arr 3
} {3}

test "JSON.GET after arrappend" {
  r JSON.GET arrdoc .arr
} {[1,2,3]}

test "JSON.ARRAPPEND multiple values" {
  r JSON.ARRAPPEND arrdoc .arr 4 5
} {5}

puts "\n=== JSON.ARRLEN ==="

test "JSON.ARRLEN" {
  r JSON.ARRLEN arrdoc .arr
} {5}

test "JSON.ARRLEN on missing key" {
  r JSON.ARRLEN typedoc .str
} {(nil)}

puts "\n=== JSON.ARRINDEX ==="

test "JSON.ARRINDEX found" {
  r JSON.ARRINDEX arrdoc .arr 3
} {2}

test "JSON.ARRINDEX not found" {
  r JSON.ARRINDEX arrdoc .arr 99
} {-1}

test "JSON.ARRINDEX with start" {
  r JSON.ARRINDEX arrdoc .arr 1 1
} {-1}

puts "\n=== JSON.ARRINSERT ==="

test "JSON.ARRINSERT at beginning" {
  r JSON.SET insdoc $ {{"arr":[2,3]}}
  r JSON.ARRINSERT insdoc .arr 0 1
} {3}

test "JSON.GET after arrinsert" {
  r JSON.GET insdoc .arr
} {[1,2,3]}

puts "\n=== JSON.ARRPOP ==="

test "JSON.ARRPOP last element" {
  r JSON.SET popdoc $ {{"arr":[1,2,3]}}
  r JSON.ARRPOP popdoc .arr
} {3}

test "JSON.ARRPOP first element" {
  r JSON.ARRPOP popdoc .arr 0
} {1}

test "JSON.ARRLEN after pops" {
  r JSON.ARRLEN popdoc .arr
} {1}

puts "\n=== JSON.ARRTRIM ==="

test "JSON.ARRTRIM" {
  r JSON.SET trimdoc $ {{"arr":[0,1,2,3,4]}}
  r JSON.ARRTRIM trimdoc .arr 1 3
} {3}

test "JSON.GET after trim" {
  r JSON.GET trimdoc .arr
} {[1,2,3]}

puts "\n=== JSON.STRAPPEND / JSON.STRLEN ==="

test "JSON.STRAPPEND" {
  r JSON.SET strdoc $ {{"msg":"hello"}}
  r JSON.STRAPPEND strdoc .msg {" world"}
} {11}

test "JSON.GET after strappend" {
  r JSON.GET strdoc .msg
} {"hello world"}

test "JSON.STRLEN" {
  r JSON.STRLEN strdoc .msg
} {11}

puts "\n=== JSON.NUMINCRBY ==="

test "JSON.NUMINCRBY integer" {
  r JSON.SET numdoc $ {{"count":10,"price":9.99}}
  r JSON.NUMINCRBY numdoc .count 5
} {15}

test "JSON.NUMINCRBY float" {
  r JSON.NUMINCRBY numdoc .price 0.01
} {10}

test "JSON.NUMINCRBY negative" {
  r JSON.NUMINCRBY numdoc .count -3
} {12}

puts "\n=== JSON.NUMMULTBY ==="

test "JSON.NUMMULTBY" {
  r JSON.NUMMULTBY numdoc .count 2
} {24}

puts "\n=== JSON.OBJKEYS / JSON.OBJLEN ==="

test "JSON.OBJLEN" {
  r JSON.SET objdoc $ {{"a":1,"b":2,"c":3}}
  r JSON.OBJLEN objdoc .
} {3}

test "JSON.OBJKEYS" {
  r JSON.OBJKEYS objdoc .
} {a b c}

puts "\n=== JSON.CLEAR ==="

test "JSON.CLEAR on array" {
  r JSON.SET cleardoc $ {{"arr":[1,2,3],"num":42}}
  r JSON.CLEAR cleardoc $.arr
} {1}

test "JSON.ARRLEN after clear" {
  r JSON.ARRLEN cleardoc .arr
} {0}

puts "\n=== JSON.TOGGLE ==="

test "JSON.TOGGLE true->false" {
  r JSON.SET togdoc $ {{"flag":true}}
  r JSON.TOGGLE togdoc .flag
} {false}

test "JSON.TOGGLE false->true" {
  r JSON.TOGGLE togdoc .flag
} {true}

puts "\n=== JSON.MGET ==="

test "JSON.MGET multiple keys" {
  r JSON.SET mget1 $ {{"val":1}}
  r JSON.SET mget2 $ {{"val":2}}
  r JSON.MGET mget1 mget2 .val
} {1 2}

puts "\n=== JSON.MSET ==="

test "JSON.MSET multiple keys" {
  r JSON.MSET mset1 $ {{"a":1}} mset2 $ {{"b":2}}
} {OK}

test_assert "JSON.MSET values correct" {
  set v1 [r JSON.GET mset1 .a]
  set v2 [r JSON.GET mset2 .b]
  if {$v1 != 1} { error "mset1.a = $v1, expected 1" }
  if {$v2 != 2} { error "mset2.b = $v2, expected 2" }
}

puts "\n=== JSON.MERGE ==="

test "JSON.MERGE add new key" {
  r JSON.SET mergedoc $ {{"a":1}}
  r JSON.MERGE mergedoc $ {{"b":2}}
} {OK}

test_assert "JSON.MERGE result" {
  set result [r JSON.GET mergedoc .b]
  if {$result != 2} { error "Expected 2, got $result" }
}

test "JSON.MERGE delete with null" {
  r JSON.MERGE mergedoc $ {{"a":null}}
} {OK}

test "JSON.DEL after merge null" {
  r JSON.GET mergedoc $.a
} {[]}

puts "\n=== JSON.DEBUG ==="

test_assert "JSON.DEBUG MEMORY returns positive number" {
  r JSON.SET debugdoc $ {{"key":"value"}}
  set mem [r JSON.DEBUG MEMORY debugdoc]
  if {$mem <= 0} { error "Expected positive memory, got $mem" }
}

test "JSON.DEBUG HELP returns array" {
  set result [r JSON.DEBUG HELP]
  llength $result
} {4}

puts "\n=== JSON.RESP ==="

test_assert "JSON.RESP returns RESP representation" {
  r JSON.SET respdoc $ {{"key":"value"}}
  set result [r JSON.RESP respdoc .]
  set first [lindex $result 0]
  set lbrace "\{"
  if {$first ne $lbrace} {
    error "Expected first element to be left brace, got $first"
  }
}

puts "\n=== Wrong type errors ==="

test_error "JSON.GET on string key" {
  r SET str_key hello
  r JSON.GET str_key
} {WRONGTYPE*}

test_error "JSON.SET on string key" {
  r JSON.SET str_key $ {{"a":1}}
} {WRONGTYPE*}

puts "\n=== Edge cases ==="

test "JSON.SET empty object" {
  r JSON.SET empty_obj $ {{}}
} {OK}

test "JSON.SET empty array" {
  r JSON.SET empty_arr $ {[]}
} {OK}

test "JSON.SET null value" {
  r JSON.SET null_doc $ null
} {OK}

test "JSON.TYPE null doc" {
  r JSON.TYPE null_doc .
} {null}

test "JSON.SET deeply nested" {
  r JSON.SET deep $ {{"a":{"b":{"c":{"d":42}}}}}
} {OK}

test "JSON.GET deeply nested" {
  r JSON.GET deep .a.b.c.d
} {42}

test "JSON.SET with unicode" {
  r JSON.SET unicode $ {{"emoji":"Hello"}}
} {OK}

puts "\n=== JSONPath wildcard / recursive ==="

test_assert "JSONPath wildcard returns all array elements" {
  r JSON.SET wcdoc $ {{"arr":[1,2,3]}}
  set result [r JSON.GET wcdoc {$.arr[*]}]
  if {$result ne {[1,2,3]}} {
    error "Expected \[1,2,3\], got $result"
  }
}

test_assert "JSONPath recursive descent finds all prices" {
  r JSON.SET storedoc $ {{"store":{"book":[{"price":8.95},{"price":12.99}],"bicycle":{"price":19.95}}}}
  set result [r JSON.GET storedoc {$..price}]
  # Should be an array with 3 prices
  if {[string first "8.95" $result] < 0} {
    error "Expected 8.95 in result: $result"
  }
  if {[string first "12.99" $result] < 0} {
    error "Expected 12.99 in result: $result"
  }
  if {[string first "19.95" $result] < 0} {
    error "Expected 19.95 in result: $result"
  }
}

puts "\n=== JSONPath filter expressions ==="

test_assert "Filter existence check" {
  r JSON.SET filterdoc $ {{"items":[{"name":"a","stock":5},{"name":"b"},{"name":"c","stock":0}]}}
  set result [r JSON.GET filterdoc {$.items[?(@.stock)]}]
  if {[string first "a" $result] < 0} { error "Expected 'a' in result: $result" }
  if {[string first "c" $result] < 0} { error "Expected 'c' in result: $result" }
}

test_assert "Filter equality" {
  r JSON.SET filterdoc2 $ {{"arr":[{"x":1},{"x":2},{"x":3}]}}
  set result [r JSON.GET filterdoc2 {$.arr[?(@.x == 2)]}]
  if {$result ne {[{"x":2}]}} { error "Expected \[{\"x\":2}\], got $result" }
}

test_assert "Filter greater than" {
  set result [r JSON.GET filterdoc2 {$.arr[?(@.x > 1)]}]
  if {[string first "\"x\":2" $result] < 0} { error "Missing x:2 in result: $result" }
  if {[string first "\"x\":3" $result] < 0} { error "Missing x:3 in result: $result" }
}

test_assert "Filter string comparison" {
  r JSON.SET storedoc2 $ {{"store":{"book":[{"category":"reference","price":8.95},{"category":"fiction","price":12.99}]}}}
  set result [r JSON.GET storedoc2 {$.store.book[?(@.category == 'fiction')]}]
  if {[string first "fiction" $result] < 0} { error "Expected fiction in: $result" }
  if {[string first "reference" $result] >= 0} { error "Unexpected reference in: $result" }
}

puts "\n=== JSONPath union syntax ==="

test_assert "Union index" {
  r JSON.SET uniondoc $ {[10,20,30,40,50]}
  set result [r JSON.GET uniondoc {$[0,2,4]}]
  if {$result ne {[10,30,50]}} { error "Expected \[10,30,50\], got $result" }
}

test_assert "Union key" {
  r JSON.SET uniondoc2 $ {{"a":1,"b":2,"c":3}}
  set result [r JSON.GET uniondoc2 {$['a','c']}]
  if {$result ne {[1,3]}} { error "Expected \[1,3\], got $result" }
}

test_assert "Union in chain" {
  r JSON.SET uniondoc3 $ {{"items":[{"name":"x"},{"name":"y"},{"name":"z"}]}}
  set result [r JSON.GET uniondoc3 {$.items[0,2].name}]
  if {$result ne {["x","z"]}} { error "Expected \[\"x\",\"z\"\], got $result" }
}

puts "\n=== RDB persistence ==="

test_assert "Data survives BGSAVE + restart" {
  r DEL persist_test
  r JSON.SET persist_test $ {{"name":"Alice","scores":[10,20,30]}}

  r BGSAVE
  after 2000

  global redis_fd port module_path
  catch {redis_command $redis_fd SHUTDOWN SAVE}
  catch {close $redis_fd}
  after 1000

  start_redis $module_path $port
  set redis_fd [redis_connect localhost $port]

  set name [r JSON.GET persist_test .name]
  set scores [r JSON.ARRLEN persist_test .scores]

  if {$name ne {"Alice"}} {
    error "Name mismatch after restart: $name"
  }
  if {$scores != 3} { error "Scores length=$scores after restart, expected 3" }
}

puts "\n=== Redis 7 ModuleType & Master-Replica PSYNC ==="

test_assert "MEMORY USAGE grows with document size" {
  r JSON.SET mem_small $ {{"a":1}}
  r JSON.SET mem_big $ {{"a":"[string repeat x 4096]","arr":[1,2,3,4,5,6,7,8,9,10]}}
  set m1 [r MEMORY USAGE mem_small]
  set m2 [r MEMORY USAGE mem_big]
  if {$m2 <= $m1} { error "Expected mem_big ($m2) > mem_small ($m1)" }
  r DEL mem_small mem_big
}

test_assert "COPY clones JSON key independently (Redis 7+)" {
  set srv_info [r INFO server]
  set rver 0
  if {[regexp {redis_version:(\d+)\.} $srv_info -> maj]} { set rver $maj }
  if {$rver >= 7} {
    r JSON.SET copy_src $ {{"x":1,"arr":[10,20]}}
    r COPY copy_src copy_dst
    r JSON.NUMINCRBY copy_dst .x 99
    set v_src [r JSON.GET copy_src .x]
    set v_dst [r JSON.GET copy_dst .x]
    if {$v_src != 1 || $v_dst != 100} {
      error "COPY not independent: src=$v_src dst=$v_dst"
    }
    r DEL copy_src copy_dst
  }
}

test_assert "Master-Replica PSYNC full sync and live JSON replication" {
  global port module_path

  r JSON.SET rep_pre $ {{"name":"before","n":10,"tags":["a"]}}

  set rep_port [find_free_port]
  set rep_rdb "json_tcl_replica_${rep_port}.rdb"
  set rep_log "/tmp/json_tcl_replica_${rep_port}.log"
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
    if {![catch {set rep_fd [redis_connect localhost $rep_port]}]} { break }
    after 100
  }
  if {$rep_fd eq ""} { error "Replica failed to start on port $rep_port" }

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

  # Verify full PSYNC RDB state
  set pre_name [redis_command $rep_fd JSON.GET rep_pre .name]

  # Live replication after PSYNC
  r JSON.NUMINCRBY rep_pre .n 5
  r JSON.ARRAPPEND rep_pre .tags {"b"}
  r JSON.SET rep_live $ {{"ok":true}}
  after 300

  set live_n [redis_command $rep_fd JSON.GET rep_pre .n]
  set live_len [redis_command $rep_fd JSON.ARRLEN rep_pre .tags]
  set live_ok [redis_command $rep_fd JSON.GET rep_live .ok]

  catch {redis_command $rep_fd SHUTDOWN NOSAVE}
  catch {close $rep_fd}
  file delete -force "/tmp/$rep_rdb" $rep_log
  r DEL rep_pre rep_live

  if {$pre_name ne {"before"}} { error "Full PSYNC mismatch: $pre_name" }
  if {$live_n != 15 || $live_len != 2 || $live_ok ne "true"} {
    error "Live replication mismatch: n=$live_n len=$live_len ok=$live_ok"
  }
}

puts "\n=== Phase 1: Safety, Boundaries & Correctness ==="

test_assert "JSON.DEL overlapping recursive path does not crash" {
  r JSON.SET ovdoc $ {{"a":{"b":{"c":1}},"d":[10,20,30]}}
  set n [r JSON.DEL ovdoc {$..*}]
  if {$n < 2} { error "Expected >= 2 deleted top-level items, got $n" }
  set res [r JSON.GET ovdoc $]
  if {$res ne {[{}]}} { error "Expected \[{}], got $res" }
  r DEL ovdoc
}

test_assert "JSON.DEL across multiple arrays deletes correct indices" {
  r JSON.SET multiarr $ {{"a":[0,1,2,3],"b":[10,11,12,13]}}
  set n [r JSON.DEL multiarr {$..[0,2]}]
  if {$n != 4} { error "Expected 4 deleted, got $n" }
  set a [r JSON.GET multiarr .a]
  set b [r JSON.GET multiarr .b]
  if {$a ne {[1,3]} || $b ne {[11,13]}} { error "Unexpected arrays: a=$a b=$b" }
  r DEL multiarr
}

test_assert "Empty string object key set, get, and del" {
  r JSON.SET emptyk $ {{"":1,"a":2}}
  set v [r JSON.GET emptyk {$[""]}]
  if {$v ne {[1]}} { error "Expected \[1\], got $v" }
  r JSON.SET emptyk {$[""]} 99
  set v2 [r JSON.GET emptyk {$[""]}]
  if {$v2 ne {[99]}} { error "Expected \[99\], got $v2" }
  set d [r JSON.DEL emptyk {$[""]}]
  if {$d != 1} { error "Expected 1 deleted, got $d" }
  set v3 [r JSON.GET emptyk {$[""]}]
  if {$v3 ne {[]}} { error "Expected \[\], got $v3" }
  r DEL emptyk
}

test_assert "JSON.MSET atomic rollback on error" {
  r SET mset_str "not_json"
  r DEL mset_atomic_ok
  set caught 0
  if {[catch {r JSON.MSET mset_atomic_ok $ {{"x":1}} mset_str $ {{"y":2}}} err]} {
    set caught 1
  }
  if {!$caught} { error "Expected WRONGTYPE error" }
  if {[r EXISTS mset_atomic_ok] != 0} {
    error "JSON.MSET was not atomic: mset_atomic_ok was created!"
  }
  r DEL mset_str
}

test "JSON.ARRTRIM negative out-of-range stop empties array" {
  r JSON.SET trimneg $ {[1,2,3,4,5]}
  r JSON.ARRTRIM trimneg . -100 -50
} {0}

test "JSON.NUMMULTBY preserves integer type" {
  r JSON.SET multint $ {{"n":7}}
  r JSON.NUMMULTBY multint .n 6
  r JSON.TYPE multint .n
} {integer}

test_error "JSON.NUMINCRBY rejects overflow to Inf" {
  r JSON.SET infdoc $ {{"n":1e308}}
  r JSON.NUMINCRBY infdoc .n 1e308
} {ERR*finite*}

test_error "JSON.NUMMULTBY rejects overflow to Inf" {
  r JSON.NUMMULTBY infdoc .n 10
} {ERR*finite*}

test_error "JSON.SET rejects nesting depth > 128" {
  set deep "[string repeat {[} 130]1[string repeat {]} 130]"
  r JSON.SET toodeep $ $deep
} {ERR*depth*}

puts "\n=== Phase 2: Compact DOM, Hash Delete & Memory Shrink ==="

test_assert "MEMORY USAGE drops significantly after JSON.CLEAR on large array" {
  set items {}
  for {set i 0} {$i < 500} {incr i} { lappend items $i }
  r JSON.SET shrink_arr $ "\[[join $items ,]\]"
  set mem_before [r MEMORY USAGE shrink_arr]
  r JSON.CLEAR shrink_arr $
  set mem_after [r MEMORY USAGE shrink_arr]
  if {$mem_after * 4 >= $mem_before} {
    error "Expected memory to drop >= 4x: before=$mem_before after=$mem_after"
  }
  r DEL shrink_arr
}

test_assert "Large object insert, lookup, delete, and UNLINK" {
  set pairs {}
  for {set i 0} {$i < 200} {incr i} {
    lappend pairs "\"k$i\":\"val_$i\""
  }
  r JSON.SET bigobj $ "\{[join $pairs ,]\}"
  if {[r JSON.OBJLEN bigobj .] != 200} { error "Expected 200 keys" }
  for {set i 0} {$i < 150} {incr i} {
    r JSON.DEL bigobj "$.k$i"
  }
  if {[r JSON.OBJLEN bigobj .] != 50} { error "Expected 50 keys remaining" }
  set v [r JSON.GET bigobj .k199]
  if {$v ne {"val_199"}} { error "Expected \"val_199\", got $v" }
  r UNLINK bigobj
}

puts "\n=== Phase 3: RFC 8259, Zero-Copy GET & Formatting ==="

test "JSON.GET multi-path zero-copy serialization" {
  r JSON.SET mpdoc $ {{"a":1,"b":[2,3],"c":"hi"}}
  r JSON.GET mpdoc $.a $.c
} {{"$.a":[1],"$.c":["hi"]}}

test "JSON.GET independent SPACE formatting" {
  r JSON.GET mpdoc SPACE { } .
} {{"a": 1,"b": [2,3],"c": "hi"}}

test "JSON.GET NOESCAPE option accepted" {
  r JSON.GET mpdoc NOESCAPE $.c
} {["hi"]}

test_error "JSON.SET rejects unescaped control character in string" {
  r JSON.SET badctl $ "\"hello\nworld\""
} {ERR*control*}

puts "\n=== Phase 4: Rich JSONPath Filters, Punctuation Keys & Slices ==="

test "JSONPath compound filter && and ||" {
  r JSON.SET p4doc $ {{"limit":15,"items":[{"name":"Alpha","cat":"a","price":10},{"name":"Beta","cat":"a","price":20},{"name":"Gamma","cat":"b","price":5}]}}
  r JSON.GET p4doc {$.items[?(@.cat == 'a' && @.price > $.limit)].name}
} {["Beta"]}

test "JSONPath filter negation !" {
  r JSON.GET p4doc {$.items[?(!(@.cat == 'a'))].name}
} {["Gamma"]}

test "JSONPath filter regex =~ case-insensitive" {
  r JSON.GET p4doc {$.items[?(@.name =~ /^alp/i)].name}
} {["Alpha"]}

test "JSONPath filter on scalar array @" {
  r JSON.SET p4arr $ {[1,5,10,15,20]}
  r JSON.GET p4arr {$[?(@ >= 10 && @ < 20)]}
} {[10,15]}

test "JSONPath quoted keys with comma and colon" {
  r JSON.SET p4punct $ {{"a,b":1,"c:d":2}}
  r JSON.GET p4punct {$["a,b"]} {$["c:d"]}
} {{"$[\"a,b\"]":[1],"$[\"c:d\"]":[2]}}

test "JSONPath slice step 0 returns empty array" {
  r JSON.GET p4arr {$[0:5:0]}
} {[]}

test "JSONPath mixed union with spaces" {
  r JSON.GET p4arr {$[ 0 , 2:4 , -1 ]}
} {[1,10,15,20]}

puts "\n=== Phase 5: Large Doc, StringAppend, JSON.DEBUG & AOF/RDB E2E ==="

test_assert "JSON.STRAPPEND SSO to heap growth" {
  r JSON.SET sappend $ {{"s":"a"}}
  for {set i 0} {$i < 20} {incr i} {
    r JSON.STRAPPEND sappend .s {"0123456789"}
  }
  set len [r JSON.STRLEN sappend .s]
  if {$len != 201} { error "Expected len 201, got $len" }
  r DEL sappend
}

test "JSON.DEBUG DEPTH returns nesting depth" {
  r JSON.SET dbgdoc $ {{"a":{"b":[1,2,3]},"c":4}}
  r JSON.DEBUG DEPTH dbgdoc .
} {3}

test "JSON.DEBUG FIELDS returns total field/element count" {
  r JSON.DEBUG FIELDS dbgdoc .
} {6}

test_assert "JSON.DEBUG MEMORY JSONPath returns array" {
  set res [r JSON.DEBUG MEMORY dbgdoc {$}]
  if {[llength $res] != 1 || [lindex $res 0] <= 0} {
    error "Expected 1-element positive array, got $res"
  }
  r DEL dbgdoc
}

test_assert "Large document (~300KB) SET/GET/Filter/BGSAVE/BGREWRITEAOF" {
  set recs {}
  for {set i 0} {$i < 2000} {incr i} {
    lappend recs "\{\"id\":$i,\"name\":\"rec_$i\",\"price\":[expr {($i % 100) + 0.5}],\"active\":true\}"
  }
  set big_json "\{\"records\":\[[join $recs ,]\]\}"
  r JSON.SET largedoc $ $big_json
  if {[r JSON.ARRLEN largedoc .records] != 2000} {
    error "Expected 2000 records"
  }
  set hit [r JSON.GET largedoc {$.records[1999].name}]
  if {$hit ne {["rec_1999"]}} { error "Unexpected rec_1999: $hit" }
  set depth [r JSON.DEBUG DEPTH largedoc .]
  if {$depth != 3} { error "Expected depth 3, got $depth" }

  # Trigger AOF rewrite (exercises AofRewriteJson on large doc)
  r CONFIG SET appendonly yes
  after 500
  r BGREWRITEAOF
  after 1000
  r CONFIG SET appendonly no
  r DEL largedoc
}

puts "\n=== Phase 6: Precompiled Regex, MERGE Leaf Create & KeySpecs ==="

test_assert "JSON.MERGE creates missing leaf key when parent exists" {
  r JSON.SET mleaf $ {{"user":{"name":"Alice"}}}
  r JSON.MERGE mleaf $.user.profile {{"role":"admin"}}
  set role [r JSON.GET mleaf .user.profile.role]
  if {$role ne {"admin"}} { error "Expected \"admin\", got $role" }
  # Merging null onto a missing leaf key is a no-op
  r JSON.MERGE mleaf $.user.nonexistent null
  r DEL mleaf
}

test_assert "COMMAND INFO KeySpecs for JSON.MGET and JSON.MSET" {
  set mget_info [lindex [r COMMAND INFO JSON.MGET] 0]
  set mset_info [lindex [r COMMAND INFO JSON.MSET] 0]
  # Redis COMMAND INFO fields: [name arity flags firstkey lastkey step ...]
  if {[lindex $mget_info 3] != 1 || [lindex $mget_info 4] != -2 || [lindex $mget_info 5] != 1} {
    error "Unexpected JSON.MGET keyspec: $mget_info"
  }
  if {[lindex $mset_info 3] != 1 || [lindex $mset_info 4] != -1 || [lindex $mset_info 5] != 3} {
    error "Unexpected JSON.MSET keyspec: $mset_info"
  }
}

puts "\n=== Phase 8: RFC 9535 JSONPath Functions ==="

test "JSONPath length() on array and string" {
  r JSON.SET fn_doc $ {{"items":[{"name":"hi","tags":["a","b","c"]},{"name":"hello redis","tags":["x"]},{"name":"redis","tags":[]}]}}
  r JSON.GET fn_doc {$.items[?(length(@.tags) >= 2)].name}
} {["hi"]}

test "JSONPath count() with wildcard" {
  r JSON.GET fn_doc {$.items[?(count(@.tags[*]) == 1)].name}
} {["hello redis"]}

test "JSONPath match() vs search()" {
  set m [r JSON.GET fn_doc {$.items[?(match(@.name, 'redis'))].name}]
  set s [r JSON.GET fn_doc {$.items[?(search(@.name, 'redis'))].name}]
  r DEL fn_doc
  list $m $s
} {{["redis"]} {["hello redis","redis"]}}

puts "\n=== Code Review Hardening & Extended Edge Cases ==="

test_assert "JSON.CLEAR overlapping recursive path does not crash" {
  r JSON.SET clr_ov $ {{"a":[1,2,{"b":[3,4]}],"c":{"d":5}}}
  set n [r JSON.CLEAR clr_ov {$..*}]
  if {$n < 2} { error "Expected >= 2 cleared, got $n" }
  set res [r JSON.GET clr_ov $]
  if {$res ne {[{"a":[],"c":{}}]}} { error "Unexpected after clear: $res" }
  r DEL clr_ov
}

test_assert "JSON.ARRTRIM and JSON.ARRPOP on nested arrays recursive path" {
  r JSON.SET nest_arr $ {[[1,2,3],[4,5,6]]}
  r JSON.ARRPOP nest_arr {$..*}
  set after_pop [r JSON.GET nest_arr $]
  if {$after_pop ne {[[[1,2],[4,5]]]}} { error "Unexpected after recursive pop: $after_pop" }
  r JSON.SET nest_arr $ {[[1,2,3],[4,5,6]]}
  r JSON.ARRTRIM nest_arr {$..*} 1 0
  set after_trim [r JSON.GET nest_arr $]
  if {$after_trim ne {[[[],[]]]}} { error "Unexpected after recursive trim: $after_trim" }
  r DEL nest_arr
}

test_assert "JSON.TOGGLE on duplicate union path does not crash" {
  r JSON.SET dup_tog $ {{"flag":true}}
  set res [r JSON.TOGGLE dup_tog {$['flag','flag']}]
  if {$res ne {0 0}} { error "Expected {0 0}, got $res" }
  r DEL dup_tog
}

test_assert "JSON.MERGE strips null keys on new root and new leaf (RFC 7396)" {
  r DEL rfc_root
  r JSON.MERGE rfc_root $ {{"a":1,"b":null,"nested":{"x":2,"y":null}}}
  set r1 [r JSON.GET rfc_root $]
  if {$r1 ne {[{"a":1,"nested":{"x":2}}]}} { error "Null not stripped on new root: $r1" }
  r JSON.MERGE rfc_root $.new_leaf {{"k":10,"drop":null}}
  set r2 [r JSON.GET rfc_root $.new_leaf]
  if {$r2 ne {[{"k":10}]}} { error "Null not stripped on new leaf: $r2" }
  r DEL rfc_root
}

test_error "JSON.SET rejects combined depth > 128" {
  set lb [format %c 123]
  set rb [format %c 125]
  set base "[string repeat "${lb}\"a\":" 80]1[string repeat $rb 80]"
  set graft "[string repeat {[} 60]1[string repeat {]} 60]"
  r JSON.SET depth_guard $ $base
  set p "\$[string repeat {.a} 80]"
  r JSON.SET depth_guard $p $graft
} {ERR*depth*}

test "JSONPath grouped OR without outer parens and bare filter" {
  r JSON.SET grp_flt $ {[{"a":1,"b":0},{"a":0,"b":2},{"a":0,"b":0}]}
  set r1 [r JSON.GET grp_flt {$[?(@.a == 1) || (@.b == 2)]}]
  set r2 [r JSON.GET grp_flt {$[?@.a == 1]}]
  r DEL grp_flt
  list $r1 $r2
} {{[{"a":1,"b":0},{"a":0,"b":2}]} {[{"a":1,"b":0}]}}

test_assert "JSON.MGET mixed existing, missing, and wrong-type keys" {
  r JSON.SET mg_ok $ {{"v":42}}
  r SET mg_str "plain"
  r DEL mg_missing
  set res [r JSON.MGET mg_ok mg_missing mg_str $.v]
  r DEL mg_ok mg_str
  if {$res ne [list {[42]} (nil) (nil)]} { error "Unexpected MGET mixed: $res" }
}

test_assert "JSON.ARRINSERT negative index and multiple values in JSONPath mode" {
  r JSON.SET ains $ {{"a":[1,4],"b":[10,40]}}
  set res [r JSON.ARRINSERT ains {$.*} -1 2 3]
  set ga [r JSON.GET ains .a]
  set gb [r JSON.GET ains .b]
  r DEL ains
  if {$res ne {4 4} || $ga ne {[1,2,3,4]} || $gb ne {[10,2,3,40]}} {
    error "Unexpected ARRINSERT: res=$res a=$ga b=$gb"
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
file delete -force /tmp/json_tcl_test.rdb

exit $test_failed
