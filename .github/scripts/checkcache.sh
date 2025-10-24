#!/usr/bin/env bash

cacheHashPath="${GITHUB_WORKSPACE}/cache_hash.txt"
# create empty file if it doesn't exist
touch "${cacheHashPath}"
# read old cache state from file
oldCache=$(<"${cacheHashPath}")
# if cache state hasn't changed, echo 'false' and exit
if [[ "$oldCache" == "$1" ]]; then
  echo "false"
  exit 0
fi
# write new cache state, echo 'true', and exit
echo "$1" > "${cacheHashPath}"
echo "true"
exit 0
