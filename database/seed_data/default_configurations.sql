-- default_configurations.sql — insert system configuration defaults
-- These are read by the application at startup as fallback values.

INSERT INTO system_config (config_key, config_value, description) VALUES
    ('picking.max_wave_size',      '100',               'Max tasks per pick wave'),
    ('picking.reservation_expiry', '3600',              'Seconds before unreserved if unconfirmed'),
    ('autostore.heartbeat_sec',    '30',                'AutoStore controller heartbeat interval'),
    ('api.rate_limit',             '1000',              'Max API requests per minute per client'),
    ('logging.level',              'INFO',              'Default log level');

COMMIT;
