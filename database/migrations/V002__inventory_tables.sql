-- V002: Core inventory tables

CREATE TABLE warehouse_location (
    location_code   VARCHAR2(24)   NOT NULL,
    aisle           VARCHAR2(8),
    level           NUMBER(3),
    location_type   VARCHAR2(20),   -- 'bin', 'staging', 'port'
    is_active       NUMBER(1)       DEFAULT 1,
    CONSTRAINT pk_warehouse_location PRIMARY KEY (location_code)
);

CREATE TABLE stock_item (
    stock_id            VARCHAR2(36)  NOT NULL,
    sku_id              VARCHAR2(32)  NOT NULL,
    location_code       VARCHAR2(24)  NOT NULL,
    quantity_on_hand    NUMBER(10)    DEFAULT 0,
    quantity_reserved   NUMBER(10)    DEFAULT 0,
    last_updated        TIMESTAMP     DEFAULT SYSTIMESTAMP,
    CONSTRAINT pk_stock_item    PRIMARY KEY (stock_id),
    CONSTRAINT uq_sku_location  UNIQUE (sku_id, location_code),
    CONSTRAINT fk_stock_location
        FOREIGN KEY (location_code)
        REFERENCES warehouse_location(location_code),
    CONSTRAINT chk_qty_nonneg CHECK (quantity_on_hand >= 0),
    CONSTRAINT chk_reserved_lte_hand CHECK (quantity_reserved <= quantity_on_hand)
);

CREATE INDEX idx_stock_sku ON stock_item(sku_id);
CREATE INDEX idx_stock_location ON stock_item(location_code);
