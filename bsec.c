#include <errno.h>
#include <err.h>
#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <netdb.h>
#include <linux/i2c-dev.h>
#include <i2c/smbus.h>
#include "bme680.h"
#include "bsec_interface.h"
#include "bsec_datatypes.h"

#define UNRELIABLE 0
#define LOW_ACCURACY 1
#define MEDIUM_ACCURACY 2
#define HIGH_ACCURACY 3
#define CONFIG_FILE "/etc/bsec.conf"
#define BSEC_CONFIG_FILE "/bsec_iaq.config"
#define NUM_USED_OUTPUTS 13

int rrd_sock, rrd_port, i2c_fd;
FILE * log_out, * log_err;
uint8_t dev_addr = BME680_I2C_ADDR_PRIMARY;
float sampling_rate_s = BSEC_SAMPLE_RATE_LP;
int save_state_every_seconds;
float temperature_offset = 0.0f;
char config_file[255] = { 0 };
char bsec_config_path[255] = { 0 };
char bsec_state_file[255] = { 0 };
char log_file[255] = { 0 };
char rrd_host[255] = { 0 }, device[255] = { 0 }, rrd_db_file[16] = { 0 };
struct bme680_dev bme680;

int8_t bus_write(uint8_t dev_addr, uint8_t reg_addr, uint8_t *reg_data_ptr, uint16_t data_len) {
	if (i2c_smbus_write_i2c_block_data(i2c_fd, reg_addr, data_len, reg_data_ptr) < 0) {
		fprintf(log_err, "bus_write failed\n");
		return -1;
	};
	return 0;
}

int8_t bus_read(uint8_t dev_addr, uint8_t reg_addr, uint8_t *reg_data_ptr, uint16_t data_len) {
	if (i2c_smbus_read_i2c_block_data(i2c_fd, reg_addr, data_len, reg_data_ptr) < 0) {
		fprintf(log_err, "bus_read failed\n");
		return -1;
	}
	return 0;
}

void bme_sleep(uint32_t t_ms) {
	struct timespec t;
	t.tv_sec = t_ms / 1000;
	t.tv_nsec = (t_ms % 1000) * 1000000;
	nanosleep(&t, NULL);
}

int64_t get_timestamp_us() {
	struct timeval tv;
    int64_t system_current_time;
	int res = gettimeofday(&tv, NULL);
	if (res != 0) {
		fprintf(log_err, "get_timestamp_us error: %s\n", strerror(errno));
		return -1;
	}
	system_current_time = (int64_t)(tv.tv_sec) * 1000000 + tv.tv_usec;
	return system_current_time;
}

void rrd_connect() {
	struct sockaddr_in sock_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(rrd_port) //converts port from host to network byte order
	};
	//converts IP address in dotted notation to struct in_addr, which has s_addr member in network byte order
	int res = inet_aton(rrd_host, &sock_addr.sin_addr);
	if (res == 0) {
		fprintf(log_err, "rrd_connect: inet_aton() - bad IP address of RRD_HOST");
		exit(-1);
	}
	//rrdtool listens on rrd_host:13900/tcp
	if (rrd_sock) close(rrd_sock);
	rrd_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (rrd_sock == -1) {
		fprintf(log_err, "rrd_connect: error creating rrdtool socket");
		exit(-1);
	}
	res = connect(rrd_sock, (struct sockaddr *)&sock_addr, sizeof(sock_addr));
	if (res == -1) {
		fprintf(log_err, "rrd_connect: connect() failed");
		exit(-1);
	}
}

void save_data(int64_t timestamp, float temp, float humidity, float pressure, float gas, uint8_t gas_acc, 
	           float iaq, uint8_t iaq_acc, float iaqs, uint8_t iaqs_acc, float gasp, uint8_t gasp_acc,
	           float temp_raw, float humidity_raw, float gas_raw, float co2, uint8_t co2_acc, float bvoc, uint8_t bvoc_acc,
               bsec_library_return_t bsec_status, uint8_t output_stabilization_status, uint8_t output_run_in_status) {
	time_t t = time(NULL);
	char * acc, * status;

	if (!(gasp_acc == iaq_acc == iaqs_acc == co2_acc == bvoc_acc))
		fprintf(log_out, "%sinconsistent accuracies: gasp_acc %u, iaq_acc %u, iaqs_acc %u, co2_acc %u, bvoc_acc %u\n", asctime(localtime(&t)), gasp_acc, iaq_acc, iaqs_acc, co2_acc, bvoc_acc);

	switch (iaqs_acc) {
	case UNRELIABLE: acc = "run-in stab. ongoing";
		break;
	case LOW_ACCURACY: acc = "low";
		break;
	case MEDIUM_ACCURACY: acc = "medium";
		break;
	case HIGH_ACCURACY: acc = "high";
		break;
	default: acc = "unknown";
	}

	switch (bsec_status) {
	case BSEC_OK: status = "ok";
		break;
	case BSEC_E_DOSTEPS_INVALIDINPUT: status = "bsec_do_steps: invalid sensor id";
		break;
	case BSEC_E_DOSTEPS_VALUELIMITS: status = "bsec_do_steps: invalid input sensor signal";
		break;
	case BSEC_E_DOSTEPS_DUPLICATEINPUT: status = "bsec_do_steps: duplicate input sensor id";
		break;
	case BSEC_I_DOSTEPS_NOOUTPUTSRETURNABLE: status = "bsec_do_steps: no memory allocated for return values";
		break;
	case BSEC_W_DOSTEPS_EXCESSOUTPUTS: status = "bsec_do_steps: not enough memory allocated for return values";
		break;
	case BSEC_W_DOSTEPS_TSINTRADIFFOUTOFRANGE: status = "bsec_do_steps: duplicate timestamps";
		break;
	default: status = "bsec_do_step: unknown error";
	}
	
	if (bsec_status != BSEC_OK) 
		fprintf(log_out, "%sbsec status: %s\n", asctime(localtime(&t)), status);
	if (log_out == stdout) {
		fprintf(log_out, "P %.1f mb (hPa)\n", pressure / 100);
		fprintf(log_out, "T %.1f deg C, H %.1f%%, Gi %1.1f Ohm\n", temp, humidity, gas);
		fprintf(log_out, "Tr %.1f deg C, Hr %.1f%%, Gr %.1f Ohm\n", temp_raw, humidity_raw, gas_raw);
		fprintf(log_out, "IAQ %.1f, IAQs %.1f, CO2_equiv %.0f ppm, Breath_VOC_equiv %.1f ppm, acc %s\n", iaq, iaqs, co2, bvoc, acc);
		fprintf(log_out, "IAQ percentage (0 - the best, 100 - the worst): %.0f%%, acc %s\n", gasp, acc);
	}
	if (output_stabilization_status != 1)
		fprintf(log_out, "%soutput stabilization status %u\n", asctime(localtime(&t)), output_stabilization_status);
	if (output_run_in_status != 1)
		fprintf(log_out, "%soutput run-in status %u\n", asctime(localtime(&t)), output_run_in_status);

	char buf[255];
	memset(buf, 0, sizeof(buf));
	if (iaqs_acc >= MEDIUM_ACCURACY) {
		snprintf(buf, sizeof(buf), "update %s N:%.1f:%.1f:%.1f:%.1f:%.1f:%.0f:%.0f:%.1f\r\n", rrd_db_file, temp, humidity, pressure / 100, iaq, iaqs, gasp, co2, bvoc);
	}
	else {
		snprintf(buf, sizeof(buf), "update %s -t temperature:humidity:pressure N:%.1f:%.1f:%.1f\r\n", rrd_db_file, temp, humidity, pressure / 100);
	}
	int n = write(rrd_sock, buf, strlen(buf));
	if (n == -1) {
		fprintf(log_err, "%serror writing to rrd: %s\n", asctime(localtime(&t)), strerror(errno));
		if (errno == 32) { //broken pipe
			rrd_connect();
			n = write(rrd_sock, buf, strlen(buf));
			if (n == -1)
				fprintf(log_err, "%serror writing to rrd: %s\n", asctime(localtime(&t)), strerror(errno));
		}
	}
}

uint32_t state_load(uint8_t *state_buffer, uint32_t n_buffer)
{
	int fd = open(bsec_state_file, O_RDONLY);
	if (fd == -1) {
		fprintf(log_err, "state_load error opening file %s: %s\n", bsec_state_file, strerror(errno));
		return 0;
	}
	size_t res = read(fd, state_buffer, n_buffer);
	if (res == -1) {
		fprintf(log_err, "state_load error reading file %s: %s\n", bsec_state_file, strerror(errno));
		close(fd);
		return 0;
	}
	close(fd);
	fprintf(log_out, "state_load: read %u bytes from %s\n", res, bsec_state_file);
    return (uint32_t)res;
}

void state_save(const uint8_t *state_buffer, uint32_t length)
{
	int fd = creat(bsec_state_file, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (fd == -1) {
		fprintf(log_err, "state_save error creating file %s: %s\n", bsec_state_file, strerror(errno));
		return;
	}
	ssize_t res = write(fd, state_buffer, length);
	if (res == -1) {
		fprintf(log_err, "state_save error writing file %s: %s\n", bsec_state_file, strerror(errno));
		close(fd);
		return;
	}
	close(fd);
	fprintf(log_out, "state_save: written %u bytes to %s\n", res, bsec_state_file);
}
 
uint32_t config_load(uint8_t *config_buffer, uint32_t n_buffer)
{
	strncat(bsec_config_path, BSEC_CONFIG_FILE, strlen(BSEC_CONFIG_FILE));
	FILE * fd = fopen(bsec_config_path, "r");
	if (!fd) {
		fprintf(log_err, "config_load error opening file %s: %s\n", bsec_config_path, strerror(errno));
		return 0;
	}
	int err = fseek(fd, 4, SEEK_CUR);
	if (err == -1) {
		fprintf(log_err, "config_load error seeking %s: %s\n", bsec_config_path, strerror(errno));
		fclose(fd);
		return 0;
	}
	size_t res = fread(config_buffer, 1, n_buffer, fd);
	if (res == -1) {
		fprintf(log_err, "config_load error reading file %s: %s\n", bsec_config_path, strerror(errno));
		fclose(fd);
		return 0;
	}
	fclose(fd);
	fprintf(log_out, "config_load: read %u bytes from %s\n", res, bsec_config_path);
	return (uint32_t)res;
}

char * ltrim(char *s) {
	while (isspace(*s)) s++;
	return s;
}

char * rtrim(char *s) {
	char* back = s + strlen(s);
	while (isspace(*--back));
	*(back + 1) = '\0';
	return s;
}

char * trim(char *s) {
	return rtrim(ltrim(s));
}

int parse_config_file(const char * filename) {
	struct stat config_file_stat;
	if (stat(filename, &config_file_stat) == -1) {
		err(-1, "parse_config_file: file %s", filename);
	}

	FILE * config_fd = fopen(filename, "r");
	if (!config_fd)
		err(-1, "parse_config_file: error opening config file %s", filename);

	size_t len = 0;
	char * line = NULL, *par, *tmp_str;
	while (getline(&line, &len, config_fd) != -1) {
		//printf("parse_config_file: line %s\n", line);
		if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
		par = strtok(line, " ");
		if (strncmp(par, "I2C_DEVICE", 10) == 0) {
			tmp_str = strtok(NULL, "\r\n");
			if (tmp_str) {
				tmp_str = trim(tmp_str);
				strncpy(device, tmp_str, strlen(tmp_str) + 1);
				//printf("parse_config_file: device %s\n", device);
			}
		}
		else if (strncmp(par, "I2C_ADDRESS", 11) == 0) {
			tmp_str = strtok(NULL, "\r\n");
			if (tmp_str) {
				if (strncmp(tmp_str, "SECONDARY", 9) == 0) {
					dev_addr = BME680_I2C_ADDR_SECONDARY;
					//printf("parse_config_file: dev_addr %x\n", dev_addr);
				}
			}
		}
		else if (strncmp(par, "SAMPLE_RATE", 11) == 0) {
			tmp_str = strtok(NULL, "\r\n");
			if (tmp_str) {
				if (strncmp(tmp_str, "HP", 2) == 0)
					sampling_rate_s = BSEC_SAMPLE_RATE_HP;
				else if (strncmp(tmp_str, "ULP", 3) == 0)
					sampling_rate_s = BSEC_SAMPLE_RATE_ULP;
				//printf("parse_config_file: sample_rate %s\n", tmp_str);
			}
		}
		else if (strncmp(par, "TEMPERATURE_OFFSET", 18) == 0) {
			tmp_str = strtok(NULL, "\r\n");
			if (tmp_str) {
				tmp_str = trim(tmp_str);
				if (isdigit(tmp_str[0])) {
					temperature_offset = strtof(tmp_str, NULL);
					//printf("parse_config_file: temperature_offset %.1f\n", temperature_offset);
				}
			}
		}
		else if (strncmp(par, "CONFIG_PATH", 11) == 0) {
			tmp_str = strtok(NULL, "\r\n");
			if (tmp_str) {
				tmp_str = trim(tmp_str);
				strncpy(bsec_config_path, tmp_str, strlen(tmp_str));
				//printf("parse_config_file: bsec_config_path %s\n", bsec_config_path);
			}
		}
		else if (strncmp(par, "STATE_FILE", 10) == 0) {
			tmp_str = strtok(NULL, "\r\n");
			if (tmp_str) {
				tmp_str = trim(tmp_str);
				strncpy(bsec_state_file, tmp_str, strlen(tmp_str));
				//printf("parse_config_file: bsec_state_file %s\n", bsec_state_file);
			}
		}
		else if (strncmp(par, "SAVE_STATE_EVERY_SECONDS", 24) == 0) {
			tmp_str = strtok(NULL, "\r\n");
			if (tmp_str) {
				tmp_str = trim(tmp_str);
				if (isdigit(tmp_str[0])) {
					save_state_every_seconds = atoi(tmp_str);
					//printf("parse_config_file: save_state_every_seconds %d\n", save_state_every_seconds);
				}
			}
		}
		
		else if (strncmp(par, "LOG_FILE", 8) == 0) {
			tmp_str = strtok(NULL, "\r\n");
			if (tmp_str) {
				tmp_str = trim(tmp_str);
				strncpy(log_file, tmp_str, strlen(tmp_str));
				//printf("parse_config_file: log_file %s\n", log_file);
			}
		}
		else if (strncmp(par, "RRD_HOST", 8) == 0) {
			tmp_str = strtok(NULL, "\r\n");
			if (tmp_str) {
				tmp_str = trim(tmp_str);
				strncpy(rrd_host, tmp_str, strlen(tmp_str));
				//printf("parse_config_file: rrd_host %s\n", rrd_host);
			}
		}
		else if (strncmp(par, "RRD_PORT", 8) == 0) {
			tmp_str = strtok(NULL, "\r\n");
			if (tmp_str) {
				tmp_str = trim(tmp_str);
				if (isdigit(tmp_str[0])) {
					rrd_port = atoi(tmp_str);
					//printf("parse_config_file: rrd_port %d\n", rrd_port);
				}
			}
		}
		else if (strncmp(par, "RRD_DB", 10) == 0) {
			tmp_str = strtok(NULL, "\r\n");
			if (tmp_str) {
				tmp_str = trim(tmp_str);
				strncpy(rrd_db_file, tmp_str, strlen(tmp_str));
				//printf("parse_config_file: rrd_db_file %s\n", rrd_db_file);
			}
		}
		free(line);
		line = NULL;
		len = 0;
	}
	fclose(config_fd);
}

bsec_library_return_t update_subscription() {
	char er[255] = { 0 };
	bsec_sensor_configuration_t virt_sensors[NUM_USED_OUTPUTS];
	uint8_t virt_sensors_num = NUM_USED_OUTPUTS;
	bsec_sensor_configuration_t sensor_settings[BSEC_MAX_PHYSICAL_SENSOR];
	uint8_t sensor_settings_num = BSEC_MAX_PHYSICAL_SENSOR;

	virt_sensors[0].sensor_id = BSEC_OUTPUT_IAQ;
	virt_sensors[0].sample_rate = sampling_rate_s;
	virt_sensors[1].sensor_id = BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE;
	virt_sensors[1].sample_rate = sampling_rate_s;
	virt_sensors[2].sensor_id = BSEC_OUTPUT_RAW_PRESSURE;
	virt_sensors[2].sample_rate = sampling_rate_s;
	virt_sensors[3].sensor_id = BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY;
	virt_sensors[3].sample_rate = sampling_rate_s;
	virt_sensors[4].sensor_id = BSEC_OUTPUT_RAW_GAS;
	virt_sensors[4].sample_rate = sampling_rate_s;
	virt_sensors[5].sensor_id = BSEC_OUTPUT_RAW_TEMPERATURE;
	virt_sensors[5].sample_rate = sampling_rate_s;
	virt_sensors[6].sensor_id = BSEC_OUTPUT_RAW_HUMIDITY;
	virt_sensors[6].sample_rate = sampling_rate_s;
	virt_sensors[7].sensor_id = BSEC_OUTPUT_STATIC_IAQ;
	virt_sensors[7].sample_rate = sampling_rate_s;
	virt_sensors[8].sensor_id = BSEC_OUTPUT_GAS_PERCENTAGE;
	virt_sensors[8].sample_rate = sampling_rate_s;
	virt_sensors[9].sensor_id = BSEC_OUTPUT_STABILIZATION_STATUS;
	virt_sensors[9].sample_rate = sampling_rate_s;
	virt_sensors[10].sensor_id = BSEC_OUTPUT_RUN_IN_STATUS;
	virt_sensors[10].sample_rate = sampling_rate_s;
	virt_sensors[11].sensor_id = BSEC_OUTPUT_CO2_EQUIVALENT;
	virt_sensors[11].sample_rate = sampling_rate_s;
	virt_sensors[12].sensor_id = BSEC_OUTPUT_BREATH_VOC_EQUIVALENT;
	virt_sensors[12].sample_rate = sampling_rate_s;

	bsec_library_return_t result = bsec_update_subscription(virt_sensors, virt_sensors_num, sensor_settings, &sensor_settings_num);

	if (result != BSEC_OK)
	{
		switch (result) {
		case BSEC_E_SU_SAMPLINTVLINTEGERMULT: snprintf(er, sizeof(er), "incorrect sample rate");
			break;
		case BSEC_E_SU_MULTGASSAMPLINTVL: snprintf(er, sizeof(er), "incorrect gas-related sample rate");
			break;
		case BSEC_W_SU_UNKNOWNOUTPUTGATE: snprintf(er, sizeof(er), "invalid output sensor id");
			break;
		case BSEC_I_SU_SUBSCRIBEDOUTPUTGATES: snprintf(er, sizeof(er), "no output sensor data is requested");
			break;
		default: snprintf(er, sizeof(er), "unkown error code %u", result);
		}
		fprintf(log_err, "update_subscription: bsec_update_subscription returns %s\n", er);
	}
	return result;
}

bsec_library_return_t bsec_load_config() {
	char er[255] = { 0 };
	uint8_t work_buffer[BSEC_MAX_WORKBUFFER_SIZE] = { 0 };
	uint8_t bsec_config[BSEC_MAX_PROPERTY_BLOB_SIZE] = { 0 };
	bsec_library_return_t result = BSEC_OK;
	int bsec_config_len = config_load(bsec_config, sizeof(bsec_config));
	if (bsec_config_len != 0)
	{
		result = bsec_set_configuration(bsec_config, bsec_config_len, work_buffer, sizeof(work_buffer));
		if (result != BSEC_OK)
		{
			switch (result) {
			case BSEC_E_PARSE_SECTIONEXCEEDSWORKBUFFER: snprintf(er, sizeof(er), "insufficient n_work_buffer_size %u", sizeof(work_buffer));
				break;
			case BSEC_E_CONFIG_VERSIONMISMATCH: snprintf(er, sizeof(er), "version mismatch");
				break;
			case BSEC_E_CONFIG_FEATUREMISMATCH: snprintf(er, sizeof(er), "feature(s) not implemented");
				break;
			case BSEC_E_CONFIG_CRCMISMATCH: snprintf(er, sizeof(er), "corrupted settings");
				break;
			case BSEC_E_CONFIG_EMPTY: snprintf(er, sizeof(er), "settings are too short");
				break;
			case BSEC_E_CONFIG_INSUFFICIENTWORKBUFFER: snprintf(er, sizeof(er), "insufficient work buffer");
				break;
			case BSEC_E_CONFIG_INVALIDSTRINGSIZE: snprintf(er, sizeof(er), "invalid string size");
				break;
			case BSEC_W_SC_CALL_TIMING_VIOLATION: snprintf(er, sizeof(er), "sampling interval is too long");
				break;
			default: snprintf(er, sizeof(er), "unkown error code %u", result);
			}
			fprintf(log_err, "bsec_load_config: bsec_set_configuration returns %s\n", er);
		}
	}
	return result;
}

bsec_library_return_t bsec_load_state() {
	char er[255] = { 0 };
	uint8_t work_buffer[BSEC_MAX_WORKBUFFER_SIZE] = { 0 };
	uint8_t bsec_state[BSEC_MAX_STATE_BLOB_SIZE] = { 0 };
	bsec_library_return_t result = BSEC_OK;
	int bsec_state_len = state_load(bsec_state, sizeof(bsec_state));
	if (bsec_state_len != 0)
	{
		result = bsec_set_state(bsec_state, bsec_state_len, work_buffer, sizeof(work_buffer));
		if (result != BSEC_OK)
		{
			switch (result) {
			case BSEC_E_PARSE_SECTIONEXCEEDSWORKBUFFER: snprintf(er, sizeof(er), "insufficient n_work_buffer_size %u", sizeof(work_buffer));
				break;
			case BSEC_E_CONFIG_VERSIONMISMATCH: snprintf(er, sizeof(er), "version mismatch");
				break;
			case BSEC_E_CONFIG_FEATUREMISMATCH: snprintf(er, sizeof(er), "feature(s) not implemented");
				break;
			case BSEC_E_CONFIG_CRCMISMATCH: snprintf(er, sizeof(er), "corrupted settings");
				break;
			case BSEC_E_CONFIG_EMPTY: snprintf(er, sizeof(er), "settings are too short");
				break;
			case BSEC_E_CONFIG_INSUFFICIENTWORKBUFFER: snprintf(er, sizeof(er), "insufficient work buffer");
				break;
			case BSEC_E_CONFIG_INVALIDSTRINGSIZE: snprintf(er, sizeof(er), "invalid string size");
				break;
			case BSEC_W_SC_CALL_TIMING_VIOLATION: snprintf(er, sizeof(er), "sampling interval is too long");
				break;
			default: snprintf(er, sizeof(er), "unkown error code %u", result);
			}
			fprintf(log_err, "bsec_load_state: bsec_set_state returns %s\n", er);
		}
	}
	return result;
}

void bsec_save_state() {
	uint8_t bsec_state[BSEC_MAX_STATE_BLOB_SIZE];
	uint8_t work_buffer[BSEC_MAX_WORKBUFFER_SIZE];
	uint32_t bsec_state_len = 0;
	bsec_library_return_t status = bsec_get_state(0, bsec_state, sizeof(bsec_state), work_buffer, sizeof(work_buffer), &bsec_state_len);
	if (status == BSEC_OK) {
		state_save(bsec_state, bsec_state_len);
	}
}

void sig_handler(int sig) {
	bsec_library_return_t status;

	switch (sig) {
	case SIGHUP:
		parse_config_file(config_file);

		status = bsec_load_config();
		if (status != BSEC_OK) {
			fprintf(log_err, "sig_handler for %d: bsec_load_config returned %d\n", sig, (int)status);
			exit(-1);
		}

		status = update_subscription();
		if (status != BSEC_OK) {
			fprintf(log_err, "sig_handler for %d: update_subscription returns %d", sig, status);
			exit(-1);
		}

		if (log_out) {
			if (log_out != stdout) {
				fclose(log_out);
				log_out = fopen(log_file, "a");
				if (!log_out) {
					err(-1, "sig_handler for %d: failed to open log file %s\n", sig, log_file);
				}
				log_err = log_out;
			}
		}
		break;
		
		default:
			bsec_save_state();
			exit(0);
	}
}

void on_error(void) {
	if (i2c_fd) close(i2c_fd);
	if (rrd_sock) close(rrd_sock);
	if (log_out)
		if (log_out != stdout) fclose(log_out);
}

void measure_data(bsec_bme_settings_t *sensor_settings) {
	uint16_t interval;
	int8_t status = BME680_OK;

	if (sensor_settings->trigger_measurement)
	{
		bme680.tph_sett.os_hum = sensor_settings->humidity_oversampling;
		bme680.tph_sett.os_pres = sensor_settings->pressure_oversampling;
		bme680.tph_sett.os_temp = sensor_settings->temperature_oversampling;
		bme680.gas_sett.run_gas = sensor_settings->run_gas;
		bme680.gas_sett.heatr_temp = sensor_settings->heater_temperature;
		bme680.gas_sett.heatr_dur = sensor_settings->heating_duration;
		bme680.power_mode = BME680_FORCED_MODE;

		status = bme680_set_sensor_settings(BME680_OST_SEL | BME680_OSP_SEL | BME680_OSH_SEL | BME680_GAS_SENSOR_SEL, &bme680);

		status = bme680_set_sensor_mode(&bme680); //triggers a single measurement

		bme680_get_profile_dur(&interval, &bme680);

		bme_sleep((uint32_t)interval);
	}
	//just in case the measurement is not complete yet, we wait (for the sensor to go to the sleep mode)
	status = bme680_get_sensor_mode(&bme680);
	while (bme680.power_mode == BME680_FORCED_MODE)
	{
		bme_sleep(5);
		status = bme680_get_sensor_mode(&bme680);
	}
}

void input_data(int64_t timestamp, bsec_input_t *inputs, uint8_t *inputs_num, int32_t process_data) {
	static struct bme680_field_data data;
	int8_t status = BME680_OK;

	if (process_data)
	{
		status = bme680_get_sensor_data(&data, &bme680);

		if (data.status & BME680_NEW_DATA_MSK)
		{
			if (process_data & BSEC_PROCESS_PRESSURE)
			{
				inputs[*inputs_num].sensor_id = BSEC_INPUT_PRESSURE;
				inputs[*inputs_num].signal = data.pressure;
				inputs[*inputs_num].time_stamp = timestamp;
				(*inputs_num)++;
			}
			if (process_data & BSEC_PROCESS_TEMPERATURE)
			{
				inputs[*inputs_num].sensor_id = BSEC_INPUT_TEMPERATURE;
#ifdef BME680_FLOAT_POINT_COMPENSATION
				inputs[*inputs_num].signal = data.temperature;
#else
				inputs[*inputs_num].signal = data.temperature / 100.0f;
#endif
				inputs[*inputs_num].time_stamp = timestamp;
				(*inputs_num)++;
				inputs[*inputs_num].sensor_id = BSEC_INPUT_HEATSOURCE;
				inputs[*inputs_num].signal = temperature_offset;
				inputs[*inputs_num].time_stamp = timestamp;
				(*inputs_num)++;
			}
			if (process_data & BSEC_PROCESS_HUMIDITY)
			{
				inputs[*inputs_num].sensor_id = BSEC_INPUT_HUMIDITY;
#ifdef BME680_FLOAT_POINT_COMPENSATION
				inputs[*inputs_num].signal = data.humidity;
#else
				inputs[*inputs_num].signal = data.humidity / 1000.0f;
#endif  
				inputs[*inputs_num].time_stamp = timestamp;
				(*inputs_num)++;
			}
			if (process_data & BSEC_PROCESS_GAS)
			{
				if (data.status & BME680_GASM_VALID_MSK)
				{
					/* Place sample into input struct */
					inputs[*inputs_num].sensor_id = BSEC_INPUT_GASRESISTOR;
					inputs[*inputs_num].signal = data.gas_resistance;
					inputs[*inputs_num].time_stamp = timestamp;
					(*inputs_num)++;
				}
			}
		}
	}
}

void process_data(bsec_input_t *inputs, uint8_t inputs_num)
{
	bsec_library_return_t status = BSEC_OK;
	int64_t timestamp = 0;
	uint8_t outputs_num = BSEC_NUMBER_OUTPUTS, gas_acc = 0, iaq_acc = 0, iaqs_acc = 0, gasp_acc = 0,
		    co2_acc = 0, bvoc_acc = 0, output_stabilization_status = 0, output_run_in_status = 0;
	float temp = 0, humidity = 0, gas = 0, iaq = 0, iaqs = 0, gasp = 0, temp_raw = 0, co2 = 0, bvoc = 0,
		  humidity_raw = 0, pressure_raw = 0, gas_raw = 0;
	bsec_output_t outputs[BSEC_NUMBER_OUTPUTS];

	if (inputs_num > 0)
	{
		status = bsec_do_steps(inputs, inputs_num, outputs, &outputs_num);

		for (uint8_t i = 0; i < outputs_num; i++)
		{
			switch (outputs[i].sensor_id)
			{
			case BSEC_OUTPUT_IAQ:
				iaq = outputs[i].signal;
				iaq_acc = outputs[i].accuracy;
				break;
			case BSEC_OUTPUT_STATIC_IAQ:
				iaqs = outputs[i].signal;
				iaqs_acc = outputs[i].accuracy;
				break;
			case BSEC_OUTPUT_CO2_EQUIVALENT:
				co2 = outputs[i].signal;
				co2_acc = outputs[i].accuracy;
				break;
			case BSEC_OUTPUT_BREATH_VOC_EQUIVALENT:
				bvoc = outputs[i].signal;
				bvoc_acc = outputs[i].accuracy;
				break;
			case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE:
				temp = outputs[i].signal;
				break;
			case BSEC_OUTPUT_RAW_PRESSURE:
				pressure_raw = outputs[i].signal;
				break;
			case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY:
				humidity = outputs[i].signal;
				break;
			case BSEC_OUTPUT_RAW_GAS:
				gas_raw = outputs[i].signal;
				break;
			case BSEC_OUTPUT_RAW_TEMPERATURE:
				temp_raw = outputs[i].signal;
				break;
			case BSEC_OUTPUT_RAW_HUMIDITY:
				humidity_raw = outputs[i].signal;
				break;
			case BSEC_OUTPUT_COMPENSATED_GAS: //reserved internal debug output
				gas = outputs[i].signal;
				gas_acc = outputs[i].accuracy;
				break;
			case BSEC_OUTPUT_GAS_PERCENTAGE:
				gasp = outputs[i].signal;
				gasp_acc = outputs[i].accuracy;
				break;
			case BSEC_OUTPUT_STABILIZATION_STATUS:
				output_stabilization_status = outputs[i].signal;
			case BSEC_OUTPUT_RUN_IN_STATUS:
				output_run_in_status = outputs[i].signal;
			default:
				continue;
			}
			timestamp = outputs[i].time_stamp;
		}

		save_data(timestamp, temp, humidity, pressure_raw, gas, gas_acc, 
			      iaq, iaq_acc, iaqs, iaqs_acc, gasp, gasp_acc,
			      temp_raw, humidity_raw, gas_raw, co2, co2_acc, bvoc, bvoc_acc, 
			      status, output_stabilization_status, output_run_in_status);
	}
}

int main(int argc, char ** argv)
{
	extern char * optarg;
	extern int optind, opterr, optopt;
	const char * optstring = "df:";
	int opt, res;
	bool daemonize = false;

	bsec_version_t bsec_version;
	bsec_get_version(&bsec_version);
	memset(config_file, 0, sizeof(config_file));
	strncpy(config_file, CONFIG_FILE, sizeof(CONFIG_FILE));

	log_out = stdout;
	log_err = stderr;

	while ((opt = getopt(argc, argv, optstring)) != -1) {
		switch (opt) {
		case 'd':
			daemonize = true;
			break;
		case 'f':
			memset(config_file, 0, sizeof(config_file));
			strncpy(config_file, optarg, sizeof(optarg));
			break;
		case '?':
			fprintf(stderr, "bsec sensor with BSEC %u.%u.%u.%u, the environmental fusion library\n", bsec_version.major, bsec_version.minor, bsec_version.major_bugfix, bsec_version.minor_bugfix);
			err(-1, "Usage: %s [ -f config_file ] [ -d ]", argv[0]);
			break;
		case ':':
			fprintf(stderr, "missing -f option argument config file name\n");
			err(-1, "Usage: %s [ -f config_file ] [ -d ]", argv[0]);
			break;
		}
	}

	atexit(on_error);

	parse_config_file(config_file);

	if (daemonize) {
		log_out = fopen(log_file, "a");
		if (!log_out) {
			err(-1, "main: failed to open log file %s\n", log_file);
		}
		log_err = log_out;
		res = daemon(0, 0);
		fprintf(log_out, "BME680 RRD data collector with BSEC %u.%u.%u.%u, the environmental fusion library\n", bsec_version.major, bsec_version.minor, bsec_version.major_bugfix, bsec_version.minor_bugfix);
	}

	i2c_fd = open(device, O_RDWR);
	if (i2c_fd == -1) {
		fprintf(log_err, "main: failed to open device %s\n", device);
		exit(-1);
	}
	res = ioctl(i2c_fd, I2C_SLAVE, dev_addr);
	if (res == -1) {
		fprintf(log_err, "main: failed setting slave mode for i2c device at address %x", dev_addr);
		exit(-1);
	}

	rrd_connect();

	bme680.dev_id = dev_addr;
	bme680.intf = BME680_I2C_INTF;
	bme680.write = bus_write;
	bme680.read = bus_read;
	bme680.delay_ms = bme_sleep;
	//bme680.amb_temp = 25;

	int8_t rslt = bme680_init(&bme680);
	if (rslt != BME680_OK)
	{
		if (rslt == BME680_E_DEV_NOT_FOUND) fprintf(log_err, "bme680_init: bme680 device not found\n");
		fprintf(log_err, "main: could not intialize BME680, err %d\n", rslt);
		exit(-1);
	}

	bsec_library_return_t result = bsec_init();
	if (result != BSEC_OK) {
		fprintf(log_err, "main: could not intialize BSEC library, err %d\n", (int)result);
		exit(-1);
	}
	result = bsec_load_config();
	if (result != BSEC_OK) {
		fprintf(log_err, "main: bsec_load_config returned %d\n", (int)result);
		exit(-1);
	}

	result = bsec_load_state();
	if (result != BSEC_OK) {
		fprintf(log_err, "main: bsec_load_state returned %d\n", (int)result);
		exit(-1);
	}

	result = update_subscription();
	if (result != BSEC_OK) {
		fprintf(log_err, "main: update_subscription returned %d\n", (int)result);
		exit(-1);
	}

	struct sigaction sa_term, sa_hup, sa_int;
	sigemptyset(&sa_term.sa_mask);
	sigemptyset(&sa_hup.sa_mask);
	sigemptyset(&sa_int.sa_mask);
	sa_term.sa_handler = sig_handler;
	sa_hup.sa_handler = sig_handler;
	sa_int.sa_handler = sig_handler;
	sigaddset(&sa_term.sa_mask, SIGHUP);
	sigaddset(&sa_term.sa_mask, SIGINT);
	sigaddset(&sa_hup.sa_mask, SIGINT);
	sigaddset(&sa_hup.sa_mask, SIGTERM);
	sigaddset(&sa_int.sa_mask, SIGHUP);
	sigaddset(&sa_int.sa_mask, SIGTERM);
	if (sigaction(SIGTERM, &sa_term, NULL) == -1) {
		fprintf(log_err, "main: sigaction for SIGTERM error");
		exit(-1);
	}
	if (sigaction(SIGHUP, &sa_hup, NULL) == -1) {
		fprintf(log_err, "main: sigaction for SIGHUP error");
		exit(-1);
	}
	if (sigaction(SIGINT, &sa_int, NULL) == -1) {
		fprintf(log_err, "main: sigaction SIGINT error");
		exit(-1);
	}
	
	bsec_bme_settings_t sensor_settings;
	uint32_t samples_num = 0;
	while (1)
	{
		int64_t timestamp = get_timestamp_us() * 1000;
		bsec_sensor_control(timestamp, &sensor_settings);
		measure_data(&sensor_settings);

		uint8_t inputs_num = 0;
		bsec_input_t inputs[BSEC_MAX_PHYSICAL_SENSOR];
		input_data(timestamp, inputs, &inputs_num, sensor_settings.process_data);

		process_data(inputs, inputs_num);

		samples_num++;

		if (samples_num >= (float)save_state_every_seconds * sampling_rate_s)
		{
			bsec_save_state();
			samples_num = 0;
		}

		timestamp = get_timestamp_us() * 1000;
		int64_t sleep_time_ms = (sensor_settings.next_call - timestamp) / 1000000;
		if (sleep_time_ms > 0)
			bme_sleep((uint32_t)sleep_time_ms);
	}
    return 0;
}


