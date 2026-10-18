#!/bin/bash
./test_kv_swap_integration &
PID=$!
sleep 2
kill -9 $PID 2>/dev/null
wait $PID 2>/dev/null
