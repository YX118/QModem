#include "browser_media.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

void qmodem_voip_serial_close(struct qmodem_voip_media_engine *engine)
{
	(void)engine;
}

void qmodem_voip_serial_set_attached(struct qmodem_voip_media_engine *engine,
				     int attached)
{
	(void)engine;
	(void)attached;
}

static int all_zero(const uint8_t *data, size_t length)
{
	while (length--)
		if (*data++)
			return 0;
	return 1;
}

int main(int argc, char **argv)
{
	struct qmodem_voip_browser_media browser;
	struct qmodem_voip_media_engine engine;
	size_t certificate_length;
	size_t key_length;

	assert(argc == 1);
	(void)argv;
	memset(&browser, 0, sizeof(browser));
	memset(&engine, 0, sizeof(engine));
	assert(qmodem_voip_browser_configure(&browser, &engine, "192.0.2.1",
		NULL, NULL) == 0);
	certificate_length = browser.certificate_length;
	key_length = browser.key_length;
	assert(certificate_length == 0);
	assert(key_length == 0);

	qmodem_voip_browser_media_stop(&browser);
	assert(browser.certificate_length == certificate_length);
	assert(browser.key_length == key_length);
	assert(all_zero(browser.certificate_data, sizeof(browser.certificate_data)));
	assert(all_zero(browser.key_data, sizeof(browser.key_data)));

	qmodem_voip_browser_media_release(&browser);
	assert(browser.certificate_length == 0);
	assert(browser.key_length == 0);
	assert(all_zero(browser.certificate_data, sizeof(browser.certificate_data)));
	assert(all_zero(browser.key_data, sizeof(browser.key_data)));
	return 0;
}
