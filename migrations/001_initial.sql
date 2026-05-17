CREATE TABLE IF NOT EXISTS schema_migrations (
  version BIGINT PRIMARY KEY,
  name TEXT NOT NULL,
  applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS vix_matches (
  id BIGSERIAL PRIMARY KEY,
  room_code TEXT NOT NULL DEFAULT 'public',
  round_number BIGINT NOT NULL,
  ended_at TIMESTAMPTZ NOT NULL,
  winner_id TEXT NOT NULL,
  winner_name TEXT NOT NULL,
  winner_score INTEGER NOT NULL,
  duration_seconds INTEGER NOT NULL,
  human_players INTEGER NOT NULL,
  bot_players INTEGER NOT NULL,
  total_players INTEGER NOT NULL,
  participant_count INTEGER NOT NULL,
  participants JSONB NOT NULL DEFAULT '[]'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS vix_match_players (
  id BIGSERIAL PRIMARY KEY,
  match_id BIGINT NOT NULL REFERENCES vix_matches(id) ON DELETE CASCADE,
  room_code TEXT NOT NULL DEFAULT 'public',
  round_number BIGINT NOT NULL,
  ended_at TIMESTAMPTZ NOT NULL,
  player_id TEXT NOT NULL,
  name TEXT NOT NULL,
  is_bot BOOLEAN NOT NULL,
  is_winner BOOLEAN NOT NULL,
  score INTEGER NOT NULL,
  orb_pickups INTEGER NOT NULL,
  powerups INTEGER NOT NULL,
  quests INTEGER NOT NULL,
  control_zone_points INTEGER NOT NULL,
  abilities_used INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_vix_matches_ended_at ON vix_matches (ended_at DESC, id DESC);
CREATE INDEX IF NOT EXISTS idx_vix_matches_room_ended_at ON vix_matches (room_code, ended_at DESC, id DESC);
CREATE INDEX IF NOT EXISTS idx_vix_match_players_leaderboard
  ON vix_match_players (is_bot, name, is_winner, score, ended_at);
CREATE INDEX IF NOT EXISTS idx_vix_match_players_room
  ON vix_match_players (room_code, ended_at DESC);
