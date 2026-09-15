#pragma once
#include <string_view>
namespace service::config {
// 历史迁移保持原文；新协议的地址在各自链路和连接内保持唯一。
inline constexpr std::string_view kIndustrialProtocolAddressMigration = R"sql(
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
    WHEN 'DLT645' THEN jsonb_build_array(COALESCE(NEW.protocol_params->>'target_id',''),registration,
      NEW.protocol_params->>'device_code')::text
    WHEN 'Modbus' THEN jsonb_build_array(COALESCE(NEW.protocol_params->>'target_id',''),registration,
      COALESCE((NEW.protocol_params->>'slave_id')::integer,1))::text
    ELSE jsonb_build_array(COALESCE(NEW.protocol_params->>'target_id',''),registration)::text END;
  RETURN NEW;
END $fn$;
)sql";
}
