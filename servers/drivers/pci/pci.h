/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/drivers/pci/pci.h
 * Purpose: PCIe bus discovery and VirtIO 1.0 device scanner header.
 *
 * Licensing: MIT License. Permissive Zone 1 driver header.
 */

#ifndef SERVERS_DRIVERS_PCI_H_
#define SERVERS_DRIVERS_PCI_H_

#include <stdint.h>
#include <stdbool.h>
#include <iris/pci.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse BAR registers for a discovered PCI device.
 *
 * @param ecam Base virtual address of mapped ECAM configuration window.
 * @param bus PCI bus number (0..255).
 * @param dev PCI device number (0..31).
 * @param func PCI function number (0..7).
 * @param bars Array of 6 pci_bar_t structures to populate.
 */
void pci_parse_bars(uintptr_t ecam, uint8_t bus, uint8_t dev, uint8_t func, pci_bar_t bars[6]);

/**
 * @brief Parse VirtIO 1.0+ capabilities for a discovered VirtIO PCI device.
 *
 * @param ecam Base virtual address of mapped ECAM configuration window.
 * @param dev Pointer to virtio_pci_dev_t structure to populate.
 * @return true if at least common config and notify config are present, false otherwise.
 */
bool pci_parse_virtio_caps(uintptr_t ecam, virtio_pci_dev_t* dev);

/**
 * @brief Scan PCIe buses to locate a specific VirtIO device.
 *
 * @param ecam Base virtual address of mapped ECAM configuration window.
 * @param max_buses Number of buses to scan (e.g. 16 or 256).
 * @param target_dev_id Target VirtIO device ID (e.g. 0x1042, or 0 to match any VirtIO block device).
 * @param out_dev Output virtio_pci_dev_t structure.
 * @return true if device was found and capabilities parsed, false otherwise.
 */
bool pci_find_virtio_device(uintptr_t ecam, uint8_t max_buses, uint16_t target_dev_id, virtio_pci_dev_t* out_dev);

#ifdef __cplusplus
}
#endif

#endif /* SERVERS_DRIVERS_PCI_H_ */
