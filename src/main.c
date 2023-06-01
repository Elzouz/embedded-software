// Libraries
#include <string.h>
#include <zephyr/kernel.h>
#include <nrf_modem_at.h>
#include <modem/lte_lc.h>
#include <modem/location.h>
#include <date_time.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/gpio.h>

// Application specific imports
#include "LocationEngine/LocationEngine.h"
#include "./mqtt/mqtt.h"
#include "SmsListener/SmsListener.h"
#include "Dispatcher/Dispatcher.h"
#include "Responder/Responder.h"

LOG_MODULE_REGISTER(main, 4);

/* Indoor location engine sends 1 for PSM mode and 0 for eDRX mode */
#define POWER_MODE_COMMAND_PIN		DT_ALIAS(gpiocus10)

static const struct gpio_dt_spec power_mode_command_recv_pin = GPIO_DT_SPEC_GET_OR(POWER_MODE_COMMAND_PIN, gpios, {0});

static K_SEM_DEFINE(lte_connected, 0, 1);

static K_SEM_DEFINE(time_update_finished, 0, 1);

static void date_time_evt_handler(const struct date_time_evt *evt)
{
	k_sem_give(&time_update_finished);
}

static void lte_event_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
		     (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			LOG_INF("Connected to LTE\n");
			k_sem_give(&lte_connected);
		}
		break;
	default:
		break;
	}
}

/**
 * @brief Callback function for when an SMS is received.
*/
static void smsCallback(struct sms_data *const data, void *context)
{
	LOG_INF("smsCallback started");
	if (data == NULL) {
		printk("%s with NULL data\n", __func__);
		return;
	}

	if (data->type == SMS_TYPE_DELIVER) {
		/* When SMS message is received, print information */
		struct sms_deliver_header *header = &data->header.deliver;

		printk("\nSMS received:\n");
		printk("\tTime:   %02d-%02d-%02d %02d:%02d:%02d\n",
			header->time.year,
			header->time.month,
			header->time.day,
			header->time.hour,
			header->time.minute,
			header->time.second);

		printk("\tText:   '%s'\n", data->payload);


		// Two commands are supported over SMS. 
		// We should keep these command strings small as possible for least data transfer.
		if (strcmp(data->payload, "activate") == 0) {
			changeDispatcherState(DISPATCHER_STATE_RESPONDING);
			LOG_INF("Dispatcher state changed to DISPATCHER_STATE_RESPONDING\n");
		} else if (strcmp(data->payload, "deactivate") == 0) {
			changeDispatcherState(DISPATCHER_STATE_IDLE);
			LOG_INF("Dispatcher state changed to DISPATCHER_STATE_IDLE\n");
		} else {
			LOG_ERR("Unknown command: %s\n", data->payload);
		}

	} else if (data->type == SMS_TYPE_STATUS_REPORT) {
		printk("SMS status report received\n");
	} else {
		printk("SMS protocol message with unknown type received\n");
	}
}

/**
 * @brief Initialize LTE and wait for connection.
*/
static int initializeLte() {
	int err;

	if (IS_ENABLED(CONFIG_DATE_TIME)) {
		/* Registering early for date_time event handler to avoid missing
		 * the first event after LTE is connected.
		 */
		date_time_register_handler(date_time_evt_handler);
	}

	LOG_INF("Connecting to LTE...\n");

	lte_lc_init();
	lte_lc_register_handler(lte_event_handler);

	/* Enable PSM. */
	/* Turn on LTE power saving features. Also,
	 * request power saving features before network registration. Some
	 * networks rejects timer updates after the device has registered to the
	 * LTE network.
	 */
	lte_lc_psm_req(true);
	/** enhanced Discontinuous Reception */

	// LOG_INF("Setting EDRX mode");
	// Todo: Turn on EDRX mode after development complete.
	err = lte_lc_edrx_req(true);
	
	if (err) {
		LOG_INF("lte_lc_edrx_req, error: %d\n", err);
	}

	err = lte_lc_connect();

	k_sem_take(&lte_connected, K_FOREVER);

	return err;
}

/**
 * @brief Initialize location services.
 * 
 * DOES NOT call method to request location, only initializes the library.
*/
static int initializeLocation() {
		/* A-GPS/P-GPS needs to know the current time. */
	if (IS_ENABLED(CONFIG_DATE_TIME)) {
		LOG_INF("Waiting for current time\n");

		/* Wait for an event from the Date Time library. */
		k_sem_take(&time_update_finished, K_MINUTES(10));

		if (!date_time_is_valid()) {
			LOG_INF("Failed to get current time. Continuing anyway.\n");
		}
	}

	int err = location_init(location_event_handler);
	if (err) {
		LOG_INF("Initializing the Location library failed, error: %d\n", err);
		return -1;
	}
	
	LOG_INF("Location library initialized\n");
	return err;
}

#define BUTTON_DEBOUNCE_TIME 50
uint32_t last_button_press_time;
/**
 * @brief Callback function for when the power mode command is received through gpio interrupt.
*/
static void power_mode_command_recieved() {
	k_sleep(K_MSEC(100));
	int power_mode_command = gpio_pin_get_dt(&power_mode_command_recv_pin);
 	uint32_t newPressTime = k_uptime_get_32();
	
	if (k_uptime_get_32() - last_button_press_time < BUTTON_DEBOUNCE_TIME) {
		return;
	}

	if (power_mode_command == 0) {
		LOG_INF("Power mode set to eDRX");
		// lte_lc_psm_req(false);
		lte_lc_edrx_req(true);
		
	} else if (power_mode_command == 1) {
		LOG_INF("Power mode set to PSM");
		// lte_lc_psm_req(true);
		lte_lc_edrx_req(false);
	}
	
	last_button_press_time = newPressTime;

}

static struct gpio_callback pwr_mode_cb_data;

/**
 * @brief Application entry point.
*/
int main(void) {

retry:
	if (!device_is_ready(power_mode_command_recv_pin.port)) {
		LOG_ERR("Error: button device %s is not ready\n", power_mode_command_recv_pin.port->name);
		goto retry;
	}

	int err;	
	int retry_count = 0;

	err = gpio_pin_configure_dt(&power_mode_command_recv_pin, GPIO_INPUT | GPIO_PULL_DOWN);
	if (err != 0) {
		LOG_ERR("Error %d: failed to configure %s pin %d\n", 
			err, power_mode_command_recv_pin.port->name, power_mode_command_recv_pin.pin);
		goto retry;
	}

	err = gpio_pin_interrupt_configure_dt(&power_mode_command_recv_pin, GPIO_INT_EDGE_BOTH);
	if (err != 0) {
		LOG_ERR("Error %d: failed to configure interrupt on %s pin %d\n",
			err, power_mode_command_recv_pin.port->name, power_mode_command_recv_pin.pin);
		return;
	}


	gpio_init_callback(&pwr_mode_cb_data, power_mode_command_recieved, BIT(power_mode_command_recv_pin.pin));
	gpio_add_callback(power_mode_command_recv_pin.port, &pwr_mode_cb_data);

	// Connect to LTE network with retries if necessary.
    while (retry_count < CONFIG_LTE_CONNECT_MAX_RETRIES) {
        err = initializeLte();

        if (err) {
            LOG_INF("Failed to connect to LTE with error: %d\n", err);
            retry_count++;
            k_sleep(K_MSEC(CONFIG_LTE_CONNECT_RETRY_DELAY_S * 1000));
        } else {
			break;
		}
    }

	if (retry_count == CONFIG_LTE_CONNECT_MAX_RETRIES) {
	    LOG_ERR("Max retry limit reached for connecting to LTE, program exiting.");
		return -1;
	}

	retry_count = 0;
	// Initialize location services with retries if necessary.
    while (retry_count < CONFIG_LOCATION_SERVICES_INIT_MAX_RETRIES) {
        err = initializeLocation();

        if (err) {
            LOG_INF("Failed to initialize location services with error: %d\n", err);
            retry_count++;
            k_sleep(K_MSEC(CONFIG_LOCATION_SERVICES_INIT_RETRY_DELAY_S * 1000));
        } else {
			break;
		}
    }

	if (retry_count == CONFIG_LTE_CONNECT_MAX_RETRIES) {
	    LOG_ERR("Max retry limit reached for initializing location services, program exiting.");
		return -1;
	}

	sms_listener_init(smsCallback);

	return 0;
}
