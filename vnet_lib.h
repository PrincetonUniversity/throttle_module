#include <stdint.h>

#define __NR_get_vsock 400
#define __NR_vnet_register 401
#define __NR_vnet_apply 402
#define __NR_send_vsock 403

struct tcp_sock_stats {
	uint32_t snd_cwnd;
	uint32_t snd_una; // last unacked
	uint32_t write_seq;  //last byte queued
	uint32_t mss_size;
	uint32_t srtt;
	uint32_t snd_wnd;
	/* u32 rcv_wnd;
	    //bandwidth estimate
     u32 srtt; //this the rtt  what is the rev_rtt_est??
	 struct tcp_sack_block * selective_acks; //the array of sacks, check the actual details more
	 u32 sacked_out; ??*/
};

