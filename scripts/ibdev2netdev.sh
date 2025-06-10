#!/bin/bash
for dev in /sys/class/infiniband/*; do
  devname=$(basename "$dev")
  for port in "$dev"/ports/*; do
    portnum=$(basename "$port")
    netdev_path="$port"/gid_attrs/ndevs/0
    if [[ -f "$netdev_path" ]]; then
      netdev=$(cat "$netdev_path")
      state=$(cat "$port"/state | awk '{print $2}')
      echo "$devname port $portnum ==> $netdev ($state)"
    fi
  done
done
