#!/bin/sh

# where payload is 24h time, such as "06:45"
mosquitto_pub -h $MQTT_BROKER_HOST -u $MQTT_USERNAME -P $MQTT_PASSWORD -t 'homeassistant/text/retro-clock-radio/ALARM_TIME/set' -m "$1"
