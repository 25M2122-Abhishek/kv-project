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
        docker run -it --cpuset-cpus="0-3" --name kv_server --network kv_net -p 8080:8080 -p 5432:5432 kv_server_image <cache_capacity> <server_threads>
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

### Load Generator - In progress

