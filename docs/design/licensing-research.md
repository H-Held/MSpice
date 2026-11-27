# Licensing research (2026-11-27)

Desk research, not legal advice. Points marked (?) could not be verified from
the primary source.

## Third-party software

| Topic | Finding | Consequence for MSpice | Source |
|---|---|---|---|
| QEMU, Espressif QEMU fork | GPL-2.0 (files without notice: GPL-2.0-or-later) | Run only as a separate process; talk over a small documented socket protocol. Own QEMU peripheral models live in a separate GPL repository. | https://raw.githubusercontent.com/espressif/qemu/esp-develop/LICENSE, https://www.gnu.org/licenses/gpl-faq.html#MereAggregation |
| Shipping QEMU binaries | Mere aggregation allowed; GPL text and source (incl. patches) must be offered | Prefer download scripts over bundling | https://www.gnu.org/licenses/old-licenses/gpl-2.0.html |
| ESP32 ROM ELFs | Apache-2.0, also in binary form | May be redistributed; `.bin` files in qemu/pc-bios have no own notice (?) - download instead | https://github.com/espressif/esp-rom-elfs |
| Renode | MIT; emulation core tlib LGPL-2.1 | Good STM32 coverage, no AVR, ESP32 unclear (?) | https://github.com/renode/renode |
| simavr | GPL-3.0-or-later | Separate process only | https://github.com/buserror/simavr |
| Unicorn | GPL-2.0 library | Do not link | https://github.com/unicorn-engine/unicorn |
| ESP-IDF | Apache-2.0 | OK with NOTICE | https://github.com/espressif/esp-idf |
| STM32Cube HAL/LL | BSD-3; CMSIS Apache-2.0 | OK | https://github.com/STMicroelectronics/STM32CubeF4/blob/master/LICENSE.md |
| STM32Cube middleware | SLA0044, only for use with ST devices (?) | Never commit | https://www.st.com/SLA0044 |
| ST SVD files | Apache-2.0 per file header | OK | https://github.com/modm-io/cmsis-svd-stm32 |
| CMSIS-SVD in general | Licence per vendor | Check per folder | https://github.com/cmsis-svd/cmsis-svd-data |
| Microchip ATmega_DFP | Apache-2.0 (checked 3.6.299) | Check every other pack | https://packs.download.microchip.com/ |
| avr-libc | Modified BSD | OK | https://github.com/avrdudes/avr-libc |
| SuiteSparse KLU | LGPL-2.1-or-later (AMD, COLAMD BSD-3) | Optional, dynamic linking only | https://github.com/DrTimothyAldenDavis/SuiteSparse/blob/dev/KLU/Doc/License.txt |
| Eigen SparseLU | MPL-2.0, header-only | Possible alternative | https://gitlab.com/libeigen/eigen/-/blob/master/COPYING.README |

## Part models and datasheets

- Measured values and facts from datasheets are not protected. Technical
  drawings are protected in their form (German UrhG §2 (1) 7), not in content.
  Re-digitising curves and drawing them anew is generally fine; copying
  figures or text is not. Always cite the source document.
- Vendor SPICE models (TI, Analog Devices/LTspice, Infineon): proprietary
  terms, no general permission to redistribute found. MSpice never ships them;
  users import them locally.

## Name

TMview (mirrors EUIPO, DPMA, USPTO): no active mark "MSPICE". Expired US marks
"MSPICE" (78463676, 2004, Fastrack Design) and "MSPICE STATION" (73566517,
1985, Mentor Graphics). PSPICE (Cadence) and HSPICE (Synopsys) are registered
marks; "SPICE" alone is used generically. mspice.com is parked; package names
on PyPI, npm and crates.io were free. To do before release: direct search in
DPMAregister, EUIPO eSearch and WIPO Global Brand Database.
