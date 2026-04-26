/*
 * Send QMI_SERVREG_NOTIF_RESTART_PD_REQ to modem's SERVREG_NOTIF service.
 * Used to force modem to (re)start a sub-PD like msm/modem/wlan_pd in
 * recovery, where there's no cnss-daemon to do this for us.
 */
#include <sys/socket.h>
#include <sys/time.h>
#include <linux/qrtr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>

#define SERVREG_NOTIF_SERVICE  0x42  /* 66 */
#define SERVREG_NOTIF_VERSION  1
#define SERVREG_NOTIF_INSTANCE 180
#define MSG_RESTART_PD_REQ     0x0024

/* QMI service header (5 bytes): control flags + txn id + msg id + body len */
struct qmi_hdr {
	uint8_t  flags;        /* 0x00 = request */
	uint16_t txn_id;
	uint16_t msg_id;
	uint16_t msg_len;
} __attribute__((packed));

static int qrtr_lookup(int sock, uint32_t service, uint16_t version, uint32_t instance,
                       struct sockaddr_qrtr *out)
{
	struct qrtr_ctrl_pkt pkt;
	struct sockaddr_qrtr sq, from;
	socklen_t sl;
	uint32_t enc;
	int rc;

	sl = sizeof(sq);
	if (getsockname(sock, (struct sockaddr *)&sq, &sl) < 0)
		return -1;
	sq.sq_port = QRTR_PORT_CTRL;

	memset(&pkt, 0, sizeof(pkt));
	pkt.cmd = QRTR_TYPE_NEW_LOOKUP;
	pkt.server.service = service;
	enc = ((uint32_t)instance << 8) | version;
	pkt.server.instance = enc;

	if (sendto(sock, &pkt, sizeof(pkt), 0, (struct sockaddr *)&sq, sizeof(sq)) < 0)
		return -1;

	for (;;) {
		sl = sizeof(from);
		rc = recvfrom(sock, &pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &sl);
		if (rc < 0)
			return -1;
		if (pkt.cmd != QRTR_TYPE_NEW_SERVER)
			continue;
		if (!pkt.server.service && !pkt.server.instance &&
		    !pkt.server.node && !pkt.server.port)
			break;
		if (pkt.server.service == service && pkt.server.instance == enc) {
			out->sq_family = AF_QIPCRTR;
			out->sq_node = pkt.server.node;
			out->sq_port = pkt.server.port;
			return 0;
		}
	}
	return -ENOENT;
}

int main(int argc, char **argv)
{
	const char *pd = (argc > 1) ? argv[1] : "msm/modem/wlan_pd";
	struct sockaddr_qrtr modem;
	struct timeval tv = { .tv_sec = 5 };
	int sock, rc;
	uint8_t buf[256], rbuf[256];
	struct qmi_hdr *h = (struct qmi_hdr *)buf;
	size_t pdlen = strlen(pd);
	size_t msg_len, total;

	if (pdlen > 64) {
		fprintf(stderr, "PD path too long\n");
		return 1;
	}

	sock = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (sock < 0) { perror("socket"); return 1; }

	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	if (qrtr_lookup(sock, SERVREG_NOTIF_SERVICE, SERVREG_NOTIF_VERSION,
	                SERVREG_NOTIF_INSTANCE, &modem) < 0) {
		fprintf(stderr, "could not find SERVREG_NOTIF instance %d on qrtr\n",
		        SERVREG_NOTIF_INSTANCE);
		return 1;
	}
	fprintf(stderr, "modem SERVREG_NOTIF: node=%u port=%u\n",
	        modem.sq_node, modem.sq_port);

	/* Drain any remaining qrtr ctrl messages from the lookup so they don't
	 * bleed into our QMI response read. */
	close(sock);
	sock = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	/* Build TLV body: tag 0x01, len = pdlen+1 (incl NUL?), value = pd
	 * Actually QMI string TLV: tag, le16 length, value (NUL-terminated string,
	 * length includes NUL).
	 */
	uint8_t *body = buf + sizeof(*h);
	body[0] = 0x01;
	uint16_t blen = pdlen + 1;
	memcpy(body + 1, &blen, 2);
	memcpy(body + 3, pd, pdlen + 1);
	msg_len = 1 + 2 + pdlen + 1;

	h->flags = 0x00;             /* request */
	h->txn_id = 1;
	h->msg_id = MSG_RESTART_PD_REQ;
	h->msg_len = msg_len;
	total = sizeof(*h) + msg_len;

	fprintf(stderr, "sending RESTART_PD_REQ for '%s' (%zu bytes)\n", pd, total);
	if (sendto(sock, buf, total, 0, (struct sockaddr *)&modem, sizeof(modem)) < 0) {
		perror("sendto");
		return 1;
	}

	struct sockaddr_qrtr from;
	socklen_t sl = sizeof(from);
	rc = recvfrom(sock, rbuf, sizeof(rbuf), 0, (struct sockaddr *)&from, &sl);
	if (rc < 0) {
		fprintf(stderr, "recvfrom: %s — modem didn't respond within 5s\n",
		        strerror(errno));
		return 1;
	}
	fprintf(stderr, "got %d bytes from node=%u port=%u\n", rc, from.sq_node, from.sq_port);

	/* Decode response: hdr(5) + TLV 0x02 (response code) */
	if ((size_t)rc >= sizeof(*h) + 7) {
		const uint8_t *r = rbuf + sizeof(*h);
		uint16_t result = 0, error = 0;
		/* TLV 0x02: result = 16, error = 16 */
		if (r[0] == 0x02) {
			memcpy(&result, r + 3, 2);
			memcpy(&error,  r + 5, 2);
		}
		fprintf(stderr, "result=%u error=%u\n", result, error);
		return result == 0 ? 0 : 2;
	}
	fprintf(stderr, "short response\n");
	return 3;
}
