-- V001: Initial schema — tracking table for applied migrations
-- Applied by deploy script before any other migration

CREATE TABLE schema_version (
    version         VARCHAR2(20)  NOT NULL,
    description     VARCHAR2(200),
    applied_at      TIMESTAMP     DEFAULT SYSTIMESTAMP,
    applied_by      VARCHAR2(50)  DEFAULT USER,
    CONSTRAINT pk_schema_version PRIMARY KEY (version)
);
