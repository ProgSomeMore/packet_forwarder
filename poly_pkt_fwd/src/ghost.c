/*
 *  Extension of Semtech Semtech-Cycleo Packet Forwarder.
 *  (C) 2015 Beta Research BV
 *
 *  Description: Virtualization of nodes.
 *
 *  License: Revised BSD License, see LICENSE.TXT file include in the project
 *  Maintainer: Ruud Vlaming
 */

/* fix an issue between POSIX and C99 */
#ifdef __MACH__
#elif __STDC_VERSION__ >= 199901L
	#define _XOPEN_SOURCE 600
#else
	#define _XOPEN_SOURCE 500
#endif

#include <stdint.h>     /* C99 types */
#include <stdbool.h>    /* bool type */
#include <stdio.h>      /* printf, fprintf, snprintf, fopen, fputs */

#include <string.h>     /* memset */
#include <signal.h>     /* sigaction */
#include <time.h>       /* time, clock_gettime, strftime, gmtime */
#include <sys/time.h>   /* timeval */
#include <unistd.h>     /* getopt, access */
#include <stdlib.h>     /* atoi, exit */
#include <errno.h>      /* error messages */

#include <sys/socket.h> /* socket specific definitions */
#include <netinet/in.h> /* INET constants and stuff */
#include <arpa/inet.h>  /* IP address conversion stuff */
#include <netdb.h>      /* gai_strerror */

#include <pthread.h>
#include "trace.h"
#include "ghost.h"
#include "endianext.h"



/* -------------------------------------------------------------------------- */
/* --- PRIVATE MACROS ------------------------------------------------------- */

//#define MSG(args...)    printf(args) /* message that is destined to the user */ => moved to trace.h
#define PROTOCOL_VERSION       2
#define GHOST_PRQ_DATA        11
#define GHOST_UP_DATA         11
#define GHOST_DOWN_DATA       12

extern bool logger_enabled;

volatile bool ghost_run = false;      /* false -> ghost thread terminates cleanly */

struct timeval ghost_timeout = {0, (200 * 1000)}; /* non critical for throughput */

static pthread_mutex_t cb_ghost = PTHREAD_MUTEX_INITIALIZER; /* control access to the ghoststream measurements */

//!static int rxpktSize = sizeof(struct lgw_pkt_rx_s);
//!static int txpktSize = sizeof(struct lgw_pkt_tx_s);

static uint8_t buffRX[GHST_RX_BUFFSIZE*GHST_NM_RCV]; /* circular buffer for receiving packets */
static uint8_t buffTX[GHST_TX_BUFFSIZE*GHST_NM_SND]; /* circular buffer for sending packets */

static uint8_t ghst_rcv_end;                 /* end of receive circular packet buffer  */
static uint8_t ghst_rcv_bgn;                 /* begin of receive circular packet buffer */

static uint8_t ghst_snd_end;                 /* end of send circular packet buffer  */
static uint8_t ghst_snd_bgn;                 /* begin of send circular packet buffer */

static int sock_ghost; /* socket for ghost traffic */
static char gateway_id[16]  = ""; /* string form of gateway mac address */

/* ghost thread */
static pthread_t thrid_ghost;

/* Reference coordinates, for broadcasting (beacon) */
static struct coord_s reference_coord;

/* for debugging purposes. */
static void printBuffer(uint8_t *b, uint8_t len)  __attribute__ ((unused));
static void printBuffer(uint8_t *b, uint8_t len)
{
  int i;
  for (i=0; i<len; i++) { printf("%i,",b[i]);  } }

/* for debugging purposes. */
static void printRX(struct lgw_pkt_rx_s *p)  __attribute__ ((unused));
static void printRX(struct lgw_pkt_rx_s *p)
{ printf(
    "  p->freq_hz    = %i\n"
    "  p->if_chain   = %i\n"
    "  p->status     = %i\n"
    "  p->count_us   = %i\n"
    "  p->rf_chain   = %i\n"
    "  p->modulation = %i\n"
    "  p->bandwidth  = %i\n"
    "  p->datarate   = %i\n"
    "  p->coderate   = %i\n"
    "  p->rssi       = %f\n"
    "  p->snr        = %f\n"
    "  p->snr_min    = %f\n"
    "  p->snr_max    = %f\n"
    "  p->crc        = %i\n"
    "  p->size       = %i\n"
    "  p->payload    = %s\n",
    p->freq_hz,p->if_chain,p->status,p->count_us,
    p->rf_chain,p->modulation,p->bandwidth,p->datarate,
    p->coderate,p->rssi,p->snr,p->snr_min,p->snr_max,
    p->crc,p->size,p->payload); }


/* to bitwise convert an unsigned integer to a float */
typedef union
{ uint32_t u;
  float f; } mix;

/* Helper functions for architecture independent conversion (BE!) of data packet to structure lgw_pkt_rx_s */
//TODO: This is hard conversion from BE to LE, but we could run on a BE chip or in a BE mode (Arm supports both!)
static uint32_t u32(uint8_t *p, uint8_t i) { return (uint32_t)(p[i+3]) + ((uint32_t)(p[i+2])<<8) + ((uint32_t)(p[i+1])<<16) + ((uint32_t)(p[i])<<24);  }
static uint16_t u16(uint8_t *p, uint8_t i) { return (uint16_t)p[i+1] + ((uint16_t)p[i]<<8);  }
static uint8_t u8(uint8_t *p, uint8_t i)   { return p[i]; }
static float eflt(uint8_t *p, uint8_t i)
{ mix uf;
  uf.u = u32(p,i);
  return uf.f; }

/* Method to fill lgw_pkt_rx_s with data received by the ghost node server. */
static void readRX(struct lgw_pkt_rx_s *p, uint8_t *b, uint32_t time_us)
{ p->freq_hz    = u32(b,0);
  p->if_chain   =  u8(b,4);
  p->status     =  u8(b,5);
  p->count_us   =  time_us;
  p->rf_chain   =  u8(b,10);
  p->modulation =  u8(b,11);
  p->bandwidth  =  u8(b,12);
  p->datarate   = u32(b,13);
  p->coderate   =  u8(b,17);
  p->rssi       = eflt(b,18);
  p->snr        = eflt(b,22);
  p->snr_min    = eflt(b,26);
  p->snr_max    = eflt(b,30);
  p->crc        = u16(b,34);
  p->size       = u16(b,36);
  memcpy((p->payload),&b[38],p->size); }

static uint16_t sizeTX(struct lgw_pkt_tx_s *pkt)
{ return sizeof(struct lgw_pkt_tx_s) - 256 + (pkt->size); }


static void thread_ghost(void);

/* -------------------------------------------------------------------------- */
/* --- THREAD: RECEIVING PACKETS FROM GHOST NODES --------------------------- */


void ghost_start(const char * ghost_addr, const char * ghost_port, const struct coord_s refcoor, const char * gwid)
{
    /* You cannot start a running ghost listener.*/
    if (ghost_run) return;

    int i; /* loop variable and temporary variable for return value */

    /* copy the static coordinates (so if the gps changes, this is not reflected!) */
    reference_coord = refcoor;
    strncpy(gateway_id,gwid,sizeof gateway_id);

    struct addrinfo addresses;
    struct addrinfo *result; /* store result of getaddrinfo */
    struct addrinfo *q;      /* pointer to move into *result data */
    char host_name[64];
    char port_name[64];

    memset(&addresses, 0, sizeof addresses);
    addresses.ai_family = AF_UNSPEC;   /* should handle IP v4 or v6 automatically */
    addresses.ai_socktype = SOCK_DGRAM;

    /* Get the credentials for this server. */
    i = getaddrinfo(ghost_addr, ghost_port, &addresses, &result);
    if (i != 0)
    { MSG("ERROR: [ghost] getaddrinfo on address %s (PORT %s) returned %s\n", ghost_addr, ghost_port, gai_strerror(i));
      exit(EXIT_FAILURE); }

    /* try to open socket for ghost listener */
    for (q=result; q!=NULL; q=q->ai_next)
    { sock_ghost = socket(q->ai_family, q->ai_socktype,q->ai_protocol);
      if (sock_ghost == -1) continue; /* try next field */
      else break; }

    /* See if the connection was a success, if not, this is a permanent failure */
    if (q == NULL)
    { MSG("ERROR: [ghost] failed to open socket to any of server %s addresses (port %s)\n", ghost_addr, ghost_port);
      i = 1;
      for (q=result; q!=NULL; q=q->ai_next)
      { getnameinfo(q->ai_addr, q->ai_addrlen, host_name, sizeof host_name, port_name, sizeof port_name, NI_NUMERICHOST);
        MSG("INFO: [ghost] result %i host:%s service:%s\n", i, host_name, port_name);
        ++i; }
      exit(EXIT_FAILURE); }

    /* connect so we can send/receive packet with the server only */
    i = connect(sock_ghost, q->ai_addr, q->ai_addrlen);
    if (i != 0) {
        MSG("ERROR: [ghost] connect returned %s\n", strerror(errno));
        exit(EXIT_FAILURE); }

    freeaddrinfo(result);

    /* set the circular buffer pointers to the beginning */
    ghst_rcv_bgn = 0;
    ghst_rcv_end = 0;
    ghst_snd_bgn = 0;
    ghst_snd_end = 0;

    /* spawn thread to manage ghost connection */
    ghost_run = true;
    i = pthread_create( &thrid_ghost, NULL, (void * (*)(void *))thread_ghost, NULL);
    if (i != 0)
    { MSG("ERROR: [ghost] impossible to create ghost thread\n");
      exit(EXIT_FAILURE); }

    /* We are done here, ghost thread is initialized and should be running by now. */

}


void ghost_stop(void)
{   ghost_run = false;                /* terminate the loop. */
    pthread_cancel(thrid_ghost);      /* don't wait for ghost thread (is this okay??) */
    shutdown(sock_ghost, SHUT_RDWR);  /* close the socket. */
}



/* Call this to pull data from the receive buffer for ghost nodes.. */
int ghost_get(int max_pkt, struct lgw_pkt_rx_s *pkt_data)
{   /* Calculate the number of available packets */
    pthread_mutex_lock(&cb_ghost);
    uint8_t avail = (ghst_rcv_bgn - ghst_rcv_end + GHST_NM_RCV) % GHST_NM_RCV;
    pthread_mutex_unlock(&cb_ghost);

    /* Calculate the number of packets that we may or can copy */
    uint8_t get_pkt = (avail<max_pkt) ? avail : max_pkt;

    /* timestamp the incomming packets as if they came over the air. */
    //TODO: The timestamp should actually be given at entrance. But the difference is small,
    //      and so multiple packets can be stamped at once.
    uint32_t time_us = 0;
    if (get_pkt > 0)
    { struct timeval current_unix_time;
      struct timeval current_concentrator_time;
      gettimeofday(&current_unix_time, NULL);
      get_concentrator_time(&current_concentrator_time, current_unix_time);
      time_us = (uint32_t) (current_concentrator_time.tv_sec * 1000000UL + current_concentrator_time.tv_usec); }

    /* Do the actual copying, take into account that the read buffer is circular. */
    int i;
    for (i=0; i<get_pkt; i++)
    { int ind = (ghst_rcv_end + i) % GHST_NM_RCV;
      readRX(&pkt_data[i],(buffRX+ind*GHST_RX_BUFFSIZE),time_us); }

    /* Shift the end index of the read buffer to the new position. */
    pthread_mutex_lock(&cb_ghost);
    ghst_rcv_end = (ghst_rcv_end + get_pkt) % GHST_NM_RCV;
    pthread_mutex_unlock(&cb_ghost);

    // To get more info enable this
    if (false && (get_pkt>0))
    { LOGGER("INFO: copied %i packets from ghost, ghst_rcv_end  = %i \n",get_pkt,ghst_rcv_end);
      //for (i=0; i<get_pkt; i++)
      //{ printf("packet %i\n",i);
      //  printRX(&pkt_data[i]); }
    }

    /* return the number of packets that where copied. */
    return get_pkt; }


/* Call this to push data from the server to the receiving ghost node. */
int ghost_put(struct lgw_pkt_tx_s *pkt)
{ /* aux variable for data copy */
  int  next;
  int  offset;
  bool full;

  /* Determine the next pointer where data can be written and see if the circular buffer is full */
  next  =  (ghst_snd_bgn + 1) % GHST_NM_RCV;
  pthread_mutex_lock(&cb_ghost);
  full  =  next == ghst_snd_end;
  pthread_mutex_unlock(&cb_ghost);

  /* Calculate index where to place the data */
  offset = ghst_snd_bgn*GHST_TX_BUFFSIZE;

  /* Add identification part */
  buffTX[offset] = PROTOCOL_VERSION;
  buffTX[offset+1] = 0;  // reserved for future use
  buffTX[offset+2] = 0;  // reserved for future use
  buffTX[offset+3] = GHOST_DOWN_DATA;

  /* make a copy to the data received to the circular buffer, and shift the write index. */
  if (full)
  { return false; }
  else
  { memcpy((void *)(buffTX+offset+4),pkt,sizeof(struct lgw_pkt_tx_s));
	pthread_mutex_lock(&cb_ghost);
	ghst_snd_bgn = next;
	pthread_mutex_unlock(&cb_ghost);
	LOGGER("INFO, enqueued packet for downstream");
    return true; }

}

static void thread_ghost(void)
{   int i; /* loop variable */

    MSG("INFO: Ghost thread started.\n");

    /* local timekeeping variables */
    struct timespec send_time; /* time of the pull request */
    struct timespec recv_time; /* time of return from recv socket call */

    /* data buffers */
    uint8_t buff_up[GHST_MIN_PACKETSIZE+GHST_RX_BUFFSIZE]; /* buffer to receive upstream packets */
    uint8_t buff_req[GHST_REQ_BUFFSIZE]; /* buffer to compose pull requests */
    int msg_len;

    /* set ghoststream socket RX timeout */
    i = setsockopt(sock_ghost, SOL_SOCKET, SO_RCVTIMEO, (void *)&ghost_timeout, sizeof ghost_timeout);
    if (i != 0)
    { MSG("ERROR: [ghost] setsockopt returned %s\n", strerror(errno));
      exit(EXIT_FAILURE); }

    /* pre-fill the pull request buffer with fixed fields, check if there is enough space. */
    if (sizeof(buff_req) < 4 + sizeof gateway_id + 2*sizeof(double))
    { MSG("INTERNAL ERROR: [ghost] BUFFER TO SMALL.\n");
      exit(EXIT_FAILURE); }
    memset(buff_req,0,sizeof buff_req);

    /* Add identification part */
    buff_req[0] = PROTOCOL_VERSION;
    buff_req[1] = 0;  // reserved for future use
    buff_req[2] = 0;  // reserved for future use
    buff_req[3] = GHOST_PRQ_DATA;

    /* Add the gateway id to the request */
    memcpy(&buff_req[4],&gateway_id, sizeof gateway_id);

    /* Add the coordinates to the buffer */
    tobecpy(&buff_req[4+sizeof(gateway_id)],&reference_coord.lon,sizeof(double));
    tobecpy(&buff_req[4+sizeof(gateway_id)+sizeof(double)],&reference_coord.lat,sizeof(double));

    /* aux variable for data copy */
    int  next;
    bool full;
    uint8_t avail;
    struct lgw_pkt_tx_s *dwn_pkt;

    while (ghost_run)
    {   /* send PULL request and record time */
        // TODO zend later hier de data voor de nodes, nu alleen een pullreq.

        send(sock_ghost, (void *)buff_req, sizeof buff_req, 0);
        clock_gettime(CLOCK_MONOTONIC, &send_time);
        //!req_ack = false;
        //MSG("DEBUG: GHOST LOOP\n");
        /* listen to packets and process them until a new PULL request must be sent */
        recv_time = send_time;
        while ((int)difftimespec(recv_time, send_time) < NODE_CALL_SECS)
        {   /* try to receive a datagram */
            msg_len = recv(sock_ghost, (void *)buff_up, (sizeof buff_up)-1, 0);
            clock_gettime(CLOCK_MONOTONIC, &recv_time);

            /* The protocol requires renewal of the subscription on regular basis. So we do
             * not reset the wait time force a new pull request every NODE_CALL_SECS.
             * If we are here because of a timeout, try again, but first, inspect the
             * down message queue */
            if (msg_len < 0)
            { /* Calculate the number of available packets */
              pthread_mutex_lock(&cb_ghost);
              avail = (ghst_snd_bgn - ghst_snd_end + GHST_NM_SND) % GHST_NM_SND;
              pthread_mutex_unlock(&cb_ghost);

              /* Send one packet, take into account that the read buffer is circular. */
              if (avail > 0)
              { int blockSize = (ghst_snd_end % GHST_NM_SND)*GHST_TX_BUFFSIZE;
                dwn_pkt = (struct lgw_pkt_tx_s *) (buffTX+blockSize+4);

                /* Send may actually return -1 if it fails. We do not try again, the packet is lost. */
                // TODO: +2 seems correct (transmits the correct number of payload bytes), but why is +4 two too many???
                send(sock_ghost, (void *)(buffTX+blockSize), sizeTX(dwn_pkt)+2, 0);

                /* Shift the end index of the read buffer to the new position.
                 * In the mean time other packets could have been added. */
                pthread_mutex_lock(&cb_ghost);
                ghst_snd_end = (ghst_snd_end + 1) % GHST_NM_SND;
                pthread_mutex_unlock(&cb_ghost); }

              /* We are done, try to read some packets again. */
              continue; }

            /* if the datagram does not respect protocol, just ignore it */
            if ((msg_len < 4 + GHST_MIN_PACKETSIZE) || (msg_len > 4 + GHST_RX_BUFFSIZE) || (buff_up[0] != PROTOCOL_VERSION) || ((buff_up[3] != GHOST_UP_DATA) ))
            { LOGGER("WARNING: [ghost] ignoring invalid packet len=%d, protocol_version=%d, id=%d\n", msg_len, buff_up[0], buff_up[3]);
              continue; }

            /* The datagram is a GHOST_DATA, add string terminator, just to be safe */
            buff_up[msg_len] = 0;

            /* Determine the next pointer where data can be written and see if the circular buffer is full */
            next  =  (ghst_rcv_bgn + 1) % GHST_NM_RCV;
            pthread_mutex_lock(&cb_ghost);
            full  =  next == ghst_rcv_end;
            pthread_mutex_unlock(&cb_ghost);

            /* make a copy to the data received to the circular buffer, and shift the write index. */
            if (full)
            {  LOGGER("WARNING: [ghost] buffer is full, dropping packet)\n"); }
            else
            {  memcpy((void *)(buffRX+ghst_rcv_bgn*GHST_RX_BUFFSIZE),buff_up+4,msg_len-3);
               pthread_mutex_lock(&cb_ghost);
               ghst_rcv_bgn = next;
               pthread_mutex_unlock(&cb_ghost);
               LOGGER("RECEIVED, [ghost] ghst_rcv_bgn = %i \n", ghst_rcv_bgn); } } }

    MSG("\nINFO: End of ghost thread\n");
}
