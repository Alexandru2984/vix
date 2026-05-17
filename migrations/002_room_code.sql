ALTER TABLE vix_matches
  ADD COLUMN IF NOT EXISTS room_code TEXT NOT NULL DEFAULT 'public';

ALTER TABLE vix_match_players
  ADD COLUMN IF NOT EXISTS room_code TEXT NOT NULL DEFAULT 'public';

CREATE INDEX IF NOT EXISTS idx_vix_matches_room_ended_at
  ON vix_matches (room_code, ended_at DESC, id DESC);

CREATE INDEX IF NOT EXISTS idx_vix_match_players_room
  ON vix_match_players (room_code, ended_at DESC);
