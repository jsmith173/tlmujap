/*
 * TLMu C example app.
 *
 * Copyright (c) 2011 Edgar E. Iglesias.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <pthread.h>
#include <dlfcn.h>

#include "tlmu.h"

/* Every core is connected to a shared bus that maps:

   0x20000000 IO_RAM START
   0x10000000 IO_RAM SIZE
*/

#define IO_RAM_START 0x20000000
#define IO_RAM_SIZE  0x1000000
#define SDRAM_START   0x00000000
#define SDRAM_SIZE   0x100000

uint32_t io_ram[IO_RAM_SIZE / 4];
uint32_t sdram[SDRAM_SIZE / 4];

struct tlmu_wrap {
	struct tlmu q;
	const char *name;
};

void tlm_get_dmi_ptr(void *o, uint64_t addr, struct tlmu_dmi *dmi)
{
	if (addr >= IO_RAM_START && addr <= (IO_RAM_START + sizeof io_ram)) {
		dmi->ptr = (void *) &io_ram[0];
		dmi->base = IO_RAM_START;
		dmi->size = IO_RAM_SIZE;
		dmi->prot = TLMU_DMI_PROT_READ | TLMU_DMI_PROT_WRITE;
	}
	else if (addr >= SDRAM_START && addr <= (SDRAM_START + sizeof sdram)) {
		dmi->ptr = (void *) &sdram[0];
		dmi->base = SDRAM_START;
		dmi->size = SDRAM_SIZE;
		dmi->prot = TLMU_DMI_PROT_READ | TLMU_DMI_PROT_WRITE;
	}
}

void tlm_bus_write(void *o, int dbg, int64_t clk,
                  uint64_t addr, const void *data, int len)
{
	struct tlmu_wrap *t = o;

    if (addr >= IO_RAM_START && addr <= (IO_RAM_START + sizeof io_ram)) {
		unsigned char *dst = (void *) io_ram;
		addr -= IO_RAM_START;
		memcpy(&dst[addr], data, len);
     }
    else if (addr >= SDRAM_START && addr <= (SDRAM_START + sizeof sdram)) {
		unsigned char *dst = (void *) sdram;
		addr -= SDRAM_START;
		memcpy(&dst[addr], data, len);
     }
}

int tlm_bus_access1(void *o, int dbg, int64_t clk, int rw,
			uint64_t addr, void *data, int len)
{
	if (rw) {
		tlm_bus_write(o, dbg, clk, addr, data, len);
		return 1;
	}

	/* Read.  */
    if (addr >= IO_RAM_START && addr <= (IO_RAM_START + sizeof io_ram)) {
		unsigned char *src = (void *) io_ram;
		addr -= IO_RAM_START;
		memcpy(data, &src[addr], len);
    }
    else if (addr >= SDRAM_START && addr <= (SDRAM_START + sizeof sdram)) {
		unsigned char *src = (void *) sdram;
		addr -= SDRAM_START;
		memcpy(data, &src[addr], len);
    }
	return 1;
}

int tlm_bus_access(void *o, int64_t clk, int rw,
			uint64_t addr, void *data, int len)
{
	return tlm_bus_access1(o, 0, clk, rw, addr, data, len);
}

void tlm_bus_access_dbg(void *o, int64_t clk, int rw,
			uint64_t addr, void *data, int len)
{
	tlm_bus_access1(o, 1, clk, rw, addr, data, len);
}

void *run_tlmu(void *p)
{
	struct tlmu_wrap *t = p;
	tlmu_run(&t->q);
	return NULL;
}

void tlm_sync(void *o, int64_t time_ns)
{
}

int main(int argc, char **argv)
{
	int i;
	int err;
	struct {
		char *soname;
		char *name;
		char *cputype;
		char *elfimage;

		struct tlmu_wrap t;
		pthread_t tid;
	} sys[] = {
	{"libtlmu-arm.so", "ARM", "arm1176", "arm-guest/noname.elf"},
	{NULL, NULL, NULL, NULL}
	};

	i = 0;
	while (sys[i].name) {
		sys[i].t.name = sys[i].name;

		tlmu_init(&sys[i].t.q, sys[i].t.name);
		err = tlmu_load(&sys[i].t.q, sys[i].soname);
		if (err) {
			printf("failed to load tlmu %s\n", sys[i].soname);
			i++;
			continue;
		}

		/* Use the bare CPU core.  */
		tlmu_append_arg(&sys[i].t.q, "-M");
		tlmu_append_arg(&sys[i].t.q, "tlm-mach");

		tlmu_append_arg(&sys[i].t.q, "-icount");
		tlmu_append_arg(&sys[i].t.q, "1");

#if 0
		/* Enable exec tracing.  */
		tlmu_append_arg(&sys[i].t.q, "-d");
		tlmu_append_arg(&sys[i].t.q, "in_asm,exec,cpu");
#endif

		tlmu_append_arg(&sys[i].t.q, "-cpu");
		tlmu_append_arg(&sys[i].t.q, sys[i].cputype);

		tlmu_append_arg(&sys[i].t.q, "-kernel");
		tlmu_append_arg(&sys[i].t.q, sys[i].elfimage);

		/*
		 * Register our per instance pointer carried back in
		 * callbacks.
		 */
		tlmu_set_opaque(&sys[i].t.q, &sys[i].t);

		/* Register our callbacks.  */
		tlmu_set_bus_access_cb(&sys[i].t.q, tlm_bus_access);
		tlmu_set_bus_access_dbg_cb(&sys[i].t.q, tlm_bus_access_dbg);
		tlmu_set_bus_get_dmi_ptr_cb(&sys[i].t.q, tlm_get_dmi_ptr);
		tlmu_set_sync_cb(&sys[i].t.q, tlm_sync);

		/* Tell TLMu how often it should break out from executing
		 * guest code and synchronize.  */
		tlmu_set_sync_period_ns(&sys[i].t.q, 1 * 100 * 1000ULL);
		/* Tell TLMu if the CPU should start in running or sleeping
		 * mode.  */
		tlmu_set_boot_state(&sys[i].t.q, TLMU_BOOT_RUNNING);

		/*
		 * Tell TLMu what memory areas that map actual RAM. This needs
		 * to be done for RAM's that are not internal to the TLMu
		 * emulator, but managed by the main emulator or by other
		 * TLMu instances.
		 */
		tlmu_map_ram(&sys[i].t.q, "io_ram", IO_RAM_START, IO_RAM_SIZE, 1);
		tlmu_map_ram(&sys[i].t.q, "sdram", SDRAM_START, SDRAM_SIZE, 0);

		pthread_create(&sys[i].tid, NULL, run_tlmu, &sys[i].t);
		i++;
	}

	i = 0;
	while (sys[i].name) {
		if (sys[i].tid) {
			pthread_join(sys[i].tid, NULL);
		}
		i++;
	}
	return 0;
}
