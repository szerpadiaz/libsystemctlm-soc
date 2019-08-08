/*
 * System-C TLM-2.0 remoteport glue
 *
 * Copyright (c) 2013-2018 Xilinx Inc
 * Written by Edgar E. Iglesias
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
#include <sys/utsname.h>
#include <sys/shm.h>
#include <sys/ipc.h>

#include "systemc.h"
#include "tlm_utils/simple_initiator_socket.h"
#include "tlm_utils/simple_target_socket.h"
#include "tlm_utils/tlm_quantumkeeper.h"
#include <iostream>

extern "C" {
#include "safeio.h"
#include "remote-port-proto.h"
#include "remote-port-sk.h"
#include <fcntl.h>
};
#include "remote-port-tlm.h"

using namespace sc_core;
using namespace std;

class remoteport_tlm_sync_untimed : public Iremoteport_tlm_sync
{
public:
	remoteport_tlm_sync_untimed() {
		reset();
	};

	virtual void account_time(int64_t rclk) {
		int64_t lclk;
		int64_t delta_ns;
		sc_time delta;

		lclk = map_time(m_qk.get_current_time());
		if (lclk >= rclk) {
			/* lclk may have rounding errors due to conversions.
			 * To avoid deadlocks, we never allow the delta to be zero.
			 */
			delta_ns = 1;
		} else {
			delta_ns = rclk - lclk;
		}

		delta = sc_time((double) delta_ns, SC_NS);
		assert(delta >= SC_ZERO_TIME);
		if (delta > m_qk.get_global_quantum()) {
			delta = m_qk.get_global_quantum();
		}

#if 0
		cout << "account rclk=" << rclk << " current=" << m_qk.get_current_time() << " delta=" << delta << endl;
#endif

		// Never allow the local time to go beyond the global quantum, cap it.
		if (get_local_time() + delta >= m_qk.get_global_quantum()) {
			set_local_time(m_qk.get_global_quantum());
		} else {
			inc_local_time(delta);
		}
	}

	virtual void pre_any_cmd(remoteport_packet *pkt, bool can_sync) {
		time_start = get_current_time();
	}

	virtual void pre_sync_cmd(int64_t rclk, bool can_sync) {
		account_time(rclk);
	}

	virtual void post_any_cmd(remoteport_packet *pkt, bool can_sync) {
		sc_time time_end = get_current_time();

		// We need to advance time with something to avoid dead-locks.
		// For an RTL only co-sim environment, we want a low value since
		// this increment is pointless. But for a TLM target that consumes
		// no time in b_transport, we need to advance time with something.
		if (time_start == time_end) {
			inc_local_time(sc_time(1, SC_NS));
		}

		if (can_sync) {
			sync();
		}
	}

	// Limited access to quantum keeper.
	virtual sc_core::sc_time get_global_quantum() {
		return m_qk.get_global_quantum();
	}

	virtual sc_core::sc_time get_current_time() {
		return m_qk.get_current_time();
	}

	virtual sc_core::sc_time get_local_time() {
		return m_qk.get_local_time();
	}

	virtual void set_local_time(sc_core::sc_time t) {
		m_qk.set(t);
	}

	virtual void inc_local_time(sc_core::sc_time t) {
		m_qk.inc(t);
	}

	virtual void reset(void) {
		m_qk.reset();
	}

	virtual bool need_sync() {
		return m_qk.need_sync();
	}
	virtual void sync(void) {
		m_qk.sync();
	}

protected:
	tlm_utils::tlm_quantumkeeper m_qk;
private:
	sc_time time_start;
};

class remoteport_tlm_sync_loosely_timed : public remoteport_tlm_sync_untimed
{
public:
	remoteport_tlm_sync_loosely_timed() {
		reset();
	};

	virtual void pre_any_cmd(remoteport_packet *pkt, bool can_sync) {
	}

	virtual void post_any_cmd(remoteport_packet *pkt, bool can_sync) {
		/* We've just processed peer packets and it is
		   likely running freely. Good spot for a local sync.  */
		if (can_sync) {
			sync();
		}
	}

	virtual void pre_sync_cmd(int64_t rclk, bool can_sync) {
		account_time(rclk);
	}

	virtual void post_sync_cmd(int64_t rclk, bool can_sync) {
		/* Relaxing this sync to run in parallel with the remote
		   speeds up simulation significantly but allows us to skew off
		   time (in theory). The added inaccuracy is not really observable
		   to any side of the simulation though.  */
		if (can_sync && m_qk.need_sync()) {
			sync();
		}
	}

	virtual void pre_wire_cmd(int64_t rclk, bool can_sync) {
		account_time(rclk);
		/* Always sync here. Peer is not waiting for a response so
		 * it's a good time to achieve parallelism. We also don't
		 * want to miss pin wiggeling events (by having multiple
		 * changes merged into the same time slot when using
		 * large quantums).
		 */
		if (can_sync) {
			sync();
		}
	}

	virtual void post_wire_cmd(int64_t rclk, bool can_sync) {
		/*
		 * Yield to make line-updates visible immediately.
		 * Otherwise a line-update followed by a back-to-back
		 * transaction that inspects the state of the line
		 * may not reflect the update.
		 */
		if (can_sync) {
			wait(SC_ZERO_TIME);
		}
	}

	virtual void pre_memory_master_cmd(int64_t rclk, bool can_sync) {
		if (can_sync) {
			account_time(rclk);
		}
		if (can_sync && m_qk.need_sync()) {
			m_qk.sync();
		}
	}
};

remoteport_tlm_sync_loosely_timed remoteport_tlm_sync_loosely_timed_obj;
remoteport_tlm_sync_untimed remoteport_tlm_sync_untimed_obj;

Iremoteport_tlm_sync *remoteport_tlm_sync_loosely_timed_ptr =
	dynamic_cast<Iremoteport_tlm_sync *>(&remoteport_tlm_sync_loosely_timed_obj);
Iremoteport_tlm_sync *remoteport_tlm_sync_untimed_ptr =
	dynamic_cast<Iremoteport_tlm_sync *>(&remoteport_tlm_sync_untimed_obj);

remoteport_packet::remoteport_packet(void)
{
	u8 = NULL;
	size = 0;
	alloc(sizeof *pkt);
}

void remoteport_packet::alloc(size_t new_size)
{
	if (size < new_size) {
		u8 = (uint8_t *) realloc(u8, new_size);
		if (u8 == NULL) {
			cerr << "out of mem" << endl;
			exit(EXIT_FAILURE);
		}
		memset(u8 + size, 0, new_size - size);
		size = new_size;
	}
}

void remoteport_packet::copy(remoteport_packet &pkt)
{
	pkt.alloc(size);
	pkt.data_offset = data_offset;
	memcpy(pkt.u8, u8, size);
}

remoteport_tlm::remoteport_tlm(sc_module_name name,
			int fd,
			const char *sk_descr,
			Iremoteport_tlm_sync *sync)
	: sc_module(name),
	  rst("rst")
{
	this->fd = fd;
	this->sk_descr = sk_descr;
	this->rp_pkt_id = 0;
	this->shmid =  -1;
	this->paused = false;
	this->shData = nullptr;
	this->sync = sync;
	if (!this->sync) {
		// Default
		this->sync = remoteport_tlm_sync_loosely_timed_ptr;
	}

	memset(devs, 0, sizeof devs);
	memset(&peer, 0, sizeof peer);

	SC_THREAD(process);
}

void remoteport_tlm::register_dev(unsigned int dev_id, remoteport_tlm_dev *dev)
{
	assert(dev_id < RP_MAX_DEVS);
	assert(this->devs[dev_id] == NULL);
	this->devs[dev_id] = dev;
	dev->adaptor = this;
	dev->dev_id = dev_id;
}

unsigned int remoteport_tlm_dev::response_lookup(uint32_t id)
{
	unsigned int i;
	unsigned int ret = ~0;

	// Find a response slot waiting for id.
	for (i = 0; i < (sizeof resp / sizeof resp[0]); i++) {
		if (resp[i].used && resp[i].id == id) {
			ret = i;
			break;
		}
	}
	return ret;
}

unsigned int remoteport_tlm_dev::response_wait(uint32_t id)
{
	unsigned int i;

	// Find a free response slot.
	for (i = 0; i < (sizeof resp / sizeof resp[0]); i++) {
		if (!resp[i].used)
			break;
	}

	if (i >= (sizeof resp / sizeof resp[0])) {
	// If not OK, we ran out of outstanding response space.
	// Increase RP_MAX_OUTSTANDING_TRANSACTIONS in remote-port-tlm.h
		printf("Outstanding transactions overrun!\n");
		printf("Increase RP_MAX_OUTSTANDING_TRANSACTIONS in remote-port-tlm.h\n");
		assert(i < (sizeof resp / sizeof resp[0]));
	}

	// Now, wait for the reponse.
	resp[i].id = id;
	resp[i].used = true;

	do {
		// We only want the remote-port thread to be
		// processing RP packets. If the RP thread is
		// processing a request that in turn is calling us
		// to wait for a reponse, we process packets
		// recursively.
		//
		// Otherwise, we wait for the response slot
		// event to wake us up once the RP thread has
		// received our response.
		if (adaptor->current_process_is_adaptor()) {
			adaptor->rp_process(false);
		} else {
			wait(resp[i].ev);
		}
	} while (!resp[i].valid);
	return i;
}

void remoteport_tlm_dev::response_done(unsigned int index)
{
	resp[index].valid = false;
	resp[index].used = false;
}

int64_t remoteport_tlm::rp_map_time(sc_time t)
{
	return sync->map_time(t);
}

void remoteport_tlm::tie_off(void)
{
	unsigned int i;

	for (i = 0; i < RP_MAX_DEVS; i++) {
		if (devs[i]) {
			devs[i]->tie_off();
		}
	}
}

ssize_t remoteport_tlm::rp_read(void *rbuf, size_t count)
{
	ssize_t r = 0;

	//While no-data is available, update clocks and sync.
	//That is to avoid SystemC stay behind the remote side.
	//The idea is not to use anymore the sync-commands before and after commands.
	while(r == 0)
	{
		update_clocks();
		r = rp_safe_read(fd, rbuf, count);
	}
	//fprintf(stderr, "RP READ: r = %d \n", (int)r);

	if (r < (ssize_t)count) {
		if (r < 0)
			perror(__func__);
		exit(EXIT_FAILURE);
	}
	return r;
}

ssize_t remoteport_tlm::rp_write(const void *wbuf, size_t count)
{
	ssize_t r;

	r = rp_safe_write(fd, wbuf, count);
	if (r < (ssize_t)count) {
		if (r < 0)
			perror(__func__);
		exit(EXIT_FAILURE);
	}
	return r;
}

void remoteport_tlm::rp_cmd_hello(struct rp_pkt &pkt)
{
	if (pkt.hello.version.major != RP_VERSION_MAJOR) {
		cerr << "RP Version missmatch"
			<< " remote=" << pkt.hello.version.major
			<< "." << pkt.hello.version.minor
			<< " local=" << RP_VERSION_MAJOR
			<< "." << RP_VERSION_MINOR
			<< endl;
		exit(EXIT_FAILURE);
	}

	if (pkt.hello.caps.len) {
		void *caps = (char *) &pkt + pkt.hello.caps.offset;

		rp_process_caps(&peer, caps, pkt.hello.caps.len);
	}
}

void remoteport_tlm::rp_say_hello(void)
{
	uint32_t caps[] = {
		CAP_BUSACCESS_EXT_BASE,
		CAP_WIRE_POSTED_UPDATES,
	};
	struct rp_pkt_hello pkt = {0};
	size_t len;

	len = rp_encode_hello_caps(rp_pkt_id++, 0,
				   &pkt, RP_VERSION_MAJOR, RP_VERSION_MINOR,
				   caps, caps, sizeof caps / sizeof caps[0]);
	rp_write(&pkt, len);
	rp_write(caps, sizeof caps);
}

void remoteport_tlm::rp_cmd_sync(struct rp_pkt &pkt, bool can_sync)
{
	size_t plen;
        int64_t clk;
	remoteport_packet pkt_tx;

	//sync->pre_sync_cmd(pkt.sync.timestamp, can_sync);

	clk = sync->map_time(sync->get_current_time());
	plen = rp_encode_sync_resp(pkt.hdr.id,
				   pkt.hdr.dev, &pkt_tx.pkt->sync,
				   clk);
	rp_write(pkt_tx.pkt, plen);

	//sync->post_sync_cmd(pkt.sync.timestamp, can_sync);
}

bool remoteport_tlm::rp_process(bool can_sync)
{
	remoteport_packet pkt_rx;
	ssize_t r;

	pkt_rx.alloc(sizeof(pkt_rx.pkt->hdr) + 128);
	while (1) {
		//update_clocks();
		remoteport_tlm_dev *dev;
		unsigned char *data;
		uint32_t dlen;
		size_t datalen;

		r = rp_read(&pkt_rx.pkt->hdr, sizeof pkt_rx.pkt->hdr);
		if (r < 0)
			perror(__func__);

		rp_decode_hdr(pkt_rx.pkt);

		pkt_rx.alloc(sizeof pkt_rx.pkt->hdr + pkt_rx.pkt->hdr.len);
		r = rp_read(&pkt_rx.pkt->hdr + 1, pkt_rx.pkt->hdr.len);

		dlen = rp_decode_payload(pkt_rx.pkt);
		data = pkt_rx.u8 + sizeof pkt_rx.pkt->hdr + dlen;
		datalen = pkt_rx.pkt->hdr.len - dlen;

		dev = devs[pkt_rx.pkt->hdr.dev];
		if (pkt_rx.pkt->hdr.flags & RP_PKT_FLAGS_response) {
			unsigned int ri;

			if (pkt_rx.pkt->hdr.flags & RP_PKT_FLAGS_posted) {
				// Drop responses for posted packets.
				return true;
			}

			pkt_rx.data_offset = sizeof pkt_rx.pkt->hdr + dlen;

			ri = dev->response_lookup(pkt_rx.pkt->hdr.id);
			if (ri == ~0U) {
				printf("unhandled response: id=%d dev=%d\n",
					pkt_rx.pkt->hdr.id,
					pkt_rx.pkt->hdr.dev);
				assert(ri != ~0U);
			}

			pkt_rx.copy(dev->resp[ri].pkt);
			dev->resp[ri].valid = true;
			dev->resp[ri].ev.notify();
			return true;
		}

//		printf("%s: cmd=%d dev=%d\n", __func__, pkt_rx.pkt->hdr.cmd, pkt_rx.pkt->hdr.dev);
		sync->pre_any_cmd(&pkt_rx, can_sync);
		switch (pkt_rx.pkt->hdr.cmd) {
		case RP_CMD_hello:
			rp_cmd_hello(*pkt_rx.pkt);
			break;
		case RP_CMD_write:
			assert(dev);
			dev->cmd_write(*pkt_rx.pkt, can_sync, data, datalen);
			break;
		case RP_CMD_read:
			assert(dev);
			dev->cmd_read(*pkt_rx.pkt, can_sync);
			break;
		case RP_CMD_interrupt:
			assert(dev);
			dev->cmd_interrupt(*pkt_rx.pkt, can_sync);
			break;
		case RP_CMD_sync:
                        rp_cmd_sync(*pkt_rx.pkt, can_sync);
			break;
		default:
			assert(0);
			break;
		}
		sync->post_any_cmd(&pkt_rx, can_sync);
	}
	return false;
}

bool remoteport_tlm::current_process_is_adaptor(void)
{
	sc_process_handle h = sc_get_current_process_handle();
	return adaptor_proc == h;
}

void remoteport_tlm::process(void)
{
	adaptor_proc = sc_get_current_process_handle();

	if (fd == -1) {
		fd = sk_open(sk_descr);
		if (fd == -1) {
			printf("Failed to create remote-port socket connection!\n");
			if (sk_descr) {
				perror(sk_descr);
			}
			exit(EXIT_FAILURE);
			return;
		}
	}

	//Modify the file status flags, to allow non-blocking reads
	int flags = fcntl(fd, F_GETFL) | O_NONBLOCK;
	fcntl(fd, F_SETFL, flags);

	sync->reset();
	wait(rst.negedge_event());

	rp_say_hello();
	createShm();

	while (1) {

		rp_process(false);
	}
	sync->sync();
	return;
}

int remoteport_tlm::createShm(void)
{
	shmid = -1;
    // 1. Generate unique Keys for shared memory
	//		- The path to a file the both processes can read
	//		- An arbitrary id to generate a unique key
    char *fileName = new char[strlen(sk_descr) - strlen("unix:") + 1];
    strncpy(fileName, sk_descr + strlen("unix:"),(strlen(sk_descr) - strlen("unix:") + 1));
	const int id = 'M';
    key_t key = ftok(fileName,id);
    if(key == -1){
        perror("Unable to create shared-memory's key \n");
        return -1;
    }

    //2. Get identifier for shared memory
    size_t shmSize = sizeof(clks);
    int shmPermissions = 0666|IPC_CREAT;
    shmid = shmget(key, shmSize, shmPermissions);
    if(shmid == -1){
        perror("Unable to create shared-memory's shmid \n");
        return -1;
    }

    //3.  Attach to shared memory to get a pointer to it
    shData = (int64_t*) shmat(shmid, (void*)0, 0);
    if(shData == (int64_t*)(-1)){
        perror("%s: Unable to attached to shared-memory \n");
        return -1;
    }
    paused = false;

    return 0;
}

void remoteport_tlm::update_clocks(void)
{
    int64_t lclk =  shData[1];
    int64_t rclk = shData[0];

    if(lclk < rclk)
    {
    	int64_t delta_ns = rclk - lclk;
		sc_time delta = sc_time(delta_ns, SC_NS);
		sc_time q = sync->get_global_quantum();

		if (delta > q)
		{
			delta = q;
		}

		// Never allow the local time to go beyond the global quantum, cap it.
		if (sync->get_local_time() + delta > q)
		{
			sync->set_local_time(q);
		}
		else
		{
			sync->inc_local_time(delta);
		}

		sync->sync();
		shData[1] = sync->map_time(sync->get_current_time());
    }
}
