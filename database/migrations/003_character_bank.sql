-- Banka (help Help_Bank.dds: every character owns a bank account that works like its bag). Kept beside the
-- world-owned state row so the bank is not bounded by the 8 KiB live-state envelope; the world writes state and
-- bank in one UPDATE, so deposits/withdrawals commit atomically. Currency deposit is NONCANONICAL.
ALTER TABLE characters ADD COLUMN bank jsonb NOT NULL DEFAULT '{"items":[],"currency":0}'
 CHECK (jsonb_typeof(bank)='object' AND jsonb_typeof(bank->'items')='array' AND jsonb_typeof(bank->'currency')='number'
        AND (bank->>'currency')::numeric BETWEEN 0 AND 1000000000);
