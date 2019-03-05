/*
 * Xilinx SystemC/TLM-2.0 ZynqMP Wrapper.
 *
 * Written by Edgar E. Iglesias <edgar.iglesias@xilinx.com>
 *
 * Copyright (c) 2016, Xilinx Inc.
 * All rights reserved.
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

#define SC_INCLUDE_DYNAMIC_PROCESSES

#include <inttypes.h>

#include "tlm_utils/simple_initiator_socket.h"
#include "tlm_utils/simple_target_socket.h"
#include "tlm_utils/tlm_quantumkeeper.h"

using namespace sc_core;
using namespace std;

extern "C" {
#include "safeio.h"
#include "remote-port-proto.h"
#include "remote-port-sk.h"
};
#include "xilinx-zynqmp.h"
#include "tlm-extensions/genattr.h"
#include <sys/types.h>

xilinx_zynqmp::xilinx_zynqmp(sc_module_name name, const char *sk_descr,
				Iremoteport_tlm_sync *sync)
	: remoteport_tlm(name, -1, sk_descr, sync),

	  rp_lpd_reserved("rp_lpd_reserved"),
	  rp_wires_in("wires_in", 16, 0),
	  rp_wires_out("wires_out", 0, 4),
	  rp_irq_out("irq_out", 0, 164),
	  pl2ps_irq("pl2ps_irq", 16),
	  ps2pl_irq("ps2pl_irq", 164),
	  pl_resetn("pl_resetn", 4)
{
	unsigned int i;

	s_lpd_reserved = &rp_lpd_reserved.sk;

	for (i = 0; i < 16; i++) {
		rp_wires_in.wires_in[i](pl2ps_irq[i]);
	}

	for (i = 0; i < 164; i++) {
		rp_irq_out.wires_out[i](ps2pl_irq[i]);
	}

	// Register with Remote-Port.
	register_dev(0, &rp_wires_in);
	register_dev(1, &rp_wires_out);
	register_dev(2, &rp_irq_out);
	register_dev(3, &rp_lpd_reserved);
}

void xilinx_zynqmp::tie_off(void)
{
	remoteport_tlm::tie_off();
}

xilinx_zynqmp::~xilinx_zynqmp(void)
{
}
