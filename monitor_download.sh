#!/bin/bash
# Monitor download progress every 60 seconds

SESSION_ID="proc_97397e0adbf9"
CHECK_INTERVAL=60

echo "=== UD-Q4_K_M Download Monitor ==="
echo "Checking every ${CHECK_INTERVAL}s..."
echo ""

for i in {1..20}; do
    echo "--- Check #${i} at $(date +%H:%M:%S) ---"
    
    # Check if process is still running
    if ps -p 52449 > /dev/null 2>&1; then
        echo "✅ Process running (PID 52449)"
        
        # Check for model files
        MODEL_DIR="/Users/a1111/development/splash_of_mine/install/models"
        if [ -d "$MODEL_DIR" ]; then
            echo "📁 Checking model directory..."
            find "$MODEL_DIR" -name "*UD-Q4_K_M*" -o -name "*Qwen3.8-27B*" 2>/dev/null | head -5
            
            # Check file sizes
            du -sh "$MODEL_DIR"/.resolved/* 2>/dev/null | grep -v "^4.0K" | tail -3
        fi
        
        # Check server log for progress
        if [ -f "/tmp/splash_download.log" ]; then
            tail -5 /tmp/splash_download.log
        fi
    else
        echo "⚠️ Process stopped (PID 52449 not found)"
        break
    fi
    
    echo ""
    sleep $CHECK_INTERVAL
done

echo "=== Monitoring complete ==="
