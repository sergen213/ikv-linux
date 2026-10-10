#!/bin/sh
set -eu
# Runs once inside the owned PostgreSQL container, as its postgres user.
# psql reads the secret itself: no password in argv/environment or SQL logs.
psql --username "$POSTGRES_USER" --dbname postgres --set ON_ERROR_STOP=1 <<'SQL'
\set app_password `cat /run/secrets/app-db-password`
SELECT format('CREATE ROLE ikv LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE PASSWORD %L', :'app_password') \gexec
CREATE DATABASE ikv OWNER ikv;
SQL
