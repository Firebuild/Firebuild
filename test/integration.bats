#!/usr/bin/env bats

@test "--help" {
      result=$(./run-firebuild --help)
      echo "$result" | grep -q "in case of failure"
}

@test "bash -c ls" {
      result=$(./run-firebuild -- bash -c "ls integration.bats" 2> stderr)
      [ "$result" = "integration.bats" ]
      [ -z "$(cat stderr)" ]
}

@test "debugging with trace markers and report generation" {
      result=$(./run-firebuild -r -d 4 -- bash -c "ls integration.bats; bash -c ls | tee dirlist > /dev/null")
      [ "$result" = "integration.bats" ]
}

@test "bash exec chain" {
      result=$(./run-firebuild -- bash -c "exec bash -c exec\\ bash\\ -c\\ ls\\\\\ integration.bats" 2> stderr)
      [ "$result" = "integration.bats" ]
      [ -z "$(cat stderr)" ]
}

@test "simple pipe" {
      result=$(./run-firebuild -- bash -c 'seq 10000 | grep ^9' 2> stderr)
      [ "$result" = "$(seq 10000 | grep ^9)" ]
      [ -z "$(cat stderr)" ]
}

@test "1500 parallel sleeps" {
      result=$(./run-firebuild -- bash -c 'for i in $(seq 1500); do sleep 2 & done;  wait < <(jobs -p)' 2>stderr)
      [ "$result" = "" ]
      [ -z "$(grep -v 'accept: Too many open files' stderr)" ] # TODO (rbalint) firebuild can't accept enough sockets
}

@test "system()" {
      result=$(./run-firebuild -- ./test_system 2> stderr)
      [ "$result" = "ok" ]
      [ -z "$(cat stderr)" ]
}

@test "exec()" {
      result=$(./run-firebuild -- ./test_exec 2> stderr)
      [ "$result" = "ok" ]
      [ -z "$(cat stderr)" ]
}
