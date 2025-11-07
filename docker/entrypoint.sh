#!/bin/bash
set -e

# Allow overriding server config via command-line args
CACHE_CAPACITY="${1:-1000}"
THREADS="${2:-16}"

# Start PostgreSQL (system package). Initialize data dir if necessary
if [ ! -d "/var/lib/postgresql/12/main" ] && [ ! -d "/var/lib/postgresql/data" ]; then
    echo "Initializing database..."
    # system package places data at /var/lib/postgresql/ (versioned)
    # Use pg_ctlcluster to init if available, else use initdb
    service postgresql start
    sleep 2
fi

service postgresql start
sleep 2

# Create user and DB if not exists (run psql as postgres)
echo "Setting up database..."
su - postgres -c "psql -f /init.sql" || true

# Show existing databases (for debugging)
echo "Databases:"
su - postgres -c "psql -l"

# Launch server in foreground
echo "Starting KV server..."
/opt/kv_server/server/kv_server "$CACHE_CAPACITY" "$THREADS"
