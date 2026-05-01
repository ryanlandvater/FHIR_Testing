-- Bootstrap for local Postgres container.
-- Keep this file idempotent for repeated local bring-up.

\i /docker-entrypoint-initdb.d/migrations/001_init.sql
