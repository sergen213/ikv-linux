-- Plus Paket entitlement (guide: vendor 1/3 instead of 1/5, Şan ×3, Karizma loss 10 %). The original came from the web
-- İKV Dükkan; here an administrator grants it (POST /v1/admin/accounts/{id}/plus|unplus). NONCANONICAL source.
ALTER TABLE accounts ADD COLUMN plus boolean NOT NULL DEFAULT false;
