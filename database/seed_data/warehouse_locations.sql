-- warehouse_locations.sql — populate a sample warehouse grid for a new installation
-- Run once after migrations on a fresh database.
--
-- Layout: 3 aisles (A, B, C), 4 levels each, 5 positions per level
-- Plus 2 AutoStore ports and 1 staging area

BEGIN
    -- Bin locations: format BIN-<AISLE>-<LEVEL>-<POSITION>
    FOR aisle_idx IN 1..3 LOOP
        FOR level_idx IN 1..4 LOOP
            FOR pos_idx IN 1..5 LOOP
                INSERT INTO warehouse_location (location_code, aisle, level, location_type, is_active)
                VALUES (
                    'BIN-' || CHR(64 + aisle_idx) || '-0' || level_idx || '-0' || pos_idx,
                    CHR(64 + aisle_idx),  -- A, B, C
                    level_idx,
                    'bin',
                    1
                );
            END LOOP;
        END LOOP;
    END LOOP;

    -- AutoStore ports — where robots deliver bins to pickers
    INSERT INTO warehouse_location VALUES ('PORT-01', NULL, NULL, 'port',    1);
    INSERT INTO warehouse_location VALUES ('PORT-02', NULL, NULL, 'port',    1);

    -- Staging area — temporary holding before packing
    INSERT INTO warehouse_location VALUES ('STAGE-01', NULL, NULL, 'staging', 1);

    COMMIT;
END;
/
