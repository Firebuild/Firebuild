#!/usr/bin/env bats

load test_helper

@test "--help" {
      result=$(./run-firebuild --help)
      echo "$result" | grep -q "in case of failure"
}

@test "bash -c ls" {
      result=$(./run-firebuild -- bash -c "ls integration.bats" 2> stderr)
      [ "$result" = "integration.bats" ]
      [ -z "$(strip_stderr stderr)" ]
}

@test "debugging with trace markers and report generation" {
      result=$(./run-firebuild -r -d all -- bash -c "ls integration.bats; bash -c ls | tee dirlist > /dev/null")
      [ "$result" = "integration.bats" ]
}

@test "bash exec chain" {
      result=$(./run-firebuild -- bash -c "exec bash -c exec\\ bash\\ -c\\ ls\\\\\ integration.bats" 2> stderr)
      [ "$result" = "integration.bats" ]
      [ -z "$(strip_stderr stderr)" ]
}

@test "simple pipe" {
      result=$(./run-firebuild -- bash -c 'seq 10000 | grep ^9' 2> stderr)
      [ "$result" = "$(seq 10000 | grep ^9)" ]
      [ -z "$(strip_stderr stderr)" ]
}

@test "parallel sleeps" {
      # TODO (rbalint) firebuild needs to gracefully handle when it is out of file descriptors
      result=$(./run-firebuild -- bash -c 'for i in $(seq 200); do sleep 2 & done;  wait < <(jobs -p)' 2>stderr)
      [ "$result" = "" ]
      [ -z "$(strip_stderr stderr)" ]
}

@test "system()" {
      result=$(./run-firebuild -- ./test_system 2> stderr)
      [ "$result" = "ok" ]
      [ -z "$(strip_stderr stderr)" ]
}

@test "exec()" {
      result=$(./run-firebuild -- ./test_exec 2> stderr)
      [ "$result" = "ok" ]
      [ -z "$(strip_stderr stderr)" ]
}
