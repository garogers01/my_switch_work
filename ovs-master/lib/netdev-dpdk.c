/*
 * Copyright (c) 2014 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <pthread.h>
#include <config.h>
#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "dirs.h"
#include "dp-packet.h"
#include "dpif-netdev.h"
#include "list.h"
#include "netdev-dpdk.h"
#include "netdev-provider.h"
#include "netdev-vport.h"
#include "odp-util.h"
#include "ofp-print.h"
#include "ovs-numa.h"
#include "ovs-thread.h"
#include "ovs-rcu.h"
#include "packets.h"
#include "shash.h"
#include "sset.h"
#include "unaligned.h"
#include "timeval.h"
#include "unixctl.h"
#include "openvswitch/vlog.h"
#include "rte_config.h"
#include "rte_mbuf.h"
#include "rte_virtio_net.h"
#include "rte_memcmp.h"
#include "rte_jhash.h"
#include "rte_hash_crc.h"

VLOG_DEFINE_THIS_MODULE(dpdk);
static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 20);

#define DPDK_PORT_WATCHDOG_INTERVAL 5

#define OVS_CACHE_LINE_SIZE CACHE_LINE_SIZE
#define OVS_VPORT_DPDK "ovs_dpdk"

/*
 * need to reserve tons of extra space in the mbufs so we can align the
 * DMA addresses to 4KB.
 * The minimum mbuf size is limited to avoid scatter behaviour and drop in
 * performance for standard Ethernet MTU.
 */
#define MTU_TO_MAX_LEN(mtu)  ((mtu) + ETHER_HDR_LEN + ETHER_CRC_LEN)
#define MBUF_SIZE_MTU(mtu)   (MTU_TO_MAX_LEN(mtu)        \
                              + sizeof(struct dp_packet) \
                              + RTE_PKTMBUF_HEADROOM)
#define MBUF_SIZE_DRIVER     (2048                       \
                              + sizeof (struct rte_mbuf) \
                              + RTE_PKTMBUF_HEADROOM)
#define MBUF_SIZE(mtu)       MAX(MBUF_SIZE_MTU(mtu), MBUF_SIZE_DRIVER)

/* Max and min number of packets in the mempool.  OVS tries to allocate a
 * mempool with MAX_NB_MBUF: if this fails (because the system doesn't have
 * enough hugepages) we keep halving the number until the allocation succeeds
 * or we reach MIN_NB_MBUF */

#define MAX_NB_MBUF          (4096 * 64)
#define MIN_NB_MBUF          (4096 * 4)
#define MP_CACHE_SZ          RTE_MEMPOOL_CACHE_MAX_SIZE
//#define MP_CACHE_SZ          256

/* MAX_NB_MBUF can be divided by 2 many times, until MIN_NB_MBUF */
BUILD_ASSERT_DECL(MAX_NB_MBUF % ROUND_DOWN_POW2(MAX_NB_MBUF/MIN_NB_MBUF) == 0);

/* The smallest possible NB_MBUF that we're going to try should be a multiple
 * of MP_CACHE_SZ. This is advised by DPDK documentation. */
BUILD_ASSERT_DECL((MAX_NB_MBUF / ROUND_DOWN_POW2(MAX_NB_MBUF/MIN_NB_MBUF))
                  % MP_CACHE_SZ == 0);

#define SOCKET0              0

#define NIC_PORT_RX_Q_SIZE 2048  /* Size of Physical NIC RX Queue, Max (n+32<=4096)*/
#define NIC_PORT_TX_Q_SIZE 2048  /* Size of Physical NIC TX Queue, Max (n+32<=4096)*/

static char *cuse_dev_name = NULL;    /* Character device cuse_dev_name. */
static char *vhost_sock_dir = NULL;   /* Location of vhost-user sockets */

/*
 * Maximum amount of time in micro seconds to try and enqueue to vhost.
 */
#define VHOST_ENQ_RETRY_USECS 100

static const struct rte_eth_conf port_conf = {
    .rxmode = {
        .mq_mode = ETH_MQ_RX_RSS,
        .split_hdr_size = 0,
        .header_split   = 0, /* Header Split disabled */
        .hw_ip_checksum = 0, /* IP checksum offload disabled */
        .hw_vlan_filter = 0, /* VLAN filtering disabled */
        .jumbo_frame    = 0, /* Jumbo Frame Support disabled */
        .hw_strip_crc   = 0,
    },
    .rx_adv_conf = {
        .rss_conf = {
            .rss_key = NULL,
            .rss_hf = ETH_RSS_IP | ETH_RSS_UDP | ETH_RSS_TCP,
        },
    },
    .txmode = {
        .mq_mode = ETH_MQ_TX_NONE,
    },
};

enum { MAX_TX_QUEUE_LEN = 384 };
enum { DPDK_RING_SIZE = 256 };
BUILD_ASSERT_DECL(IS_POW2(DPDK_RING_SIZE));
enum { DRAIN_TSC = 200000ULL };

enum dpdk_dev_type {
    DPDK_DEV_ETH = 0,
    DPDK_DEV_VHOST = 1,
};

static int rte_eal_init_ret = ENODEV;

static struct ovs_mutex dpdk_mutex = OVS_MUTEX_INITIALIZER;

/* Contains all 'struct dpdk_dev's. */
static struct ovs_list dpdk_list OVS_GUARDED_BY(dpdk_mutex)
    = OVS_LIST_INITIALIZER(&dpdk_list);

static struct ovs_list dpdk_mp_list OVS_GUARDED_BY(dpdk_mutex)
    = OVS_LIST_INITIALIZER(&dpdk_mp_list);

/* This mutex must be used by non pmd threads when allocating or freeing
 * mbufs through mempools. Since dpdk_queue_pkts() and dpdk_queue_flush() may
 * use mempools, a non pmd thread should hold this mutex while calling them */
static struct ovs_mutex nonpmd_mempool_mutex = OVS_MUTEX_INITIALIZER;

struct dpdk_mp {
    struct rte_mempool *mp;
    int mtu;
    int socket_id;
    int refcount;
    struct ovs_list list_node OVS_GUARDED_BY(dpdk_mutex);
};

/* There should be one 'struct dpdk_tx_queue' created for
 * each cpu core. */
struct dpdk_tx_queue {
    bool flush_tx;                 /* Set to true to flush queue everytime */
                                   /* pkts are queued. */
    int count;
    rte_spinlock_t tx_lock;        /* Protects the members and the NIC queue
                                    * from concurrent access.  It is used only
                                    * if the queue is shared among different
                                    * pmd threads (see 'txq_needs_locking'). */
    uint64_t tsc;
    struct rte_mbuf *burst_pkts[MAX_TX_QUEUE_LEN];
};

/* dpdk has no way to remove dpdk ring ethernet devices
   so we have to keep them around once they've been created
*/

static struct ovs_list dpdk_ring_list OVS_GUARDED_BY(dpdk_mutex)
    = OVS_LIST_INITIALIZER(&dpdk_ring_list);

struct dpdk_ring {
    /* For the client rings */
    struct rte_ring *cring_tx;
    struct rte_ring *cring_rx;
    int user_port_id; /* User given port no, parsed from port name */
    int eth_port_id; /* ethernet device port id */
    struct ovs_list list_node OVS_GUARDED_BY(dpdk_mutex);
};

struct netdev_dpdk {
    struct netdev up;
    int port_id;
    int max_packet_len;
    enum dpdk_dev_type type;

    struct dpdk_tx_queue *tx_q;

    struct ovs_mutex mutex OVS_ACQ_AFTER(dpdk_mutex);

    struct dpdk_mp *dpdk_mp;
    int mtu;
    int socket_id;
    int buf_size;
    struct netdev_stats stats;
    /* Protects stats */
    rte_spinlock_t stats_lock;

    struct eth_addr hwaddr;
    enum netdev_flags flags;

    struct rte_eth_link link;
    int link_reset_cnt;

    /* To create dpdk patch ports */
    struct netdev *peer;

    /* The user might request more txqs than the NIC has.  We remap those
     * ('up.n_txq') on these ('real_n_txq').
     * If the numbers match, 'txq_needs_locking' is false, otherwise it is
     * true and we will take a spinlock on transmission */
    int real_n_txq;
    bool txq_needs_locking;

    /* Spinlock for vhost transmission.  Other DPDK devices use spinlocks in
     * dpdk_tx_queue */
    rte_spinlock_t vhost_tx_lock;

    /* virtio-net structure for vhost device */
    OVSRCU_TYPE(struct virtio_net *) virtio_dev;

    /* Identifier used to distinguish vhost devices from each other */
    char vhost_id[PATH_MAX];

    /* In dpdk_list. */
    struct ovs_list list_node OVS_GUARDED_BY(dpdk_mutex);
};

struct netdev_rxq_dpdk {
    struct netdev_rxq up;
    int port_id;
};

static bool thread_is_pmd(void);

static int netdev_dpdk_construct(struct netdev *);

struct virtio_net * netdev_dpdk_get_virtio(const struct netdev_dpdk *dev);

static bool
is_dpdk_class(const struct netdev_class *class)
{
    return class->construct == netdev_dpdk_construct;
}

/* XXX: use dpdk malloc for entire OVS. in fact huge page should be used
 * for all other segments data, bss and text. */

static void *
dpdk_rte_mzalloc(size_t sz)
{
    void *ptr;

    ptr = rte_zmalloc(OVS_VPORT_DPDK, sz, OVS_CACHE_LINE_SIZE);
    if (ptr == NULL) {
        out_of_memory();
    }
    return ptr;
}

/* XXX this function should be called only by pmd threads (or by non pmd
 * threads holding the nonpmd_mempool_mutex) */
void
free_dpdk_buf(struct dp_packet *p)
{
    struct rte_mbuf *pkt = (struct rte_mbuf *) p;

    rte_pktmbuf_free_seg(pkt);
}

static void
__rte_pktmbuf_init(struct rte_mempool *mp,
                   void *opaque_arg OVS_UNUSED,
                   void *_m,
                   unsigned i OVS_UNUSED)
{
    struct rte_mbuf *m = _m;
    uint32_t buf_len = mp->elt_size - sizeof(struct dp_packet);

    RTE_MBUF_ASSERT(mp->elt_size >= sizeof(struct dp_packet));

    memset(m, 0, mp->elt_size);

    /* start of buffer is just after mbuf structure */
    m->buf_addr = (char *)m + sizeof(struct dp_packet);
    m->buf_physaddr = rte_mempool_virt2phy(mp, m) +
                    sizeof(struct dp_packet);
    m->buf_len = (uint16_t)buf_len;

    /* keep some headroom between start of buffer and data */
    m->data_off = RTE_MIN(RTE_PKTMBUF_HEADROOM, m->buf_len);

    /* init some constant fields */
    m->pool = mp;
    m->nb_segs = 1;
    m->port = 0xff;
}

static void
ovs_rte_pktmbuf_init(struct rte_mempool *mp,
                     void *opaque_arg OVS_UNUSED,
                     void *_m,
                     unsigned i OVS_UNUSED)
{
    struct rte_mbuf *m = _m;

    __rte_pktmbuf_init(mp, opaque_arg, _m, i);

    dp_packet_init_dpdk((struct dp_packet *) m, m->buf_len);
}

static struct dpdk_mp *
dpdk_mp_get(int socket_id, int mtu) OVS_REQUIRES(dpdk_mutex)
{
    struct dpdk_mp *dmp = NULL;
    char mp_name[RTE_MEMPOOL_NAMESIZE];
    unsigned mp_size;

    LIST_FOR_EACH (dmp, list_node, &dpdk_mp_list) {
        if (dmp->socket_id == socket_id && dmp->mtu == mtu) {
            dmp->refcount++;
            return dmp;
        }
    }

    dmp = dpdk_rte_mzalloc(sizeof *dmp);
    dmp->socket_id = socket_id;
    dmp->mtu = mtu;
    dmp->refcount = 1;

    mp_size = MAX_NB_MBUF;
    do {
        if (snprintf(mp_name, RTE_MEMPOOL_NAMESIZE, "ovs_mp_%d_%d_%u",
                     dmp->mtu, dmp->socket_id, mp_size) < 0) {
            return NULL;
        }

        dmp->mp = rte_mempool_create(mp_name, mp_size, MBUF_SIZE(mtu),
                                     MP_CACHE_SZ,
                                     sizeof(struct rte_pktmbuf_pool_private),
                                     rte_pktmbuf_pool_init, NULL,
                                     ovs_rte_pktmbuf_init, NULL,
                                     socket_id, 0);
    } while (!dmp->mp && rte_errno == ENOMEM && (mp_size /= 2) >= MIN_NB_MBUF);

    if (dmp->mp == NULL) {
        return NULL;
    } else {
        VLOG_DBG("Allocated \"%s\" mempool with %u mbufs", mp_name, mp_size );
    }

    list_push_back(&dpdk_mp_list, &dmp->list_node);
    return dmp;
}

static void
dpdk_mp_put(struct dpdk_mp *dmp)
{

    if (!dmp) {
        return;
    }

    dmp->refcount--;
    ovs_assert(dmp->refcount >= 0);

#if 0
    /* I could not find any API to destroy mp. */
    if (dmp->refcount == 0) {
        list_delete(dmp->list_node);
        /* destroy mp-pool. */
    }
#endif
}

static void
check_link_status(struct netdev_dpdk *dev)
{
    struct rte_eth_link link;

    rte_eth_link_get_nowait(dev->port_id, &link);

    if (dev->link.link_status != link.link_status) {
        netdev_change_seq_changed(&dev->up);

        dev->link_reset_cnt++;
        dev->link = link;
        if (dev->link.link_status) {
            VLOG_DBG_RL(&rl, "Port %d Link Up - speed %u Mbps - %s",
                        dev->port_id, (unsigned)dev->link.link_speed,
                        (dev->link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
                         ("full-duplex") : ("half-duplex"));
        } else {
            VLOG_DBG_RL(&rl, "Port %d Link Down", dev->port_id);
        }
    }
}

static void *
dpdk_watchdog(void *dummy OVS_UNUSED)
{
    struct netdev_dpdk *dev;

    pthread_detach(pthread_self());

    for (;;) {
        ovs_mutex_lock(&dpdk_mutex);
        LIST_FOR_EACH (dev, list_node, &dpdk_list) {
            ovs_mutex_lock(&dev->mutex);
            check_link_status(dev);
            ovs_mutex_unlock(&dev->mutex);
        }
        ovs_mutex_unlock(&dpdk_mutex);
        xsleep(DPDK_PORT_WATCHDOG_INTERVAL);
    }

    return NULL;
}

static int
dpdk_eth_dev_queue_setup(struct netdev_dpdk *dev, int n_rxq, int n_txq)
{
    int diag = 0;
    int i;

    /* A device may report more queues than it makes available (this has
     * been observed for Intel xl710, which reserves some of them for
     * SRIOV):  rte_eth_*_queue_setup will fail if a queue is not
     * available.  When this happens we can retry the configuration
     * and request less queues */
    while (n_rxq && n_txq) {
        if (diag) {
            VLOG_INFO("Retrying setup with (rxq:%d txq:%d)", n_rxq, n_txq);
        }

        diag = rte_eth_dev_configure(dev->port_id, n_rxq, n_txq, &port_conf);
        if (diag) {
            break;
        }

        for (i = 0; i < n_txq; i++) {
            diag = rte_eth_tx_queue_setup(dev->port_id, i, NIC_PORT_TX_Q_SIZE,
                                          dev->socket_id, NULL);
            if (diag) {
                VLOG_INFO("Interface %s txq(%d) setup error: %s",
                          dev->up.name, i, rte_strerror(-diag));
                break;
            }
        }

        if (i != n_txq) {
            /* Retry with less tx queues */
            n_txq = i;
            continue;
        }

        for (i = 0; i < n_rxq; i++) {
            diag = rte_eth_rx_queue_setup(dev->port_id, i, NIC_PORT_RX_Q_SIZE,
                                          dev->socket_id, NULL,
                                          dev->dpdk_mp->mp);
            if (diag) {
                VLOG_INFO("Interface %s rxq(%d) setup error: %s",
                          dev->up.name, i, rte_strerror(-diag));
                break;
            }
        }

        if (i != n_rxq) {
            /* Retry with less rx queues */
            n_rxq = i;
            continue;
        }

        dev->up.n_rxq = n_rxq;
        dev->real_n_txq = n_txq;

        return 0;
    }

    return diag;
}


static int
dpdk_eth_dev_init(struct netdev_dpdk *dev) OVS_REQUIRES(dpdk_mutex)
{
    struct rte_pktmbuf_pool_private *mbp_priv;
    struct rte_eth_dev_info info;
    struct ether_addr eth_addr;
    int diag;
    int n_rxq, n_txq;

    if (dev->port_id < 0 || dev->port_id >= rte_eth_dev_count()) {
        return ENODEV;
    }

    rte_eth_dev_info_get(dev->port_id, &info);

    n_rxq = MIN(info.max_rx_queues, dev->up.n_rxq);
    n_txq = MIN(info.max_tx_queues, dev->up.n_txq);

    diag = dpdk_eth_dev_queue_setup(dev, n_rxq, n_txq);
    if (diag) {
        VLOG_ERR("Interface %s(rxq:%d txq:%d) configure error: %s",
                 dev->up.name, n_rxq, n_txq, rte_strerror(-diag));
        return -diag;
    }

    diag = rte_eth_dev_start(dev->port_id);
    if (diag) {
        VLOG_ERR("Interface %s start error: %s", dev->up.name,
                 rte_strerror(-diag));
        return -diag;
    }

    rte_eth_promiscuous_enable(dev->port_id);
    rte_eth_allmulticast_enable(dev->port_id);

    memset(&eth_addr, 0x0, sizeof(eth_addr));
    rte_eth_macaddr_get(dev->port_id, &eth_addr);
    VLOG_INFO_RL(&rl, "Port %d: "ETH_ADDR_FMT"",
                    dev->port_id, ETH_ADDR_BYTES_ARGS(eth_addr.addr_bytes));

    memcpy(dev->hwaddr.ea, eth_addr.addr_bytes, ETH_ADDR_LEN);
    rte_eth_link_get_nowait(dev->port_id, &dev->link);

    mbp_priv = rte_mempool_get_priv(dev->dpdk_mp->mp);
    dev->buf_size = mbp_priv->mbuf_data_room_size - RTE_PKTMBUF_HEADROOM;

    dev->flags = NETDEV_UP | NETDEV_PROMISC;
    return 0;
}

static struct netdev_dpdk *
netdev_dpdk_cast(const struct netdev *netdev)
{
    return CONTAINER_OF(netdev, struct netdev_dpdk, up);
}

void netdev_dpdk_set_peer(struct netdev *netdev_, const char *peer);

void
netdev_dpdk_set_peer(struct netdev *netdev_, const char *peer)
{
    struct netdev_dpdk *peerdev;
    struct netdev_dpdk *netdev = netdev_dpdk_cast(netdev_);
    char *dev_name;

    dev_name = netdev_get_name(netdev_);
   
    printf("Name %s\n", dev_name);
    /* patch should be "port_name":patch */

    dev_name = strsep(&dev_name, ":"); 
    
    printf("Name %s\n", dev_name);

    LIST_FOR_EACH (netdev, list_node, &dpdk_list) {
         if (!strcmp(netdev_get_name(&netdev->up),dev_name)) {
             break;
         }
    }
    printf("peer %s\n", peer);
    
    LIST_FOR_EACH (peerdev, list_node, &dpdk_list) {
         if (!strcmp(peer,netdev_get_name(&peerdev->up))) {
            netdev->peer=peerdev;
            printf("Line %d\n", __LINE__);
            //printf("Name %s  ", netdev_get_name(&netdev->up));
            printf("Line %d\n", __LINE__);
            printf("  Peer %s\n", 
            netdev_get_name(&peerdev->up));
            printf("Line %d\n", __LINE__);
         }
    }
              
}

void netdev_dpdk_rem_peer(struct netdev *netdev_);

void 
netdev_dpdk_rem_peer(struct netdev *netdev_)
{
    struct netdev_dpdk *netdev = netdev_dpdk_cast(netdev_);

    netdev->peer = NULL;
}

netdev_dpdk_alloc(void)
{
    struct netdev_dpdk *netdev = dpdk_rte_mzalloc(sizeof *netdev);
    return &netdev->up;
}

static void
netdev_dpdk_alloc_txq(struct netdev_dpdk *netdev, unsigned int n_txqs)
{
    unsigned i;

    netdev->tx_q = dpdk_rte_mzalloc(n_txqs * sizeof *netdev->tx_q);
    for (i = 0; i < n_txqs; i++) {
        int numa_id = ovs_numa_get_numa_id(i);

        if (!netdev->txq_needs_locking) {
            /* Each index is considered as a cpu core id, since there should
             * be one tx queue for each cpu core.  If the corresponding core
             * is not on the same numa node as 'netdev', flags the
             * 'flush_tx'. */
            netdev->tx_q[i].flush_tx = netdev->socket_id == numa_id;
        } else {
            /* Queues are shared among CPUs. Always flush */
            netdev->tx_q[i].flush_tx = true;
        }
        rte_spinlock_init(&netdev->tx_q[i].tx_lock);
    }
}

static int
netdev_dpdk_init(struct netdev *netdev_, unsigned int port_no,
                 enum dpdk_dev_type type)
    OVS_REQUIRES(dpdk_mutex)
{
    struct netdev_dpdk *netdev = netdev_dpdk_cast(netdev_);
    int sid;
    int err = 0;

    ovs_mutex_init(&netdev->mutex);
    ovs_mutex_lock(&netdev->mutex);

    rte_spinlock_init(&netdev->stats_lock);

    /* If the 'sid' is negative, it means that the kernel fails
     * to obtain the pci numa info.  In that situation, always
     * use 'SOCKET0'. */
    if (type == DPDK_DEV_ETH) {
        sid = rte_eth_dev_socket_id(port_no);
    } else {
        sid = rte_lcore_to_socket_id(rte_get_master_lcore());
    }

    netdev->socket_id = sid < 0 ? SOCKET0 : sid;
    netdev->port_id = port_no;
    netdev->type = type;
    netdev->flags = 0;
    netdev->mtu = ETHER_MTU;
    netdev->max_packet_len = MTU_TO_MAX_LEN(netdev->mtu);

    netdev->dpdk_mp = dpdk_mp_get(netdev->socket_id, netdev->mtu);
    if (!netdev->dpdk_mp) {
        err = ENOMEM;
        goto unlock;
    }

    netdev_->n_txq = NR_QUEUE;
    netdev_->n_rxq = NR_QUEUE;
    netdev->real_n_txq = NR_QUEUE;

    if (type == DPDK_DEV_ETH) {
        netdev_dpdk_alloc_txq(netdev, NR_QUEUE);
        err = dpdk_eth_dev_init(netdev);
        if (err) {
            goto unlock;
        }
    }

    list_push_back(&dpdk_list, &netdev->list_node);

unlock:
    if (err) {
        rte_free(netdev->tx_q);
    }
    ovs_mutex_unlock(&netdev->mutex);
    return err;
}

static int
dpdk_dev_parse_name(const char dev_name[], const char prefix[],
                    unsigned int *port_no)
{
    const char *cport;

    if (strncmp(dev_name, prefix, strlen(prefix))) {
        return ENODEV;
    }

    cport = dev_name + strlen(prefix);
    *port_no = strtol(cport, NULL, 0); /* string must be null terminated */
    return 0;
}

static int
vhost_construct_helper(struct netdev *netdev_) OVS_REQUIRES(dpdk_mutex)
{
    struct netdev_dpdk *netdev = netdev_dpdk_cast(netdev_);

    if (rte_eal_init_ret) {
        return rte_eal_init_ret;
    }

    rte_spinlock_init(&netdev->vhost_tx_lock);
    return netdev_dpdk_init(netdev_, -1, DPDK_DEV_VHOST);
}

static int
netdev_dpdk_vhost_cuse_construct(struct netdev *netdev_)
{
    struct netdev_dpdk *netdev = netdev_dpdk_cast(netdev_);
    int err;

    ovs_mutex_lock(&dpdk_mutex);
    strncpy(netdev->vhost_id, netdev->up.name, sizeof(netdev->vhost_id));
    err = vhost_construct_helper(netdev_);
    ovs_mutex_unlock(&dpdk_mutex);
    return err;
}

static int
netdev_dpdk_vhost_user_construct(struct netdev *netdev_)
{
    struct netdev_dpdk *netdev = netdev_dpdk_cast(netdev_);
    int err;

    ovs_mutex_lock(&dpdk_mutex);
    /* Take the name of the vhost-user port and append it to the location where
     * the socket is to be created, then register the socket.
     */
    snprintf(netdev->vhost_id, sizeof(netdev->vhost_id), "%s/%s",
            vhost_sock_dir, netdev_->name);
    err = rte_vhost_driver_register(netdev->vhost_id);
    if (err) {
        VLOG_ERR("vhost-user socket device setup failure for socket %s\n",
                 netdev->vhost_id);
    }
    VLOG_INFO("Socket %s created for vhost-user port %s\n", netdev->vhost_id, netdev_->name);
    err = vhost_construct_helper(netdev_);
    ovs_mutex_unlock(&dpdk_mutex);
    return err;
}

static int
netdev_dpdk_construct(struct netdev *netdev)
{
    unsigned int port_no;
    int err;

    if (rte_eal_init_ret) {
        return rte_eal_init_ret;
    }

    /* Names always start with "dpdk" */
    err = dpdk_dev_parse_name(netdev->name, "dpdk", &port_no);
    if (err) {
        return err;
    }

    ovs_mutex_lock(&dpdk_mutex);
    err = netdev_dpdk_init(netdev, port_no, DPDK_DEV_ETH);
    ovs_mutex_unlock(&dpdk_mutex);
    return err;
}

static void
netdev_dpdk_destruct(struct netdev *netdev_)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev_);

    ovs_mutex_lock(&dev->mutex);
    rte_eth_dev_stop(dev->port_id);
    ovs_mutex_unlock(&dev->mutex);

    ovs_mutex_lock(&dpdk_mutex);
    rte_free(dev->tx_q);
    list_remove(&dev->list_node);
    dpdk_mp_put(dev->dpdk_mp);
    ovs_mutex_unlock(&dpdk_mutex);
}

static void
netdev_dpdk_vhost_destruct(struct netdev *netdev_)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev_);

    /* Can't remove a port while a guest is attached to it. */
    if (netdev_dpdk_get_virtio(dev) != NULL) {
        VLOG_ERR("Can not remove port, vhost device still attached");
                return;
    }

    ovs_mutex_lock(&dpdk_mutex);
    list_remove(&dev->list_node);
    dpdk_mp_put(dev->dpdk_mp);
    ovs_mutex_unlock(&dpdk_mutex);
}

static void
netdev_dpdk_dealloc(struct netdev *netdev_)
{
    struct netdev_dpdk *netdev = netdev_dpdk_cast(netdev_);

    rte_free(netdev);
}

static int
netdev_dpdk_get_config(const struct netdev *netdev_, struct smap *args)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev_);

    ovs_mutex_lock(&dev->mutex);

    smap_add_format(args, "configured_rx_queues", "%d", netdev_->n_rxq);
    smap_add_format(args, "requested_tx_queues", "%d", netdev_->n_txq);
    smap_add_format(args, "configured_tx_queues", "%d", dev->real_n_txq);
    ovs_mutex_unlock(&dev->mutex);

    return 0;
}

static int
netdev_dpdk_get_numa_id(const struct netdev *netdev_)
{
    struct netdev_dpdk *netdev = netdev_dpdk_cast(netdev_);

    return netdev->socket_id;
}

/* Sets the number of tx queues and rx queues for the dpdk interface.
 * If the configuration fails, do not try restoring its old configuration
 * and just returns the error. */
static int
netdev_dpdk_set_multiq(struct netdev *netdev_, unsigned int n_txq,
                       unsigned int n_rxq)
{
    struct netdev_dpdk *netdev = netdev_dpdk_cast(netdev_);
    int err = 0;
    int old_rxq, old_txq;

    if (netdev->up.n_txq == n_txq && netdev->up.n_rxq == n_rxq) {
        return err;
    }

    ovs_mutex_lock(&dpdk_mutex);
    ovs_mutex_lock(&netdev->mutex);

    rte_eth_dev_stop(netdev->port_id);

    old_txq = netdev->up.n_txq;
    old_rxq = netdev->up.n_rxq;
    netdev->up.n_txq = n_txq;
    netdev->up.n_rxq = n_rxq;

    rte_free(netdev->tx_q);
    err = dpdk_eth_dev_init(netdev);
    netdev_dpdk_alloc_txq(netdev, netdev->real_n_txq);
    if (err) {
        /* If there has been an error, it means that the requested queues
         * have not been created.  Restore the old numbers. */
        netdev->up.n_txq = old_txq;
        netdev->up.n_rxq = old_rxq;
    }

    netdev->txq_needs_locking = netdev->real_n_txq != netdev->up.n_txq;

    ovs_mutex_unlock(&netdev->mutex);
    ovs_mutex_unlock(&dpdk_mutex);

    return err;
}

static int
netdev_dpdk_vhost_set_multiq(struct netdev *netdev_, unsigned int n_txq,
                             unsigned int n_rxq)
{
    struct netdev_dpdk *netdev = netdev_dpdk_cast(netdev_);
    int err = 0;

    if (netdev->up.n_txq == n_txq && netdev->up.n_rxq == n_rxq) {
        return err;
    }

    ovs_mutex_lock(&dpdk_mutex);
    ovs_mutex_lock(&netdev->mutex);

    netdev->up.n_txq = n_txq;
    netdev->real_n_txq = 1;
    netdev->up.n_rxq = 1;

    ovs_mutex_unlock(&netdev->mutex);
    ovs_mutex_unlock(&dpdk_mutex);

    return err;
}

static struct netdev_rxq *
netdev_dpdk_rxq_alloc(void)
{
    struct netdev_rxq_dpdk *rx = dpdk_rte_mzalloc(sizeof *rx);

    return &rx->up;
}

static struct netdev_rxq_dpdk *
netdev_rxq_dpdk_cast(const struct netdev_rxq *rx)
{
    return CONTAINER_OF(rx, struct netdev_rxq_dpdk, up);
}

static int
netdev_dpdk_rxq_construct(struct netdev_rxq *rxq_)
{
    struct netdev_rxq_dpdk *rx = netdev_rxq_dpdk_cast(rxq_);
    struct netdev_dpdk *netdev = netdev_dpdk_cast(rx->up.netdev);

    ovs_mutex_lock(&netdev->mutex);
    rx->port_id = netdev->port_id;
    ovs_mutex_unlock(&netdev->mutex);

    return 0;
}

static void
netdev_dpdk_rxq_destruct(struct netdev_rxq *rxq_ OVS_UNUSED)
{
}

static void
netdev_dpdk_rxq_dealloc(struct netdev_rxq *rxq_)
{
    struct netdev_rxq_dpdk *rx = netdev_rxq_dpdk_cast(rxq_);

    rte_free(rx);
}

static inline void
dpdk_queue_flush__(struct netdev_dpdk *dev, int qid)
{
    struct dpdk_tx_queue *txq = &dev->tx_q[qid];
    uint32_t nb_tx = 0;

    while (nb_tx != txq->count) {
        uint32_t ret;

        ret = rte_eth_tx_burst(dev->port_id, qid, txq->burst_pkts + nb_tx,
                               txq->count - nb_tx);
        if (!ret) {
            break;
        }

        nb_tx += ret;
    }

    if (OVS_UNLIKELY(nb_tx != txq->count)) {
        /* free buffers, which we couldn't transmit, one at a time (each
         * packet could come from a different mempool) */
        int i;

        for (i = nb_tx; i < txq->count; i++) {
            rte_pktmbuf_free_seg(txq->burst_pkts[i]);
        }
        //rte_spinlock_lock(&dev->stats_lock);
        dev->stats.tx_dropped += txq->count-nb_tx;
        //rte_spinlock_unlock(&dev->stats_lock);
    }

    txq->count = 0;
    txq->tsc = rte_get_timer_cycles();
}

static inline void
dpdk_queue_flush(struct netdev_dpdk *dev, int qid)
{
    struct dpdk_tx_queue *txq = &dev->tx_q[qid];

    if (txq->count == 0) {
        return;
    }
    dpdk_queue_flush__(dev, qid);
}

static bool
is_vhost_running(struct virtio_net *dev)
{
    return (dev != NULL && (dev->flags & VIRTIO_DEV_RUNNING));
}

static inline void
netdev_dpdk_vhost_update_rx_counters(struct netdev_stats *stats,
                                     struct dp_packet **packets, int count)
{
    int i;
    struct dp_packet *packet;

    stats->rx_packets += count;
    for (i = 0; i < count; i++) {
        packet = packets[i];

        if (OVS_UNLIKELY(dp_packet_size(packet) < ETH_HEADER_LEN)) {
            /* This only protects the following multicast counting from
             * too short packets, but it does not stop the packet from
             * further processing. */
            stats->rx_errors++;
            stats->rx_length_errors++;
            continue;
        }

        struct eth_header *eh = (struct eth_header *) dp_packet_data(packet);
        if (OVS_UNLIKELY(eth_addr_is_multicast(eh->eth_dst))) {
            stats->multicast++;
        }

        stats->rx_bytes += dp_packet_size(packet);
    }
}

/* ---------------------   GAR ---------------- */

/* Exact match cache for frequently used flows
 *
 * The cache uses a 32-bit hash of the packet (which can be the RSS hash) to
 * search its entries for a miniflow that matches exactly the miniflow of the
 * packet. It stores the 'dpcls_rule' (rule) that matches the miniflow.
 *
 * A cache entry holds a reference to its 'dp_netdev_flow'.
 *
 * A miniflow with a given hash can be in one of EM_FLOW_HASH_SEGS different
 * entries. The 32-bit hash is split into EM_FLOW_HASH_SEGS values (each of
 * them is EM_FLOW_HASH_SHIFT bits wide and the remainder is thrown away). Each
 * value is the index of a cache entry where the miniflow could be.
 *
 *
 * Thread-safety
 * =============
 *
 * Each pmd_thread has its own private exact match cache.
 * If dp_netdev_input is not called from a pmd thread, a mutex is used.
 */

#define EM_FLOW_HASH_SHIFT 13
#define EM_FLOW_HASH_ENTRIES (1u << EM_FLOW_HASH_SHIFT)
#define EM_FLOW_HASH_MASK (EM_FLOW_HASH_ENTRIES - 1)
#define EM_FLOW_HASH_SEGS 4

struct emc_entry {
    //struct dp_netdev_flow *flow;
    //struct netdev_flow_key key;   /* key.hash used for emc hash value. */
    uint8_t mask[256];
    uint32_t hash;
};

struct emc_cache {
    struct emc_entry entries[EM_FLOW_HASH_ENTRIES];
    int sweep_idx;                /* For emc_cache_slow_sweep(). */
};

struct emc_cache flow_cache;

/* Iterate in the exact match cache through every entry that might contain a
 * miniflow with hash 'HASH'. */
#define EMC_FOR_EACH_POS_WITH_HASH(EMC, CURRENT_ENTRY, HASH)                 \
    for (uint32_t i__ = 0, srch_hash__ = (HASH);                             \
         (CURRENT_ENTRY) = &(EMC)->entries[srch_hash__ & EM_FLOW_HASH_MASK], \
         i__ < EM_FLOW_HASH_SEGS;                                            \
         i__++, srch_hash__ >>= EM_FLOW_HASH_SHIFT)
//00 00 05 00 0b 00 00 00 05 00 0a 00 08 00 45 3a 00 00 00 00 00 00 00 11 f7 00 c0 a8 01 03 c0 a8 01 02 00 3f 00 3f 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 

uint8_t entry_start[64] = {0x00 ,0x00 ,0x05 ,0x00 ,0x0b ,0x00 ,0x00 ,0x00 ,0x05 
                       ,0x00 ,0x0a ,0x00 ,0x08 ,0x00 ,0x45 ,0x3a ,0x00 ,0x00 
                       ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x11 ,0x00 ,0x00 ,0xc0 
                       ,0xa8 ,0x01 ,0x02 ,0xc0 ,0xA9 ,0x01 ,0x01 ,0x00 ,0x3F 
                       ,0x00 ,0x3F ,0x00 ,0x00 ,0xff ,0xff ,0xff ,0xff ,0x00 
                       ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 
                       ,0x00 ,0x00 ,0x40 ,0x0e ,0xe2 ,0x67 ,0x71 ,0x7f ,0x00 
                       ,0x00 }; 

uint8_t entry_start2[64] = {0x00 ,0x00 ,0x05 ,0x00 ,0x0a ,0x00 ,0x00 ,0x00 ,0x05 
                       ,0x00 ,0x0b ,0x00 ,0x08 ,0x00 ,0x45 ,0x3a ,0x00 ,0x00 
                       ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x11 ,0x00 ,0x00 ,0xc0 
                       ,0xa9 ,0x01 ,0x02 ,0xc0 ,0xA8 ,0x01 ,0x01 ,0x00 ,0x3F 
                       ,0x00 ,0x3F ,0x00 ,0x00 ,0xff ,0xff ,0xff ,0xff ,0x00 
                       ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 
                       ,0x00 ,0x00 ,0x40 ,0x0e ,0xe2 ,0x67 ,0x71 ,0x7f ,0x00 
                       ,0x00 }; 

void
dpdk_emc_cache_init(void);
void
dpdk_emc_cache_init(void)
{
    int i;
    //struct ip_header *ip;
    struct emc_entry *current_entry;
    uint32_t rslt;
    uint32_t *src;
    uint32_t *dst;
    uint32_t *ins;
    int x;

    flow_cache.sweep_idx = 0;
    for (i = 0; i < ARRAY_SIZE(flow_cache.entries); i++) {
        flow_cache.entries[i].hash=0;
    }
/* port a to port b */
    for (i = 0; i < (ARRAY_SIZE(flow_cache.entries))/2; i++) {
        //ip = (struct ip_header *) &entry_start[sizeof (struct eth_header)];

        src = (uint32_t *)&entry_start[28];
        dst = (uint32_t *)&entry_start[32];

        *src =*src + 1;
        *dst =*dst + 1;
    //printf("entry : \n");
    //for (x=0; x<64; x++)
         //printf("%02x ", entry_start[x]);
    //printf("\n\n");

        //rslt=rte_jhash(&entry_start,38,0);
        rslt=rte_hash_crc(&entry_start,38,0);

    rslt = rslt & EM_FLOW_HASH_MASK;
    //printf("rslt = %d\n", rslt);

        EMC_FOR_EACH_POS_WITH_HASH(&flow_cache, current_entry, rslt) {
          if (rte_memcmp(current_entry->mask, entry_start, 38 ) != 0) {
            if (current_entry->hash == 0) {
               rte_memcpy(current_entry->mask, entry_start, 38); 
               current_entry->hash=rslt;
               ins = 1;
            }
          }
          if (ins)
            break;
        }
     }
/* port b to port a */
    for (i = (ARRAY_SIZE(flow_cache.entries))/2; 
         i < (ARRAY_SIZE(flow_cache.entries)); i++) {
        //ip = (struct ip_header *) &entry_start[sizeof (struct eth_header)];

        src = (uint32_t *)&entry_start2[28];
        dst = (uint32_t *)&entry_start2[32];

        *src =*src + 1;
        *dst =*dst + 1;
    //printf("entry : \n");
    //for (x=0; x<64; x++)
         //printf("%02x ", entry_start[x]);
    //printf("\n\n");

        //rslt=rte_jhash(&entry_start2,38,0);
        rslt=rte_hash_crc(&entry_start2,38,0);

    rslt = rslt & EM_FLOW_HASH_MASK;
    //printf("rslt = %d\n", rslt);

        EMC_FOR_EACH_POS_WITH_HASH(&flow_cache, current_entry, rslt) {
          if (rte_memcmp(current_entry->mask, entry_start2, 38 ) != 0) {
            if (current_entry->hash == 0) {
               rte_memcpy(current_entry->mask, entry_start2, 38); 
               current_entry->hash=rslt;
               ins = 1;
            }
          }
          if (ins)
            break;
        }
     }
    if(rslt == 6156)
    {
    //printf("entry : \n");
    //for (x=0; x<64; x++)
         //printf("%02x ", entry_start2[x]);
    //printf("\n\n");

    //printf("current_entry : \n");
    //for (x=0; x<64; x++)
         //printf("%02x ", current_entry->mask[x]);
    //printf("\n\n");
    }

   
}

/* --------------   eth ip udp/tcp ------------------------ */

/* Packet Identifier Masks */
                             /*0 -------------------------------------64 */
static uint8_t eth_ip_tcp_udp[32] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                 0x00,0x00,0x00,0x00,0x08,0x00,0x00,0x00,
                                 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,
                                 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};

static uint8_t eth_ip_udp[32] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                 0x00,0x00,0x00,0x00,0x08,0x00,0x00,0x00,
                                 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x11,
                                 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};

static uint8_t eth_ip_tcp[32] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                 0x00,0x00,0x00,0x00,0x08,0x00,0x00,0x00,
                                 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x06,
                                 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};

/* Tuple Masks */

//00 00 05 00 0b 00 00 00 05 00 0a 00 08 00 45 00 00 00 00 00 00 00 00 11 f7 00 c0 a8 01 02 c0 A8 
//01 01 00 3F 00 3F 00 00 ff ff ff ff 00 00 00 00 00 00 00 00 00 00 00 00 40 0e e2 67 71 7f 00 00 


//00 00 05 00 0b 00 00 00 05 00 0a 00 08 00 45 00 00 00 00 00 00 00 00 11 f7 00 c0 a8 01 02 c0 00 00 00 00 00 00 00 00 00 ff ff ff ff 00 00 00 00 00 00 00 00 00 00 00 00 40 0e e2 67 71 7f 00 00 
static uint8_t cmp_buf[64] = {0x00, 0x00, 0x05, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x05, 0x00,
                              0x0a, 0x00, 0x08, 0x00, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00,
                              0x00, 0x00, 0x00, 0x11, 0xf7, 0x00, 0xc0, 0xa8, 0x01, 0x02,
                              0xc0, 0xa8, 0x01, 0x01, 0x00, 0x3f, 0x00, 0x3f}; 

static uint8_t eth_ip_udp_tuple[32] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                                 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                                 0x00,0x00,0xFF,0xFF,0xE0,0x00,0x00,0xFF,
                                 0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static uint8_t eth_ip_udp_tuple2[32] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,
                                 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};

static uint8_t eth_ip_tcp_tuple[32] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                                 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                                 0x00,0x00,0xFF,0xFF,0xE0,0x00,0x00,0xFF,
                                 0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static uint8_t eth_ip_tcp_tuple2[32] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,
                                 0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,
                                 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
/* --------------   End eth ip udp/tcp ------------------------ */

/* --------------   eth vlan ip udp/tcp ------------------------ */

/* Packet Identifier Masks */

                             /*0 -------------------------------------64 */
static uint8_t eth_vlan_ip_tcp_udp[32] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                     0x00,0x00,0x00,0x00,0x81,0x00,0x00,0x00,
                                     0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                     0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00 };
                                 
static uint8_t eth_vlan_ip_udp[32] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                     0x00,0x00,0x00,0x00,0x81,0x00,0x00,0x00,
                                     0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                     0x00,0x00,0x00,0x11,0x00,0x00,0x00,0x00 };
                                 
static uint8_t eth_vlan_ip_tcp[32] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                     0x00,0x00,0x00,0x00,0x81,0x00,0x00,0x00,
                                     0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                     0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00 };


/* Tuple Masks */



static uint8_t vlan_cmp_buf[64] = {0x00, 0x00, 0x05, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x05, 0x00,
                              0x0a, 0x00, 0x08, 0x00, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00,
                              0x00, 0x00, 0x00, 0x11, 0xf7, 0x00, 0xc0, 0xa8, 0x01, 0x02,
                              0xc0, 0xa8, 0x01, 0x01, 0x00, 0x3f, 0x00, 0x3f}; 

static uint8_t eth_vlan_ip_udp_tuple[32] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                                 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                                 0xFF,0xFF,0xFF,0xFF,0x00,0x00,0xFF,0xFF,
                                 0xE0,0x00,0x00,0xFF,0xFF,0x00,0xFF,0xFF };
                                 
                                 

static uint8_t eth_vlan_ip_udp_tuple2[32] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                                 0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,
                                 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};

static uint8_t eth_vlan_ip_tcp_tuple[32] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                                            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                                            0xFF,0xFF,0xFF,0xFF,0x00,0x00,0xFF,0xFF,
                                            0xE0,0x00,0x00,0xFF,0xFF,0x00,0xFF,0xFF };
                                 

static uint8_t eth_vlan_ip_tcp_tuple2[32] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                                 0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF,0x00,
                                 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
/* --------------   End vlan eth ip udp/tcp ------------------------ */
                          
int parse_lookup2(struct netdev *netdev, struct dp_packet *packet);

int parse_lookup2(struct netdev *netdev, struct dp_packet *packet)
{
    struct eth_header *eh;
    uint8_t* buf  = (char *)dp_packet_data(packet);
    volatile uint8_t mask_buf[512];
    volatile uint8_t *mask_buf_ptr = &mask_buf[0];
    volatile __m256i *mask;
    uint size = 0; /* padded to next vector line 256 bits */
    uint32_t rslt = 0;
    int x;
    struct ip_header *ip;
    struct emc_entry *current_entry;


    //printf("Buf : \n");
    //for (x=0; x< 64; x++)
      //printf("%02x ", buf[x]);
    //printf("\n\n");


/* ----- eth ip udp/tcp ------- */
 
    eh = (struct eth_header *) buf;
    /* Pull Ethernet Header */

    if ((eh->eth_type == htons(ETH_TYPE_IP))) {
         mask = (__m256i *)(&mask_buf_ptr[0]);

          //printf("buf %p buf_ptr %p mask %p *mask %x\n",buf, mask_buf_ptr,mask, *mask);

         *mask = *((__m256i *) buf) & *((__m256i *)&eth_ip_tcp_udp[0]); 

         //printf("ip Proto %0x %0x %0x\n", ip->ip_proto, IPPROTO_UDP, IPPROTO_TCP);
         //printf("ip Proto %0x %0x %0x\n", htons(ip->ip_proto), IPPROTO_UDP, IPPROTO_TCP);
         //printf("eth Proto %0x\n", eh->eth_type);
    
         if( !rte_memcmp(&mask_buf[0], &eth_ip_udp[0], 32)) {
         //if( (__m256i)(*mask) & (__m256i) (*(__m256i *)&eth_ip_tcp_udp[0]) ) {
     //printf("UDP\n");
             *mask = *((__m256i *) buf) & *((__m256i *)&eth_ip_udp_tuple[0]); 
             buf += 32;
             mask_buf_ptr += 32;
             mask = (__m256i *)&(mask_buf_ptr[0]);

             *mask = *((__m256i *) buf) & *((__m256i *)&eth_ip_udp_tuple2[0]); 
             size = 64;
         } else if( !rte_memcmp(&mask_buf[0], &eth_ip_tcp[0], 32)) {
    
//printf("TCP\n");
             *mask = *((__m256i *) buf) & *((__m256i *)&eth_ip_tcp_tuple[0]); 
             buf += 32;
             mask_buf_ptr += 32;
             mask = (__m256i *)&(mask_buf_ptr[0]);

             *mask = *((__m256i *) buf) & *((__m256i *)&eth_ip_tcp_tuple2[0]); 
             size = 64;

/* ----- END eth ip udp/tcp ------- */
      }

    } else if ((eh->eth_type == htons(ETH_TYPE_VLAN))) {

/* ----- eth vlan ip udp/tcp ------- */

        mask = (__m256i *)(&mask_buf_ptr[0]);

     //printf("buf %p buf_ptr %p mask %p *mask %x\n",buf, mask_buf_ptr,mask, *mask);

        *mask = *((__m256i *) buf) & *((__m256i *)&eth_vlan_ip_tcp_udp[0]); 


    //printf("ip Proto %0x %0x %0x\n", ip->ip_proto, IPPROTO_UDP, IPPROTO_TCP);
    //printf("ip Proto %0x %0x %0x\n", htons(ip->ip_proto), IPPROTO_UDP, IPPROTO_TCP);
    //printf("eth Proto %0x\n", eh->eth_type);
    
        if( !rte_memcmp(&mask_buf[0], &eth_vlan_ip_udp[0], 32)) {
//printf("VLAN UDP\n");
            *mask = *((__m256i *) buf) & *((__m256i *)&eth_vlan_ip_udp_tuple[0]); 
            buf += 32;
            mask_buf_ptr += 32;
            mask = (__m256i *)&(mask_buf_ptr[0]);

            *mask = *((__m256i *) buf) & *((__m256i *)&eth_vlan_ip_udp_tuple2[0]); 
             size = 64;
        } else if( !rte_memcmp(&mask_buf[0], &eth_vlan_ip_tcp[0], 32)) {
       
//printf("VLAN TCP\n");
            *mask = *((__m256i *) buf) & *((__m256i *)&eth_vlan_ip_tcp_tuple[0]); 
            buf += 32;
            mask_buf_ptr += 32;
            mask = (__m256i *)&(mask_buf_ptr[0]);

            *mask = *((__m256i *) buf) & *((__m256i *)&eth_vlan_ip_tcp_tuple2[0]); 
             size = 64;
        }
    }
/* ----- END eth vlan ip udp/tcp ------- */

    //printf("Mask : \n");
    //for (x=0; x<38; x++)
         //printf("%02x ", mask_buf[x]);
    //printf("\n\n");

   // rslt=rte_jhash(mask_buf,size,0);
    //rslt=rte_jhash(mask_buf,38,0);
      rslt=rte_hash_crc(&entry_start,38,0);
    rslt = rslt & EM_FLOW_HASH_MASK;
    //printf("rslt %d\n", rslt);
    EMC_FOR_EACH_POS_WITH_HASH(&flow_cache, current_entry, rslt) {
    //printf("Mask : \n");
    //for (x=0; x<38; x++)
         //printf("%02x ", current_entry->mask[x]);
    //printf("\n\n");
    if (!rte_memcmp(current_entry->mask, mask_buf, 38 )) {
          //printf ("found \n");
          rslt = current_entry->hash;
          goto out;
          }
    }
    //rslt=rte_memcmp(cmp_buf,mask_buf, size);

out :
    return (int) rslt;
}

#if 0
int parse_lookup(struct netdev *netdev, struct dp_packet *packet);

int parse_lookup(struct netdev *netdev, struct dp_packet *packet)
{
    struct eth_header *eh;
    uint8_t* buf __attribute__ ((unaligned)) = (char *)dp_packet_data(packet);
    volatile uint8_t mask_buf[512];
    volatile uint8_t *mask_buf_ptr __attribute__ ((unaligned))= &mask_buf[0];
    volatile __m256i *mask;
    uint size = 0;
    uint rslt = 0;
    int x;
    struct ip_header *ip;

    static uint8_t eth_mask[16] = {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
                          0xff,0xff,0x00,0x00};

    static uint8_t ip_mask1[16] = {0xFF,0xFF,0x00,0x00,0xFF,0xFF,0xE0,0x00,0xFF,0xFF,0x00,0x00,
                          0xFF,0xFF,0xFF,0xFF};
    static uint8_t ip_mask_udp[16] =  {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,
                          0x00,0x00,0x00,0x00};
    static uint8_t ip_mask_tcp[16] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,
                          0x00,0x00,0x00,0x00};
    static uint8_t ip_mask_tcp2[16] =   {0x00,0x3F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                          0x00,0x00,0x00,0x00};

    //printf("Buf : \n");
    //for (x=0; x< 64; x++)
      //printf("%02x ", buf[x]);
    //printf("\n\n");

    eh = (struct eth_header *) buf;


    /* Pull Ethernet Header */
    size = sizeof(struct eth_header);
    mask = (__m256i *)(&mask_buf_ptr[0]);
     //printf("buf %p buf_ptr %p mask %p *mask %x\n",buf, mask_buf_ptr,mask, *mask);
    *mask = *((__m256i *) buf) & *((__m256i *)&eth_mask[0]); 

    //printf("Mask : \n");
    //for (x=0; x< 64; x++)
      //printf("%02x ", mask_buf[x]);
    //printf("\n\n");
    ip = (struct ip_header *) (buf+14);
    //printf("ip Proto %0x %0x %0x\n", ip->ip_proto, IPPROTO_UDP, IPPROTO_TCP);
    //printf("ip Proto %0x %0x %0x\n", htons(ip->ip_proto), IPPROTO_UDP, IPPROTO_TCP);
    //printf("eth Proto %0x\n", eh->eth_type);

    buf +=14;
    mask_buf_ptr+=14;
    mask = (__m256i *)&(mask_buf_ptr[0]);

    if ((eh->eth_type == htons(ETH_TYPE_IP))) {
    

//printf("IP \n");

         /* Pull first 128 bits of IP Header */
         //buf += sizeof(struct eth_header);
         //mask_buf_ptr += sizeof(struct eth_header);

         //printf("buf %p buf_ptr %p mask %p *mask %x\n",buf, mask_buf_ptr,mask, *mask);
         *mask = *((__m256i *) buf) & *((__m256i* )ip_mask1); 

         //printf("Mask : \n");
         //for (x=0; x< 64; x++)
           //printf("%02x ", mask_buf[x]);
         //printf("\n\n");

         /* Pull IP + protocol */
         size += 16;
         mask_buf_ptr += 16;
         mask = (__m256i *)&(mask_buf_ptr[0]);

         //printf("buf %p buf_ptr %p mask %p *mask %x\n",buf, mask_buf_ptr,mask, *mask);

         if (ip->ip_proto == IPPROTO_UDP) { 

    //printf("ip Proto UDP %0x\n", ip->ip_proto);
    //printf("eth Proto %0x\n", eh->eth_type);
             /* Pull UDP  */

             buf += 16;
             *mask = *((__m256i *) buf) & *((__m256i *)ip_mask_udp); 
   
             size += sizeof(struct udp_header) + 4;

         } else if (ip->ip_proto == IPPROTO_TCP) { 
    //printf("ip Proto TCP %0x\n", ip->ip_proto);
    //printf("eth Proto %0x\n", eh->eth_type);

             /* Pull TCP  */

             *mask = *((__m256i *) buf) & *((__m256i *)ip_mask_tcp); 
   

            size += 16;
            mask_buf_ptr += 16;
            buf += 16;
            mask = (__m256i *)&(mask_buf_ptr[0]);

            *mask = *((__m256i *) buf) & *((__m256i *)ip_mask_tcp2); 

            size += sizeof(struct tcp_header) + 4;

         }

     } else if ((eh->eth_type == ETH_TYPE_ARP)) { 

     }

    rslt=rte_memcmp(mask_buf,mask_buf, size);

    //printf("Mask : \n");
    //for (x=0; x< 64; x++)
      //printf("%02x ", mask_buf[x]);
    //printf("\n\n");
    
    return (int) rslt;

}
#endif

/* ---------------------   GAR ---------------- */

static int
netdev_dpdk_eth_send(struct netdev *netdev, int qid,
                     struct dp_packet **pkts, int cnt, bool may_steal);

static void
__netdev_dpdk_vhost_send(struct netdev *netdev, struct dp_packet **pkts,
                         int cnt, bool may_steal);
/*
 * The receive path for the vhost port is the TX path out from guest.
 */
static int
netdev_dpdk_vhost_rxq_recv(struct netdev_rxq *rxq_,
                           struct dp_packet **packets, int *c)
{
    struct netdev_rxq_dpdk *rx = netdev_rxq_dpdk_cast(rxq_);
    struct netdev *netdev = rx->up.netdev;
    struct netdev_dpdk *vhost_dev = netdev_dpdk_cast(netdev);
    struct virtio_net *virtio_dev = netdev_dpdk_get_virtio(vhost_dev);
    int qid = 1;
    uint16_t nb_rx = 0;
    int loop = 0;
    volatile int mask = 0;

    if (OVS_UNLIKELY(!is_vhost_running(virtio_dev))) {
        return EAGAIN;
    }

    nb_rx = rte_vhost_dequeue_burst(virtio_dev, qid,
                                    vhost_dev->dpdk_mp->mp,
                                    (struct rte_mbuf **)packets,
                                    NETDEV_MAX_BURST);

    if (nb_rx) {

        for (loop=0;loop < nb_rx; loop ++)
        {
           mask = parse_lookup2(netdev,packets[loop]); 
        }

        loop = mask;
        if (vhost_dev->peer) {
            if (((struct netdev_dpdk*)vhost_dev->peer)->type == DPDK_DEV_VHOST) {
              __netdev_dpdk_vhost_send(vhost_dev->peer, packets, nb_rx, 1);
              nb_rx = 0;
            }else {
              netdev_dpdk_eth_send(vhost_dev->peer, 0, packets, nb_rx, 1); 
              nb_rx = 0;
        }
      }
    }

    if (!nb_rx) {
        return EAGAIN;
    }

    //rte_spinlock_lock(&vhost_dev->stats_lock);
    netdev_dpdk_vhost_update_rx_counters(&vhost_dev->stats, packets, nb_rx);
    //rte_spinlock_unlock(&vhost_dev->stats_lock);

    *c = (int) nb_rx;
    return 0;
}

static int
netdev_dpdk_rxq_recv(struct netdev_rxq *rxq_, struct dp_packet **packets,
                     int *c)
{
    struct netdev_rxq_dpdk *rx = netdev_rxq_dpdk_cast(rxq_);
    struct netdev *netdev = rx->up.netdev;
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    int nb_rx;

    /* There is only one tx queue for this core.  Do not flush other
     * queues.
     * Do not flush tx queue which is shared among CPUs
     * since it is always flushed */
    if (rxq_->queue_id == rte_lcore_id() &&
        OVS_LIKELY(!dev->txq_needs_locking)) {
        dpdk_queue_flush(dev, rxq_->queue_id);
    }

    nb_rx = rte_eth_rx_burst(rx->port_id, rxq_->queue_id,
                             (struct rte_mbuf **) packets,
                             NETDEV_MAX_BURST);
    if (nb_rx) {
        int loop;
        int mask;
        for (loop=0;loop < nb_rx; loop ++)
        {
           mask = parse_lookup2(dev,packets[loop]); 
        }
        if (dev->peer) {
            if (((struct netdev_dpdk*)dev->peer)->type == DPDK_DEV_VHOST) {
              __netdev_dpdk_vhost_send(dev->peer, packets, nb_rx, 1);
              nb_rx = 0;
            }else {
              netdev_dpdk_eth_send(dev->peer, 0, packets, nb_rx, 1); 
              nb_rx = 0;
        }
      }
    }
    if (!nb_rx) {
        return EAGAIN;
    }

    *c = nb_rx;

    return 0;
}

static inline void
netdev_dpdk_vhost_update_tx_counters(struct netdev_stats *stats,
                                     struct dp_packet **packets,
                                     int attempted,
                                     int dropped)
{
    int i;
    int sent = attempted - dropped;

    stats->tx_packets += sent;
    stats->tx_dropped += dropped;

    for (i = 0; i < sent; i++) {
        stats->tx_bytes += dp_packet_size(packets[i]);
    }
}

static void
__netdev_dpdk_vhost_send(struct netdev *netdev, struct dp_packet **pkts,
                         int cnt, bool may_steal)
{
    struct netdev_dpdk *vhost_dev = netdev_dpdk_cast(netdev);
    struct virtio_net *virtio_dev = netdev_dpdk_get_virtio(vhost_dev);
    struct rte_mbuf **cur_pkts = (struct rte_mbuf **) pkts;
    unsigned int total_pkts = cnt;
    uint64_t start = 0;

    if (OVS_UNLIKELY(!is_vhost_running(virtio_dev))) {
        //rte_spinlock_lock(&vhost_dev->stats_lock);
        vhost_dev->stats.tx_dropped+= cnt;
        //rte_spinlock_unlock(&vhost_dev->stats_lock);
        goto out;
    }

    /* There is vHost TX single queue, So we need to lock it for TX. */
    //rte_spinlock_lock(&vhost_dev->vhost_tx_lock);

    do {
        unsigned int tx_pkts=0;

        tx_pkts = rte_vhost_enqueue_burst(virtio_dev, VIRTIO_RXQ,
                                          cur_pkts, cnt);
        if (OVS_LIKELY(tx_pkts)) {
            /* Packets have been sent.*/
            cnt -= tx_pkts;
               /* Prepare for possible next iteration.*/
               cur_pkts = &cur_pkts[tx_pkts];
        } else {
            uint64_t timeout = VHOST_ENQ_RETRY_USECS * rte_get_timer_hz() / 1E6;
            unsigned int expired = 0;

            if (!start) {
                start = rte_get_timer_cycles();
            }

            /*
             * Unable to enqueue packets to vhost interface.
             * Check available entries before retrying.
             */
            while (!rte_vring_available_entries(virtio_dev, VIRTIO_RXQ)) {
                if (OVS_UNLIKELY((rte_get_timer_cycles() - start) > timeout)) {
                    expired = 1;
                    break;
                }
            }
            if (expired) {
                /* break out of main loop. */
                break;
            }
        }
    } while (cnt);
    //rte_spinlock_unlock(&vhost_dev->vhost_tx_lock);

    //rte_spinlock_lock(&vhost_dev->stats_lock);
    netdev_dpdk_vhost_update_tx_counters(&vhost_dev->stats, pkts, total_pkts,
                                         cnt);
    //rte_spinlock_unlock(&vhost_dev->stats_lock);

out:
    if (may_steal) {
        int i;
       
        if (total_pkts !=0) 
         rte_pktmbuf_bulk_free((struct rte_mbuf **)pkts,total_pkts);
        //for (i = 0; i < total_pkts; i++) {


            //dp_packet_delete(pkts[i]);
        //}
    }
}

inline static void
dpdk_queue_pkts(struct netdev_dpdk *dev, int qid,
               struct rte_mbuf **pkts, int cnt)
{
    struct dpdk_tx_queue *txq = &dev->tx_q[qid];
    uint64_t diff_tsc;

    int i = 0;

    while (i < cnt) {
        int freeslots = MAX_TX_QUEUE_LEN - txq->count;
        int tocopy = MIN(freeslots, cnt-i);

        memcpy(&txq->burst_pkts[txq->count], &pkts[i],
               tocopy * sizeof (struct rte_mbuf *));

        txq->count += tocopy;
        i += tocopy;

        if (txq->count == MAX_TX_QUEUE_LEN || txq->flush_tx) {
            dpdk_queue_flush__(dev, qid);
        }
        diff_tsc = rte_get_timer_cycles() - txq->tsc;
        if (diff_tsc >= DRAIN_TSC) {
            dpdk_queue_flush__(dev, qid);
        }
    }
}

/* Tx function. Transmit packets indefinitely */
static void
dpdk_do_tx_copy(struct netdev *netdev, int qid, struct dp_packet **pkts,
                int cnt)
    OVS_NO_THREAD_SAFETY_ANALYSIS
{
#if !defined(__CHECKER__) && !defined(_WIN32)
    const size_t PKT_ARRAY_SIZE = cnt;
#else
    /* Sparse or MSVC doesn't like variable length array. */
    enum { PKT_ARRAY_SIZE = NETDEV_MAX_BURST };
#endif
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    struct rte_mbuf *mbufs[PKT_ARRAY_SIZE];
    int dropped = 0;
    int newcnt = 0;
    int i;

    /* If we are on a non pmd thread we have to use the mempool mutex, because
     * every non pmd thread shares the same mempool cache */

    if (!thread_is_pmd()) {
        ovs_mutex_lock(&nonpmd_mempool_mutex);
    }

    for (i = 0; i < cnt; i++) {
        int size = dp_packet_size(pkts[i]);

        if (OVS_UNLIKELY(size > dev->max_packet_len)) {
            VLOG_WARN_RL(&rl, "Too big size %d max_packet_len %d",
                         (int)size , dev->max_packet_len);

            dropped++;
            continue;
        }

        mbufs[newcnt] = rte_pktmbuf_alloc(dev->dpdk_mp->mp);

        if (!mbufs[newcnt]) {
            dropped += cnt - i;
            break;
        }

        /* We have to do a copy for now */
        memcpy(rte_pktmbuf_mtod(mbufs[newcnt], void *), dp_packet_data(pkts[i]), size);

        rte_pktmbuf_data_len(mbufs[newcnt]) = size;
        rte_pktmbuf_pkt_len(mbufs[newcnt]) = size;

        newcnt++;
    }

    if (OVS_UNLIKELY(dropped)) {
        //rte_spinlock_lock(&dev->stats_lock);
        dev->stats.tx_dropped += dropped;
        //rte_spinlock_unlock(&dev->stats_lock);
    }

    if (dev->type == DPDK_DEV_VHOST) {
        __netdev_dpdk_vhost_send(netdev, (struct dp_packet **) mbufs, newcnt, true);
    } else {
        dpdk_queue_pkts(dev, qid, mbufs, newcnt);
        dpdk_queue_flush(dev, qid);
    }

    if (!thread_is_pmd()) {
        ovs_mutex_unlock(&nonpmd_mempool_mutex);
    }
}

static int
netdev_dpdk_vhost_send(struct netdev *netdev, int qid OVS_UNUSED, struct dp_packet **pkts,
                 int cnt, bool may_steal)
{
    if (OVS_UNLIKELY(pkts[0]->source != DPBUF_DPDK)) {
        int i;

        dpdk_do_tx_copy(netdev, qid, pkts, cnt);
        if (may_steal) {
            for (i = 0; i < cnt; i++) {
                dp_packet_delete(pkts[i]);
            }
        }
    } else {
        __netdev_dpdk_vhost_send(netdev, pkts, cnt, may_steal);
    }
    return 0;
}

static inline void
netdev_dpdk_send__(struct netdev_dpdk *dev, int qid,
                   struct dp_packet **pkts, int cnt, bool may_steal)
{
    int i;

    if (OVS_UNLIKELY(dev->txq_needs_locking)) {
        qid = qid % dev->real_n_txq;
        //rte_spinlock_lock(&dev->tx_q[qid].tx_lock);
    }

    if (OVS_UNLIKELY(!may_steal ||
                     pkts[0]->source != DPBUF_DPDK)) {
        struct netdev *netdev = &dev->up;

        dpdk_do_tx_copy(netdev, qid, pkts, cnt);

        if (may_steal) {
            for (i = 0; i < cnt; i++) {
                dp_packet_delete(pkts[i]);
            }
        }
    } else {
        int next_tx_idx = 0;
        int dropped = 0;

        for (i = 0; i < cnt; i++) {
            int size = dp_packet_size(pkts[i]);

            if (OVS_UNLIKELY(size > dev->max_packet_len)) {
                if (next_tx_idx != i) {
                    dpdk_queue_pkts(dev, qid,
                                    (struct rte_mbuf **)&pkts[next_tx_idx],
                                    i-next_tx_idx);
                }

                VLOG_WARN_RL(&rl, "Too big size %d max_packet_len %d",
                             (int)size , dev->max_packet_len);

                dp_packet_delete(pkts[i]);
                dropped++;
                next_tx_idx = i + 1;
            }
        }
        if (next_tx_idx != cnt) {
           dpdk_queue_pkts(dev, qid,
                            (struct rte_mbuf **)&pkts[next_tx_idx],
                            cnt-next_tx_idx);
        }

        if (OVS_UNLIKELY(dropped)) {
            //rte_spinlock_lock(&dev->stats_lock);
            dev->stats.tx_dropped += dropped;
            //rte_spinlock_unlock(&dev->stats_lock);
        }
    }

    if (OVS_UNLIKELY(dev->txq_needs_locking)) {
        //rte_spinlock_unlock(&dev->tx_q[qid].tx_lock);
    }
}

static int
netdev_dpdk_eth_send(struct netdev *netdev, int qid,
                     struct dp_packet **pkts, int cnt, bool may_steal)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);

    netdev_dpdk_send__(dev, qid, pkts, cnt, may_steal);
    return 0;
}

static int
netdev_dpdk_set_etheraddr(struct netdev *netdev, const struct eth_addr mac)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);

    ovs_mutex_lock(&dev->mutex);
    if (!eth_addr_equals(dev->hwaddr, mac)) {
        dev->hwaddr = mac;
        netdev_change_seq_changed(netdev);
    }
    ovs_mutex_unlock(&dev->mutex);

    return 0;
}

static int
netdev_dpdk_get_etheraddr(const struct netdev *netdev, struct eth_addr *mac)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);

    ovs_mutex_lock(&dev->mutex);
    *mac = dev->hwaddr;
    ovs_mutex_unlock(&dev->mutex);

    return 0;
}

static int
netdev_dpdk_get_mtu(const struct netdev *netdev, int *mtup)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);

    ovs_mutex_lock(&dev->mutex);
    *mtup = dev->mtu;
    ovs_mutex_unlock(&dev->mutex);

    return 0;
}

static int
netdev_dpdk_set_mtu(const struct netdev *netdev, int mtu)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    int old_mtu, err;
    struct dpdk_mp *old_mp;
    struct dpdk_mp *mp;

    ovs_mutex_lock(&dpdk_mutex);
    ovs_mutex_lock(&dev->mutex);
    if (dev->mtu == mtu) {
        err = 0;
        goto out;
    }

    mp = dpdk_mp_get(dev->socket_id, dev->mtu);
    if (!mp) {
        err = ENOMEM;
        goto out;
    }

    rte_eth_dev_stop(dev->port_id);

    old_mtu = dev->mtu;
    old_mp = dev->dpdk_mp;
    dev->dpdk_mp = mp;
    dev->mtu = mtu;
    dev->max_packet_len = MTU_TO_MAX_LEN(dev->mtu);

    err = dpdk_eth_dev_init(dev);
    if (err) {
        dpdk_mp_put(mp);
        dev->mtu = old_mtu;
        dev->dpdk_mp = old_mp;
        dev->max_packet_len = MTU_TO_MAX_LEN(dev->mtu);
        dpdk_eth_dev_init(dev);
        goto out;
    }

    dpdk_mp_put(old_mp);
    netdev_change_seq_changed(netdev);
out:
    ovs_mutex_unlock(&dev->mutex);
    ovs_mutex_unlock(&dpdk_mutex);
    return err;
}

static int
netdev_dpdk_get_carrier(const struct netdev *netdev_, bool *carrier);

static int
netdev_dpdk_vhost_get_stats(const struct netdev *netdev,
                            struct netdev_stats *stats)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);

    ovs_mutex_lock(&dev->mutex);
    memset(stats, 0, sizeof(*stats));
    /* Unsupported Stats */
    stats->collisions = UINT64_MAX;
    stats->rx_crc_errors = UINT64_MAX;
    stats->rx_fifo_errors = UINT64_MAX;
    stats->rx_frame_errors = UINT64_MAX;
    stats->rx_missed_errors = UINT64_MAX;
    stats->rx_over_errors = UINT64_MAX;
    stats->tx_aborted_errors = UINT64_MAX;
    stats->tx_carrier_errors = UINT64_MAX;
    stats->tx_errors = UINT64_MAX;
    stats->tx_fifo_errors = UINT64_MAX;
    stats->tx_heartbeat_errors = UINT64_MAX;
    stats->tx_window_errors = UINT64_MAX;
    stats->rx_dropped += UINT64_MAX;

    //rte_spinlock_lock(&dev->stats_lock);
    /* Supported Stats */
    stats->rx_packets += dev->stats.rx_packets;
    stats->tx_packets += dev->stats.tx_packets;
    stats->tx_dropped += dev->stats.tx_dropped;
    stats->multicast = dev->stats.multicast;
    stats->rx_bytes = dev->stats.rx_bytes;
    stats->tx_bytes = dev->stats.tx_bytes;
    stats->rx_errors = dev->stats.rx_errors;
    stats->rx_length_errors = dev->stats.rx_length_errors;
    //rte_spinlock_unlock(&dev->stats_lock);

    ovs_mutex_unlock(&dev->mutex);

    return 0;
}

static int
netdev_dpdk_get_stats(const struct netdev *netdev, struct netdev_stats *stats)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    struct rte_eth_stats rte_stats;
    bool gg;

    netdev_dpdk_get_carrier(netdev, &gg);
    ovs_mutex_lock(&dev->mutex);
    rte_eth_stats_get(dev->port_id, &rte_stats);

    memset(stats, 0, sizeof(*stats));

    stats->rx_packets = rte_stats.ipackets;
    stats->tx_packets = rte_stats.opackets;
    stats->rx_bytes = rte_stats.ibytes;
    stats->tx_bytes = rte_stats.obytes;
    /* DPDK counts imissed as errors, but count them here as dropped instead */
    stats->rx_errors = rte_stats.ierrors - rte_stats.imissed;
    stats->tx_errors = rte_stats.oerrors;
    stats->multicast = rte_stats.imcasts;

    //rte_spinlock_lock(&dev->stats_lock);
    stats->tx_dropped = dev->stats.tx_dropped;
    //rte_spinlock_unlock(&dev->stats_lock);

    /* These are the available DPDK counters for packets not received due to
     * local resource constraints in DPDK and NIC respectively. */
    stats->rx_dropped = rte_stats.rx_nombuf + rte_stats.imissed;
    stats->collisions = UINT64_MAX;

    stats->rx_length_errors = rte_stats.ibadlen;
    stats->rx_over_errors = UINT64_MAX;
    stats->rx_crc_errors = rte_stats.ibadcrc;
    stats->rx_frame_errors = UINT64_MAX;
    stats->rx_fifo_errors = UINT64_MAX;
    stats->rx_missed_errors = rte_stats.imissed;

    stats->tx_aborted_errors = UINT64_MAX;
    stats->tx_carrier_errors = UINT64_MAX;
    stats->tx_fifo_errors = UINT64_MAX;
    stats->tx_heartbeat_errors = UINT64_MAX;
    stats->tx_window_errors = UINT64_MAX;

    ovs_mutex_unlock(&dev->mutex);

    return 0;
}

static int
netdev_dpdk_get_features(const struct netdev *netdev_,
                         enum netdev_features *current,
                         enum netdev_features *advertised OVS_UNUSED,
                         enum netdev_features *supported OVS_UNUSED,
                         enum netdev_features *peer OVS_UNUSED)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev_);
    struct rte_eth_link link;

    ovs_mutex_lock(&dev->mutex);
    link = dev->link;
    ovs_mutex_unlock(&dev->mutex);

    if (link.link_duplex == ETH_LINK_AUTONEG_DUPLEX) {
        if (link.link_speed == ETH_LINK_SPEED_AUTONEG) {
            *current = NETDEV_F_AUTONEG;
        }
    } else if (link.link_duplex == ETH_LINK_HALF_DUPLEX) {
        if (link.link_speed == ETH_LINK_SPEED_10) {
            *current = NETDEV_F_10MB_HD;
        }
        if (link.link_speed == ETH_LINK_SPEED_100) {
            *current = NETDEV_F_100MB_HD;
        }
        if (link.link_speed == ETH_LINK_SPEED_1000) {
            *current = NETDEV_F_1GB_HD;
        }
    } else if (link.link_duplex == ETH_LINK_FULL_DUPLEX) {
        if (link.link_speed == ETH_LINK_SPEED_10) {
            *current = NETDEV_F_10MB_FD;
        }
        if (link.link_speed == ETH_LINK_SPEED_100) {
            *current = NETDEV_F_100MB_FD;
        }
        if (link.link_speed == ETH_LINK_SPEED_1000) {
            *current = NETDEV_F_1GB_FD;
        }
        if (link.link_speed == ETH_LINK_SPEED_10000) {
            *current = NETDEV_F_10GB_FD;
        }
    }

    return 0;
}

static int
netdev_dpdk_get_ifindex(const struct netdev *netdev)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev);
    int ifindex;

    ovs_mutex_lock(&dev->mutex);
    ifindex = dev->port_id;
    ovs_mutex_unlock(&dev->mutex);

    return ifindex;
}

static int
netdev_dpdk_get_carrier(const struct netdev *netdev_, bool *carrier)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev_);

    ovs_mutex_lock(&dev->mutex);
    check_link_status(dev);
    *carrier = dev->link.link_status;

    ovs_mutex_unlock(&dev->mutex);

    return 0;
}

static int
netdev_dpdk_vhost_get_carrier(const struct netdev *netdev_, bool *carrier)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev_);
    struct virtio_net *virtio_dev = netdev_dpdk_get_virtio(dev);

    ovs_mutex_lock(&dev->mutex);

    if (is_vhost_running(virtio_dev)) {
        *carrier = 1;
    } else {
        *carrier = 0;
    }

    ovs_mutex_unlock(&dev->mutex);

    return 0;
}

static long long int
netdev_dpdk_get_carrier_resets(const struct netdev *netdev_)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev_);
    long long int carrier_resets;

    ovs_mutex_lock(&dev->mutex);
    carrier_resets = dev->link_reset_cnt;
    ovs_mutex_unlock(&dev->mutex);

    return carrier_resets;
}

static int
netdev_dpdk_set_miimon(struct netdev *netdev_ OVS_UNUSED,
                       long long int interval OVS_UNUSED)
{
    return EOPNOTSUPP;
}

static int
netdev_dpdk_update_flags__(struct netdev_dpdk *dev,
                           enum netdev_flags off, enum netdev_flags on,
                           enum netdev_flags *old_flagsp) OVS_REQUIRES(dev->mutex)
{
    int err;

    if ((off | on) & ~(NETDEV_UP | NETDEV_PROMISC)) {
        return EINVAL;
    }

    *old_flagsp = dev->flags;
    dev->flags |= on;
    dev->flags &= ~off;

    if (dev->flags == *old_flagsp) {
        return 0;
    }

    if (dev->type == DPDK_DEV_ETH) {
        if (dev->flags & NETDEV_UP) {
            err = rte_eth_dev_start(dev->port_id);
            if (err)
                return -err;
        }

        if (dev->flags & NETDEV_PROMISC) {
            rte_eth_promiscuous_enable(dev->port_id);
        }

        if (!(dev->flags & NETDEV_UP)) {
            rte_eth_dev_stop(dev->port_id);
        }
    }

    return 0;
}

static int
netdev_dpdk_update_flags(struct netdev *netdev_,
                         enum netdev_flags off, enum netdev_flags on,
                         enum netdev_flags *old_flagsp)
{
    struct netdev_dpdk *netdev = netdev_dpdk_cast(netdev_);
    int error;

    ovs_mutex_lock(&netdev->mutex);
    error = netdev_dpdk_update_flags__(netdev, off, on, old_flagsp);
    ovs_mutex_unlock(&netdev->mutex);

    return error;
}

static int
netdev_dpdk_get_status(const struct netdev *netdev_, struct smap *args)
{
    struct netdev_dpdk *dev = netdev_dpdk_cast(netdev_);
    struct rte_eth_dev_info dev_info;

    if (dev->port_id < 0)
        return ENODEV;

    ovs_mutex_lock(&dev->mutex);
    rte_eth_dev_info_get(dev->port_id, &dev_info);
    ovs_mutex_unlock(&dev->mutex);

    smap_add_format(args, "driver_name", "%s", dev_info.driver_name);

    smap_add_format(args, "port_no", "%d", dev->port_id);
    smap_add_format(args, "numa_id", "%d", rte_eth_dev_socket_id(dev->port_id));
    smap_add_format(args, "driver_name", "%s", dev_info.driver_name);
    smap_add_format(args, "min_rx_bufsize", "%u", dev_info.min_rx_bufsize);
    smap_add_format(args, "max_rx_pktlen", "%u", dev_info.max_rx_pktlen);
    smap_add_format(args, "max_rx_queues", "%u", dev_info.max_rx_queues);
    smap_add_format(args, "max_tx_queues", "%u", dev_info.max_tx_queues);
    smap_add_format(args, "max_mac_addrs", "%u", dev_info.max_mac_addrs);
    smap_add_format(args, "max_hash_mac_addrs", "%u", dev_info.max_hash_mac_addrs);
    smap_add_format(args, "max_vfs", "%u", dev_info.max_vfs);
    smap_add_format(args, "max_vmdq_pools", "%u", dev_info.max_vmdq_pools);

    smap_add_format(args, "pci-vendor_id", "0x%u", dev_info.pci_dev->id.vendor_id);
    smap_add_format(args, "pci-device_id", "0x%x", dev_info.pci_dev->id.device_id);

    return 0;
}

static void
netdev_dpdk_set_admin_state__(struct netdev_dpdk *dev, bool admin_state)
    OVS_REQUIRES(dev->mutex)
{
    enum netdev_flags old_flags;

    if (admin_state) {
        netdev_dpdk_update_flags__(dev, 0, NETDEV_UP, &old_flags);
    } else {
        netdev_dpdk_update_flags__(dev, NETDEV_UP, 0, &old_flags);
    }
}

static void
netdev_dpdk_set_admin_state(struct unixctl_conn *conn, int argc,
                            const char *argv[], void *aux OVS_UNUSED)
{
    bool up;

    if (!strcasecmp(argv[argc - 1], "up")) {
        up = true;
    } else if ( !strcasecmp(argv[argc - 1], "down")) {
        up = false;
    } else {
        unixctl_command_reply_error(conn, "Invalid Admin State");
        return;
    }

    if (argc > 2) {
        struct netdev *netdev = netdev_from_name(argv[1]);
        if (netdev && is_dpdk_class(netdev->netdev_class)) {
            struct netdev_dpdk *dpdk_dev = netdev_dpdk_cast(netdev);

            ovs_mutex_lock(&dpdk_dev->mutex);
            netdev_dpdk_set_admin_state__(dpdk_dev, up);
            ovs_mutex_unlock(&dpdk_dev->mutex);

            netdev_close(netdev);
        } else {
            unixctl_command_reply_error(conn, "Not a DPDK Interface");
            netdev_close(netdev);
            return;
        }
    } else {
        struct netdev_dpdk *netdev;

        ovs_mutex_lock(&dpdk_mutex);
        LIST_FOR_EACH (netdev, list_node, &dpdk_list) {
            ovs_mutex_lock(&netdev->mutex);
            netdev_dpdk_set_admin_state__(netdev, up);
            ovs_mutex_unlock(&netdev->mutex);
        }
        ovs_mutex_unlock(&dpdk_mutex);
    }
    unixctl_command_reply(conn, "OK");
}

/*
 * Set virtqueue flags so that we do not receive interrupts.
 */
static void
set_irq_status(struct virtio_net *dev)
{
    dev->virtqueue[VIRTIO_RXQ]->used->flags = VRING_USED_F_NO_NOTIFY;
    dev->virtqueue[VIRTIO_TXQ]->used->flags = VRING_USED_F_NO_NOTIFY;
}

/*
 * A new virtio-net device is added to a vhost port.
 */
static int
new_device(struct virtio_net *dev)
{
    struct netdev_dpdk *netdev;
    bool exists = false;

    ovs_mutex_lock(&dpdk_mutex);
    /* Add device to the vhost port with the same name as that passed down. */
    LIST_FOR_EACH(netdev, list_node, &dpdk_list) {
        if (strncmp(dev->ifname, netdev->vhost_id, IF_NAME_SZ) == 0) {
            ovs_mutex_lock(&netdev->mutex);
            ovsrcu_set(&netdev->virtio_dev, dev);
            ovs_mutex_unlock(&netdev->mutex);
            exists = true;
            dev->flags |= VIRTIO_DEV_RUNNING;
            /* Disable notifications. */
            set_irq_status(dev);
            break;
        }
    }
    ovs_mutex_unlock(&dpdk_mutex);

    if (!exists) {
        VLOG_INFO("vHost Device '%s' (%ld) can't be added - name not found",
                   dev->ifname, dev->device_fh);

        return -1;
    }

    VLOG_INFO("vHost Device '%s' (%ld) has been added",
               dev->ifname, dev->device_fh);
    return 0;
}

/*
 * Remove a virtio-net device from the specific vhost port.  Use dev->remove
 * flag to stop any more packets from being sent or received to/from a VM and
 * ensure all currently queued packets have been sent/received before removing
 *  the device.
 */
static void
destroy_device(volatile struct virtio_net *dev)
{
    struct netdev_dpdk *vhost_dev;

    ovs_mutex_lock(&dpdk_mutex);
    LIST_FOR_EACH (vhost_dev, list_node, &dpdk_list) {
        if (netdev_dpdk_get_virtio(vhost_dev) == dev) {

            ovs_mutex_lock(&vhost_dev->mutex);
            dev->flags &= ~VIRTIO_DEV_RUNNING;
            ovsrcu_set(&vhost_dev->virtio_dev, NULL);
            ovs_mutex_unlock(&vhost_dev->mutex);

            /*
             * Wait for other threads to quiesce before
             * setting the virtio_dev to NULL.
             */
            ovsrcu_synchronize();
            /*
             * As call to ovsrcu_synchronize() will end the quiescent state,
             * put thread back into quiescent state before returning.
             */
            ovsrcu_quiesce_start();
        }
    }
    ovs_mutex_unlock(&dpdk_mutex);

    VLOG_INFO("vHost Device '%s' (%ld) has been removed",
               dev->ifname, dev->device_fh);
}

struct virtio_net *
netdev_dpdk_get_virtio(const struct netdev_dpdk *dev)
{
    return ovsrcu_get(struct virtio_net *, &dev->virtio_dev);
}

/*
 * These callbacks allow virtio-net devices to be added to vhost ports when
 * configuration has been fully complete.
 */
static const struct virtio_net_device_ops virtio_net_device_ops =
{
    .new_device =  new_device,
    .destroy_device = destroy_device,
};

static void *
start_vhost_loop(void *dummy OVS_UNUSED)
{
     pthread_detach(pthread_self());
     /* Put the cuse thread into quiescent state. */
     ovsrcu_quiesce_start();
     rte_vhost_driver_session_start();
     return NULL;
}

static int
dpdk_vhost_class_init(void)
{
    rte_vhost_driver_callback_register(&virtio_net_device_ops);
    ovs_thread_create("vhost_thread", start_vhost_loop, NULL);
    return 0;
}

static int
dpdk_vhost_cuse_class_init(void)
{
    int err = -1;


    /* Register CUSE device to handle IOCTLs.
     * Unless otherwise specified on the vswitchd command line, cuse_dev_name
     * is set to vhost-net.
     */
    err = rte_vhost_driver_register(cuse_dev_name);

    if (err != 0) {
        VLOG_ERR("CUSE device setup failure.");
        return -1;
    }

    dpdk_vhost_class_init();
    return 0;
}

static int
dpdk_vhost_user_class_init(void)
{
    dpdk_vhost_class_init();
    return 0;
}

static void
dpdk_common_init(void)
{
    unixctl_command_register("netdev-dpdk/set-admin-state",
                             "[netdev] up|down", 1, 2,
                             netdev_dpdk_set_admin_state, NULL);

    ovs_thread_create("dpdk_watchdog", dpdk_watchdog, NULL);
}

/* Client Rings */

static int
dpdk_ring_create(const char dev_name[], unsigned int port_no,
                 unsigned int *eth_port_id)
{
    struct dpdk_ring *ivshmem;
    char ring_name[10];
    int err;

    ivshmem = dpdk_rte_mzalloc(sizeof *ivshmem);
    if (ivshmem == NULL) {
        return ENOMEM;
    }

    /* XXX: Add support for multiquque ring. */
    err = snprintf(ring_name, 10, "%s_tx", dev_name);
    if (err < 0) {
        return -err;
    }

    /* Create single consumer/producer rings, netdev does explicit locking. */
    ivshmem->cring_tx = rte_ring_create(ring_name, DPDK_RING_SIZE, SOCKET0,
                                        RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (ivshmem->cring_tx == NULL) {
        rte_free(ivshmem);
        return ENOMEM;
    }

    err = snprintf(ring_name, 10, "%s_rx", dev_name);
    if (err < 0) {
        return -err;
    }

    /* Create single consumer/producer rings, netdev does explicit locking. */
    ivshmem->cring_rx = rte_ring_create(ring_name, DPDK_RING_SIZE, SOCKET0,
                                        RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (ivshmem->cring_rx == NULL) {
        rte_free(ivshmem);
        return ENOMEM;
    }

    err = rte_eth_from_rings(dev_name, &ivshmem->cring_rx, 1,
                             &ivshmem->cring_tx, 1, SOCKET0);

    if (err < 0) {
        rte_free(ivshmem);
        return ENODEV;
    }

    ivshmem->user_port_id = port_no;
    ivshmem->eth_port_id = rte_eth_dev_count() - 1;
    list_push_back(&dpdk_ring_list, &ivshmem->list_node);

    *eth_port_id = ivshmem->eth_port_id;
    return 0;
}

static int
dpdk_ring_open(const char dev_name[], unsigned int *eth_port_id) OVS_REQUIRES(dpdk_mutex)
{
    struct dpdk_ring *ivshmem;
    unsigned int port_no;
    int err = 0;

    /* Names always start with "dpdkr" */
    err = dpdk_dev_parse_name(dev_name, "dpdkr", &port_no);
    if (err) {
        return err;
    }

    /* look through our list to find the device */
    LIST_FOR_EACH (ivshmem, list_node, &dpdk_ring_list) {
         if (ivshmem->user_port_id == port_no) {
            VLOG_INFO("Found dpdk ring device %s:", dev_name);
            *eth_port_id = ivshmem->eth_port_id; /* really all that is needed */
            return 0;
         }
    }
    /* Need to create the device rings */
    return dpdk_ring_create(dev_name, port_no, eth_port_id);
}

static int
netdev_dpdk_ring_send(struct netdev *netdev_, int qid,
                      struct dp_packet **pkts, int cnt, bool may_steal)
{
    struct netdev_dpdk *netdev = netdev_dpdk_cast(netdev_);
    unsigned i;

    /* When using 'dpdkr' and sending to a DPDK ring, we want to ensure that the
     * rss hash field is clear. This is because the same mbuf may be modified by
     * the consumer of the ring and return into the datapath without recalculating
     * the RSS hash. */
    for (i = 0; i < cnt; i++) {
        dp_packet_rss_invalidate(pkts[i]);
    }

    netdev_dpdk_send__(netdev, qid, pkts, cnt, may_steal);
    return 0;
}

static int
netdev_dpdk_ring_construct(struct netdev *netdev)
{
    unsigned int port_no = 0;
    int err = 0;

    if (rte_eal_init_ret) {
        return rte_eal_init_ret;
    }

    ovs_mutex_lock(&dpdk_mutex);

    err = dpdk_ring_open(netdev->name, &port_no);
    if (err) {
        goto unlock_dpdk;
    }

    err = netdev_dpdk_init(netdev, port_no, DPDK_DEV_ETH);

unlock_dpdk:
    ovs_mutex_unlock(&dpdk_mutex);
    return err;
}

#define NETDEV_DPDK_CLASS(NAME, INIT, CONSTRUCT, DESTRUCT, MULTIQ, SEND, \
    GET_CARRIER, GET_STATS, GET_FEATURES, GET_STATUS, RXQ_RECV)          \
{                                                             \
    NAME,                                                     \
    INIT,                       /* init */                    \
    NULL,                       /* netdev_dpdk_run */         \
    NULL,                       /* netdev_dpdk_wait */        \
                                                              \
    netdev_dpdk_alloc,                                        \
    CONSTRUCT,                                                \
    DESTRUCT,                                                 \
    netdev_dpdk_dealloc,                                      \
    netdev_dpdk_get_config,                                   \
    NULL,                       /* netdev_dpdk_set_config */  \
    NULL,                       /* get_tunnel_config */       \
    NULL,                       /* build header */            \
    NULL,                       /* push header */             \
    NULL,                       /* pop header */              \
    netdev_dpdk_get_numa_id,    /* get_numa_id */             \
    MULTIQ,                     /* set_multiq */              \
                                                              \
    SEND,                       /* send */                    \
    NULL,                       /* send_wait */               \
                                                              \
    netdev_dpdk_set_etheraddr,                                \
    netdev_dpdk_get_etheraddr,                                \
    netdev_dpdk_get_mtu,                                      \
    netdev_dpdk_set_mtu,                                      \
    netdev_dpdk_get_ifindex,                                  \
    GET_CARRIER,                                              \
    netdev_dpdk_get_carrier_resets,                           \
    netdev_dpdk_set_miimon,                                   \
    GET_STATS,                                                \
    GET_FEATURES,                                             \
    NULL,                       /* set_advertisements */      \
                                                              \
    NULL,                       /* set_policing */            \
    NULL,                       /* get_qos_types */           \
    NULL,                       /* get_qos_capabilities */    \
    NULL,                       /* get_qos */                 \
    NULL,                       /* set_qos */                 \
    NULL,                       /* get_queue */               \
    NULL,                       /* set_queue */               \
    NULL,                       /* delete_queue */            \
    NULL,                       /* get_queue_stats */         \
    NULL,                       /* queue_dump_start */        \
    NULL,                       /* queue_dump_next */         \
    NULL,                       /* queue_dump_done */         \
    NULL,                       /* dump_queue_stats */        \
                                                              \
    NULL,                       /* get_in4 */                 \
    NULL,                       /* set_in4 */                 \
    NULL,                       /* get_in6 */                 \
    NULL,                       /* add_router */              \
    NULL,                       /* get_next_hop */            \
    GET_STATUS,                                               \
    NULL,                       /* arp_lookup */              \
                                                              \
    netdev_dpdk_update_flags,                                 \
                                                              \
    netdev_dpdk_rxq_alloc,                                    \
    netdev_dpdk_rxq_construct,                                \
    netdev_dpdk_rxq_destruct,                                 \
    netdev_dpdk_rxq_dealloc,                                  \
    RXQ_RECV,                                                 \
    NULL,                       /* rx_wait */                 \
    NULL,                       /* rxq_drain */               \
}

static int
process_vhost_flags(char *flag, char *default_val, int size,
                    char **argv, char **new_val)
{
    int changed = 0;

    /* Depending on which version of vhost is in use, process the vhost-specific
     * flag if it is provided on the vswitchd command line, otherwise resort to
     * a default value.
     *
     * For vhost-user: Process "-cuse_dev_name" to set the custom location of
     * the vhost-user socket(s).
     * For vhost-cuse: Process "-vhost_sock_dir" to set the custom name of the
     * vhost-cuse character device.
     */
    if (!strcmp(argv[1], flag) && (strlen(argv[2]) <= size)) {
        changed = 1;
        *new_val = strdup(argv[2]);
        VLOG_INFO("User-provided %s in use: %s", flag, *new_val);
    } else {
        VLOG_INFO("No %s provided - defaulting to %s", flag, default_val);
        *new_val = default_val;
    }

    return changed;
}

int
dpdk_init(int argc, char **argv)
{
    int result;
    int base = 0;
    char *pragram_name = argv[0];

    if (argc < 2 || strcmp(argv[1], "--dpdk"))
        return 0;

    /* Remove the --dpdk argument from arg list.*/
    argc--;
    argv++;

#ifdef VHOST_CUSE
    if (process_vhost_flags("-cuse_dev_name", strdup("vhost-net"),
                            PATH_MAX, argv, &cuse_dev_name)) {
#else
    if (process_vhost_flags("-vhost_sock_dir", strdup(ovs_rundir()),
                            NAME_MAX, argv, &vhost_sock_dir)) {
        struct stat s;
        int err;

        err = stat(vhost_sock_dir, &s);
        if (err) {
            VLOG_ERR("vHostUser socket DIR '%s' does not exist.",
                     vhost_sock_dir);
            return err;
        }
#endif
        /* Remove the vhost flag configuration parameters from the argument
         * list, so that the correct elements are passed to the DPDK
         * initialization function
         */
        argc -= 2;
        argv += 2;    /* Increment by two to bypass the vhost flag arguments */
        base = 2;
    }

    /* Keep the program name argument as this is needed for call to
     * rte_eal_init()
     */
    argv[0] = pragram_name;

    /* Make sure things are initialized ... */
    result = rte_eal_init(argc, argv);
    if (result < 0) {
        ovs_abort(result, "Cannot init EAL");
    }

    rte_memzone_dump(stdout);
    rte_eal_init_ret = 0;

    if (argc > result) {
        argv[result] = argv[0];
    }

    /* We are called from the main thread here */
    RTE_PER_LCORE(_lcore_id) = NON_PMD_CORE_ID;

    return result + 1 + base;
}

static const struct netdev_class dpdk_class =
    NETDEV_DPDK_CLASS(
        "dpdk",
        NULL,
        netdev_dpdk_construct,
        netdev_dpdk_destruct,
        netdev_dpdk_set_multiq,
        netdev_dpdk_eth_send,
        netdev_dpdk_get_carrier,
        netdev_dpdk_get_stats,
        netdev_dpdk_get_features,
        netdev_dpdk_get_status,
        netdev_dpdk_rxq_recv);

static const struct netdev_class dpdk_ring_class =
    NETDEV_DPDK_CLASS(
        "dpdkr",
        NULL,
        netdev_dpdk_ring_construct,
        netdev_dpdk_destruct,
        netdev_dpdk_set_multiq,
        netdev_dpdk_ring_send,
        netdev_dpdk_get_carrier,
        netdev_dpdk_get_stats,
        netdev_dpdk_get_features,
        netdev_dpdk_get_status,
        netdev_dpdk_rxq_recv);

static const struct netdev_class OVS_UNUSED dpdk_vhost_cuse_class =
    NETDEV_DPDK_CLASS(
        "dpdkvhostcuse",
        dpdk_vhost_cuse_class_init,
        netdev_dpdk_vhost_cuse_construct,
        netdev_dpdk_vhost_destruct,
        netdev_dpdk_vhost_set_multiq,
        netdev_dpdk_vhost_send,
        netdev_dpdk_vhost_get_carrier,
        netdev_dpdk_vhost_get_stats,
        NULL,
        NULL,
        netdev_dpdk_vhost_rxq_recv);

static const struct netdev_class OVS_UNUSED dpdk_vhost_user_class =
    NETDEV_DPDK_CLASS(
        "dpdkvhostuser",
        dpdk_vhost_user_class_init,
        netdev_dpdk_vhost_user_construct,
        netdev_dpdk_vhost_destruct,
        netdev_dpdk_vhost_set_multiq,
        netdev_dpdk_vhost_send,
        netdev_dpdk_vhost_get_carrier,
        netdev_dpdk_vhost_get_stats,
        NULL,
        NULL,
        netdev_dpdk_vhost_rxq_recv);

void
netdev_dpdk_register(void)
{
    static struct ovsthread_once once = OVSTHREAD_ONCE_INITIALIZER;

    if (rte_eal_init_ret) {
        return;
    }

    if (ovsthread_once_start(&once)) {
        dpdk_common_init();
        netdev_register_provider(&dpdk_class);
        netdev_register_provider(&dpdk_ring_class);
#ifdef VHOST_CUSE
        netdev_register_provider(&dpdk_vhost_cuse_class);
#else
        netdev_register_provider(&dpdk_vhost_user_class);
#endif
        ovsthread_once_done(&once);
    }
}

int
pmd_thread_setaffinity_cpu(unsigned cpu)
{
    cpu_set_t cpuset;
    int err;

    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    err = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (err) {
        VLOG_ERR("Thread affinity error %d",err);
        return err;
    }
    /* NON_PMD_CORE_ID is reserved for use by non pmd threads. */
    ovs_assert(cpu != NON_PMD_CORE_ID);
    RTE_PER_LCORE(_lcore_id) = cpu;

    return 0;
}

static bool
thread_is_pmd(void)
{
    return rte_lcore_id() != NON_PMD_CORE_ID;
}
