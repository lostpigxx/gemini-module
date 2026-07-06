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
} {2}

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
