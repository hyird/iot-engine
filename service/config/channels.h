#pragma once
#include <string_view>
namespace service::config {
inline constexpr std::string_view kChannelMigration = R"sql(
DO $schema$ BEGIN
LOCK TABLE link,device IN SHARE ROW EXCLUSIVE MODE;
-- Duplicate active physical endpoints require an explicit resolution; silently
-- choosing serial settings or a slave address would change acquisition behavior.
CREATE UNIQUE INDEX channel_edge_serial_unique ON link(edge_node_id,(endpoint->>'interface'))
WHERE deleted_at IS NULL AND execution='edge' AND endpoint->>'transport'='serial';
CREATE UNIQUE INDEX channel_edge_tcp_unique ON link(edge_node_id,(endpoint->>'interface'),
 (endpoint->>'mode'),(endpoint->>'ip'),((endpoint->>'port')::integer))
WHERE deleted_at IS NULL AND execution='edge' AND endpoint->>'transport'='tcp';
ALTER TABLE device ADD COLUMN protocol_address TEXT;
CREATE FUNCTION bind_device_channel() RETURNS trigger LANGUAGE plpgsql AS $fn$
DECLARE channel link%ROWTYPE; model_protocol TEXT; registration TEXT;
BEGIN
  SELECT * INTO STRICT channel FROM link WHERE id=NEW.link_id FOR SHARE;
  IF NEW.deleted_at IS NULL AND channel.deleted_at IS NOT NULL THEN
    RAISE EXCEPTION 'Cannot bind a deleted channel';
  END IF;
  SELECT protocol INTO STRICT model_protocol FROM protocol_revision
    WHERE id=NEW.protocol_config_id AND revision=NEW.protocol_revision;
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
CREATE TRIGGER device_channel_binding BEFORE INSERT OR UPDATE ON device
FOR EACH ROW EXECUTE FUNCTION bind_device_channel();
UPDATE device SET protocol_params=protocol_params;
ALTER TABLE device ALTER COLUMN protocol_address SET NOT NULL;
CREATE UNIQUE INDEX device_channel_address_unique ON device(link_id,protocol_address)
WHERE deleted_at IS NULL;
CREATE FUNCTION protect_channel_binding() RETURNS trigger LANGUAGE plpgsql AS $fn$
BEGIN
  IF (NEW.protocol,NEW.execution,NEW.edge_node_id,NEW.deleted_at) IS DISTINCT FROM
     (OLD.protocol,OLD.execution,OLD.edge_node_id,OLD.deleted_at)
     AND EXISTS(SELECT 1 FROM device WHERE link_id=OLD.id AND deleted_at IS NULL) THEN
    RAISE EXCEPTION 'Move or remove bound devices before changing channel identity';
  END IF;
  RETURN NEW;
END $fn$;
CREATE TRIGGER channel_binding_guard BEFORE UPDATE ON link
FOR EACH ROW EXECUTE FUNCTION protect_channel_binding();
END $schema$;
)sql";
}
