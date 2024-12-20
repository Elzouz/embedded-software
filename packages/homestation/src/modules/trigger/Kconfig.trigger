menu "Trigger"

config MQTT_TRIGGER_THREAD_STACK_SIZE
	int "Thread stack size"
	default 512

config MQTT_TRIGGER_TIMEOUT_SECONDS
	int "Trigger timer timeout"
	default 60

module = MQTT_TRIGGER
module-str = Trigger
source "subsys/logging/Kconfig.template.log_config"

endmenu # Trigger
