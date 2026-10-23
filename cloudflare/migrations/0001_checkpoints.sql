-- Copyright 2026 Robert McConnell
-- SPDX-License-Identifier: Apache-2.0
-- D1 / SQLite: strictly increasing sequence per (adapter, operator grant).
-- This is logical append-only storage, NOT Cloudflare-account WORM.
CREATE TABLE IF NOT EXISTS tronix_checkpoints (
    artifact_sha TEXT NOT NULL CHECK(length(artifact_sha) = 64),
    grant_sha TEXT NOT NULL CHECK(length(grant_sha) = 64),
    seq INTEGER NOT NULL CHECK(seq > 0),
    chain_head TEXT NOT NULL CHECK(length(chain_head) = 64),
    payload TEXT NOT NULL CHECK(length(payload) <= 2048),
    payload_sha256 TEXT NOT NULL CHECK(length(payload_sha256) = 64),
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (artifact_sha, grant_sha, seq)
);
CREATE INDEX IF NOT EXISTS tronix_checkpoints_latest
ON tronix_checkpoints (artifact_sha, grant_sha, seq DESC);
CREATE TRIGGER IF NOT EXISTS tronix_checkpoints_no_delete
BEFORE DELETE ON tronix_checkpoints
BEGIN SELECT RAISE(ABORT, 'checkpoint deletion forbidden'); END;
CREATE TRIGGER IF NOT EXISTS tronix_checkpoints_no_update
BEFORE UPDATE ON tronix_checkpoints
BEGIN SELECT RAISE(ABORT, 'checkpoint mutation forbidden'); END;
CREATE TRIGGER IF NOT EXISTS tronix_checkpoints_monotonic
BEFORE INSERT ON tronix_checkpoints
WHEN EXISTS (
  SELECT 1 FROM tronix_checkpoints
  WHERE artifact_sha = NEW.artifact_sha
    AND grant_sha = NEW.grant_sha
    AND seq >= NEW.seq
)
BEGIN SELECT RAISE(ABORT, 'checkpoint sequence not monotonic'); END;
