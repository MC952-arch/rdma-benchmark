#!/bin/bash

# Define colors
black='\E[30;50m'
red='\E[31;50m'
green='\E[32;50m'
yellow='\E[33;50m'
blue='\E[34;50m'
magenta='\E[35;50m'
cyan='\E[36;50m'
white='\E[37;50m'
bold='\033[1m'

gid_count=0

# cecho: colored echo
function cecho() {
  echo -en "$1"
  shift
  echo -n "$*"
  tput sgr0
}

# becho: bold echo
function becho() {
  echo -en "$bold"
  echo -n "$*"
  tput sgr0
}

# Print header
echo -e "DEV\tPORT\tINDEX\tGID\t\t\t\t\tIPv4\t\tVER\tDEV"
echo -e "---\t----\t-----\t---\t\t\t\t\t------------\t---\t---"

DEVS=$1
if [ -z "$DEVS" ]; then
  DEVS=$(ls /sys/class/infiniband/)
fi

for d in $DEVS; do
  for p in $(ls /sys/class/infiniband/$d/ports/); do
    for g in $(ls /sys/class/infiniband/$d/ports/$p/gids/); do
      gid=$(cat /sys/class/infiniband/$d/ports/$p/gids/$g)

      # Skip empty or link-local addresses
      if [[ "$gid" == "0000:0000:0000:0000:0000:0000:0000:0000" || "$gid" == "fe80:0000:0000:0000:0000:0000:0000:0000" ]]; then
        continue
      fi

      ndev=$(cat /sys/class/infiniband/$d/ports/$p/gid_attrs/ndevs/$g 2>/dev/null)
      type_raw=$(cat /sys/class/infiniband/$d/ports/$p/gid_attrs/types/$g 2>/dev/null)
      type=$(echo "$type_raw" | grep -o "[Vv].*")

      # If IPv4-mapped address
      if [[ "${gid:0:4}" == "0000" ]]; then
        ipv4=$(printf "%d.%d.%d.%d" 0x${gid:30:2} 0x${gid:32:2} 0x${gid:35:2} 0x${gid:37:2})
        echo -e "$d\t$p\t$g\t$gid\t$ipv4\t$type\t$ndev"
      else
        echo -e "$d\t$p\t$g\t$gid\t\t\t$type\t$ndev"
      fi

      gid_count=$((gid_count + 1))
    done
  done
done

echo
echo "GIDs found: $gid_count"
