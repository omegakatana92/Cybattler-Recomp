// YM2151 via ymfm (BSD-3-Clause, Aaron Giles). Timers are driven by the
// machine scheduler in YM input clocks.
#include "ymfm_opm.h"
#include "machine.h"
#include <vector>
#include <cstring>

extern "C" void mach_snd_irq(int level);

namespace {
struct iface : ymfm::ymfm_interface {
	bool irq = false;
	void ymfm_set_timer(uint32_t tnum, int32_t clocks) override { g_m.ym_timer[tnum] = clocks; }
	void ymfm_update_irq(bool asserted) override { irq = asserted; if (asserted) mach_snd_irq(4); }
	void expire(int t) { m_engine->engine_timer_expired(t); }
};
iface *g_if;
ymfm::ym2151 *g_chip;
}

extern "C" {

void ym_init(unsigned clock)
{
	(void)clock;
	g_if = new iface();
	g_chip = new ymfm::ym2151(*g_if);
	g_chip->reset();
}

void ym_reset(void)
{
	g_m.ym_timer[0] = g_m.ym_timer[1] = -1;
	g_chip->reset();
}

void ym_write(int port, uint8_t data) { g_chip->write(port & 1, data); }
uint8_t ym_read(void) { return g_chip->read(1); }
int ym_irq(void) { return g_if->irq; }

void ym_advance(int clocks, float *l, float *r, int *nsamples)
{
	int n = 0;
	// timers
	for (int t = 0; t < 2; t++) {
		if (g_m.ym_timer[t] < 0) continue;
		g_m.ym_timer[t] -= clocks;
		while (g_m.ym_timer[t] != -1 && g_m.ym_timer[t] <= 0) {
			int32_t over = g_m.ym_timer[t];
			g_m.ym_timer[t] = -1;
			g_if->expire(t);              // engine re-arms via ymfm_set_timer
			if (g_m.ym_timer[t] > 0) g_m.ym_timer[t] += over;
			else break;
		}
	}
	// audio: one sample per 64 input clocks
	g_m.ym_frac += clocks;
	while (g_m.ym_frac >= 64) {
		g_m.ym_frac -= 64;
		ymfm::ym2151::output_data out;
		g_chip->generate(&out, 1);
		l[n] = out.data[0] / 32768.0f;
		r[n] = out.data[1] / 32768.0f;
		n++;
	}
	*nsamples = n;
}

int ym_save(uint8_t *buf, int max)
{
	std::vector<uint8_t> v;
	ymfm::ymfm_saved_state st(v, true);
	g_chip->save_restore(st);
	uint8_t irq = g_if->irq;
	v.push_back(irq);
	if ((int)v.size() > max) return -1;
	memcpy(buf, v.data(), v.size());
	return (int)v.size();
}

int ym_load(const uint8_t *buf, int len)
{
	std::vector<uint8_t> v(buf, buf + len);
	g_if->irq = v.back();
	v.pop_back();
	ymfm::ymfm_saved_state st(v, false);
	g_chip->save_restore(st);
	return 0;
}

}
