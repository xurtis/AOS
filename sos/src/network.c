/*
 * Copyright 2018, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(DATA61_GPL)
 */
#include "network.h"

#include <autoconf.h>
#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <poll.h>

#include <cspace/cspace.h>
#include <clock/timestamp.h>

#undef PACKED // picotcp complains as it redefines this macro
#include <pico_stack.h>
#include <pico_device.h>
#include <pico_config.h>
#include <pico_ipv4.h>
#include <pico_socket.h>
#include <pico_nat.h>
#include <pico_icmp4.h>
#include <pico_dns_client.h>
#include <pico_dev_loop.h>
#include <pico_dhcp_client.h>
#include <pico_dhcp_server.h>
#include <pico_ipfilter.h>
#include "pico_bsd_sockets.h"

#include <ethernet/ethernet.h>

#include "vmem_layout.h"
#include "dma.h"
#include "mapping.h"
#include "ut.h"


#ifndef SOS_NFS_DIR
#  ifdef CONFIG_SOS_NFS_DIR
#    define SOS_NFS_DIR CONFIG_SOS_NFS_DIR
#  else
#    define SOS_NFS_DIR ""
#  endif
#endif


/* TODO: Read this out on boot instead of hard-coding it... */
const uint8_t OUR_MAC[6] = {0x00,0x1e,0x06,0x36,0x05,0xe5};

static struct pico_device pico_dev;

static int pico_eth_send(UNUSED struct pico_device *dev, void *input_buf, int len)
{
    if (ethif_send(input_buf, len) != ETHIF_NOERROR) {
        /* If we get an error, just report that we didn't send anything */
        return 0;
    }
    /* Currently assuming that sending always succeeds unless we get an error code.
     * Given how the u-boot driver is structured, this seems to be a safe assumption. */
    return len;
}

static int pico_eth_poll(UNUSED struct pico_device *dev, int loop_score)
{
    while (loop_score > 0) {
        int len;
        int retval = ethif_recv(&len); /* This will internally call 'raw_recv_callback'
                                        * (if a packet is actually available) */
        if(retval == ETHIF_ERROR || len == 0) {
            break;
        }
        loop_score--;
    }

    /* return (original_loop_score - amount_of_packets_received) */
    return loop_score;
}

/* Called by ethernet driver when a frame is received (inside an ethif_recv()) */
void raw_recv_callback(uint8_t *in_packet, int len)
{
    /* Note that in_packet *must* be copied somewhere in this function, as the memory
     * will be re-used by the ethernet driver after this function returns. */
    pico_stack_recv(&pico_dev, in_packet, len);
}

/* This is a bit of a hack - we need a DMA size field in the ethif driver. */
ethif_dma_addr_t ethif_dma_malloc(uint32_t size, uint32_t align)
{
    dma_addr_t addr = sos_dma_malloc(size, align);
    ethif_dma_addr_t eaddr =
        { .paddr = addr.paddr, .vaddr = addr.vaddr, .size = size };
    ZF_LOGD("ethif_dma_malloc -> vaddr: %lx, paddr: %lx\n, sz: %lx",
            eaddr.vaddr, eaddr.paddr, eaddr.size);
    return eaddr;
}

void network_tick() {
    pico_stack_tick();
}

void network_init(UNUSED cspace_t *cspace, UNUSED seL4_CPtr interrupt_ntfn)
{
    ZF_LOGI("\nInitialising network...\n\n");

    /* Initialise ethernet interface first, because we won't bother initialising
     * picotcp if the interface fails to be brought up */

    /* Map the ethernet MAC MMIO registers into our address space */
    uint64_t eth_base_vaddr =
        (uint64_t)sos_map_device(cspace, ODROIDC2_ETH_PHYS_ADDR, ODROIDC2_ETH_PHYS_SIZE);

    /* Populate DMA operations required by the ethernet driver */
    ethif_dma_ops_t ethif_dma_ops;
    ethif_dma_ops.dma_malloc = &ethif_dma_malloc;
    ethif_dma_ops.dma_phys_to_virt = &sos_dma_phys_to_virt;
    ethif_dma_ops.flush_dcache_range = &sos_dma_cache_clean_invalidate;
    ethif_dma_ops.invalidate_dcache_range = &sos_dma_cache_invalidate;

    /* Try initializing the device... */
    int error = ethif_init(eth_base_vaddr, OUR_MAC, &ethif_dma_ops, &raw_recv_callback);
    ZF_LOGF_IF(error != 0, "Failed to initialise ethernet interface");

    /* Extract IP from .config */
    struct pico_ip4 netmask;
    struct pico_ip4 ipaddr;
    struct pico_ip4 gateway;
    struct pico_ip4 zero;

    pico_bsd_init();
    pico_stack_init();

    memset(&pico_dev, 0, sizeof(struct pico_device));

    pico_dev.send = pico_eth_send;
    pico_dev.poll = pico_eth_poll; // TODO NULL if async

    pico_dev.mtu = MAXIMUM_TRANSFER_UNIT;

    error = pico_device_init(&pico_dev, "sos picotcp", OUR_MAC);
    ZF_LOGF_IF(error, "Failed to init picotcp");

    pico_string_to_ipv4(CONFIG_SOS_GATEWAY, &gateway.addr);
    pico_string_to_ipv4(CONFIG_SOS_NETMASK, &netmask.addr);
    pico_string_to_ipv4(CONFIG_SOS_IP, &ipaddr.addr);
    pico_string_to_ipv4("0.0.0.0", &zero.addr);

    pico_ipv4_link_add(&pico_dev, ipaddr, netmask);
    pico_ipv4_route_add(zero, zero, gateway, 1, NULL);
}

/* The below shall be resurrected when/if we move to IRQ driven */

#if 0

#include "ringbuffer.h"

#define ARP_PRIME_TIMEOUT_MS     1000
#define ARP_PRIME_RETRY_DELAY_MS   10
#define N_DMA_BUFS 512
#define N_RX_BUFS 256
#define N_TX_BUFS 128
#define BUF_SIZE 2048

static struct pico_device pico_dev;

typedef struct {
    long buf_no;
    int length;
} rx_t;

/* declare our ring buffer data type macros */
ringBuffer_typedef(long, long_buf_t);
ringBuffer_typedef(rx_t, rx_buf_t);

/* our local structure for tracking network dma buffers */
static struct {
    /* list of allocated dma buffers */
    dma_addr_t dma_bufs[N_DMA_BUFS];

    /* ring buffer of free dma_bufs, tracked by indices into the dma_buf array */
    long_buf_t free_pool;

    /* ring buffer of recieve buffers, tracked by indices into the dma_buf array */
    rx_buf_t rx_queue;

    /* number of transmit buffers allocated */
    int n_tx_bufs;
} buffers;

/* remove a buffer from the pool of dma bufs */
static long alloc_dma_buf(void) {

    long_buf_t *pool = &buffers.free_pool;
    if (unlikely(isBufferEmpty(pool))) {
        ZF_LOGE("Out of preallocated eth buffers.");
        return -1;
    }

    int allocated_buf_no;
    bufferRead(pool, allocated_buf_no);
    return allocated_buf_no;
}

static void free_dma_buf(long buf_no)
{
    assert(buf_no < N_DMA_BUFS);
    long_buf_t *pool = &buffers.free_pool;
    assert(!isBufferFull(pool));
    bufferWrite(pool, buf_no);
}

static void init_buffers(void)
{
    memset(&buffers, 0, sizeof(buffers));

    /* initialise the free buffer pool */
    bufferInit(buffers.free_pool, N_DMA_BUFS, long);
    ZF_LOGF_IF(buffers.free_pool.elems == NULL, "Failed to calloc dma free pool");

    bufferInit(buffers.rx_queue, N_RX_BUFS, rx_t);
    ZF_LOGF_IF(buffers.rx_queue.elems == NULL, "Failed to calloc rx queue");

    long_buf_t *pool = &buffers.free_pool;

	/* allocate dma buffers for ethernet */
    for (int i = 0; i < N_DMA_BUFS; i++) {
        //TODO what is the drivers requirement for alignment
        buffers.dma_bufs[i] = sos_dma_malloc(BUF_SIZE, BUF_SIZE);
        ZF_LOGF_IF(buffers.dma_bufs[i].vaddr == 0, "Failed to dma malloc buffer %d", i);

        seL4_Error err = sos_dma_cache_clean_invalidate(buffers.dma_bufs[i].vaddr, BUF_SIZE);
        ZF_LOGF_IF(err != seL4_NoError, "Failed to clean invalidate buffer %d", i);
        bufferWrite(pool, i);
	}
}

/* pico TCP OS layer */
static UNUSED uintptr_t pico_allocate_rx_buf(UNUSED void *iface, size_t buf_size, void **cookie)
{
    if (buf_size > BUF_SIZE) {
        ZF_LOGE("Requested buf size %zu too large, max %zu", buf_size, (size_t) BUF_SIZE);
	    return 0;
    }

    long buf_no = alloc_dma_buf();
    if (buf_no == -1) {
        /* no buffers left! */
        return 0;
    }

    sos_dma_cache_invalidate(buffers.dma_bufs[buf_no].vaddr, BUF_SIZE);
    *cookie = (void *) buf_no;
    return buffers.dma_bufs[buf_no].paddr;
}

static UNUSED void pico_rx_complete(UNUSED void *iface, size_t num_bufs, void **cookies, int *lens)
{
    if (num_bufs > 1) {
        ZF_LOGW("Frame splitting not handled\n");

         /* Frame splitting is not handled. Warn and return bufs to pool. */
         for (unsigned int i = 0; i < num_bufs; i++) {
            free_dma_buf((long) cookies[i]);
        }
    } else {
        rx_buf_t *rx_queue = &buffers.rx_queue;
        assert(!isBufferFull(rx_queue));
        rx_t rx = {
            .buf_no = (long) cookies[0],
            .length = lens[0]
        };
        bufferWrite(rx_queue, rx);
        //pico_dev.__serving_interrupt = 1; // TODO use if async
    }
    return;
}

static UNUSED void pico_tx_complete(UNUSED void *iface, void *cookie)
{
    free_dma_buf((long) cookie);
    buffers.n_tx_bufs--;
}

static int pico_eth_send(UNUSED struct pico_device *dev, void *input_buf, int len)
{
    if (unlikely(len > BUF_SIZE)) {
        ZF_LOGE("Buffer size %d too big, max %d", len, BUF_SIZE);
        return 0;
    }

    if (buffers.n_tx_bufs == N_TX_BUFS) {
        return 0;
    }

    long buf_no = alloc_dma_buf();
    buffers.n_tx_bufs++;
    dma_addr_t *buf = &buffers.dma_bufs[buf_no];
    memcpy((void *) buf->vaddr, input_buf, len);
    sos_dma_cache_clean(buf->vaddr, len);
    int ret = len;
    //TODO this is what we need from the ethernet driver
    //eth_raw_tx(&buf->paddr, &ret, (void *) buf_no);
    //TODO probably need to handle some errors
    //TODO return how much was sent
    return ret;
}

static int pico_eth_poll(struct pico_device *dev, int loop_score)
{
    while (loop_score > 0) {
        rx_buf_t *rx_queue = &buffers.rx_queue;
        if (isBufferEmpty(rx_queue)) {
            // if async
            // eth_device->pico_dev.__serving_interrupt = 0;
            break;
        }

        /* get data from the rx buffer */
        rx_t rx;
        bufferRead(rx_queue, rx);
        sos_dma_cache_invalidate(buffers.dma_bufs[rx.buf_no].vaddr, rx.length);
        pico_stack_recv(dev, (void *) buffers.dma_bufs[rx.buf_no].vaddr, rx.length);
        free_dma_buf(rx.buf_no);
        loop_score--;
    }

    return loop_score;
}

#endif
