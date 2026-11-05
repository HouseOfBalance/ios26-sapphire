/*
 * redwood_adsp.c - Stub for physical hardware targets (sapphire)
 */
#include <sys/types.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/redwood_audio.h>

bool
redwood_adsp_apr(uint8_t service, uint16_t port, uint32_t token, uint32_t opcode, const void *payload, uint32_t length,
    uint32_t wait_ms, struct redwood_apr_answer *answer)
{
	(void)service;
	(void)port;
	(void)token;
	(void)opcode;
	(void)payload;
	(void)length;
	(void)wait_ms;
	(void)answer;
	return false;
}

void
redwood_adsp_apr_events(redwood_apr_events_t handler)
{
	(void)handler;
}

void
redwood_adsp_apr_events_second(redwood_apr_events_t handler)
{
	(void)handler;
}

const uint8_t *
redwood_adsp_fw_file(const char *name, uint64_t *size)
{
	(void)name;
	if (size) {
		*size = 0;
	}
	return NULL;
}

void
redwood_adsp_fw_free(const uint8_t *data, uint64_t size)
{
	(void)data;
	(void)size;
}

void
redwood_adsp_init(void)
{
	/* Unused on physical hardware (sapphire) */
}

void
redwood_adsp_stop(void)
{
	/* Unused on physical hardware (sapphire) */
}

void
redwood_adsp_quiesce(void)
{
	/* Unused on physical hardware (sapphire) */
}
