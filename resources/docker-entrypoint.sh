#!/bin/sh
echo "--------------------------------------------------------------------------------" >&2
echo "DOCKER WARNING: Docker Out Of Memory handling can be unreliable." >&2
echo "DOCKER WARNING: If your program unexpectedly exits, it might have been terminated by the Out Of Memory killer without a visible notice." >&2
echo "DOCKER WARNING: The --store option can be used to partly reduce memory usage." >&2
echo "--------------------------------------------------------------------------------" >&2

# jemalloc (see the Dockerfile), unless TM_NO_JEMALLOC is set or something is preloaded already
if [ -z "$TM_NO_JEMALLOC" ] && [ -z "$LD_PRELOAD" ]; then
	for lib in /usr/lib/*-linux-gnu/libjemalloc.so.2; do
		[ -e "$lib" ] && export LD_PRELOAD="$lib" && break
	done
fi

# Proceed to run the command passed to the script
exec /usr/src/app/tilemaker "$@"
