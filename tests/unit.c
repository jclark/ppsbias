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
	/* Backends: where each line's input is, given the identification register's value. */
	size_t off;
	uint32_t mask;
	assert(bcm2835_backend.input(0, 53, &off, &mask) && off == 0x38 && mask == (1u << 21));
	assert(bcm2835_backend.input(0, 57, &off, &mask) && off == 0x38 && mask == (1u << 25));
	assert(rp1_backend.input(0, 27, &off, &mask) && off == 0x0008 && mask == (1u << 27));
	assert(rp1_backend.input(0, 28, &off, &mask) && off == 0x4008 && mask == 1);
	assert(rp1_backend.input(0, 53, &off, &mask) && off == 0x8008 && mask == (1u << 19));
	assert(rockchip_backend.input(0x01000c2b, 24, &off, &mask) && off == 0x70 && mask == (1u << 24));
	assert(rockchip_backend.input(0x010219c8, 24, &off, &mask) && off == 0x70);
	assert(rockchip_backend.input(0, 24, &off, &mask) && off == 0x50 && mask == (1u << 24));
	assert(!rockchip_backend.input(0x12345678, 24, &off, &mask));
	assert(k3_backend.input(0x44832905, 11, &off, &mask) && off == 0x20 && mask == (1u << 11));
	assert(k3_backend.input(0x44832905, 72, &off, &mask) && off == 0x70 && mask == (1u << 8));
	assert(!k3_backend.input(0x44822905, 11, &off, &mask));
	assert(mxc_backend.input(0, 1, &off, &mask) && off == 0x08 && mask == (1u << 1));
	/* Lookup and address translation against the synthetic device tree from fixture.sh. */
	char root[] = "/tmp/ppsbias-unit-XXXXXX", cmd[256], dt[64], sys[64], real[PATH_MAX];
	assert(mkdtemp(root));
	snprintf(cmd, sizeof cmd, "./fixture.sh %s", root);
	assert(system(cmd) == 0);
	snprintf(dt, sizeof dt, "%s/dt", root); snprintf(sys, sizeof sys, "%s/sys", root);
	dt_root = dt; sys_root = sys;
	assert(realpath(dt, real));
	/* Each case: the controller found, and the CPU address and size of its first register range
	 * and of the backend's register range (the same, except for RP1's RIO range). */
	static const struct {
		const char *pps; const struct backend *backend; const char *node; unsigned line;
		uint64_t first, first_size, regs, regs_size;
	} cases[] = {
		{ "pps0", &rp1_backend, "/axi/pcie@1000120000/rp1/gpio@d0000", 18,
		  UINT64_C(0x1f000d0000), 0xc000, UINT64_C(0x1f000e0000), 0xc000 },
		{ "ppsmain", &rp1_backend, "/axim/pcie@1000120000/pci@0,0/dev@0,0/pci-ep-bus@1/gpio@d0000", 18,
		  UINT64_C(0x1f000d0000), 0xc000, UINT64_C(0x1f000e0000), 0xc000 },
		{ "ppsbcm", &bcm2835_backend, "/soc/gpio@7e200000", 18, 0x3f200000, 0xb4, 0x3f200000, 0xb4 },
		{ "pps2711", &bcm2835_backend, "/soc4/gpio@7e200000", 18, 0xfe200000, 0xb4, 0xfe200000, 0xb4 },
		{ "ppsrk3", &rockchip_backend, "/pinctrl/gpio@fe760000", 3, 0xfe760000, 0x100, 0xfe760000, 0x100 },
		{ "ppsk3", &k3_backend, "/bus@f0000/gpio@601000", 18, 0x601000, 0x100, 0x601000, 0x100 },
		{ "ppsmxc", &mxc_backend, "/soc@0/bus@30000000/gpio@30200000", 18, 0x30200000, 0x10000, 0x30200000, 0x10000 },
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		struct controller ctl;
		uint64_t base, size;
		assert(!locate(cases[i].pps, &ctl));
		assert(ctl.backend == cases[i].backend && ctl.line == cases[i].line && !strcmp(ctl.node + ctl.root_len, cases[i].node));
		assert(!translate(ctl.node, real, 0, &base, &size) && base == cases[i].first && size == cases[i].first_size);
		assert(!translate(ctl.node, real, ctl.backend->reg, &base, &size) &&
		       base == cases[i].regs && size == cases[i].regs_size);
	}
	struct controller ctl;
	uint64_t base, size;
	assert(!locate("ppsnoranges", &ctl) && strstr(translate(ctl.node, real, 0, &base, &size), "no ranges"));
	assert(!locate("pps0", &ctl) && strstr(translate(ctl.node, real, 3, &base, &size), "reg property"));
	assert(strstr(locate("ppsk3big", &ctl), "outside the controller range"));
	assert(strstr(locate("ppsbad", &ctl), "not supported"));
	assert(strstr(locate("ppscells", &ctl), "two GPIO cells"));
	assert(strstr(locate("ppsother", &ctl), "not a pps-gpio device"));
	assert(strstr(locate("ptp", &ctl), "not a pps-gpio device"));
	snprintf(cmd, sizeof cmd, "rm -r %s", root);
	assert(system(cmd) == 0);
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
