#define main ppsbias_main
#include "ppsbias.c"
#undef main
#include <assert.h>
int main(void)
{
	uint64_t n;
	assert(number("0.000001", 1000000, 0, 499999000, &n) && n == 1);
	assert(number("499.999000", 1000000, 0, 499999000, &n) && n == 499999000);
	assert(number("1.0000001", 1000000, 0, 499999000, &n) && n == 1000000);
	assert(number("1.0000009", 1000000, 0, 499999000, &n) && n == 1000001);
	assert(number("1e-3", 1000000, 0, 499999000, &n) && n == 1000);
	assert(!number("18446744073709551616", 1, 0, UINT64_MAX, &n));
	double values[] = {10, 30, 20};
	struct stats st = compute(values, 3);
	assert(st.mean == 20 && st.median == 20 && st.sd == 10);
	struct sample a = { .event = { .present = true, .physical = 2 * NS }, .valid = true, .polled_ns = 2 * NS },
		b = { .event = { .present = true, .physical = 3 * NS, .kernel = 3 * NS + 500 }, .valid = true },
		c = { .event = { .present = true, .physical = 4 * NS }, .valid = true, .polled_ns = 4 * NS + 1 };
	double bias;
	assert(!interpolate(&a, &b, &c, &bias) && bias == 499.5);
	a.polled_fraction = .25; c.polled_fraction = .75;
	assert(!interpolate(&a, &b, &c, &bias) && bias == 499);
	c.event.physical = 5 * NS;
	assert(!strcmp(interpolate(&a, &b, &c, &bias), "invalid_pairing"));
	assert(!strcmp(interpolate(NULL, &b, &c, &bias), "missing_neighbor"));
	assert(!sequence_status(UINT32_MAX, 0, true));
	assert(!strcmp(sequence_status(5, 3, true), "sequence_reset"));
	assert(!strcmp(sequence_status(5, 7, true), "pps_sequence_gap"));
	uint32_t mask;
	assert(register_offset(&models[0], 53, &mask) == 0x38 && mask == (1u << 21));
	assert(register_offset(&models[1], 57, &mask) == 0x38 && mask == (1u << 25));
	assert(register_offset(&models[2], 28, &mask) == 0x14008 && mask == 1);
	assert(register_offset(&models[2], 53, &mask) == 0x18008 && mask == (1u << 19));
	char *output = NULL;
	size_t output_size = 0;
	FILE *saved_stdout = stdout;
	stdout = open_memstream(&output, &output_size);
	assert(stdout);
	timestamp("timestamp", INT64_C(1788888888123456789)); putchar('\n');
	int result = fclose(stdout);
	stdout = saved_stdout;
	assert(result == 0 && !strcmp(output, "timestamp=1788888888.123456789\n"));
	free(output);
	return 0;
}
