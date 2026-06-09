-- V003: Order management tables

CREATE TABLE order_header (
    order_id        VARCHAR2(36)   NOT NULL,
    customer_ref    VARCHAR2(64),
    status          VARCHAR2(20)   DEFAULT 'NEW',
    created_at      TIMESTAMP      DEFAULT SYSTIMESTAMP,
    updated_at      TIMESTAMP      DEFAULT SYSTIMESTAMP,
    CONSTRAINT pk_order_header PRIMARY KEY (order_id),
    CONSTRAINT chk_order_status
        CHECK (status IN ('NEW','RELEASED','PICKING','PACKING','SHIPPED','CANCELLED'))
);

CREATE TABLE order_line (
    line_id         VARCHAR2(36)   NOT NULL,
    order_id        VARCHAR2(36)   NOT NULL,
    sku_id          VARCHAR2(32)   NOT NULL,
    qty_ordered     NUMBER(10)     NOT NULL,
    qty_picked      NUMBER(10)     DEFAULT 0,
    CONSTRAINT pk_order_line PRIMARY KEY (line_id),
    CONSTRAINT fk_order_line_header
        FOREIGN KEY (order_id)
        REFERENCES order_header(order_id),
    CONSTRAINT chk_qty_ordered CHECK (qty_ordered > 0)
);

CREATE INDEX idx_order_line_order ON order_line(order_id);
