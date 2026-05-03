/*
 * Copyright 2019, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(DATA61_GPL)
 */
#include <errno.h>
#include <assert.h>

#include "uboot/common.h"
#include "uboot/net.h"
#include "uboot/miiphy.h"
#include "io.h"
#include "uboot/netdev.h"
#include "unimplemented.h"

#include <ethernet/ethernet.h>
#include <utils/zf_log.h>

#include "../../sos/src/std_sync.h"

static struct eth_device uboot_eth_dev;

ethif_dma_ops_t dma_ops = {NULL};
ethif_recv_callback_t ethif_recv_callback = NULL;

ethif_err_t ethif_send(uint8_t *buf, uint32_t len)
{
    eth_lock(); // STDSYNCTRACKING
    assert(buf);
    ethif_err_t ret = (uboot_eth_dev.send(&uboot_eth_dev, buf, len) == 0) ? ETHIF_NOERROR : ETHIF_ERROR;
    eth_unlock(); // STDSYNCTRACKING
    return ret;
}

ethif_err_t ethif_recv(int *len)
{
    eth_lock(); // STDSYNCTRACKING
    assert(len);
    int result = uboot_eth_dev.recv(&uboot_eth_dev);
    if (result >= 0) {
        *len = result;
        eth_unlock(); // STDSYNCTRACKING
        return ETHIF_NOERROR;
    }
    eth_unlock(); // STDSYNCTRACKING
    return ETHIF_ERROR;
}

void uboot_process_received_packet(uint8_t *in_packet, int len)
{
    ethif_recv_callback(in_packet, len);
}

ethif_dma_ops_t *uboot_get_dma_ops()
{
    return &dma_ops;
}

int designware_ack(struct eth_device *dev);
void ethif_irq(void)
{
    eth_lock();
    designware_ack(&uboot_eth_dev);
    eth_unlock();
}

ethif_err_t ethif_init_nb(uint64_t base_addr, uint8_t mac_out[6], ethif_dma_ops_t *ops,
                       ethif_recv_callback_t recv_callback)
{
    assert(ops);
    assert(recv_callback);
    assert(mac_out);

    ZF_LOGI("Initialising ethernet interface...");

    /* Save a copy of the DMA ops for use by the driver */
    memcpy(&dma_ops, ops, sizeof(ethif_dma_ops_t));

    ethif_recv_callback = recv_callback;

    uboot_timer_init();

    /* Initialise the MII abstraction layer */
    miiphy_init();

    /* Initialise the actual Realtek PHY */
    phy_init();

    /* Populate the eth_dev functions, also does some more PHY init */
    int ret = designware_initialize(base_addr, 0, &uboot_eth_dev);

    if (ret != 0) {
        ZF_LOGE("Failed: designware_initialize.");
        return ETHIF_ERROR;
    }

    /* We must read the MAC address out of the device after the register
     * addresses have been initialized (designware_initialize), but before
     * initializing the hardware (uboot_eth_dev.init), because initializing
     * the hardware involves a soft reset which will wipe the MAC address
     * which was configured by u-boot */

    ret = designware_read_hwaddr(&uboot_eth_dev, mac_out);

    if (ret != 0) {
        ZF_LOGE("Failed: designware_read_hwaddr.");
        return ETHIF_ERROR;
    }

    ZF_LOGI("Read MAC as [%02x:%02x:%02x:%02x:%02x:%02x]",
            mac_out[0],
            mac_out[1],
            mac_out[2],
            mac_out[3],
            mac_out[4],
            mac_out[5]);

    /* NOTE: This line determines what our MAC address will be, it is
     * internally programmed into the device during the next init step*/
    memcpy(uboot_eth_dev.enetaddr, mac_out, 6);

    /* Bring up the interface - the last initialisation step */
    ret = uboot_eth_dev.init(&uboot_eth_dev);

    if (ret != 0) {
        ZF_LOGE("Failed: uboot_eth_dev.init().");
        return ETHIF_ERROR;
    }

    ZF_LOGI("interface UP");

    return ETHIF_NOERROR;
}

ethif_err_t ethif_init(uint64_t base_addr, uint8_t mac_out[6], ethif_dma_ops_t *ops,
                       ethif_recv_callback_t recv_callback) {
    eth_lock(); // STDSYNCTRACKING
    ethif_err_t ret = ethif_init_nb(base_addr, mac_out, ops, recv_callback);
    eth_unlock(); // STDSYNCTRACKING
    return ret;
}