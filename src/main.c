/* Nodo BLE Mesh con Sensor Server: illuminamento (lux), temperatura di colore
 * (K) e cromaticita' CIE 1931 (tutti standard SIG), piu' lo spettro (0xF341,
 * unico tipo custom) da AS7341 (11 canali spettrali, I2C 0x39). Zephyr non ha
 * un driver AS7341: l'accesso ai registri e' qui sotto.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>
#include <zephyr/bluetooth/mesh/rpr_srv.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/mesh.h>
#include <bluetooth/mesh/models.h>
#include <bluetooth/mesh/sensor_types.h>

LOG_MODULE_REGISTER(as7341_node, LOG_LEVEL_INF);

/* Non esposta nell'header pubblico ma esportata dallo stack (come nel provisioner). */
extern uint16_t bt_mesh_primary_addr(void);

#define SAMPLE_PERIOD_S 5
#define PUBLISH_PERIOD_S 30

/* ---- Driver AS7341 ----------------------------------------------------- */

#define AS_ADDR 0x39
#define AS_ID 0x92
#define AS_ENABLE 0x80
#define AS_ATIME 0x81
#define AS_STATUS2 0xA3
#define AS_CFG1 0xAA
#define AS_CFG6 0xAF
#define AS_ASTEP_L 0xCA
#define AS_CH0_L 0x95

#define AS_ATIME_V 29
#define AS_ASTEP_V 599
/* tempo di integrazione = (ATIME+1)*(ASTEP+1)*2.78 us ~ 50 ms */
#define AS_TINT_MS ((AS_ATIME_V + 1) * (AS_ASTEP_V + 1) * 2.78f / 1000.0f)

static const struct device *as_bus;
static uint8_t as_gain; /* codice CFG1: guadagno = 2^(gain-1), 0 => 0.5x */

/* SMUX: F1-F4 + clear + NIR, poi F5-F8 + clear + NIR (da datasheet/Adafruit). */
static const uint8_t smux_f1f4[20] = {0x30, 0x01, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x50, 0x00,
				      0x00, 0x00, 0x20, 0x04, 0x00, 0x30, 0x01, 0x50, 0x00, 0x06};
static const uint8_t smux_f5f8[20] = {0x00, 0x00, 0x00, 0x40, 0x02, 0x00, 0x10, 0x03, 0x50, 0x10,
				      0x03, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x50, 0x00, 0x06};

static int as_wr(uint8_t reg, uint8_t val)
{
	return i2c_reg_write_byte(as_bus, AS_ADDR, reg, val);
}

static int as_rd(uint8_t reg, uint8_t *val)
{
	return i2c_reg_read_byte(as_bus, AS_ADDR, reg, val);
}

static int as_set_smux(const uint8_t *cfg)
{
	int err = as_wr(AS_ENABLE, 0x01); /* PON, spettrale off */

	if (!err) {
		err = as_wr(AS_CFG6, 0x10); /* scrittura SMUX */
	}
	for (int i = 0; !err && i < 20; i++) {
		err = as_wr(i, cfg[i]);
	}
	if (!err) {
		err = as_wr(AS_ENABLE, 0x11); /* PON | SMUXEN */
	}
	for (int i = 0; !err && i < 50; i++) {
		uint8_t e;

		k_msleep(2);
		err = as_rd(AS_ENABLE, &e);
		if (!err && !(e & 0x10)) {
			return 0;
		}
	}
	return err ? err : -ETIMEDOUT;
}

/* Attende AVALID (dato spettrale pronto). */
static int as_wait_data(void)
{
	for (int i = 0; i < 100; i++) {
		uint8_t st;
		int err;

		k_msleep(10);
		err = as_rd(AS_STATUS2, &st);
		if (err || (st & 0x40)) {
			return err;
		}
	}
	return -ETIMEDOUT;
}

/* Misura 6 canali con la mappa SMUX data; out = 4 filtri, clear, NIR. */
static int as_measure(const uint8_t *cfg, uint16_t out[6])
{
	uint8_t buf[12];
	int err = as_set_smux(cfg);

	if (!err) {
		err = as_wr(AS_ENABLE, 0x03); /* PON | SP_EN */
	}
	if (!err) {
		err = as_wait_data();
	}
	if (!err) {
		err = i2c_burst_read(as_bus, AS_ADDR, AS_CH0_L, buf, sizeof(buf));
	}
	as_wr(AS_ENABLE, 0x01);
	if (err) {
		return err;
	}
	for (int i = 0; i < 6; i++) {
		out[i] = buf[2 * i] | (buf[2 * i + 1] << 8);
	}
	return 0;
}

static int as_init(const struct device *bus)
{
	uint8_t id;

	as_bus = bus;
	if (!device_is_ready(bus) || as_rd(AS_ID, &id) || (id >> 2) != 0x09) {
		return -ENODEV;
	}
	as_gain = 6; /* 32x */
	return as_wr(AS_ENABLE, 0x01) ?: as_wr(AS_ATIME, AS_ATIME_V) ?:
	       as_wr(AS_ASTEP_L, AS_ASTEP_V & 0xff) ?: as_wr(AS_ASTEP_L + 1, AS_ASTEP_V >> 8) ?:
	       as_wr(AS_CFG1, as_gain);
}

/* CIE 1931 xbar/ybar/zbar alle lunghezze d'onda centrali di F1..F8 (415..680 nm). */
static const float cie_x[8] = {0.044f, 0.336f, 0.096f, 0.036f, 0.514f, 1.026f, 0.642f, 0.047f};
static const float cie_y[8] = {0.001f, 0.030f, 0.139f, 0.607f, 0.995f, 0.757f, 0.265f, 0.017f};
static const float cie_z[8] = {0.207f, 1.772f, 0.813f, 0.060f, 0.006f, 0.001f, 0.000f, 0.000f};

/* Fattore di taratura conteggi-base -> lux: indicativo, da tarare con un luxmetro. */
#define LUX_K 0.35f

static struct {
	bool valid;
	float lux;
	float cx, cy;
	uint32_t cct;
	uint16_t ch[8], clear, nir;
	uint8_t gain;
	float norm; /* 1 / (guadagno * tempo di integrazione in ms) */
} cache;

/* Scelta guadagno automatica sul canale clear. */
static void as_autogain(uint16_t clear)
{
	uint8_t g = as_gain;

	if (clear > 50000 && g > 0) {
		g--;
	} else if (clear < 3000 && g < 9) {
		g++;
	}
	if (g != as_gain) {
		as_gain = g;
		as_wr(AS_CFG1, as_gain);
	}
}

static int as_sample(void)
{
	uint16_t a[6], b[6];
	int err = as_measure(smux_f1f4, a) ?: as_measure(smux_f5f8, b);

	if (err) {
		return err;
	}
	for (int i = 0; i < 4; i++) {
		cache.ch[i] = a[i];
		cache.ch[4 + i] = b[i];
	}
	cache.clear = a[4];
	cache.nir = a[5];
	cache.gain = as_gain;

	float gain = as_gain ? (float)(1u << (as_gain - 1)) : 0.5f;
	float norm = 1.0f / (gain * AS_TINT_MS);

	cache.norm = norm;
	float X = 0, Y = 0, Z = 0;

	for (int i = 0; i < 8; i++) {
		float c = cache.ch[i] * norm;

		X += c * cie_x[i];
		Y += c * cie_y[i];
		Z += c * cie_z[i];
	}
	cache.lux = Y * LUX_K;
	if (X + Y + Z > 0.0f) {
		float x = X / (X + Y + Z), y = Y / (X + Y + Z);

		cache.cx = x;
		cache.cy = y;
		float n = (x - 0.3320f) / (0.1858f - y); /* McCamy */
		float cct = 449.0f * n * n * n + 3525.0f * n * n + 6823.3f * n + 5520.33f;

		cache.cct = cct < 800.0f ? 800 : cct > 65000.0f ? 65000 : (uint32_t)cct;
	}
	cache.valid = true;
	as_autogain(cache.clear);
	return 0;
}

/* ---- Sensori ---------------------------------------------------------- */

static const struct bt_mesh_sensor_descriptor desc = {
	.tolerance = {
		.negative = BT_MESH_SENSOR_TOLERANCE_ENCODE(0),
		.positive = BT_MESH_SENSOR_TOLERANCE_ENCODE(0),
	},
	.sampling_type = BT_MESH_SENSOR_SAMPLING_UNSPECIFIED,
	.period = 0,
	.update_interval = 0,
};

/* Calibrazione lux dall'app (Sensor Setting Set, property 0x004E, metodo Silvair):
 * l'app scrive il lux reale R in centilux; coefficiente = R / lux grezzo attuale,
 * applicato a tutte le letture e salvato in flash. Vedi CALIBRAZIONE-LUX.md.
 */
#define LUX_CAL_UNKNOWN 0xFFFFFFu

static struct {
	float k;		/* coefficiente utente sopra LUX_K */
	uint32_t ref_cl;	/* ultimo lux reale scritto dall'app (centilux) */
} lux_cal = {.k = 1.0f, .ref_cl = LUX_CAL_UNKNOWN};

/* Lux calibrati in centilux. */
static int lux_centi(void)
{
	return (int)(cache.lux * lux_cal.k * 100.0f);
}

static int lux_cal_settings_set(const char *name, size_t len, settings_read_cb read_cb,
				void *cb_arg)
{
	if (!strcmp(name, "luxcal") && len == sizeof(lux_cal)) {
		read_cb(cb_arg, &lux_cal, sizeof(lux_cal));
		return 0;
	}
	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(as7341, "as7341", NULL, lux_cal_settings_set, NULL, NULL);

static void lux_cal_get(struct bt_mesh_sensor_srv *srv, struct bt_mesh_sensor *sensor,
			const struct bt_mesh_sensor_setting *setting, struct bt_mesh_msg_ctx *ctx,
			struct bt_mesh_sensor_value *rsp)
{
	struct sensor_value v = {.val1 = lux_cal.ref_cl / 100, .val2 = (lux_cal.ref_cl % 100) * 10000};

	if (lux_cal.ref_cl == LUX_CAL_UNKNOWN ||
	    bt_mesh_sensor_value_from_sensor_value(setting->type->channels[0].format, &v, rsp)) {
		rsp->format = setting->type->channels[0].format;
		memset(rsp->raw, 0xFF, 3);
	}
}

static int lux_cal_set(struct bt_mesh_sensor_srv *srv, struct bt_mesh_sensor *sensor,
		       const struct bt_mesh_sensor_setting *setting, struct bt_mesh_msg_ctx *ctx,
		       const struct bt_mesh_sensor_value *value)
{
	struct sensor_value v;

	if (bt_mesh_sensor_value_to_sensor_value(value, &v) != BT_MESH_SENSOR_VALUE_NUMBER) {
		/* "non noto" (0xFFFFFF): azzera la calibrazione */
		lux_cal.k = 1.0f;
		lux_cal.ref_cl = LUX_CAL_UNKNOWN;
	} else {
		float real = sensor_value_to_float(&v);

		if (!cache.valid || cache.lux < 0.0005f || real <= 0.0f) {
			return -EINVAL;
		}
		lux_cal.k = real / cache.lux;
		lux_cal.ref_cl = (uint32_t)(real * 100.0f + 0.5f);
		LOG_INF("calibrazione lux: reale %d.%02u, grezzo %d.%03d, k=%d.%03d",
			(int)real, (unsigned)(lux_cal.ref_cl % 100), (int)cache.lux,
			(int)((cache.lux - (int)cache.lux) * 1000), (int)lux_cal.k,
			(int)((lux_cal.k - (int)lux_cal.k) * 1000));
	}
	return settings_save_one("as7341/luxcal", &lux_cal, sizeof(lux_cal));
}

static const struct bt_mesh_sensor_setting lux_settings[] = {
	{
		.type = &bt_mesh_sensor_present_amb_light_level,
		.get = lux_cal_get,
		.set = lux_cal_set,
	},
};

static int lux_get(struct bt_mesh_sensor_srv *srv, struct bt_mesh_sensor *sensor,
		   struct bt_mesh_msg_ctx *ctx, struct bt_mesh_sensor_value *rsp)
{
	struct sensor_value v;

	if (!cache.valid) {
		return -EAGAIN;
	}
	sensor_value_from_float(&v, cache.lux * lux_cal.k);
	int err = bt_mesh_sensor_value_from_sensor_value(sensor->type->channels[0].format, &v,
							 rsp);

	return (err && err != -ERANGE) ? err : 0;
}

static int cct_get(struct bt_mesh_sensor_srv *srv, struct bt_mesh_sensor *sensor,
		   struct bt_mesh_msg_ctx *ctx, struct bt_mesh_sensor_value *rsp)
{
	struct sensor_value v = {.val1 = cache.cct};

	if (!cache.valid) {
		return -EAGAIN;
	}
	int err = bt_mesh_sensor_value_from_sensor_value(sensor->type->channels[0].format, &v,
							 rsp);

	return (err && err != -ERANGE) ? err : 0;
}

static int chrom_get(struct bt_mesh_sensor_srv *srv, struct bt_mesh_sensor *sensor,
		     struct bt_mesh_msg_ctx *ctx, struct bt_mesh_sensor_value *rsp)
{
	const float xy[2] = {cache.cx, cache.cy};

	if (!cache.valid) {
		return -EAGAIN;
	}
	for (int i = 0; i < 2; i++) {
		struct sensor_value v;

		sensor_value_from_float(&v, xy[i]);
		int err = bt_mesh_sensor_value_from_sensor_value(sensor->type->channels[i].format,
								 &v, &rsp[i]);

		if (err && err != -ERANGE) {
			return err;
		}
	}
	return 0;
}

static struct bt_mesh_sensor lux_sensor = {
	.type = &bt_mesh_sensor_present_amb_light_level,
	.get = lux_get,
	.descriptor = &desc,
	.settings = {.list = lux_settings, .count = ARRAY_SIZE(lux_settings)},
};
static struct bt_mesh_sensor cct_sensor = {
	.type = &bt_mesh_sensor_present_correlated_col_temp,
	.get = cct_get,
	.descriptor = &desc,
};
static struct bt_mesh_sensor chrom_sensor = {
	.type = &bt_mesh_sensor_present_cie_1931_chromaticity_coords,
	.get = chrom_get,
	.descriptor = &desc,
};

/* Spettro: tipo personalizzato (non standard). 10 canali count_16, nell'ordine
 * F1..F8 (415..680 nm), clear, NIR. Valore = conteggi / (guadagno * ms) * 100,
 * saturato a 65534.
 */
#define SPEC_CH 10
#define SPEC_PROP_ID 0xF341
#define SPEC_SCALE 100.0f

#ifdef CONFIG_BT_MESH_SENSOR_LABELS
#define SPEC_CHAN(n) {.format = &bt_mesh_sensor_format_count_16, .name = n}
#else
#define SPEC_CHAN(n) {.format = &bt_mesh_sensor_format_count_16}
#endif

static const struct bt_mesh_sensor_channel spec_channels[SPEC_CH] = {
	SPEC_CHAN("F1 415nm"), SPEC_CHAN("F2 445nm"), SPEC_CHAN("F3 480nm"),
	SPEC_CHAN("F4 515nm"), SPEC_CHAN("F5 555nm"), SPEC_CHAN("F6 590nm"),
	SPEC_CHAN("F7 630nm"), SPEC_CHAN("F8 680nm"), SPEC_CHAN("Clear"), SPEC_CHAN("NIR"),
};

static const struct bt_mesh_sensor_type spec_type = {
	.id = SPEC_PROP_ID,
	.channel_count = SPEC_CH,
	.channels = spec_channels,
};

static int spec_get(struct bt_mesh_sensor_srv *srv, struct bt_mesh_sensor *sensor,
		    struct bt_mesh_msg_ctx *ctx, struct bt_mesh_sensor_value *rsp)
{
	uint16_t raw[SPEC_CH];

	if (!cache.valid) {
		return -EAGAIN;
	}
	memcpy(raw, cache.ch, sizeof(cache.ch));
	raw[8] = cache.clear;
	raw[9] = cache.nir;
	for (int i = 0; i < SPEC_CH; i++) {
		float f = raw[i] * cache.norm * SPEC_SCALE;
		struct sensor_value v = {.val1 = f > 65534.0f ? 65534 : (int32_t)f};
		int err = bt_mesh_sensor_value_from_sensor_value(spec_channels[i].format, &v,
								 &rsp[i]);

		if (err && err != -ERANGE) {
			return err;
		}
	}
	return 0;
}

static struct bt_mesh_sensor spec_sensor = {
	.type = &spec_type,
	.get = spec_get,
	.descriptor = &desc,
};

static struct bt_mesh_sensor *const sensors[] = {&lux_sensor, &cct_sensor, &chrom_sensor,
						&spec_sensor};
static struct bt_mesh_sensor_srv sensor_srv =
	BT_MESH_SENSOR_SRV_INIT(sensors, ARRAY_SIZE(sensors));

/* ---- Stato nodo / LED RGB ---------------------------------------------
 * Provisionabile : blu, lampeggio breve ogni 1 s
 * Provisioning   : blu fisso (link PB-ADV/PB-GATT aperto)
 * Provisionato   : verde, impulso breve ogni 3 s
 * Reset          : rosso, lampeggio veloce per 2 s, poi torna provisionabile
 */
enum node_state { ST_UNPROV, ST_LINK, ST_PROV, ST_RESET };

static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_b = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

static volatile enum node_state state = ST_UNPROV;
/* Identify dall'app (Health Server attention): ha priorita' su ogni stato. */
static volatile bool attention;
static volatile uint32_t attention_t;
static volatile bool attention_lit;

/* Bilanciamento del bianco: il rosso del LED e' molto piu' efficiente degli
 * altri canali, quindi i tre colori usano un duty diverso (su 8 passi).
 */
#define WHITE_R 1
#define WHITE_G 8
#define WHITE_B 4

static void white_timer_fn(struct k_timer *t)
{
	static uint8_t ph;

	ph = (ph + 1) & 7;
	if (attention_lit) {
		gpio_pin_set_dt(&led_r, ph < WHITE_R);
		gpio_pin_set_dt(&led_g, ph < WHITE_G);
		gpio_pin_set_dt(&led_b, ph < WHITE_B);
	}
}
static K_TIMER_DEFINE(white_timer, white_timer_fn, NULL);

#define LED_TICK_MS 100
#define RESET_SHOW_MS 2000

static void led_set(bool r, bool g, bool b)
{
	gpio_pin_set_dt(&led_r, r);
	gpio_pin_set_dt(&led_g, g);
	gpio_pin_set_dt(&led_b, b);
}

static void led_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(led_work, led_fn);

static void led_fn(struct k_work *w)
{
	static uint32_t t;

	t++;
	if (attention) {
		/* 4 lampi bianchi (100 ms on / 100 ms off), poi 800 ms di pausa. */
		uint32_t p = attention_t++ % 12;
		bool on = p < 8 && !(p & 1);

		attention_lit = on;
		if (!on) {
			led_set(false, false, false);
		}
		k_work_reschedule(&led_work, K_MSEC(LED_TICK_MS));
		return;
	}
	switch (state) {
	case ST_UNPROV:
		led_set(false, false, (t % 10) == 0);
		break;
	case ST_LINK:
		led_set(false, false, true);
		break;
	case ST_PROV:
		led_set(false, (t % 30) == 0, false);
		break;
	case ST_RESET:
		led_set(t & 1, false, false);
		break;
	}
	k_work_reschedule(&led_work, K_MSEC(LED_TICK_MS));
}

static void rearm_work_fn(struct k_work *w)
{
	/* Dopo un reset (mesh o locale) il nodo deve tornare provisionabile:
	 * lo stack non riabilita da solo i bearer di provisioning.
	 */
	int err = bt_mesh_prov_enable(BT_MESH_PROV_ADV | BT_MESH_PROV_GATT);

	if (err) {
		LOG_ERR("bt_mesh_prov_enable err=%d", err);
	}
	state = ST_UNPROV;
	LOG_INF("reset completato: in attesa di provisioning");
}
static K_WORK_DELAYABLE_DEFINE(rearm_work, rearm_work_fn);

static void node_reset_begin(void)
{
	state = ST_RESET;
	cache.valid = false;
	k_work_reschedule(&rearm_work, K_MSEC(RESET_SHOW_MS));
}

/* ---- Composition ------------------------------------------------------ */

BT_MESH_HEALTH_PUB_DEFINE(health_pub, 0);

static void attention_on(const struct bt_mesh_model *mod)
{
	attention_t = 0;
	attention = true;
	k_timer_start(&white_timer, K_USEC(250), K_USEC(250));
}

static void attention_off(const struct bt_mesh_model *mod)
{
	attention = false;
	attention_lit = false;
	k_timer_stop(&white_timer);
	led_set(false, false, false);
}

static const struct bt_mesh_health_srv_cb health_cb = {
	.attn_on = attention_on,
	.attn_off = attention_off,
};

static struct bt_mesh_health_srv health_srv = {
	.cb = &health_cb,
};

static struct bt_mesh_elem elements[] = {
	BT_MESH_ELEM(0,
		     BT_MESH_MODEL_LIST(BT_MESH_MODEL_CFG_SRV,
					BT_MESH_MODEL_HEALTH_SRV(&health_srv, &health_pub),
					BT_MESH_MODEL_SENSOR_SRV(&sensor_srv),
					BT_MESH_MODEL_RPR_SRV),
		     BT_MESH_MODEL_NONE),
};

static const struct bt_mesh_comp comp = {
	.cid = CONFIG_BT_COMPANY_ID,
	.elem = elements,
	.elem_count = ARRAY_SIZE(elements),
};

static uint8_t dev_uuid[16];

static void prov_link_open(bt_mesh_prov_bearer_t bearer)
{
	LOG_INF("provisioning: link aperto (bearer=%d)", bearer);
	state = ST_LINK;
}

static void prov_link_close(bt_mesh_prov_bearer_t bearer)
{
	LOG_INF("provisioning: link chiuso (bearer=%d)", bearer);
	if (state == ST_LINK) {
		state = bt_mesh_is_provisioned() ? ST_PROV : ST_UNPROV;
	}
}

static void prov_complete(uint16_t net_idx, uint16_t addr)
{
	LOG_INF("provisionato: net_idx=%u addr=0x%04x", net_idx, addr);
	state = ST_PROV;
}

/* Node Reset ricevuto dalla mesh (Config Client del provisioner): lo stack ha
 * gia' cancellato le chiavi, qui si segnala e si riabilita il provisioning.
 */
static void prov_reset(void)
{
	LOG_INF("reset da mesh (Node Reset)");
	node_reset_begin();
}

static const struct bt_mesh_prov prov = {
	.uuid = dev_uuid,
	.link_open = prov_link_open,
	.link_close = prov_link_close,
	.complete = prov_complete,
	.reset = prov_reset,
};

/* ---- Shell USB: reset locale e stato ---------------------------------- */

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "reset locale: cancello provisioning e chiavi mesh");
	node_reset_begin();
	bt_mesh_reset();
	return 0;
}

/* Il bootloader Adafruit entra in DFU seriale se GPREGRET = 0x4e al reset. */
static int cmd_dfu(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "riavvio nel bootloader DFU...");
	k_msleep(100);
	nrf_power_gpregret_set(NRF_POWER, 0, 0x4e);
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const names[] = {"provisionabile", "link provisioning", "provisionato",
					    "reset"};

	shell_print(sh, "stato: %s", names[state]);
	if (bt_mesh_is_provisioned()) {
		shell_print(sh, "indirizzo: 0x%04x", bt_mesh_primary_addr());
	}
	if (cache.valid) {
		int cl = lux_centi();

		shell_print(sh, "%d.%02d lux (grezzo %d.%03d, k %d.%03d)  %u K  gain %u", cl / 100,
			    cl % 100, (int)cache.lux, (int)((cache.lux - (int)cache.lux) * 1000),
			    (int)lux_cal.k, (int)((lux_cal.k - (int)lux_cal.k) * 1000), cache.cct,
			    cache.gain);
		shell_print(sh, "F1-F8: %u %u %u %u %u %u %u %u", cache.ch[0], cache.ch[1],
			    cache.ch[2], cache.ch[3], cache.ch[4], cache.ch[5], cache.ch[6],
			    cache.ch[7]);
		shell_print(sh, "clear %u  NIR %u", cache.clear, cache.nir);
	} else {
		shell_print(sh, "nessuna misura valida");
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(node_cmds,
	SHELL_CMD(reset, NULL, "Reset mesh locale (torna provisionabile)", cmd_reset),
	SHELL_CMD(dfu, NULL, "Riavvia nel bootloader (aggiornamento firmware)", cmd_dfu),
	SHELL_CMD(status, NULL, "Stato nodo e ultima misura", cmd_status),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(node, &node_cmds, "Comandi nodo AS7341", NULL);

/* ---- Campionamento / pubblicazione ------------------------------------ */

static void publish_all(void)
{
	struct bt_mesh_sensor_value v[SPEC_CH];

	if (!cache.valid || !bt_mesh_is_provisioned()) {
		return;
	}
	for (int i = 0; i < ARRAY_SIZE(sensors); i++) {
		if (sensors[i]->get(&sensor_srv, sensors[i], NULL, v)) {
			continue;
		}
		/* -EADDRNOTAVAIL = publicazione non ancora configurata: normale. */
		int err = bt_mesh_sensor_srv_pub(&sensor_srv, NULL, sensors[i], v);

		if (err && err != -EADDRNOTAVAIL) {
			LOG_WRN("pub sensore %d err=%d", i, err);
		}
	}
}

static void sample_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(sample_work, sample_fn);

static void sample_fn(struct k_work *w)
{
	static int ticks;
	int err = as_sample();

	if (err == 0) {
		int cl = lux_centi();

		LOG_INF("%d.%02d lux  %u K  clear %u  gain %u", cl / 100, cl % 100, cache.cct,
			cache.clear, cache.gain);
		if (++ticks >= PUBLISH_PERIOD_S / SAMPLE_PERIOD_S) {
			ticks = 0;
			publish_all();
		}
	} else {
		LOG_WRN("as_sample err=%d", err);
	}
	k_work_reschedule(&sample_work, K_SECONDS(SAMPLE_PERIOD_S));
}

/* ---- Avvio ------------------------------------------------------------- */

static void bt_ready(int err)
{
	if (err) {
		LOG_ERR("bt init err=%d", err);
		return;
	}
	err = bt_mesh_init(&prov, &comp);
	if (err) {
		LOG_ERR("mesh init err=%d", err);
		return;
	}
	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}
	/* No-op se settings_load() ha ripristinato un provisioning. */
	bt_mesh_prov_enable(BT_MESH_PROV_ADV | BT_MESH_PROV_GATT);
	state = bt_mesh_is_provisioned() ? ST_PROV : ST_UNPROV;
	LOG_INF("mesh pronta, %s",
		bt_mesh_is_provisioned() ? "provisionato" : "in attesa di provisioning");
}

int main(void)
{
	const struct gpio_dt_spec *leds[] = {&led_r, &led_g, &led_b};

	for (int i = 0; i < 3; i++) {
		if (gpio_is_ready_dt(leds[i])) {
			gpio_pin_configure_dt(leds[i], GPIO_OUTPUT_INACTIVE);
		}
	}
	k_work_reschedule(&led_work, K_MSEC(LED_TICK_MS));
	/* Prova D4/D5 (i2c1) poi D9/D10 (i2c0). */
	if (as_init(DEVICE_DT_GET(DT_NODELABEL(i2c1))) == 0) {
		LOG_INF("AS7341 su i2c1 (D4/D5)");
	} else if (as_init(DEVICE_DT_GET(DT_NODELABEL(i2c0))) == 0) {
		LOG_INF("AS7341 su i2c0 (D9/D10)");
	} else {
		LOG_ERR("AS7341 non trovato (controlla cablaggio, 0x39)");
		as_bus = NULL;
	}
	if (as_bus) {
		k_work_reschedule(&sample_work, K_SECONDS(SAMPLE_PERIOD_S));
	}

	/* UUID dal Device ID hardware, cosi' resta stabile tra i riavvii. */
	uint8_t id[8];

	hwinfo_get_device_id(id, sizeof(id));
	memcpy(dev_uuid, id, sizeof(id));
	memcpy(dev_uuid + 8, id, sizeof(id));

	int err = bt_enable(bt_ready);

	if (err) {
		LOG_ERR("bt_enable err=%d", err);
	}
	return 0;
}
