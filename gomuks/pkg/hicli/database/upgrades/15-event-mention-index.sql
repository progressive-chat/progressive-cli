-- v15 (compatible with v10+): Add index for mentions
CREATE INDEX event_mention_idx ON event (timestamp DESC) WHERE unread_type > 0;
