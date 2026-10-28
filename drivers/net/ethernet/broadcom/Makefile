# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Broadcom network device drivers.
#

obj-$(CONFIG_B44) += b44.o
obj-$(CONFIG_BCM4908_ENET) += bcm4908_enet.o
obj-$(CONFIG_BCM63XX_ENET) += bcm63xx_enet.o
obj-$(CONFIG_BCMBCA_XRDP_ETH) += bcmbca-xrdp.o
bcmbca-xrdp-y := bcmbca_xrdp.o bcmbca_runner.o bcm6846_runner.o
obj-$(CONFIG_BCMBCA_XRDP_ETH_KUNIT_TEST) += bcmbca_xrdp_test.o
obj-$(CONFIG_BCMGENET) += genet/
obj-$(CONFIG_BNX2) += bnx2.o
obj-$(CONFIG_CNIC) += cnic.o
obj-$(CONFIG_BNX2X) += bnx2x/
obj-$(CONFIG_SB1250_MAC) += sb1250-mac.o
obj-$(CONFIG_TIGON3) += tg3.o
obj-$(CONFIG_BGMAC) += bgmac.o
obj-$(CONFIG_BGMAC_BCMA) += bgmac-bcma.o bgmac-bcma-mdio.o
obj-$(CONFIG_BGMAC_PLATFORM) += bgmac-platform.o
obj-$(CONFIG_SYSTEMPORT) += bcmsysport.o
obj-$(CONFIG_BNXT) += bnxt/
obj-$(CONFIG_BCMASP) += asp2/
obj-$(CONFIG_BNGE) += bnge/

# Keep host-generated files in the output tree, including flex/bison output.
subdir- += runner-asm

$(obj)/runner-asm/runner-as: FORCE
	$(Q)$(MAKE) $(build)=$(obj)/runner-asm

quiet_cmd_runner = RUNNER  $@
      cmd_runner = $(obj)/runner-asm/runner-as $< $@

$(obj)/bcm6846_runner.bin: $(src)/bcm6846_runner.rnr $(obj)/runner-asm/runner-as FORCE
	$(call if_changed,runner)

targets += bcm6846_runner.bin
$(obj)/bcm6846_runner.o: $(obj)/bcm6846_runner.bin
AFLAGS_bcm6846_runner.o += -I$(obj)
