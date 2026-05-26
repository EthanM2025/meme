#!/bin/bash
cd /Users/ethan/Desktop/stopwatch-official-test/mac-server || exit 1
exec /usr/bin/python3 -u server.py >> /tmp/server.log 2>&1
