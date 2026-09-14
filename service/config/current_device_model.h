#pragma once
#include <string_view>
namespace service::config {
// 保留已执行迁移的校验和；在此删除设备类型历史快照及其绑定。
inline constexpr std::string_view kCurrentDeviceModelMigration = R"sql(
DO $schema$ BEGIN
LOCK TABLE protocol_config, device IN ACCESS EXCLUSIVE MODE;
DROP VIEW device_model;
DROP TRIGGER retain_protocol ON protocol_config;
DROP TRIGGER version_protocol ON protocol_config;
DROP TRIGGER bind_model ON device;
DROP FUNCTION retain_protocol_revision();
DROP FUNCTION version_protocol_config();
DROP FUNCTION bind_device_model();
CREATE OR REPLACE FUNCTION bind_device_channel() RETURNS trigger LANGUAGE plpgsql AS $fn$
DECLARE channel link%ROWTYPE; model_protocol TEXT; registration TEXT;
BEGIN
  SELECT * INTO STRICT channel FROM link WHERE id=NEW.link_id FOR SHARE;
  IF NEW.deleted_at IS NULL AND channel.deleted_at IS NOT NULL THEN
    RAISE EXCEPTION 'Cannot bind a deleted channel';
  END IF;
  SELECT protocol INTO STRICT model_protocol FROM protocol_config
    WHERE id=NEW.protocol_config_id;
  IF model_protocol <> channel.protocol THEN RAISE EXCEPTION 'Channel and model protocols differ'; END IF;
  IF channel.execution='edge' THEN
    NEW.protocol_params := jsonb_set(NEW.protocol_params,'{registration}','{"mode":"OFF"}');
    IF channel.protocol='Modbus' THEN
      NEW.protocol_params := jsonb_set(NEW.protocol_params,'{modbus_mode}',
        to_jsonb(CASE WHEN channel.endpoint->>'transport'='serial' THEN 'RTU'::text ELSE 'TCP'::text END));
    END IF;
  END IF;
  registration := CASE upper(COALESCE(NEW.protocol_params#>>'{registration,mode}','OFF'))
    WHEN 'OFF' THEN '' WHEN 'HEX' THEN 'HEX:' || upper(regexp_replace(
      COALESCE(NEW.protocol_params#>>'{registration,content}',''),'\s','','g'))
    ELSE 'ASCII:' || COALESCE(NEW.protocol_params#>>'{registration,content}','') END;
  NEW.protocol_address := CASE channel.protocol
    WHEN 'SL651' THEN NEW.protocol_params->>'device_code'
    WHEN 'Modbus' THEN jsonb_build_array(COALESCE(NEW.protocol_params->>'target_id',''),registration,
      COALESCE((NEW.protocol_params->>'slave_id')::integer,1))::text
    ELSE jsonb_build_array(COALESCE(NEW.protocol_params->>'target_id',''),registration)::text END;
  RETURN NEW;
END $fn$;

ALTER TABLE device DROP CONSTRAINT device_model_revision_fk;
ALTER TABLE device_data DROP CONSTRAINT telemetry_model_revision_fk;
ALTER TABLE command_operation DROP CONSTRAINT command_model_revision_fk;
ALTER TABLE device DROP COLUMN protocol_revision;
ALTER TABLE device_data DROP COLUMN model_revision;
ALTER TABLE command_operation DROP COLUMN model_revision;
DROP TABLE protocol_revision;
DROP FUNCTION reject_model_revision_mutation();
ALTER TABLE protocol_config DROP COLUMN revision;
CREATE VIEW device_model AS
SELECT d.id AS device_id, p.id,p.protocol,p.name,p.config,p.remark,
       p.created_by,p.created_at,p.updated_at,p.enabled,p.deleted_at
FROM device d JOIN protocol_config p ON p.id=d.protocol_config_id;
END $schema$;
)sql";
}
