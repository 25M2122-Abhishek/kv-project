# KV Server

A lightweight high-performance Key-Value store written in C that exposes RESTful HTTP APIs using the CivetWeb embedded web server.  

Data is durably stored in PostgreSQL, while hot data is served from a thread-safe LRU in-memory cache for low-latency performance.

---

## Features
- HTTP REST API (`/kv`)
- JSON request and response format (uses Jansson)
- PostgreSQL storage using `libpq`
- LRU caching layer
- Multi-threaded HTTP workers (CivetWeb)

---

## Build Instructions 

### Server

- Build Server
    ```bash
        cd kv_project
        docker build -t kv_server_image -f docker/Dockerfile . 
    ```

- Create Docker Network allows the server and load generator containers to communicate
    ```bash
        docker network create kv_net
    ```
- Start Server
    ```bash
        docker run -it --cpuset-cpus="0-1" --name kv_server --network kv_net -p 8080:8080 -p 5432:5432 kv_server_image <cache_capacity> <server_threads>
    ```

- Stop Server
    ```bash
        Cltr + C
    ```

- Resume Server
    ```bash
        docker start -ai kv_server
    ```

- Test Server (Use another terminal)
    ```bash
        curl -X POST http://localhost:8080/kv \
            -H "Content-Type: application/json" \
            -d '{"key":"jhon","value":"doe"}'

        curl "http://localhost:8080/kv?key=jhon"

        curl -X DELETE "http://localhost:8080/kv?key=jhon"
    ```

- Check Database
    ```bash
        docker exec -it --user postgres kv_server psql -U postgres -d kvdb
        
        kvdb=# \dt

        kvdb=# SELECT * FROM kv_store;
    ```

---

# KV Load Generator

## Features

## Build Instructions

### Load Generator 

- Build Server
    ```bash
        cd kv_project
        docker build -t kv_loadgen_image -f loadgen/Dockerfile loadgen 
    ```

- Usage
    ```bash
        docker run --rm --cpuset-cpus="4-6" --network kv_net kv_loadgen_image \
        --server <url> \
        --threads <N> \ 
        --duration <S> \
        --mix <GET/POST/DELETE> \
        --key-prefix <prefix> \
        --workload <put-all|get-all|get-popular|mix> \
        --key-pool-size <N> \
        --popular-size <N> \
    ```

- Example 1 - MIX workload
    ```bash
        docker run --rm --cpuset-cpus="4-6" --network kv_net kv_loadgen_image --server http://kv_server:8080/kv --threads 4 --duration 30 --mix 60,30,10 --workload mix
    ```

- Example 2 - PUT-ALL workload
    ```bash
        docker run --rm --cpuset-cpus="4-6" --network kv_net kv_loadgen_image --server http://kv_server:8080/kv --threads 8 --duration 30 --workload put-all 
    ```

- Example 3 - GET-ALL workload
    ```bash
        docker run --rm --cpuset-cpus="4-6" --network kv_net kv_loadgen_image --server http://kv_server:8080/kv --threads 4 --duration 30 --workload get-all
    ```

- Example 4 - GET-POPULAR workload
    ```bash
        docker run --rm --cpuset-cpus="4-6" --network kv_net kv_loadgen_image --server http://kv_server:8080/kv --threads 8 --duration 30 --workload get-popular --popular-size 100
    ```

- Check Attached CPU cores
    ```bash
        docker ps
        docker inspect <container_id> | Select-String "CpusetCpus"
    ```

# KV Monitor

- Build Monitor
    ```bash
        docker build -t kv_monitor -f kv_monitor/Dockerfile .
    ```

- Run Monitor
    ```bash
        docker run --rm --name kv_monitor --network kv_net --privileged -v ${PWD}\kv_monitor\logs:/app/results/monitor_logs kv_monitor "4-6" "0-3" "7" 1 1 1    
    ```
