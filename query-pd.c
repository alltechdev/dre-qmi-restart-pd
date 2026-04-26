/* Send QMI_SERVREG_NOTIF_QUERY_STATE for a PD path. */
#include <sys/socket.h>
#include <sys/time.h>
#include <linux/qrtr.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SVC 0x42
#define VER 1
#define INST 180
#define MSG_QUERY_STATE_REQ 0x0021

struct qmi_hdr { uint8_t flags; uint16_t txn; uint16_t mid; uint16_t mlen; } __attribute__((packed));

int main(int argc, char **argv) {
	const char *pd = argc>1 ? argv[1] : "msm/modem/wlan_pd";
	int s = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	struct sockaddr_qrtr sq, modem = {0}, from;
	socklen_t sl = sizeof(sq);
	getsockname(s, (void*)&sq, &sl);
	struct qrtr_ctrl_pkt p = {0};
	p.cmd = QRTR_TYPE_NEW_LOOKUP;
	p.server.service = SVC;
	p.server.instance = ((uint32_t)INST << 8) | VER;
	sq.sq_port = QRTR_PORT_CTRL;
	sendto(s, &p, sizeof(p), 0, (void*)&sq, sizeof(sq));
	struct timeval tv = {2, 0};
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	for(;;) {
		sl = sizeof(from);
		int r = recvfrom(s, &p, sizeof(p), 0, (void*)&from, &sl);
		if (r < 0) break;
		if (p.cmd != QRTR_TYPE_NEW_SERVER) continue;
		if (!p.server.service) break;
		if (p.server.service==SVC) { modem.sq_family=AF_QIPCRTR; modem.sq_node=p.server.node; modem.sq_port=p.server.port; break; }
	}
	if (!modem.sq_port) { fprintf(stderr, "no servreg_notif found\n"); return 1; }
	uint8_t buf[256] = {0};
	struct qmi_hdr *h = (void*)buf;
	uint8_t *body = buf + sizeof(*h);
	body[0] = 0x01;
	uint16_t bl = strlen(pd) + 1;
	memcpy(body+1, &bl, 2);
	memcpy(body+3, pd, strlen(pd)+1);
	h->flags = 0; h->txn = 1; h->mid = MSG_QUERY_STATE_REQ; h->mlen = 1+2+strlen(pd)+1;
	sendto(s, buf, sizeof(*h)+h->mlen, 0, (void*)&modem, sizeof(modem));
	uint8_t r[256];
	sl = sizeof(from);
	int n = recvfrom(s, r, sizeof(r), 0, (void*)&from, &sl);
	fprintf(stderr, "got %d bytes\n", n);
	for (int i = 0; i < n; i++) fprintf(stderr, "%02x ", r[i]);
	fprintf(stderr, "\n");
	/* parse: hdr(5), then TLVs. response (0x02) has resp(0x02) + curr_state_valid(0x10) + curr_state */
	const uint8_t *p2 = r + sizeof(*h);
	int rem = n - sizeof(*h);
	while (rem > 3) {
		uint8_t tag = p2[0];
		uint16_t len; memcpy(&len, p2+1, 2);
		fprintf(stderr, "TLV tag=0x%02x len=%u: ", tag, len);
		for (int i = 0; i < len && i < 8; i++) fprintf(stderr, "%02x ", p2[3+i]);
		fprintf(stderr, "\n");
		if (tag == 0x10 && len == 4) {
			uint32_t st;
			memcpy(&st, p2+3, 4);
			const char *ss = st==1?"UP":st==2?"DOWN":st==3?"EARLY_DOWN":st==0?"INIT":"UNKNOWN";
			fprintf(stderr, "** state=%u (%s)\n", st, ss);
		}
		p2 += 3 + len; rem -= 3 + len;
	}
	return 0;
}
