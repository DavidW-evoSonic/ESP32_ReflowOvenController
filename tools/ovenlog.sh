#!/bin/bash
OUT="$1"
echo "wall,time,temp,dt,setpoint,low,high,power,step,steps,lag,openDoor,heating,extending,extended,mcu,state,fault" > "$OUT"
for i in $(seq 1 1200); do
  J=$(curl -s -m 2 http://${OVEN:?set OVEN to the oven IP}/status)
  if [ -n "$J" ]; then
    echo "$J" | python3 -c '
import sys,json,time
try:
    d=json.load(sys.stdin)
except Exception:
    sys.exit(0)
k="time temp dt setpoint low high power step steps lag openDoor heating extending extended mcu state fault".split()
print("%.1f,"%time.time()+",".join(str(d.get(x,"")) for x in k))
' >> "$OUT"
  fi
  sleep 1
done
# Poll /status to CSV once a second.
#
#   OVEN=192.168.x.y ./tools/ovenlog.sh logs/my_run.csv
#
# The firmware keeps no run history -- the chart lives only in the browser tab
# and its CSV button is the only other way out -- so a run that is not logged
# here is gone. Captures past Complete too, which is where the true peak often
# lands: run 2's 241.57 degC peak arrived during the dwell, and run 1's 221.2
# arrived six seconds AFTER the fault.
