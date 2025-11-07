-- Create role
DO
$$
BEGIN
   IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'kvuser') THEN
      CREATE ROLE kvuser LOGIN PASSWORD 'kvpass';
   END IF;
END
$$;

-- Try creating database (ignore if already exists)
CREATE DATABASE kvdb OWNER kvuser;

\connect kvdb

CREATE TABLE IF NOT EXISTS kv_store (
    key TEXT PRIMARY KEY,
    value TEXT
);

ALTER TABLE kv_store OWNER TO kvuser;
GRANT ALL PRIVILEGES ON TABLE kv_store TO kvuser;
GRANT CONNECT ON DATABASE kvdb TO kvuser;
GRANT USAGE ON SCHEMA public TO kvuser;
