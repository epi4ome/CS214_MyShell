#!/bin/bash
MYSH=../mysh
PASS=0; FAIL=0
for t in 1 2 3 4 5 6; do
	in="test${t}.input"; exp="test${t}.expected"
	out="test${t}.out"
	printf "Running test %s... " "$t"
	cat "$in" | "$MYSH" >"$out" 2>&1
	rc=$?
	expected_rc=0
	case $t in
		4) expected_rc=1;;
	esac
	if [ $rc -ne $expected_rc ]; then
		echo "FAIL (exit $rc != $expected_rc)"
		FAIL=$((FAIL+1))
		continue
	fi
	if [ -f "$exp" ]; then
		if diff -u "$exp" "$out" >/dev/null; then
			echo "PASS"
			PASS=$((PASS+1))
		else
			echo "FAIL (output mismatch)"
			echo "---- expected ----"
			sed -n '1,200p' "$exp"
			echo "---- actual ----"
			sed -n '1,200p' "$out"
			FAIL=$((FAIL+1))
		fi
	else
		echo "No expected file; rc ok"
		PASS=$((PASS+1))
	fi
done

echo "Passed: $PASS, Failed: $FAIL"
if [ $FAIL -ne 0 ]; then exit 2; fi
exit 0
