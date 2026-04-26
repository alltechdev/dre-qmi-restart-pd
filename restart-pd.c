/*
 * Send QMI_SERVREG_NOTIF_REGISTER_LISTENER then RESTART_PD to modem's
 * SERVREG_NOTIF service. RESTART_PD on its own returns INVALID_HANDLE
 * (error=9) on the OnePlus dre modem firmware; the modem expects the
 * caller to have an open listener for the PD before accepting restart.
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

#define SERVREG_NOTIF_SERVICE  0x42
#define SERVREG_NOTIF_VERSION  1
#define SERVREG_NOTIF_INSTANCE 180

#define MSG_REGISTER_LISTENER_REQ   0x0020
#define MSG_QUERY_STATE_REQ         0x0021
#define MSG_RESTART_PD_REQ          0x0024

struct qmi_hdr {
	uint8_t  flags;
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

/* Issue one QMI request, wait for its response. Returns response body bytes
 * past the QMI header. Sets *result and *error from the qmi_response_type_v01
 * (TLV 0x02). */
static int qmi_call(int sock, struct sockaddr_qrtr *peer,
                    uint16_t txn, uint16_t msg_id,
                    const uint8_t *body, size_t body_len,
                    uint16_t *result, uint16_t *error,
                    uint8_t *resp_buf, size_t resp_buf_len)
{
	uint8_t buf[512];
	struct qmi_hdr *h = (struct qmi_hdr *)buf;
	struct sockaddr_qrtr from;
	socklen_t sl;
	int rc;

	if (body_len + sizeof(*h) > sizeof(buf))
		return -EMSGSIZE;

	h->flags  = 0x00;
	h->txn_id = txn;
	h->msg_id = msg_id;
	h->msg_len = body_len;
	memcpy(buf + sizeof(*h), body, body_len);

	if (sendto(sock, buf, sizeof(*h) + body_len, 0,
	           (struct sockaddr *)peer, sizeof(*peer)) < 0)
		return -errno;

	for (;;) {
		sl = sizeof(from);
		rc = recvfrom(sock, resp_buf, resp_buf_len, 0,
		              (struct sockaddr *)&from, &sl);
		if (rc < 0)
			return -errno;
		/* Skip qrtr ctrl messages bleeding from the lookup */
		if (from.sq_port == (uint32_t)-2)
			continue;
		break;
	}

	*result = *error = 0;
	if ((size_t)rc >= sizeof(*h)) {
		const uint8_t *p = resp_buf + sizeof(*h);
		int rem = rc - sizeof(*h);
		while (rem > 3) {
			uint8_t tag = p[0];
			uint16_t len; memcpy(&len, p + 1, 2);
			if (tag == 0x02 && len == 4) {
				memcpy(result, p + 3, 2);
				memcpy(error,  p + 5, 2);
			}
			p += 3 + len; rem -= 3 + len;
		}
	}
	return rc;
}

/* Encode TLVs. QMI string TLVs in Qualcomm's servreg ei tables use length
 * = strlen() WITHOUT the trailing NUL.
 */
static size_t encode_pd_path(uint8_t *out, const char *pd, int with_enable)
{
	size_t off = 0;
	uint16_t pl = strlen(pd);
	if (with_enable) {
		out[off++] = 0x01; out[off++] = 1; out[off++] = 0;
		out[off++] = 1;
		out[off++] = 0x02;
	} else {
		out[off++] = 0x01;
	}
	memcpy(out + off, &pl, 2); off += 2;
	memcpy(out + off, pd, pl); off += pl;
	return off;
}

int main(int argc, char **argv)
{
	const char *pd = argc > 1 ? argv[1] : "msm/modem/wlan_pd";
	struct sockaddr_qrtr modem;
	struct timeval tv = { .tv_sec = 5 };
	int sock, rc;
	uint8_t body[128], resp[256];
	size_t blen;
	uint16_t result, error;

	sock = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (sock < 0) { perror("socket"); return 1; }
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	if (qrtr_lookup(sock, SERVREG_NOTIF_SERVICE, SERVREG_NOTIF_VERSION,
	                SERVREG_NOTIF_INSTANCE, &modem) < 0) {
		fprintf(stderr, "no SERVREG_NOTIF instance %d on qrtr\n",
		        SERVREG_NOTIF_INSTANCE);
		return 1;
	}
	fprintf(stderr, "modem SERVREG_NOTIF: node=%u port=%u\n",
	        modem.sq_node, modem.sq_port);

	/* fresh socket so qrtr ctrl messages from the lookup don't bleed in */
	close(sock);
	sock = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	/* Step 1: REGISTER_LISTENER (enable=1, service_name=pd) */
	blen = encode_pd_path(body, pd, 1);
	rc = qmi_call(sock, &modem, 1, MSG_REGISTER_LISTENER_REQ,
	              body, blen, &result, &error, resp, sizeof(resp));
	fprintf(stderr, "REGISTER_LISTENER %s -> rc=%d result=%u error=%u\n",
	        pd, rc, result, error);
	if (result != 0) {
		fprintf(stderr, "(continuing anyway)\n");
	}

	/* Step 2: QUERY_STATE */
	blen = encode_pd_path(body, pd, 0);
	rc = qmi_call(sock, &modem, 2, MSG_QUERY_STATE_REQ,
	              body, blen, &result, &error, resp, sizeof(resp));
	fprintf(stderr, "QUERY_STATE %s -> rc=%d result=%u error=%u\n",
	        pd, rc, result, error);
	/* parse curr_state TLV 0x10 (optional, len=4) */
	if (rc > 0) {
		const uint8_t *p = resp + sizeof(struct qmi_hdr);
		int rem = rc - sizeof(struct qmi_hdr);
		while (rem > 3) {
			uint8_t tag = p[0];
			uint16_t len; memcpy(&len, p + 1, 2);
			if (tag == 0x10 && len == 4) {
				uint32_t st; memcpy(&st, p + 3, 4);
				const char *sn = st==1?"UP":st==2?"DOWN":st==3?"EARLY_DOWN":st==4?"UNINIT":"?";
				fprintf(stderr, "  current state: %u (%s)\n", st, sn);
			}
			p += 3 + len; rem -= 3 + len;
		}
	}

	/* Step 3: RESTART_PD */
	blen = encode_pd_path(body, pd, 0);
	rc = qmi_call(sock, &modem, 3, MSG_RESTART_PD_REQ,
	              body, blen, &result, &error, resp, sizeof(resp));
	fprintf(stderr, "RESTART_PD %s -> rc=%d result=%u error=%u\n",
	        pd, rc, result, error);
	return result == 0 ? 0 : 2;
}
